#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <bit>       // for std::bit_cast
#include <cstddef>
#include <cstdint>
#include <cstdlib>   // for std::abort
#include <functional> // for std::invoke
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
    uint32_t top;
    uint32_t split;
  };

  static constexpr uint64_t pack(uint32_t top, uint32_t split) noexcept {
    return (uint64_t(top) << 32) | uint64_t(split);
  }

  static constexpr split_state unpack(uint64_t v) noexcept {
    return split_state{ uint32_t(v >> 32), uint32_t(v) };
  }

  static constexpr bool is_little_endian =
#if defined(__cpp_lib_endian)
      (std::endian::native == std::endian::little);
#else
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
      (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
#else
      true;
#endif
#endif

  LF_FORCEINLINE auto top_split64() const noexcept -> std::atomic_ref<std::uint64_t> {
    return std::atomic_ref<std::uint64_t>(m_top_split_raw);
  }

  LF_FORCEINLINE auto split32() const noexcept -> std::atomic_ref<std::uint32_t> {
    auto* p32 = reinterpret_cast<std::uint32_t*>(&m_top_split_raw);
    return std::atomic_ref<std::uint32_t>(p32[is_little_endian ? 0 : 1]);
  }

  LF_FORCEINLINE auto array_at(std::ptrdiff_t idx) noexcept -> std::atomic_ref<T> {
    return std::atomic_ref<T>(m_array[mask_index(idx)]);
  }

  template <typename F>
  auto pop_cold_path(F&& when_empty) noexcept -> std::invoke_result_t<F>;

  template <typename F>
  constexpr auto pop_empty(F&& when_empty) -> std::invoke_result_t<F>;

  constexpr auto shrink_shared() noexcept -> bool;
  constexpr auto grow_shared() noexcept -> void;

  LF_FORCEINLINE void publish_bottom(std::memory_order mo) noexcept {
    m_bottom.store(static_cast<uint32_t>(m_worker.bottom), mo);
  }

  LF_FORCEINLINE void store_split_preserve_top(uint32_t new_split, std::memory_order succ_mo) noexcept {
    split32().store(new_split, succ_mo);
  }

  [[nodiscard]] LF_FORCEINLINE auto mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t;

  T* m_array;
  const std::ptrdiff_t m_mask;
  const std::ptrdiff_t m_capacity;

  alignas(k_cache_line) mutable std::uint64_t m_top_split_raw;

  alignas(k_cache_line) std::atomic<uint32_t> m_bottom;

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
    : m_array(nullptr),
      m_mask(static_cast<std::ptrdiff_t>(cap) - 1),
      m_capacity(static_cast<std::ptrdiff_t>(cap)),
      m_top_split_raw(pack(0u, 0u)),
      m_bottom(0u),
      m_allstolen(true),
      m_splitreq(false) {

  if (cap > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::length_error("Capacity too large");
  }
  if ((cap & (cap - 1)) != 0) {
    std::abort();
  }
  if (cap > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::length_error("Capacity too large for Lace (uint32 indices)");
  }

  const std::size_t bytes = sizeof(T) * cap;
  void* raw = impl::allocate_virtual(bytes);
  if (!raw) throw std::bad_alloc();

  m_array = static_cast<T*>(raw);

  m_worker.bottom = 0;
  m_worker.osplit = 0;
  m_worker.o_allstolen = true;

  top_split64().store(pack(0u, 0u), relaxed);
  m_bottom.store(0u, relaxed);
}

template <dequeable T>
lace_deque<T>::~lace_deque() noexcept {
  if (m_array) {
    impl::deallocate_virtual(m_array, sizeof(T) * static_cast<std::size_t>(m_capacity));
  }
}

template <dequeable T>
constexpr auto lace_deque<T>::size() const noexcept -> std::size_t {
  return static_cast<std::size_t>(ssize());
}

template <dequeable T>
constexpr auto lace_deque<T>::ssize() const noexcept -> std::ptrdiff_t {
  split_state s = unpack(top_split64().load(acquire));
  uint32_t const bot = m_bottom.load(acquire);
  if (bot <= s.top) return 0;
  return static_cast<std::ptrdiff_t>(bot - s.top);
}

template <dequeable T>
constexpr auto lace_deque<T>::empty() const noexcept -> bool {
  if (m_allstolen.load(acquire)) return true;
  split_state s = unpack(top_split64().load(acquire));
  uint32_t const bot = m_bottom.load(acquire);
  return s.top >= bot;
}


template <dequeable T>
constexpr auto lace_deque<T>::capacity() const noexcept -> std::ptrdiff_t { return m_capacity; }

template <dequeable T>
constexpr void lace_deque<T>::push(T const& val) noexcept {
  m_allstolen.store(false, release);

  if (m_worker.o_allstolen) {
    const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);

    array_at(bot).store(val, relaxed);

    ++m_worker.bottom;
    publish_bottom(release);

    top_split64().store(pack(bot, bot), release);

    m_worker.osplit = static_cast<std::ptrdiff_t>(bot);
    m_worker.o_allstolen = false;

    m_splitreq.store(false, relaxed);
    return;
  }

  // // Check for capacity
  // {
  //   split_state s = unpack(m_top_split.load(acquire));
  //   uint32_t const bot = static_cast<uint32_t>(m_worker.bottom);
  //   LF_ASSERT((bot - s.top) < static_cast<uint32_t>(m_capacity));
  // }

  array_at(m_worker.bottom).store(val, relaxed);
  ++m_worker.bottom;
  publish_bottom(release);

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
    publish_bottom(relaxed);

    T val = array_at(m_worker.bottom).load(relaxed);

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

  uint64_t old_v = top_split64().load(acquire);
  split_state s = unpack(old_v);

  if (static_cast<int32_t>(s.split - s.top) > 0) {
    uint64_t new_v = pack(s.top + 1u, s.split);

    if (top_split64().compare_exchange_strong(old_v, new_v, std::memory_order_acq_rel, acquire)) {
      T tmp = array_at(s.top).load(acquire);
      return {.code = err::none, .val = tmp};
    }

    return {.code = err::lost, .val = {}};
  }

  m_splitreq.store(true, release);
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
  publish_bottom(relaxed);

  split_state s = unpack(top_split64().load(relaxed));
  if (s.top > static_cast<uint32_t>(m_worker.bottom)) {
    return pop_empty(std::forward<F>(when_empty));
  }

  T val = array_at(m_worker.bottom).load(relaxed);

  if (m_splitreq.load(relaxed)) {
    grow_shared();
  }
  return val;
}

template <dequeable T>
template <typename F>
constexpr auto lace_deque<T>::pop_empty(F&& when_empty) -> std::invoke_result_t<F> {
  m_worker.osplit = m_worker.bottom;
  m_worker.o_allstolen = true;

  publish_bottom(release);
  m_allstolen.store(true, release);
  m_splitreq.store(false, relaxed);

  return std::invoke(std::forward<F>(when_empty));
}

template <dequeable T>
constexpr auto lace_deque<T>::shrink_shared() noexcept -> bool {
  uint64_t old_v = top_split64().load(relaxed);
  split_state s = unpack(old_v);

  if (s.top == s.split) {
    m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  uint32_t const top0 = s.top;
  uint32_t const split0 = s.split;
  uint32_t newsplit = top0 + ((split0 - top0) >> 1U);

  store_split_preserve_top(newsplit, relaxed);

  impl::thread_fence_seq_cst();

  split_state fresh = unpack(top_split64().load(relaxed));
  uint32_t top1 = fresh.top;

  if (top1 == split0) {
    m_allstolen.store(true, release);
    m_worker.o_allstolen = true;
    return true;
  }

  if (top1 > newsplit) {
    newsplit = top1 + ((split0 - top1) >> 1U);
    store_split_preserve_top(newsplit, relaxed);
  }

  m_worker.osplit = static_cast<std::ptrdiff_t>(newsplit);
  m_worker.o_allstolen = false;
  m_allstolen.store(false, release);
  return false;
}

template <dequeable T>
constexpr auto lace_deque<T>::grow_shared() noexcept -> void {
  m_splitreq.store(false, relaxed);

  uint32_t const split = static_cast<uint32_t>(m_worker.osplit);
  uint32_t const bot   = static_cast<uint32_t>(m_worker.bottom);

  if (bot <= split) {
    return;
  }

  uint32_t const diff = (bot - split + 1u) >> 1U;
  uint32_t const new_split = split + diff;

  store_split_preserve_top(new_split, release);

  m_worker.osplit = static_cast<std::ptrdiff_t>(new_split);
  m_worker.o_allstolen = false;
  m_allstolen.store(false, release);
}

template <dequeable T>
[[nodiscard]] LF_FORCEINLINE auto lace_deque<T>::mask_index(std::ptrdiff_t idx) const noexcept -> std::size_t {
  return static_cast<std::size_t>(idx) & static_cast<std::size_t>(m_mask);
}

} // namespace ext
} // namespace lf

#endif /* LIBFORK_CORE_EXT_DEQUE_LACE_HPP */
