#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <new>
#include <limits>
#include <stdexcept>
#include <cstdlib> // for abort

#include "libfork/core/impl/atomics.hpp" // for thread_fence_seq_cst
#include "libfork/core/impl/utility.hpp" // for k_cache_line, immovable
#include "libfork/core/macro.hpp"        // for LF_ASSERT, etc

#include "libfork/core/ext/deque_common.hpp" // For dequeable, steal_t, err, return_nullopt

// Platform headers for mmap/VirtualAlloc
#if defined(_WIN32) || defined(_WIN64)
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace lf {

// Virtual memory functions
namespace impl {
    inline auto allocate_virtual(std::size_t bytes) -> void* {
    #if defined(_WIN32) || defined(_WIN64)
        void* ptr = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        return ptr;
    #else
        void* ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (ptr == MAP_FAILED) ? nullptr : ptr;
    #endif
    }

    inline auto deallocate_virtual(void* ptr, std::size_t bytes) -> void {
    #if defined(_WIN32) || defined(_WIN64)
        VirtualFree(ptr, 0, MEM_RELEASE);
    #else
        munmap(ptr, bytes);
    #endif
    }
}

inline namespace ext {

template <dequeable T>
class lace_deque : impl::immovable<lace_deque<T>> {
  static constexpr std::size_t k_cache_line = 128;
  static constexpr std::size_t k_default_cap = 1 << 24;

 public:
  using value_type = T;

  constexpr lace_deque() : lace_deque(k_default_cap) {}

  explicit lace_deque(const std::size_t cap):
    m_mask(static_cast<std::ptrdiff_t>(cap) - 1),
    m_capacity(static_cast<std::ptrdiff_t>(cap)),
    m_top(0),
    m_split(0),
    m_allstolen(false),
    m_splitreq(false)
    {

      if (cap > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
          throw std::length_error("Capacity too large");
      }
      if ((cap & (cap - 1)) != 0) {
          abort();
      }

      const std::size_t bytes = sizeof(std::atomic<T>) * cap;
      void* raw = impl::allocate_virtual(bytes);
      if (!raw) throw std::bad_alloc();

      m_array = static_cast<std::atomic<T>*>(raw);

      // ''Touch'' each page to ensure it is committed
      volatile char* touch_ptr = static_cast<char*>(raw);
      for (std::size_t i = 0; i < bytes; i += 4096) {
        touch_ptr[i] = 0;
      }

      m_worker.bottom = 0;
      m_worker.osplit = 0;
  }

  ~lace_deque() noexcept {
      if (m_array) {
          impl::deallocate_virtual(m_array, sizeof(std::atomic<T>) * static_cast<std::size_t>(m_capacity));
      }
  }

  // --- Basic Accessors ---

  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t {
      return static_cast<std::size_t>(ssize());
  }

  [[nodiscard]] constexpr auto ssize() const noexcept -> std::ptrdiff_t {
      std::ptrdiff_t const bottom = static_cast<uint32_t>(m_worker.bottom);
      uint32_t const top = m_top.load(relaxed);
      return std::max(bottom - static_cast<std::ptrdiff_t>(top), std::ptrdiff_t{0});
  }

  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t {
      return m_capacity;
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
      if (m_worker.o_allstolen) return true;
      uint32_t const top = m_top.load(seq_cst);
      return static_cast<std::ptrdiff_t>(top) >= m_worker.bottom;
  }

  constexpr void push(T const &val) noexcept {
    LF_ASSERT(m_worker.bottom - m_top.load(relaxed) < m_capacity);
    if (m_worker.o_allstolen) [[unlikely]] {
      const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);

      (m_array + mask_index(bot))->store(val, relaxed);

      // Reset top and split
      m_top.store(bot, relaxed);
      m_split.store(bot + 1, relaxed);

      m_worker.osplit = m_worker.bottom + 1;
      ++m_worker.bottom;

      m_allstolen.store(false, release);
      m_worker.o_allstolen = false;
      m_splitreq.store(false, relaxed);

      return;
    }

    (m_array + mask_index(m_worker.bottom))->store(val, relaxed);
    ++m_worker.bottom;

    if (m_splitreq.load(relaxed)) [[unlikely]] {
      grow_shared();
    }
  }

  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F &&when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {
    if (m_worker.bottom > m_worker.osplit) [[likely]] {
      --m_worker.bottom;
      T val = (m_array + mask_index(m_worker.bottom))->load(relaxed);

      if (m_splitreq.load(std::memory_order_relaxed)) [[unlikely]] {
        grow_shared();
      }
      return val;
    }

    return pop_cold_path(std::forward<F>(when_empty));
  }

  template <typename F>
  LF_NOINLINE auto pop_cold_path(F &&when_empty) noexcept -> std::invoke_result_t<F> {
    if (m_worker.o_allstolen || m_worker.bottom <= 0) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    if (shrink_shared()) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    --m_worker.bottom;

    const uint32_t top = m_top.load(relaxed);

    if (static_cast<int32_t>(top - static_cast<uint32_t>(m_worker.bottom)) > 0) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    T val = (m_array + mask_index(m_worker.bottom))->load(relaxed);

    if (m_splitreq.load(relaxed)) {
      grow_shared();
    }
    return val;
  }

  template <typename F>
  constexpr auto pop_stolen(F&& when_empty) -> std::invoke_result_t<F> {
      m_worker.osplit = m_worker.bottom;
      m_allstolen.store(true, release);
      m_worker.o_allstolen = true;
      return std::invoke(std::forward<F>(when_empty));
  }

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T> {
      if (m_allstolen.load(relaxed)) { return {.code = err::empty}; }

      // Read split first, then top.
      const uint32_t s = m_split.load(acquire);
      uint32_t t = m_top.load(acquire);

      if (static_cast<int32_t>(s - t) > 0) {
          T tmp = (m_array + mask_index(t))->load(acquire);

          // Contend ONLY on m_top. 's' is not involved in the CAS.
          if (!m_top.compare_exchange_strong(const_cast<uint32_t&>(t), t + 1, seq_cst, relaxed)) {
              return {.code = err::lost, .val = {}};
          }
          return {.code = err::none, .val = tmp};
      }

      if (!m_splitreq.load(relaxed)) {
          m_splitreq.store(true, release);
      }

      return {.code = err::empty, .val = {}};
  }

 private:
  constexpr auto grow_shared() noexcept -> void {
      std::ptrdiff_t const new_s = (m_worker.osplit + m_worker.bottom + 1) >> 1U;

      m_split.store(static_cast<uint32_t>(new_s), release);

      m_worker.osplit = new_s;
      m_splitreq.store(false, relaxed);
  }

  constexpr auto shrink_shared() noexcept -> bool {
    const uint32_t top = m_top.load(relaxed);
    const uint32_t split = m_split.load(relaxed);

    if (top == split) {
      goto declare_empty;
    }

    const uint32_t new_split_val = top + ((split - top) >> 1U);

    m_split.store(new_split_val, release);

    std::atomic_thread_fence(std::memory_order_seq_cst);

    const uint32_t fresh_top = m_top.load(acquire);

    if (static_cast<int32_t>(fresh_top - static_cast<uint32_t>(m_worker.bottom)) >= 0) {
      goto declare_empty;
    }

    int32_t const diff = static_cast<int32_t>(new_split_val - static_cast<uint32_t>(m_worker.bottom));
    m_worker.osplit = m_worker.bottom + static_cast<std::ptrdiff_t>(diff);

    if (static_cast<int32_t>(fresh_top - new_split_val) > 0) {
      int32_t const top_diff = static_cast<int32_t>(fresh_top - static_cast<uint32_t>(m_worker.bottom));
      m_worker.osplit = m_worker.bottom + static_cast<std::ptrdiff_t>(top_diff);
    }

    if (m_worker.bottom > m_worker.osplit) return false;

    declare_empty:
        m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  [[nodiscard]] LF_FORCEINLINE auto mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t {
      return static_cast<std::size_t>(idx) & static_cast<std::size_t>(m_mask);
  }

  std::atomic<T>* m_array;
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

  alignas(k_cache_line) std::atomic<uint32_t> m_top;
  std::atomic<uint32_t> m_split;
  std::atomic<bool> m_allstolen;

  alignas(k_cache_line) std::atomic<bool> m_splitreq;

  struct alignas(k_cache_line) {
    std::ptrdiff_t bottom{0};
    std::ptrdiff_t osplit{0};
    bool o_allstolen{false};
  } m_worker;

  // Convenience aliases
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;
  static constexpr std::memory_order acquire = std::memory_order_acquire;
  static constexpr std::memory_order release = std::memory_order_release;
  static constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
  static constexpr std::memory_order acq_rel = std::memory_order_seq_cst;
};

} // namespace ext

} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */