#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "libfork/core/impl/atomics.hpp" // for thread_fence_seq_cst
#include "libfork/core/impl/utility.hpp" // for k_cache_line, immovable
#include "libfork/core/macro.hpp"        // for LF_ASSERT

namespace lf {
inline namespace ext {

// Lace-style deque with (tail, split) public and bottom private.
// This implementation also publishes `bottom` atomically so empty()/size() are safe
// to call from any thread (important for libfork schedulers like lazy_pool).
template <dequeable T>
class lace_deque : private impl::immovable<lace_deque<T>> {
public:
  using value_type = T;

  lace_deque()
      : lace_deque(default_capacity()) {}

  explicit lace_deque(std::ptrdiff_t cap)
      : m_capacity(cap),
        m_mask(cap - 1),
        m_array(new std::atomic<T>[static_cast<std::size_t>(cap)]),
        m_tail_split(pack_state({0u, 0u})),
        m_bottom_pub(0u),
        m_allstolen(true),
        m_splitreq(false) {
    // cap must be power of 2
    // (libfork uses this assumption heavily across deques)
    if (cap <= 0 || (cap & (cap - 1)) != 0) {
      std::abort();
    }

    // owner-private initial state
    m_owner.bottom = 0;
    m_owner.osplit = 0;
    m_owner.o_allstolen = true;
  }

  ~lace_deque() noexcept { delete[] m_array; }

  auto capacity() const noexcept -> std::ptrdiff_t { return m_capacity; }

  // Safe for any thread: conservative but correct.
  auto empty() const noexcept -> bool {
    // Fast: if marked all-stolen, it's empty.
    if (m_allstolen.load(acquire)) {
      return true;
    }

    // Otherwise compute bottom - tail using published atomics.
    const auto ts = unpack_state(m_tail_split.load(acquire));
    const auto bot = m_bottom_pub.load(acquire);

    // tail and bottom are monotonic uint32 indices.
    // If tail >= bottom, there is no work in this deque.
    return ts.tail >= bot;
  }

  auto size() const noexcept -> std::size_t {
    const auto ts = unpack_state(m_tail_split.load(acquire));
    const auto bot = m_bottom_pub.load(acquire);
    if (bot <= ts.tail) {
      return 0;
    }
    return static_cast<std::size_t>(bot - ts.tail);
  }

  auto ssize() const noexcept -> std::ptrdiff_t {
    const auto ts = unpack_state(m_tail_split.load(acquire));
    const auto bot = m_bottom_pub.load(acquire);
    if (bot <= ts.tail) {
      return 0;
    }
    return static_cast<std::ptrdiff_t>(bot - ts.tail);
  }

  template <typename F = return_nullopt<T>>
  auto pop(F&& when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>)
      -> std::invoke_result_t<F> {
    // Private fast-path
    if (m_owner.bottom > m_owner.osplit) {
      --m_owner.bottom;
      publish_bottom_relaxed();

      T val = m_array[idx(m_owner.bottom)].load(relaxed);

      // If a thief asked for work, share some.
      if (m_splitreq.load(relaxed)) {
        grow_shared();
      }
      return val;
    }

    // No private work
    if (m_owner.o_allstolen) {
      return pop_empty(std::forward<F>(when_empty));
    }

    // Try to reclaim from shared region by shrinking split.
    if (shrink_shared()) {
      return pop_empty(std::forward<F>(when_empty));
    }

    // After shrink_shared we should have private work again.
    --m_owner.bottom;
    publish_bottom_relaxed();

    T val = m_array[idx(m_owner.bottom)].load(relaxed);

    if (m_splitreq.load(relaxed)) {
      grow_shared();
    }
    return val;
  }

  // Owner-only
  void push(T const& val) {
    // If we were empty/allstolen, re-init to a "private-only" deque:
    // tail == split == old bottom (no shared), and new element is private.
    if (m_owner.o_allstolen) {
      const auto bot = static_cast<std::uint32_t>(m_owner.bottom);

      m_array[idx(m_owner.bottom)].store(val, relaxed);
      ++m_owner.bottom;
      publish_bottom_relaxed();

      // Publish (tail, split) = (bot, bot).
      // Use release so any thread seeing this state also sees prior stores.
      m_tail_split.store(pack_state({bot, bot}), release);

      m_owner.osplit = static_cast<std::ptrdiff_t>(bot);
      m_owner.o_allstolen = false;

      // Clear flags (release so readers don't see false before init writes).
      m_splitreq.store(false, relaxed);
      m_allstolen.store(false, release);
      return;
    }

    // Normal push into private region.
    // (Capacity check is best-effort; indices are monotonic uint32.)
    {
      const auto ts = unpack_state(m_tail_split.load(acquire));
      const auto bot = static_cast<std::uint32_t>(m_owner.bottom);
      if ((bot - ts.tail) >= static_cast<std::uint32_t>(m_capacity)) {
        // Fixed-capacity deque: fail hard rather than silently corrupt.
        std::abort();
      }
    }

    m_array[idx(m_owner.bottom)].store(val, relaxed);
    ++m_owner.bottom;
    publish_bottom_relaxed();

    if (m_splitreq.load(relaxed)) {
      grow_shared();
    }
  }

  // Any thread
  auto steal() noexcept -> steal_t<T> {
    // If known empty, bail quickly.
    if (m_allstolen.load(acquire)) {
      return {err::empty, T{}};
    }

    const auto old = m_tail_split.load(acquire);
    const auto s = unpack_state(old);

    // Nothing shared -> request split to move (owner will share).
    if (s.tail >= s.split) {
      m_splitreq.store(true, release);
      return {err::empty, T{}};
    }

    // Try to claim the current tail.
    const state desired_s{static_cast<std::uint32_t>(s.tail + 1u), s.split};
    const auto desired = pack_state(desired_s);

    std::uint64_t expected = old;
    if (!m_tail_split.compare_exchange_strong(
            expected, desired,
            std::memory_order_acq_rel,
            acquire)) {
      return {err::lost, T{}};
    }

    // Successfully stole `s.tail`.
    // Acquire keeps it simple and correct w.r.t. owner's release publishing.
    T val = m_array[static_cast<std::ptrdiff_t>(s.tail) & m_mask]
                .load(acquire);
    return {err::none, val};
  }

private:
  struct state {
    std::uint32_t tail;
    std::uint32_t split;
  };

  static constexpr auto default_capacity() noexcept -> std::ptrdiff_t {
    // Keep your original large default if you want, but 1<<24 is huge.
    return (1u << 20); // 1,048,576
  }

  static constexpr auto pack_state(state s) noexcept -> std::uint64_t {
    return (std::uint64_t{s.split} << 32) | std::uint64_t{s.tail};
  }

  static constexpr auto unpack_state(std::uint64_t v) noexcept -> state {
    return state{
        static_cast<std::uint32_t>(v & 0xFFFF'FFFFu),
        static_cast<std::uint32_t>(v >> 32),
    };
  }

  auto idx(std::ptrdiff_t i) const noexcept -> std::ptrdiff_t { return i & m_mask; }

  void publish_bottom_relaxed() noexcept {
    m_bottom_pub.store(static_cast<std::uint32_t>(m_owner.bottom),
                       relaxed);
  }

  template <typename F>
  auto pop_empty(F&& when_empty) noexcept(std::is_nothrow_invocable_v<F>)
      -> std::invoke_result_t<F> {
    // Mark empty for everyone.
    m_owner.o_allstolen = true;
    m_allstolen.store(true, release);

    // No pending split requests matter when empty.
    m_splitreq.store(false, relaxed);

    // Make bottom visible too.
    publish_bottom_relaxed();

    return std::invoke(std::forward<F>(when_empty));
  }

  // Owner-only: share some private work by moving split towards bottom.
  void grow_shared() noexcept {
    m_splitreq.store(false, relaxed);

    // Snapshot public (tail, split) and private bottom.
    const auto old = m_tail_split.load(acquire);
    const auto s = unpack_state(old);

    const auto bot = static_cast<std::uint32_t>(m_owner.bottom);
    const auto split = static_cast<std::uint32_t>(m_owner.osplit);

    // If there is nothing private, nothing to share.
    if (bot <= split) {
      return;
    }

    // Diff = (bottom - split + 1) / 2
    const std::uint32_t diff = (bot - split + 1u) / 2u;
    const std::uint32_t new_split = split + diff;

    // Update published split (preserving current tail).
    store_split_blind(new_split, release);

    m_owner.osplit = static_cast<std::ptrdiff_t>(new_split);

    // Definitely not empty.
    m_owner.o_allstolen = false;
    m_allstolen.store(false, release);
  }

  // Owner-only: reclaim some shared work by shrinking split towards tail.
  // Returns true iff empty.
  auto shrink_shared() noexcept -> bool {
    const auto s0 = unpack_state(m_tail_split.load(relaxed));
    const std::uint32_t tail0 = s0.tail;
    const std::uint32_t split0 = s0.split;

    // No shared region at all: empty from owner's POV (no private either here).
    if (tail0 == split0) {
      m_owner.o_allstolen = true;
      m_allstolen.store(true, release);
      return true;
    }

    // newsplit = (tail + split) / 2
    std::uint32_t newsplit = tail0 + ((split0 - tail0) / 2u);

    // Blindly store split to newsplit (preserving whatever tail is now).
    store_split_blind(newsplit, relaxed);

    // Lace uses a strong fence here to serialize with stealers.
    impl::thread_fence_seq_cst();

    // Re-read tail after fence.
    const auto s1 = unpack_state(m_tail_split.load(relaxed));
    const std::uint32_t tail1 = s1.tail;

    // If tail reached the old split, everything got stolen while we shrank.
    if (tail1 == split0) {
      m_owner.o_allstolen = true;
      m_allstolen.store(true, release);
      return true;
    }

    // If tail raced ahead past our computed newsplit, recompute.
    if (tail1 > newsplit) {
      newsplit = tail1 + ((split0 - tail1) / 2u);
      store_split_blind(newsplit, relaxed);
    }

    m_owner.osplit = static_cast<std::ptrdiff_t>(newsplit);
    m_owner.o_allstolen = false;
    m_allstolen.store(false, release);
    return false;
  }

  // Store split = fixed_split while preserving current tail (CAS loop).
  void store_split_blind(std::uint32_t fixed_split, std::memory_order mo) noexcept {
    std::uint64_t cur = m_tail_split.load(relaxed);
    for (;;) {
      const auto s = unpack_state(cur);
      const std::uint64_t desired = pack_state({s.tail, fixed_split});
      if (m_tail_split.compare_exchange_weak(cur, desired, mo, relaxed)) {
        return;
      }
    }
  }

private:
  // Owner-private fields
  struct owner_state {
    std::ptrdiff_t bottom{};
    std::ptrdiff_t osplit{};
    bool o_allstolen{true};
  } m_owner;

  // Storage
  std::ptrdiff_t m_capacity{};
  std::ptrdiff_t m_mask{};
  std::atomic<T>* m_array{nullptr};

  // Public/shared
  alignas(64) std::atomic<std::uint64_t> m_tail_split;   // packed {tail, split}
  alignas(64) std::atomic<std::uint32_t> m_bottom_pub;   // published bottom
  alignas(64) std::atomic<bool> m_allstolen;             // true iff known empty
  alignas(64) std::atomic<bool> m_splitreq;              // thieves request sharing

  static constexpr std::memory_order relaxed = std::memory_order_relaxed;
  static constexpr std::memory_order acquire = std::memory_order_acquire;
  static constexpr std::memory_order release = std::memory_order_release;
  static constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
};

} // namespace ext
} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */
