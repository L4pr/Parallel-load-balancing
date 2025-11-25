#ifndef C9703881_3D9C_41A5_A7A2_44615C4CFA6A
#define C9703881_3D9C_41A5_A7A2_44615C4CFA6A

// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <atomic>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <new>
#include <limits>
#include <stdexcept>
#include <cstdlib> // for abort

#include "libfork/core/impl/atomics.hpp" // for thread_fence_seq_cst
#include "libfork/core/impl/utility.hpp" // for k_cache_line, immovable
#include "libfork/core/macro.hpp"        // for LF_ASSERT, etc

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

template <typename T>
concept atomicable = std::is_trivially_copyable_v<T> &&
                     std::is_copy_constructible_v<T> &&
                     std::is_move_constructible_v<T> &&
                     std::is_copy_assignable_v<T> &&
                     std::is_move_assignable_v<T>;

template <typename T>
concept lock_free = atomicable<T> && std::atomic<T>::is_always_lock_free;

template <typename T>
concept dequeable = lock_free<T> && std::default_initializable<T>;

enum class err : int {
  none = 0,
  lost,
  empty,
};

/**
 * @brief The return type of `lf::deque` `steal()` operation.
 *
 * This type is suitable for structured bindings. We return a custom type instead of a
 * `std::optional` to allow for more information to be returned as to why a steal may fail.
 */
template <typename T>
struct steal_t {
    /**
     * @brief Check if the operation succeeded.
     */
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return code == err::none; }
    /**
     * @brief Get the value like ``std::optional``.
     *
     * Requires ``code == err::none`` .
     */
    [[nodiscard]] constexpr auto operator*() noexcept -> T & {
        LF_ASSERT(code == err::none);
        return val;
    }
    /**
     * @brief Get the value like ``std::optional``.
     *
     * Requires ``code == err::none`` .
     */
    [[nodiscard]] constexpr auto operator*() const noexcept -> T const & {
        LF_ASSERT(code == err::none);
        return val;
    }
    /**
     * @brief Get the value ``like std::optional``.
     *
     * Requires ``code == err::none`` .
     */
    [[nodiscard]] constexpr auto operator->() noexcept -> T * {
        LF_ASSERT(code == err::none);
        return std::addressof(val);
    }
    /**
     * @brief Get the value ``like std::optional``.
     *
     * Requires ``code == err::none`` .
     */
    [[nodiscard]] constexpr auto operator->() const noexcept -> T const * {
        LF_ASSERT(code == err::none);
        return std::addressof(val);
    }

    /**
     * @brief The error code of the ``steal()`` operation.
     */
    err code;
    /**
     * @brief The value stolen from the deque, Only valid if ``code == err::stolen``.
     */
    T val;
};

/**
 * @brief A functor that returns ``std::nullopt``.
 */
template <typename T>
struct return_nullopt {
    /**
     * @brief Returns ``std::nullopt``.
     */
    LF_STATIC_CALL constexpr auto operator()() LF_STATIC_CONST noexcept -> std::optional<T> { return {}; }
};

struct TopSplit {
    uint32_t top;
    uint32_t split;
};

union PackedIndex {
    uint64_t whole;
    TopSplit parts;
};

template <dequeable T>
class deque : impl::immovable<deque<T>> {
  static constexpr std::size_t k_cache_line = 128;
  static constexpr std::size_t k_default_cap = 1 << 20;

 public:
  using value_type = T;

  constexpr deque() : deque(k_default_cap) {}

  explicit deque(const std::size_t cap):
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

      m_packed.store(0, relaxed);
      m_bottom.store(0, relaxed);
      m_splitreq.store(false, relaxed);
      m_osplit = 0;
  }

  ~deque() noexcept {
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
      PackedIndex p { .whole = m_packed.load(relaxed) };
      return std::max(bottom - static_cast<std::ptrdiff_t>(p.parts.top), std::ptrdiff_t{0});
  }

  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t {
      return m_capacity;
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
      std::ptrdiff_t const bottom = m_bottom.load(relaxed);
      PackedIndex p { .whole = m_packed.load(relaxed) };
      return p.parts.top >= bottom;
  }

  constexpr void push(T const &val) noexcept {
      std::ptrdiff_t const bottom = m_bottom.load(relaxed);
      (m_array + mask_index(bottom))->store(val, relaxed);

      std::atomic_thread_fence(release);
      m_bottom.store(bottom + 1, relaxed);

      if (m_splitreq.load(relaxed)) {
          grow_shared(bottom + 1);
      }
  }

  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F &&when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {

      const std::ptrdiff_t bottom = m_bottom.load(relaxed) - 1;
      m_bottom.store(bottom, relaxed);

      if (bottom >= m_osplit) {
          return (m_array + mask_index(bottom))->load(relaxed);
      }

      if (shrink_shared(bottom)) {
          return (m_array + mask_index(bottom))->load(relaxed);
      }

      m_bottom.store(bottom + 1, relaxed);
      return std::invoke(std::forward<F>(when_empty));
  }

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T> {
      PackedIndex old_p { .whole = m_packed.load(acquire) };
      impl::thread_fence_seq_cst();

      if (old_p.parts.top < old_p.parts.split) {
          T tmp = (m_array + mask_index(old_p.parts.top))->load(relaxed);

          PackedIndex new_p = old_p;
          new_p.parts.top++;

          if (!m_packed.compare_exchange_strong(old_p.whole, new_p.whole, seq_cst, relaxed)) {
              return {.code = err::lost, .val = {}};
          }
          return {.code = err::none, .val = tmp};
      }

      std::ptrdiff_t const bottom = m_bottom.load(acquire);

      if (old_p.parts.top < bottom && !m_splitreq.load(relaxed)) {
          m_splitreq.store(true, relaxed);
      }

      return {.code = err::empty, .val = {}};
  }

 private:
  constexpr auto grow_shared(const std::ptrdiff_t bottom) noexcept -> void {
      std::ptrdiff_t const new_s = (m_osplit + bottom) / 2;

      PackedIndex old_p { .whole = m_packed.load(relaxed) };
      PackedIndex new_p;
      do {
          new_p = old_p;
          new_p.parts.split = static_cast<uint32_t>(new_s);
      } while (!m_packed.compare_exchange_weak(old_p.whole, new_p.whole, release, relaxed));

      m_osplit = new_s;
      m_splitreq.store(false, relaxed);
  }

  constexpr auto shrink_shared(const std::ptrdiff_t bottom) noexcept -> bool {
      PackedIndex old_p { .whole = m_packed.load(relaxed) };

      uint32_t top = old_p.parts.top;
      uint32_t split = old_p.parts.split;

      if (top == split) return false;

      uint32_t new_split_val = (split + top) / 2;
      if (new_split_val == split) new_split_val = top;

      PackedIndex new_p;
      do {
          new_p = old_p;
          new_p.parts.split = new_split_val;
      } while (!m_packed.compare_exchange_weak(old_p.whole, new_p.whole, relaxed, relaxed));

      m_osplit = new_split_val;

      impl::thread_fence_seq_cst();

      PackedIndex fresh_p { .whole = m_packed.load(relaxed) };

      if (fresh_p.parts.top > new_split_val) {
          m_osplit = fresh_p.parts.top;
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

#endif /* C9703881_3D9C_41A5_A7A2_44615C4CFA6A */
