#ifndef LIBFORK_CORE_EXT_DEQUE_BLOCKING_HPP
#define LIBFORK_CORE_EXT_DEQUE_BLOCKING_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <algorithm>   // for max
#include <atomic>      // for atomic, atomic_thread_fence, memory_order, memo...
#include <bit>         // for has_single_bit
#include <cstddef>     // for ptrdiff_t, size_t
#include <functional>  // for invoke
#include <memory>      // for unique_ptr, make_unique
#include <thread>      // for std::this_thread::yield
#include <type_traits> // for invoke_result_t
#include <utility>     // for addressof, forward, exchange
#include <vector>      // for vector
#include <version>     // for ptrdiff_t

#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
#endif

#include "libfork/core/impl/atomics.hpp" // for thread_fence_seq_cst
#include "libfork/core/impl/utility.hpp" // for k_cache_line, immovable
#include "libfork/core/macro.hpp"        // for LF_ASSERT

// --- NEW INCLUDE ---
#include "libfork/core/ext/deque_common.hpp" // For dequeable, steal_t, err, return_nullopt

/**
 * @file deque_blocking.hpp
 *
 * @brief An implementation of the "Blocking Work-Stealing Queue using CAS"
 */

namespace lf {

namespace impl {

/**
 * @brief A basic wrapper around a c-style array that provides modulo load/stores.
 *
 * This class is designed for internal use only. It provides a c-style API that is
 * used efficiently by deque for low level atomic operations.
 *
 * @tparam T The type of the elements in the array.
 */
template <dequeable T>
struct atomic_ring_buf {
  /**
   * @brief Construct a new ring buff object
   *
   * @param cap The capacity of the buffer, MUST be a power of 2.
   */
  constexpr explicit atomic_ring_buf(std::ptrdiff_t cap) : m_cap{cap}, m_mask{cap - 1} {
    LF_ASSERT(cap > 0 && std::has_single_bit(static_cast<std::size_t>(cap)));
  }
  /**
   * @brief Get the capacity of the buffer.
   */
  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t { return m_cap; }
  /**
   * @brief Store ``val`` at ``index % this->capacity()``.
   */
  constexpr auto store(std::ptrdiff_t index, T const &val) noexcept -> void {
    LF_ASSERT(index >= 0);
    (m_buf.get() + (index & m_mask))->store(val, std::memory_order_relaxed); // NOLINT Avoid cast.
  }
  /**
   * @brief Load value at ``index % this->capacity()``.
   */
  [[nodiscard]] constexpr auto load(std::ptrdiff_t index) const noexcept -> T {
    LF_ASSERT(index >= 0);
    return (m_buf.get() + (index & m_mask))->load(std::memory_order_relaxed); // NOLINT Avoid cast.
  }
  /**
   * @brief Copies elements in range ``[bottom, top)`` into a new ring buffer.
   *
   * This function allocates a new buffer and returns a pointer to it.
   * The caller is responsible for deallocating the memory.
   *
   * @param bot The bottom of the range to copy from (inclusive).
   * @param top The top of the range to copy from (exclusive).
   */
  [[nodiscard]] constexpr auto resize(std::ptrdiff_t bot, std::ptrdiff_t top) const -> atomic_ring_buf<T> * {

    auto *ptr = new atomic_ring_buf{2 * m_cap}; // NOLINT

    for (std::ptrdiff_t i = top; i != bot; ++i) {
      ptr->store(i, load(i));
    }

    return ptr;
  }

 private:
  /**
   * @brief An array of atomic elements.
   */
  using array_t = std::atomic<T>[]; // NOLINT
  /**
   * @brief Capacity of the buffer.
   */
  std::ptrdiff_t m_cap;
  /**
   * @brief Bit mask to perform modulo capacity operations.
   */
  std::ptrdiff_t m_mask;

#ifdef __cpp_lib_smart_ptr_for_overwrite
  std::unique_ptr<array_t> m_buf = std::make_unique_for_overwrite<array_t>(static_cast<std::size_t>(m_cap));
#else
  std::unique_ptr<array_t> m_buf = std::make_unique<array_t>(static_cast<std::size_t>(m_cap));
#endif
};

} // namespace impl

inline namespace ext {

/**
 * @brief An unbounded lock-free single-producer multiple-consumer work-stealing deque.
 *
 * \rst
 *
 * Implements the "Chase-Lev" deque described in the papers, `"Dynamic Circular Work-Stealing deque"
 * <https://doi.org/10.1145/1073970.1073974>`_ and `"Correct and Efficient Work-Stealing for Weak
 * Memory Models" <https://doi.org/10.1145/2442516.2442524>`_.
 *
 * Only the deque owner can perform ``pop()`` and ``push()`` operations where the deque behaves
 * like a LIFO stack. Others can (only) ``steal()`` data from the deque, they see a FIFO deque.
 * All threads must have finished using the deque before it is destructed.
 *
 *
 * Example:
 *
 * .. include:: ../../../test/source/core/deque.cpp
 * :code:
 * :start-after: // !BEGIN-EXAMPLE
 * :end-before: // !END-EXAMPLE
 *
 * \endrst
 *
 * @tparam T The type of the elements in the deque.
 */
template <dequeable T>
class blocking_deque : impl::immovable<blocking_deque<T>> {

  static constexpr std::ptrdiff_t k_default_capacity = 1024;
  static constexpr std::size_t k_garbage_reserve = 64;

 public:
  /**
   * @brief The type of the elements in the deque.
   */
  using value_type = T;
  /**
   * @brief Construct a new empty deque object.
   */
  constexpr blocking_deque() : blocking_deque(k_default_capacity) {}
  /**
   * @brief Construct a new empty deque object.
   *
   * @param cap The capacity of the deque (must be a power of 2).
   */
  constexpr explicit blocking_deque(std::ptrdiff_t cap);
  /**
   * @brief Get the number of elements in the deque.
   */
  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t;
  /**
   * @brief Get the number of elements in the deque as a signed integer.
   */
  [[nodiscard]] constexpr auto ssize() const noexcept -> ptrdiff_t;
  /**
   * @brief Get the capacity of the deque.
   */
  [[nodiscard]] constexpr auto capacity() const noexcept -> ptrdiff_t;
  /**
   * @brief Check if the deque is empty.
   */
  [[nodiscard]] constexpr auto empty() const noexcept -> bool;
  /**
   * @brief Push an item into the deque.
   *
   * Only the owner thread can insert an item into the deque.
   * This operation can trigger the deque to resize if more space is required.
   * This may throw if an allocation is required and then fails.
   *
   * @param val Value to add to the deque.
   */
  constexpr void push(T const &val);
  /**
   * @brief Pop an item from the deque.
   *
   * Only the owner thread can pop out an item from the deque. If the buffer is empty calls `when_empty` and
   * returns the result. By default, `when_empty` is a no-op that returns a null `std::optional<T>`.
   */
  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F &&when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F>;

  /**
   * @brief Steal an item from the deque.
   *
   * Any threads can try to steal an item from the deque. This operation can fail if the deque is
   * empty or if another thread simultaneously stole an item from the deque.
   */
  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T>;

  /**
   * @brief Destroy the deque object.
   *
   * All threads must have finished using the deque before it is destructed.
   */
  constexpr ~blocking_deque() noexcept;

 private:
  // Blocking spinlock acquire (used by Owner)
  void lock() noexcept {
    int expected = 0;
    if (m_flag.compare_exchange_strong(expected, 1, acquire)) return;

    while (true) {
      expected = 0;
      if (m_flag.load(relaxed) == 0) {
        if (m_flag.compare_exchange_weak(expected, 1, acquire)) return;
      }

      #if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
      #else
      std::this_thread::yield();
      #endif
    }
  }

  // Non-blocking try-lock (used by Thief)
  bool try_lock() noexcept {
    int expected = 0;
    return m_flag.compare_exchange_strong(expected, 1, acquire);
  }

  // Release lock
  void unlock() noexcept {
    m_flag.store(0, release);
  }

  // Internal helper to check empty without strict memory ordering
  // Useful for the "optimization" checks mentioned in the paper
  bool empty_relaxed() const noexcept {
    return m_top.load(relaxed) >= m_bottom.load(relaxed);
  }

  // CAS Lock Flag: 0 = Free, 1 = Locked
  alignas(impl::k_cache_line) std::atomic<int> m_flag{0};

  alignas(impl::k_cache_line) std::atomic<std::ptrdiff_t> m_top;
  alignas(impl::k_cache_line) std::atomic<std::ptrdiff_t> m_bottom;

  alignas(impl::k_cache_line) std::atomic<impl::atomic_ring_buf<T> *> m_buf;
  std::vector<std::unique_ptr<impl::atomic_ring_buf<T>>> m_garbage;

  // Convenience aliases.
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;
  static constexpr std::memory_order consume = std::memory_order_consume;
  static constexpr std::memory_order acquire = std::memory_order_acquire;
  static constexpr std::memory_order release = std::memory_order_release;
  static constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
};

template <dequeable T>
constexpr blocking_deque<T>::blocking_deque(std::ptrdiff_t cap)
    : m_top(0),
      m_bottom(0),
      m_buf(new impl::atomic_ring_buf<T>{cap}) {
  m_garbage.reserve(k_garbage_reserve);
}

template <dequeable T>
constexpr auto blocking_deque<T>::size() const noexcept -> std::size_t {
  return static_cast<std::size_t>(ssize());
}

template <dequeable T>
constexpr auto blocking_deque<T>::ssize() const noexcept -> std::ptrdiff_t {
  ptrdiff_t const bottom = m_bottom.load(relaxed);
  ptrdiff_t const top = m_top.load(relaxed);
  return std::max(bottom - top, ptrdiff_t{0});
}

template <dequeable T>
constexpr auto blocking_deque<T>::capacity() const noexcept -> ptrdiff_t {
  return m_buf.load(relaxed)->capacity();
}

template <dequeable T>
constexpr auto blocking_deque<T>::empty() const noexcept -> bool {
  ptrdiff_t const bottom = m_bottom.load(relaxed);
  ptrdiff_t const top = m_top.load(relaxed);
  return top >= bottom;
}

template <dequeable T>
constexpr auto blocking_deque<T>::push(T const &val) -> void {

  lock(); // Acquire lock for push operation

  std::ptrdiff_t const bottom = m_bottom.load(relaxed);
  std::ptrdiff_t const top = m_top.load(relaxed);
  impl::atomic_ring_buf<T> *buf = m_buf.load(relaxed);

  if (buf->capacity() < (bottom - top) + 1) {
    impl::atomic_ring_buf<T> *bigger = buf->resize(bottom, top);

    m_garbage.emplace_back(buf);
    buf = bigger;

    m_buf.store(buf, relaxed);
  }

  buf->store(bottom, val);
  m_bottom.store(bottom + 1, relaxed);

  unlock(); // Release lock after push operation
}

template <dequeable T>
template <std::invocable F>
  requires std::convertible_to<T, std::invoke_result_t<F>>
constexpr auto
blocking_deque<T>::pop(F &&when_empty) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {

  if (empty_relaxed()) {
    return std::invoke(std::forward<F>(when_empty));
  }

  lock();

  std::ptrdiff_t const bottom = m_bottom.load(relaxed) - 1;
  std::ptrdiff_t const top = m_top.load(relaxed);
  impl::atomic_ring_buf<T> *buf = m_buf.load(relaxed);

  if (top <= bottom) {
    m_bottom.store(bottom, relaxed);
    auto val = buf->load(bottom);

    unlock(); // release lock after pop
    return val;
  }

  unlock(); // Release lock
  return std::invoke(std::forward<F>(when_empty));
}

template <dequeable T>
constexpr auto blocking_deque<T>::steal() noexcept -> steal_t<T> {

  if (empty_relaxed()) {
    return {.code = err::empty, .val = {}};
  }

  if (try_lock()) {
    // Lock acquired.
    std::ptrdiff_t const top = m_top.load(relaxed);
    std::ptrdiff_t const bottom = m_bottom.load(relaxed);
    impl::atomic_ring_buf<T> *buf = m_buf.load(relaxed);

    if (top < bottom) {
      T val = buf->load(top);
      m_top.store(top + 1, relaxed);

      unlock(); // Release lock
      return {.code = err::none, .val = val};
    }
    unlock();
    return {.code = err::empty, .val = {}};
  }

  return {.code = err::lost, .val = {}};
}

template <dequeable T>
constexpr blocking_deque<T>::~blocking_deque() noexcept {
  delete m_buf.load(); // NOLINT
}

} // namespace ext

} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_BLOCKING_HPP */