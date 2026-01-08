#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <atomic>
#include <bit> // std::bit_cast
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::abort
#include <functional> // std::invoke
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

// Virtual memory functions
namespace impl {
inline auto allocate_virtual(std::size_t bytes) -> void* {
#if defined(_WIN32) || defined(_WIN64)
  // NOTE: This commits the full range. If you want Linux-like "reserve + fault pages",
  // you can MEM_RESERVE and commit per-page in a touch loop.
  void* ptr = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  return ptr;
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
  // Default: 16 Million items. Safe within uint32_t limits.
  static constexpr std::size_t k_default_cap = 1 << 24;

 public:
  using value_type = T;

  constexpr lace_deque();
  explicit lace_deque(const std::size_t cap);
  ~lace_deque() noexcept;

  // --- Basic Accessors ---
  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t;
  [[nodiscard]] constexpr auto ssize() const noexcept -> std::ptrdiff_t;
  [[nodiscard]] constexpr auto capacity() const noexcept -> std::ptrdiff_t;
  [[nodiscard]] constexpr auto empty() const noexcept -> bool;

  // --- Operations ---
  constexpr void push(T const& val) noexcept;

  template <std::invocable F = return_nullopt<T>>
    requires std::convertible_to<T, std::invoke_result_t<F>>
  constexpr auto pop(F&& when_empty = {}) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F>;

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T>;

 private:
  // Packed state: Tail (public bottom) and Split (boundary)
  struct alignas(8) split_state {
    uint32_t tail;
    uint32_t split;
  };
  static_assert(sizeof(split_state) == 8);

  // Safe type-punning via C++20 bit_cast
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

  // Update ONLY split while preserving current tail (CAS-loop to avoid clobbering tail increments).
  LF_FORCEINLINE void set_split_preserve_tail(uint32_t new_split, std::memory_order mo) noexcept {
    uint64_t cur = m_tail_split.load(relaxed);
    for (;;) {
      split_state st = unpack(cur);
      uint64_t desired = pack(st.tail, new_split);
      if (m_tail_split.compare_exchange_weak(cur, desired, mo, relaxed)) {
        return;
      }
    }
  }

  template <typename F>
  auto pop_cold_path(F&& when_empty) noexcept -> std::invoke_result_t<F>;

  template <typename F>
  constexpr auto pop_empty(F&& when_empty) -> std::invoke_result_t<F>;

  // Returns true if it determines the deque is empty / fully stolen.
  constexpr auto shrink_shared() noexcept -> bool;
  constexpr auto grow_shared() noexcept -> void;

  [[nodiscard]] LF_FORCEINLINE auto mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t;

  std::atomic<T>* m_array{nullptr};
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

  // Packed (tail, split) must be updated atomically as a single 64-bit word.
  alignas(k_cache_line) std::atomic<uint64_t> m_tail_split;

  alignas(k_cache_line) std::atomic<bool> m_allstolen;
  alignas(k_cache_line) std::atomic<bool> m_splitreq;

  struct alignas(k_cache_line) {
    std::ptrdiff_t bottom{0};
    std::ptrdiff_t osplit{0};
    bool o_allstolen{true};
  } m_worker;

  // Convenience aliases
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;
  static constexpr std::memory_order acquire = std::memory_order_acquire;
  static constexpr std::memory_order release = std::memory_order_release;
  static constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
};

template <dequeable T>
constexpr lace_deque<T>::lace_deque() : lace_deque(k_default_cap) {}

template <dequeable T>
lace_deque<T>::lace_deque(const std::size_t cap)
    : m_mask(static_cast<std::ptrdiff_t>(cap) - 1),
      m_capacity(static_cast<std::ptrdiff_t>(cap)),
      m_tail_split(pack(0u, 0u)),
      m_allstolen(true),
      m_splitreq(false) {
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "lace_deque requires lock-free 64-bit atomics for packed (tail,split).");

  if (cap > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::length_error("Capacity too large");
  }
  if ((cap & (cap - 1)) != 0) {
    std::abort();
  }

  const std::size_t bytes = sizeof(std::atomic<T>) * cap;
  void* raw = impl::allocate_virtual(bytes);
  if (!raw) throw std::bad_alloc();

  m_array = static_cast<std::atomic<T>*>(raw);

  // Pre-fault pages without mutating object bytes (read-only touch).
  volatile unsigned char sink = 0;
  volatile unsigned char* touch_ptr = static_cast<volatile unsigned char*>(raw);
  for (std::size_t i = 0; i < bytes; i += 4096) {
    sink ^= touch_ptr[i];
  }
  (void)sink;

  m_worker.bottom = 0;
  m_worker.osplit = 0;
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
  std::ptrdiff_t const bottom = m_worker.bottom;
  split_state st = load_state(relaxed);
  return std::max(bottom - static_cast<std::ptrdiff_t>(st.tail), std::ptrdiff_t{0});
}

template <dequeable T>
constexpr auto lace_deque<T>::capacity() const noexcept -> std::ptrdiff_t {
  return m_capacity;
}

template <dequeable T>
constexpr auto lace_deque<T>::empty() const noexcept -> bool {
  if (m_worker.o_allstolen) return true;
  split_state st = load_state(seq_cst);
  return static_cast<std::ptrdiff_t>(st.tail) >= m_worker.bottom;
}

template <dequeable T>
constexpr void lace_deque<T>::push(T const& val) noexcept {
  if (m_worker.o_allstolen) {
    const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);

    (m_array + mask_index(bot))->store(val, relaxed);

    // Publish (tail, split) after writing the task.
    store_state(bot, bot + 1, release);

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

template <dequeable T>
template <std::invocable F>
  requires std::convertible_to<T, std::invoke_result_t<F>>
constexpr auto lace_deque<T>::pop(F&& when_empty) noexcept(std::is_nothrow_invocable_v<F>) -> std::invoke_result_t<F> {
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

template <dequeable T>
[[nodiscard]] constexpr auto lace_deque<T>::steal() noexcept -> steal_t<T> {
  if (m_allstolen.load(acquire)) {
    return {.code = err::empty, .val = {}};
  }

  uint64_t old_v = m_tail_split.load(acquire);
  split_state st = unpack(old_v);

  if ((static_cast<int32_t>(st.split) - static_cast<int32_t>(st.tail)) > 0) {
    // After observing split>tail with acquire, task publication is visible.
    T tmp = (m_array + mask_index(st.tail))->load(relaxed);

    uint64_t new_v = pack(st.tail + 1, st.split);
    if (m_tail_split.compare_exchange_strong(old_v, new_v, std::memory_order_acq_rel, relaxed)) {
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
  if (m_worker.o_allstolen || m_worker.bottom <= 0) {
    return pop_empty(std::forward<F>(when_empty));
  }

  if (shrink_shared()) {
    return pop_empty(std::forward<F>(when_empty));
  }

  --m_worker.bottom;

  // Verify a thief hasn't stolen past our new bottom.
  split_state st = load_state(relaxed);
  uint32_t const bot_u = static_cast<uint32_t>(m_worker.bottom);
  if (static_cast<int32_t>(st.tail - bot_u) > 0) {
    return pop_empty(std::forward<F>(when_empty));
  }

  T val = (m_array + mask_index(m_worker.bottom))->load(relaxed);

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
  split_state st0 = load_state(relaxed);

  // If shared portion is empty and we are here (no private either), it's fully stolen.
  if (st0.tail == st0.split) {
    m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  uint32_t const old_split = st0.split;

  // Move split towards tail (halve shared region).
  uint32_t new_split = st0.tail + ((st0.split - st0.tail) >> 1U);

  set_split_preserve_tail(new_split, relaxed);
  m_worker.osplit = static_cast<std::ptrdiff_t>(new_split);

  // Lace requires a full fence here to detect the "tail jumped beyond new split" scenario.
  std::atomic_thread_fence(seq_cst);

  uint32_t fresh_tail = load_state(relaxed).tail;

  if (fresh_tail > new_split) {
    // Thieves may have stolen beyond our new split; adjust to the final position.
    // Use the original (old_split) snapshot as the upper bound.
    new_split = fresh_tail + ((old_split - fresh_tail) >> 1U);
    set_split_preserve_tail(new_split, relaxed);
    m_worker.osplit = static_cast<std::ptrdiff_t>(new_split);
  }

  // If we still have no private work, then we're empty.
  if (m_worker.bottom > m_worker.osplit) {
    return false;
  }

  m_allstolen.store(true, release);
  m_worker.o_allstolen = true;
  return true;
}

template <dequeable T>
constexpr auto lace_deque<T>::grow_shared() noexcept -> void {
  // Move split towards bottom (halve private region).
  std::ptrdiff_t const new_s_val = (m_worker.osplit + m_worker.bottom + 1) >> 1U;
  uint32_t const new_split = static_cast<uint32_t>(new_s_val);

  // Publishing more shared work must be a release to make those tasks visible to thieves.
  set_split_preserve_tail(new_split, release);

  m_worker.osplit = new_s_val;
  m_splitreq.store(false, relaxed);
}

template <dequeable T>
[[nodiscard]] LF_FORCEINLINE auto lace_deque<T>::mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t {
  return static_cast<std::size_t>(idx) & static_cast<std::size_t>(m_mask);
}

} // namespace ext
} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */
