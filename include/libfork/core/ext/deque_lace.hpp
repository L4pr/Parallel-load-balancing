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
        return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
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
    m_allstolen(1),
    m_splitreq(0)
    {
      if (cap > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
          throw std::length_error("Capacity too large");
      }
      if ((cap & (cap - 1)) != 0) { abort(); }

      const std::size_t bytes = sizeof(std::atomic<T>) * cap;
      void* raw = impl::allocate_virtual(bytes);
      if (!raw) throw std::bad_alloc();

      m_array = static_cast<std::atomic<T>*>(raw);

      // Touch memory pages
      volatile char* touch_ptr = static_cast<char*>(raw);
      for (std::size_t i = 0; i < bytes; i += 4096) { touch_ptr[i] = 0; }

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
      return static_cast<std::ptrdiff_t>(m_top.load(seq_cst)) >= m_worker.bottom;
  }

  constexpr void push(T const &val) noexcept {
    if (m_worker.o_allstolen) [[unlikely]] {
      const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);
      (m_array + mask_index(bot))->store(val, relaxed);

      // Matches original Lace: tail=bot, split=bot+1
      m_top.store(bot, relaxed);
      m_split.store(bot + 1, relaxed);
      m_worker.osplit = static_cast<std::ptrdiff_t>(bot + 1);
      ++m_worker.bottom;

      m_allstolen.store(0, release);
      m_worker.o_allstolen = false;
      m_splitreq.store(0, relaxed);
      return;
    }

    LF_ASSERT(ssize() < m_capacity);
    (m_array + mask_index(m_worker.bottom))->store(val, relaxed);
    ++m_worker.bottom;

    if (__builtin_expect(m_splitreq.load(relaxed), 0)) {
      grow_shared();
    }
  }

  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F &&when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {
    const std::ptrdiff_t head = m_worker.bottom - 1;
    // Fast path from original Lace sync
    if (__builtin_expect(m_splitreq.load(relaxed) == 0, 1)) {
        if (__builtin_expect(m_worker.osplit <= head, 1)) {
            m_worker.bottom = head;
            return (m_array + mask_index(head))->load(relaxed);
        }
    }
    return pop_cold_path(std::forward<F>(when_empty));
  }

  template <typename F>
  LF_NOINLINE auto pop_cold_path(F &&when_empty) noexcept -> std::invoke_result_t<F> {
    const std::ptrdiff_t head = m_worker.bottom - 1;

    // Check if stolen or empty
    if (m_worker.o_allstolen || (m_worker.osplit > head && shrink_shared(head))) {
        return pop_stolen(std::forward<F>(when_empty));
    }

    // Task is private
    m_worker.bottom = head;
    T val = (m_array + mask_index(head))->load(relaxed);

    if (m_splitreq.load(relaxed)) grow_shared();
    return val;
  }

  template <typename F>
  constexpr auto pop_stolen(F&& when_empty) -> std::invoke_result_t<F> {
      m_worker.osplit = m_worker.bottom;
      m_worker.o_allstolen = true;
      m_allstolen.store(1, release);
      return std::invoke(std::forward<F>(when_empty));
  }

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T> {
      if (m_allstolen.load(acquire)) return {.code = err::empty};

      uint32_t t = m_top.load(relaxed);
      uint32_t s = m_split.load(relaxed);

      if (static_cast<int32_t>(s - t) > 0) {
          if (m_top.compare_exchange_weak(t, t + 1, seq_cst, relaxed)) {
              T tmp = (m_array + mask_index(t))->load(relaxed);
              return {.code = err::none, .val = tmp};
          }
          return {.code = err::lost};
      }

      if (m_splitreq.load(relaxed) == 0) {
          m_splitreq.store(1, release);
      }
      return {.code = err::empty};
  }

 private:
  constexpr auto grow_shared() noexcept -> void {
      uint32_t head_idx = static_cast<uint32_t>(m_worker.bottom);
      uint32_t split_idx = static_cast<uint32_t>(m_worker.osplit);
      uint32_t newsplit = (split_idx + head_idx + 1) / 2;

      m_split.store(newsplit, release);
      m_worker.osplit = static_cast<std::ptrdiff_t>(newsplit);
      m_splitreq.store(0, relaxed);
  }

  // Exact match of lace_shrink_shared
  constexpr auto shrink_shared(std::ptrdiff_t head) noexcept -> bool {
    uint32_t tail = m_top.load(relaxed);
    uint32_t split = m_split.load(relaxed);

    if (tail != split) {
        uint32_t newsplit = (tail + split) / 2;
        m_split.store(newsplit, relaxed);
        std::atomic_thread_fence(seq_cst); // Dekker synchronization

        tail = m_top.load(relaxed);
        if (tail != split) {
            if (__builtin_expect(tail > newsplit, 0)) {
                newsplit = (tail + split) / 2;
                m_split.store(newsplit, relaxed);
            }
            m_worker.osplit = static_cast<std::ptrdiff_t>(newsplit);
            return false;
        }
    }

    m_allstolen.store(1, release);
    m_worker.o_allstolen = true;
    return true;
  }

  [[nodiscard]] LF_FORCEINLINE auto mask_index(std::ptrdiff_t i) const noexcept -> std::size_t {
      return static_cast<std::size_t>(i & m_mask);
  }

  std::atomic<T>* m_array;
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

  alignas(k_cache_line) std::atomic<uint32_t> m_top;
  std::atomic<uint32_t> m_split;
  std::atomic<uint8_t> m_allstolen;
  alignas(k_cache_line) std::atomic<uint8_t> m_splitreq;

  struct alignas(k_cache_line) {
    std::ptrdiff_t bottom{0};
    std::ptrdiff_t osplit{0};
    bool o_allstolen{false};
  } m_worker;

  static constexpr auto relaxed = std::memory_order_relaxed;
  static constexpr auto acquire = std::memory_order_acquire;
  static constexpr auto release = std::memory_order_release;
  static constexpr auto seq_cst = std::memory_order_seq_cst;
};

} // namespace ext
} // namespace lf

#endif