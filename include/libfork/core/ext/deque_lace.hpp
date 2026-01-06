#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <bit> // for std::bit_cast
#include <cstddef>
#include <cstdint>
#include <cstdlib> // for std::abort
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

  // Safe type-punning via C++20 bit_cast
  static constexpr auto pack(uint32_t t, uint32_t s) noexcept -> uint64_t {
    return std::bit_cast<uint64_t>(split_state{t, s});
  }

  static constexpr auto unpack(uint64_t v) noexcept -> split_state {
    return std::bit_cast<split_state>(v);
  }

  template <typename F>
  auto pop_cold_path(F&& when_empty) noexcept -> std::invoke_result_t<F>;

  template <typename F>
  constexpr auto pop_empty(F&& when_empty) -> std::invoke_result_t<F>;

  constexpr auto shrink_shared() noexcept -> bool;
  constexpr auto grow_shared() noexcept -> void;

  [[nodiscard]] LF_FORCEINLINE auto mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t;

  std::atomic<T>* m_array;
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

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
      m_tail_split(0),
      m_allstolen(true),
      m_splitreq(false) {
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

  volatile char* touch_ptr = static_cast<char*>(raw);
  for (std::size_t i = 0; i < bytes; i += 4096) {
    touch_ptr[i] = 0;
  }

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
  split_state s = unpack(m_tail_split.load(relaxed));
  return std::max(bottom - static_cast<std::ptrdiff_t>(s.tail), std::ptrdiff_t{0});
}

template <dequeable T>
constexpr auto lace_deque<T>::capacity() const noexcept -> std::ptrdiff_t { return m_capacity; }

template <dequeable T>
constexpr auto lace_deque<T>::empty() const noexcept -> bool {
  if (m_worker.o_allstolen) return true;
  split_state s = unpack(m_tail_split.load(seq_cst));
  return static_cast<std::ptrdiff_t>(s.tail) >= m_worker.bottom;
}

template <dequeable T>
constexpr void lace_deque<T>::push(T const& val) noexcept {
  if (m_worker.o_allstolen) {
    const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);
    (m_array + mask_index(bot))->store(val, relaxed);

    m_tail_split.store(pack(bot, bot + 1), relaxed);

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
  split_state s = unpack(old_v);

  if (static_cast<int32_t>(s.split - s.tail) > 0) {

    T tmp = (m_array + mask_index(s.tail))->load(acquire);

    uint64_t new_v = pack(s.tail + 1, s.split);

    if (m_tail_split.compare_exchange_strong(old_v, new_v, release, relaxed)) {
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

  // Verify consistency // TODO: needed?
  split_state s = unpack(m_tail_split.load(relaxed));
  if (static_cast<int32_t>(s.tail - static_cast<uint32_t>(m_worker.bottom)) > 0) {
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
  uint64_t old_v = m_tail_split.load(relaxed);
  split_state s = unpack(old_v);

  if (s.tail == s.split) {
    m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  while (true) {
      s = unpack(old_v);
      if (s.tail == s.split) {
          m_allstolen.store(true, release);
          m_worker.o_allstolen = true;
          return true;
      }

      uint32_t new_split_val = s.tail + ((s.split - s.tail) >> 1U);
      uint64_t new_v = pack(s.tail, new_split_val);

      if (m_tail_split.compare_exchange_weak(old_v, new_v, release, relaxed)) {
          s.split = new_split_val;
          break;
      }
  }

  int32_t const diff = static_cast<int32_t>(s.split - static_cast<uint32_t>(m_worker.bottom));
  m_worker.osplit = m_worker.bottom + static_cast<std::ptrdiff_t>(diff);

  impl::thread_fence_seq_cst();

  split_state fresh_s = unpack(m_tail_split.load(relaxed));

  if (static_cast<int32_t>(fresh_s.tail - s.split) > 0) {
    if (static_cast<int32_t>(fresh_s.tail - static_cast<uint32_t>(m_worker.bottom)) >= 0) {
      m_allstolen.store(true, release);
      m_worker.o_allstolen = true;
      return true;
    }

    int32_t const top_diff = static_cast<int32_t>(fresh_s.tail - static_cast<uint32_t>(m_worker.bottom));
    m_worker.osplit = m_worker.bottom + static_cast<std::ptrdiff_t>(top_diff);
  }

  if (m_worker.bottom > m_worker.osplit) {
    return false;
  }

  m_allstolen.store(true, release);
  m_worker.o_allstolen = true;
  return true;
}

template <dequeable T>
constexpr auto lace_deque<T>::grow_shared() noexcept -> void {
  uint64_t old_v = m_tail_split.load(relaxed);

  while (true) {
      split_state s = unpack(old_v);

      std::ptrdiff_t const new_s_val = (m_worker.osplit + m_worker.bottom + 1) >> 1U;
      uint32_t new_split_idx = static_cast<uint32_t>(new_s_val);

      uint64_t new_v = pack(s.tail, new_split_idx);

      if (m_tail_split.compare_exchange_weak(old_v, new_v, release, relaxed)) {
          m_worker.osplit = new_s_val;
          break;
      }
  }

  m_splitreq.store(false, relaxed);
}

template <dequeable T>
[[nodiscard]] LF_FORCEINLINE auto lace_deque<T>::mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t {
  return static_cast<std::size_t>(idx) & static_cast<std::size_t>(m_mask);
}

} // namespace ext
} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */