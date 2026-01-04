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

struct TopSplit {
    uint32_t top;
    uint32_t split;
};

[[nodiscard]] LF_FORCEINLINE static uint32_t get_top(uint64_t val) noexcept {
  return static_cast<uint32_t>(val);
}

[[nodiscard]] LF_FORCEINLINE static uint32_t get_split(uint64_t val) noexcept {
  return static_cast<uint32_t>(val >> 32);
}

[[nodiscard]] LF_FORCEINLINE static uint64_t pack(uint32_t top, uint32_t split) noexcept {
  return static_cast<uint64_t>(top) | (static_cast<uint64_t>(split) << 32);
}

template <dequeable T>
class lace_deque : impl::immovable<lace_deque<T>> {
  static constexpr std::size_t k_cache_line = 128;
  static constexpr std::size_t k_default_cap = 1 << 20;

 public:
  using value_type = T;

  constexpr lace_deque() : lace_deque(k_default_cap) {}

  explicit lace_deque(const std::size_t cap):
    m_mask(static_cast<std::ptrdiff_t>(cap) - 1),
    m_capacity(static_cast<std::ptrdiff_t>(cap)) {

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

      m_thief.packed.store(0, relaxed);
      m_splitreq.store(false, relaxed);
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
      uint32_t const top = static_cast<uint32_t>(m_thief.packed.load(relaxed));
      return std::max(bottom - static_cast<std::ptrdiff_t>(top), std::ptrdiff_t{0});
  }

  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t {
      return m_capacity;
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
      if (m_worker.o_allstolen) return true;
      uint32_t const top = m_thief.packed.load(seq_cst);
      return static_cast<std::ptrdiff_t>(top) >= m_worker.bottom;
  }

  constexpr void push(T const &val) noexcept {
    if (m_worker.o_allstolen) [[unlikely]] {
      uint32_t bot = static_cast<uint32_t>(m_worker.bottom);

      (m_array + mask_index(bot))->store(val, std::memory_order_relaxed);

      m_thief.packed.store(pack(bot, bot + 1), std::memory_order_relaxed);

      m_thief.allstolen.store(false, std::memory_order_release);

      m_worker.osplit = bot + 1;
      m_worker.o_allstolen = false;
      m_worker.bottom = bot + 1;
      m_splitreq.store(false, std::memory_order_relaxed);

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
      T val = (m_array + mask_index(m_worker.bottom))->load(std::memory_order_relaxed);

      if (m_splitreq.load(std::memory_order_relaxed)) [[unlikely]] {
        grow_shared();
      }
      return val;
    }

    return pop_cold_path(std::forward<F>(when_empty));
  }

  template <typename F>
  auto pop_cold_path(F &&when_empty) noexcept -> std::invoke_result_t<F> {
    if (m_worker.o_allstolen || m_worker.bottom <= 0) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    if (shrink_shared()) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    --m_worker.bottom;

    const uint64_t state = m_thief.packed.load(relaxed);

    if (static_cast<int32_t>(get_top(state) - static_cast<uint32_t>(m_worker.bottom)) > 0) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    T val = (m_array + mask_index(m_worker.bottom))->load(std::memory_order_relaxed);

    if (m_splitreq.load(std::memory_order_relaxed)) {
      grow_shared();
    }
    return val;
  }

  template <typename F>
  constexpr auto pop_stolen(F&& when_empty) -> std::invoke_result_t<F> {
      m_worker.bottom = 0;
      m_worker.osplit = 1;
      m_thief.allstolen.store(true, std::memory_order_release);
      m_worker.o_allstolen = true;
      return std::invoke(std::forward<F>(when_empty));
  }

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T> {
      if (m_thief.allstolen.load(acquire)) { return {.code = err::empty}; }

      uint64_t old_p = m_thief.packed.load(std::memory_order_acquire);
      const uint32_t top = get_top(old_p);

      if (const uint32_t split = get_split(old_p); top < split) {
          T tmp = (m_array + mask_index(top))->load(acquire);

          if (!m_thief.packed.compare_exchange_strong(old_p, pack(top + 1, split), std::memory_order_acq_rel, relaxed)) {
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

      uint64_t old_p = m_thief.packed.load(relaxed);
      uint64_t new_p = 0;
      do {
          new_p = pack(get_top(old_p), static_cast<uint32_t>(new_s));
      } while (!m_thief.packed.compare_exchange_weak(old_p, new_p, release, relaxed));

      m_worker.osplit = new_s;
      m_splitreq.store(false, relaxed);
  }

  constexpr auto shrink_shared() noexcept -> bool {
      uint64_t old_p = m_thief.packed.load(relaxed);
      const uint32_t top = get_top(old_p);
      const uint32_t split = get_split(old_p);

      if (top != split) {

        if (const uint32_t new_split_val = top + ((split - top) >> 1U);
            m_thief.packed.compare_exchange_strong(old_p, pack(top, new_split_val), seq_cst, relaxed)) {

          m_worker.osplit = static_cast<std::ptrdiff_t>(new_split_val);

          impl::thread_fence_seq_cst();

          const uint32_t fresh_top = get_top(m_thief.packed.load(relaxed));

          if (static_cast<int32_t>(fresh_top - static_cast<uint32_t>(m_worker.bottom)) < 0) {
            if (static_cast<int32_t>(fresh_top - new_split_val) > 0) {
              m_worker.osplit = static_cast<std::ptrdiff_t>(fresh_top);
            }

            if (static_cast<int32_t>(static_cast<uint32_t>(m_worker.osplit) - static_cast<uint32_t>(m_worker.bottom)) < 0) {
              return false;
            }
          }
        }
        return false;
      }
      declare_empty:
        m_thief.allstolen.store(true, release);
        m_worker.o_allstolen = true;
        m_worker.bottom = 0;
        return true;
    }

  [[nodiscard]] auto mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t {
      return static_cast<std::size_t>(idx) & static_cast<std::size_t>(m_mask);
  }

  std::atomic<T>* m_array;
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

  struct alignas(k_cache_line) {
    std::atomic<uint64_t> packed;
    std::atomic<bool> allstolen{false};
  } m_thief;

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
};

} // namespace ext

} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */