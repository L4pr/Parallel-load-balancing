#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "libfork/core/ext/deque_common.hpp"
#include "libfork/core/impl/atomics.hpp"
#include "libfork/core/impl/utility.hpp"
#include "libfork/core/macro.hpp"

// Platform headers for mmap/VirtualAlloc
#if defined(_WIN32) || defined(_WIN64)
  #define NOMINMAX
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace lf {

namespace impl {

// Allocate a virtually-contiguous region.
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
  (void)bytes;
  VirtualFree(ptr, 0, MEM_RELEASE);
#else
  munmap(ptr, bytes);
#endif
}

} // namespace impl

inline namespace ext {

template <dequeable T>
class lace_deque : impl::immovable<lace_deque<T>> {
  static constexpr std::size_t k_cache_line = 128;
  static constexpr std::size_t k_default_cap = 1u << 24; // 16M, power-of-two

 public:
  using value_type = T;

  constexpr lace_deque();
  explicit lace_deque(std::size_t cap);
  ~lace_deque() noexcept;

  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t;
  [[nodiscard]] constexpr auto ssize() const noexcept -> std::ptrdiff_t;
  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t;
  [[nodiscard]] constexpr auto empty() const noexcept -> bool;

  constexpr void push(T const& val) noexcept;

  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F&& when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F>;

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T>;

 private:
  struct alignas(8) split_state {
    uint32_t tail;
    uint32_t split;
  };
  static_assert(sizeof(split_state) == 8);

  static constexpr auto pack(uint32_t t, uint32_t s) noexcept -> uint64_t {
    return std::bit_cast<uint64_t>(split_state{t, s});
  }
  static constexpr auto unpack(uint64_t v) noexcept -> split_state {
    return std::bit_cast<split_state>(v);
  }

  [[nodiscard]] LF_FORCEINLINE auto load_state(std::memory_order mo) const noexcept -> split_state {
    return unpack(m_tail_split.load(mo));
  }

  LF_FORCEINLINE void store_state(uint32_t t, uint32_t s, std::memory_order mo) noexcept {
    m_tail_split.store(pack(t, s), mo);
  }

  // Update ONLY split while preserving current tail (CAS loop). Avoids overlapping atomics UB.
  LF_FORCEINLINE void set_split_preserve_tail(uint32_t new_split, std::memory_order succ_mo) noexcept {
    uint64_t cur = m_tail_split.load(relaxed);
    for (;;) {
      split_state st = unpack(cur);
      uint64_t desired = pack(st.tail, new_split);
      if (m_tail_split.compare_exchange_weak(cur, desired, succ_mo, relaxed)) return;
    }
  }

  template <typename F>
  LF_NOINLINE auto pop_cold_path(F&& when_empty) noexcept -> std::invoke_result_t<F>;

  template <typename F>
  constexpr auto pop_empty(F&& when_empty) -> std::invoke_result_t<F>;

  // Returns true if fully stolen / empty.
  constexpr auto shrink_shared() noexcept -> bool;
  constexpr auto grow_shared() noexcept -> void;

  [[nodiscard]] LF_FORCEINLINE auto mask_index(uint32_t idx) const noexcept -> std::size_t {
    return static_cast<std::size_t>(idx & m_mask);
  }

  // Element storage: atomic<T> (requires T lock-free via dequeable concept)
  std::atomic<T>* m_array{nullptr};
  const uint32_t m_mask;
  const uint32_t m_capacity;

  // Packed (tail, split) must be a single atomic word (no partial updates).
  alignas(k_cache_line) std::atomic<uint64_t> m_tail_split;

  // Shared flags
  alignas(k_cache_line) std::atomic<bool> m_allstolen;
  alignas(k_cache_line) std::atomic<bool> m_splitreq;

  // Owner-only state (uint32_t to match Lace indexing and avoid signed overflow UB)
  struct alignas(k_cache_line) {
    uint32_t bottom{0};
    uint32_t osplit{0};
    bool o_allstolen{true};
  } m_worker;

  // Orders
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;
  static constexpr std::memory_order acquire = std::memory_order_acquire;
  static constexpr std::memory_order release = std::memory_order_release;
  static constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
};

// ---- impl ----

template <dequeable T>
constexpr lace_deque<T>::lace_deque() : lace_deque(k_default_cap) {}

template <dequeable T>
lace_deque<T>::lace_deque(std::size_t cap)
    : m_mask(static_cast<uint32_t>(cap) - 1u),
      m_capacity(static_cast<uint32_t>(cap)),
      m_tail_split(pack(0u, 0u)),
      m_allstolen(true),
      m_splitreq(false) {
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "lace_deque requires lock-free 64-bit atomics for packed (tail,split).");

  if (cap == 0) std::abort();
  if (cap > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::length_error("Capacity too large for Lace (uint32_t indices)");
  }
  if ((cap & (cap - 1)) != 0) {
    std::abort();
  }

  const std::size_t bytes = sizeof(std::atomic<T>) * cap;
  void* raw = impl::allocate_virtual(bytes);
  if (!raw) throw std::bad_alloc();

  m_array = static_cast<std::atomic<T>*>(raw);

  // Optional prefault: write a byte per page (safe for atomic<T> raw storage because we never
  // access these bytes as atomic<T> until we start storing values).
  // If you want to avoid this entirely, remove the loop.
  volatile unsigned char* p = static_cast<volatile unsigned char*>(raw);
  for (std::size_t i = 0; i < bytes; i += 4096) {
    p[i] = 0;
  }

  m_worker.bottom = 0;
  m_worker.osplit = 0;
  m_worker.o_allstolen = true;
}

template <dequeable T>
lace_deque<T>::~lace_deque() noexcept {
  if (m_array) {
    impl::deallocate_virtual(m_array, sizeof(std::atomic<T>) * static_cast<std::size_t>(m_capacity));
  }
}

template <dequeable T>
constexpr auto lace_deque<T>::size() const noexcept -> std::size_t {
  return static_cast<std::size_t>(ssize());
}

template <dequeable T>
constexpr auto lace_deque<T>::ssize() const noexcept -> std::ptrdiff_t {
  uint32_t const bottom = m_worker.bottom;
  uint32_t const tail = load_state(relaxed).tail;
  if (bottom <= tail) return 0;
  return static_cast<std::ptrdiff_t>(bottom - tail);
}

template <dequeable T>
constexpr auto lace_deque<T>::capacity() const noexcept -> std::ptrdiff_t {
  return static_cast<std::ptrdiff_t>(m_capacity);
}

template <dequeable T>
constexpr auto lace_deque<T>::empty() const noexcept -> bool {
  if (m_worker.o_allstolen) return true;
  split_state st = load_state(seq_cst);
  return st.tail >= m_worker.bottom;
}

template <dequeable T>
constexpr void lace_deque<T>::push(T const& val) noexcept {
  if (m_worker.o_allstolen) {
    uint32_t const bot = m_worker.bottom;

    (m_array + mask_index(bot))->store(val, relaxed);

    // Publish state after element write:
    // - thieves do acquire-load of (tail,split)
    // - so this release-store ensures the element is visible.
    store_state(bot, bot + 1u, release);

    m_worker.osplit = bot + 1u;
    m_worker.bottom = bot + 1u;

    m_allstolen.store(false, release);
    m_worker.o_allstolen = false;
    m_splitreq.store(false, relaxed);
    return;
  }

  uint32_t const bot = m_worker.bottom;
  (m_array + mask_index(bot))->store(val, relaxed);
  m_worker.bottom = bot + 1u;

  if (m_splitreq.load(relaxed)) [[unlikely]] {
    grow_shared();
  }
}

template <dequeable T>
template <std::invocable F>
  requires std::convertible_to<T, std::invoke_result_t<F>>
constexpr auto lace_deque<T>::pop(F&& when_empty) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {
  if (m_worker.bottom > m_worker.osplit) [[likely]] {
    uint32_t bot = m_worker.bottom - 1u;
    m_worker.bottom = bot;

    T val = (m_array + mask_index(bot))->load(relaxed);

    if (m_splitreq.load(relaxed)) [[unlikely]] {
      grow_shared();
    }
    return val;
  }
  return pop_cold_path(std::forward<F>(when_empty));
}

template <dequeable T>
[[nodiscard]] constexpr auto lace_deque<T>::steal() noexcept -> steal_t<T> {
  if (m_allstolen.load(acquire)) {
    return {.code = err::empty, .val = {}};
  }

  uint64_t old_v = m_tail_split.load(acquire);
  split_state st = unpack(old_v);

  if (st.tail < st.split) {
    // Seeing split>tail under acquire is enough to see the element store.
    T tmp = (m_array + mask_index(st.tail))->load(relaxed);

    uint64_t new_v = pack(st.tail + 1u, st.split);
    if (m_tail_split.compare_exchange_strong(old_v, new_v, std::memory_order_acq_rel, acquire)) {
      return {.code = err::none, .val = tmp};
    }
    return {.code = err::lost, .val = {}};
  }

  if (!m_splitreq.load(relaxed)) {
    m_splitreq.store(true, release);
  }
  return {.code = err::empty, .val = {}};
}

template <dequeable T>
template <typename F>
LF_NOINLINE auto lace_deque<T>::pop_cold_path(F&& when_empty) noexcept -> std::invoke_result_t<F> {
  if (m_worker.o_allstolen || m_worker.bottom == 0u) {
    return pop_empty(std::forward<F>(when_empty));
  }

  if (shrink_shared()) {
    return pop_empty(std::forward<F>(when_empty));
  }

  uint32_t bot = m_worker.bottom - 1u;
  m_worker.bottom = bot;

  // If a thief already stole past our bottom, treat as empty.
  uint32_t const tail = load_state(relaxed).tail;
  if (tail > bot) {
    return pop_empty(std::forward<F>(when_empty));
  }

  T val = (m_array + mask_index(bot))->load(relaxed);

  if (m_splitreq.load(relaxed)) {
    grow_shared();
  }
  return val;
}

template <dequeable T>
template <typename F>
constexpr auto lace_deque<T>::pop_empty(F&& when_empty) -> std::invoke_result_t<F> {
  m_worker.osplit = m_worker.bottom;
  m_allstolen.store(true, release);
  m_worker.o_allstolen = true;
  return std::invoke(std::forward<F>(when_empty));
}

template <dequeable T>
constexpr auto lace_deque<T>::shrink_shared() noexcept -> bool {
  // Mirror Lace: snapshot tail/split once, shrink split, fence, reread tail, maybe adjust.
  split_state st0 = load_state(relaxed);
  uint32_t tail = st0.tail;
  uint32_t split = st0.split;

  if (tail == split) {
    m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  uint32_t const old_split = split;
  uint32_t new_split = (tail + split) >> 1U;

  // Move split down. (Relaxed like Lace.)
  set_split_preserve_tail(new_split, relaxed);
  m_worker.osplit = new_split;

  // Lace needs a full fence here.
  impl::thread_fence_seq_cst();

  // Re-read tail after fence.
  tail = load_state(relaxed).tail;

  // If tail reached old_split, shared region drained => empty.
  if (tail == old_split) {
    m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  // If tail jumped beyond our new split, adjust once more using old_split snapshot.
  if (tail > new_split) {
    new_split = (tail + old_split) >> 1U;
    set_split_preserve_tail(new_split, relaxed);
    m_worker.osplit = new_split;
  }

  return false;
}

template <dequeable T>
constexpr auto lace_deque<T>::grow_shared() noexcept -> void {
  // Mirror Lace: move split towards bottom to make more work stealable.
  uint32_t const os = m_worker.osplit;
  uint32_t const bot = m_worker.bottom;

  if (os < bot) {
    uint32_t const diff = (bot - os + 1u) >> 1U; // ceil((bot-os)/2)
    uint32_t const new_split = os + diff;

    // Publishing more shared work must be release.
    set_split_preserve_tail(new_split, release);
    m_worker.osplit = new_split;
  }

  m_splitreq.store(false, relaxed);
}

} // namespace ext
} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */
