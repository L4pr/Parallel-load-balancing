#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <new>
#include <limits>
#include <stdexcept>
#include <cstdlib>

#include "libfork/core/impl/atomics.hpp"
#include "libfork/core/impl/utility.hpp"
#include "libfork/core/macro.hpp"
#include "libfork/core/ext/deque_common.hpp"

#if defined(_WIN32) || defined(_WIN64)
    #define NOMINMAX
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace lf {

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
    m_allstolen(true), // Start empty
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

      volatile char* touch_ptr = static_cast<char*>(raw);
      for (std::size_t i = 0; i < bytes; i += 4096) {
        touch_ptr[i] = 0;
      }

      m_worker.bottom = 0;
      m_worker.osplit = 0;
      m_worker.o_allstolen = true;
  }

  ~lace_deque() noexcept {
      if (m_array) {
          impl::deallocate_virtual(m_array, sizeof(std::atomic<T>) * static_cast<std::size_t>(m_capacity));
      }
  }

  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t {
      return static_cast<std::size_t>(ssize());
  }

  [[nodiscard]] constexpr auto ssize() const noexcept -> std::ptrdiff_t {
      return static_cast<std::ptrdiff_t>(m_worker.bottom) - static_cast<std::ptrdiff_t>(m_top.load(relaxed));
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
    if (m_worker.o_allstolen) [[unlikely]] {
      const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);
      (m_array + mask_index(bot))->store(val, relaxed);

      m_top.store(bot, relaxed);
      m_split.store(bot + 1, relaxed);
      m_worker.osplit = m_worker.bottom + 1;
      ++m_worker.bottom;

      m_allstolen.store(false, release); // Publish availability last
      m_worker.o_allstolen = false;
      m_splitreq.store(false, relaxed);
      return;
    }

    LF_ASSERT(ssize() < m_capacity);
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

      if (m_splitreq.load(relaxed)) [[unlikely]] {
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
    if (static_cast<int32_t>(m_top.load(relaxed) - static_cast<uint32_t>(m_worker.bottom)) > 0) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    T val = (m_array + mask_index(m_worker.bottom))->load(relaxed);
    if (m_splitreq.load(relaxed)) {
      grow_shared();
    }
    return val;
  }

  template <typename F