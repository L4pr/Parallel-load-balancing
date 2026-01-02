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

static constexpr uint32_t get_top(uint64_t val) noexcept {
  return static_cast<uint32_t>(val);
}

static constexpr uint32_t get_split(uint64_t val) noexcept {
  return static_cast<uint32_t>(val >> 32);
}

static constexpr uint64_t pack(uint32_t top, uint32_t split) noexcept {
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

      m_packed.store(0, relaxed);
      m_bottom.store(0, relaxed);
      m_splitreq.store(false, relaxed);
      m_osplit = 0;
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
      std::ptrdiff_t const bottom = m_bottom.load(relaxed);
      uint32_t top = get_top(m_packed.load(relaxed));
      return std::max(bottom - static_cast<std::ptrdiff_t>(top), std::ptrdiff_t{0});
  }

  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t {
      return m_capacity;
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
      std::ptrdiff_t const bottom = m_bottom.load(relaxed);
      uint32_t top = get_top(m_packed.load(relaxed));
      return static_cast<std::ptrdiff_t>(top) >= bottom;
  }

  constexpr void push(T const &val) noexcept {
      std::ptrdiff_t const bottom = m_bottom.load(relaxed);
      (m_array + mask_index(bottom))->store(val, relaxed);

      m_bottom.store(bottom + 1, release);

      if (m_splitreq.load(relaxed)) [[unlikely]] {
          grow_shared(bottom + 1);
      }
  }

  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F &&when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {

      const std::ptrdiff_t bottom = m_bottom.load(relaxed) - 1;
      m_bottom.store(bottom, relaxed);

      if (bottom >= m_osplit ) [[likely]] {
          return (m_array + mask_index(bottom))->load(relaxed);
      }

      if (shrink_shared(bottom)) {
          return (m_array + mask_index(bottom))->load(relaxed);
      }

      m_bottom.store(bottom + 1, relaxed);
      return std::invoke(std::forward<F>(when_empty));
  }

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T> {
      uint64_t old_p = m_packed.load(acquire);
      impl::thread_fence_seq_cst();

      uint32_t top = get_top(old_p);
      uint32_t split = get_split(old_p);

      if (top < split) {
          T tmp = (m_array + mask_index(top))->load(relaxed);

          uint64_t new_p = pack(top + 1, split);

          if (!m_packed.compare_exchange_strong(old_p, new_p, std::memory_order_acq_rel, relaxed)) {
              return {.code = err::lost, .val = {}};
          }
          return {.code = err::none, .val = tmp};
      }

      std::ptrdiff_t const bottom = m_bottom.load(acquire);

      if (top < bottom && !m_splitreq.load(relaxed)) {
          m_splitreq.store(true, relaxed);
      }

      return {.code = err::empty, .val = {}};
  }

 private:
  constexpr auto grow_shared(const std::ptrdiff_t bottom) noexcept -> void {
      std::ptrdiff_t const new_s = (m_osplit + bottom) >> 1;

      uint64_t old_p = m_packed.load(relaxed);
      uint64_t new_p;
      do {
          uint32_t top = get_top(old_p);
          new_p = pack(top, static_cast<uint32_t>(new_s));
      } while (!m_packed.compare_exchange_weak(old_p, new_p, release, relaxed));

      m_osplit = new_s;
      m_splitreq.store(false, relaxed);
  }

  constexpr auto shrink_shared(const std::ptrdiff_t bottom) noexcept -> bool {
      uint64_t old_p = m_packed.load(relaxed);
      uint32_t top = get_top(old_p);
      uint32_t split = get_split(old_p);

      if (top == split) return false;

      uint32_t new_split_val = (split + top) >> 1;
      if (new_split_val == split) new_split_val = top;

      uint64_t new_p;
      do {
          uint32_t current_top = get_top(old_p);
          new_p = pack(current_top, new_split_val);
      } while (!m_packed.compare_exchange_weak(old_p, new_p, relaxed, relaxed));

      m_osplit = static_cast<std::ptrdiff_t>(new_split_val);

      impl::thread_fence_seq_cst();

      uint64_t fresh_p = m_packed.load(relaxed);
      uint32_t fresh_top = get_top(fresh_p);

      if (fresh_top > new_split_val) {
        m_osplit = static_cast<std::ptrdiff_t>(fresh_top);
      }

      return bottom >= m_osplit;
  }

  [[nodiscard]] std::size_t mask_index(std::ptrdiff_t idx) const noexcept {
      return static_cast<std::size_t>(idx) & static_cast<std::size_t>(m_mask);
  }

  std::atomic<T>* m_array;
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

  alignas(k_cache_line) std::atomic<uint64_t> m_packed;
  alignas(k_cache_line) std::atomic<bool> m_splitreq;

  alignas(k_cache_line) std::atomic<std::ptrdiff_t> m_bottom;
  std::ptrdiff_t m_osplit;

  // Convenience aliases
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;
  static constexpr std::memory_order acquire = std::memory_order_acquire;
  static constexpr std::memory_order release = std::memory_order_release;
  static constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
};

} // namespace ext

} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */