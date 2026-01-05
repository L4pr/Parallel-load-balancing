#ifndef LIBFORK_CORE_EXT_DEQUE_LACE_HPP
#define LIBFORK_CORE_EXT_DEQUE_LACE_HPP

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
    m_top(0), m_split(0), m_allstolen(true), m_splitreq(false) {
      if ((cap & (cap - 1)) != 0) abort();
      m_array = static_cast<std::atomic<T>*>(impl::allocate_virtual(sizeof(std::atomic<T>) * cap));
      if (!m_array) throw std::bad_alloc();
      m_worker.bottom = 0; m_worker.osplit = 0; m_worker.o_allstolen = true;
  }

  ~lace_deque() noexcept {
      if (m_array) impl::deallocate_virtual(m_array, sizeof(std::atomic<T>) * m_capacity);
  }

  [[nodiscard]] constexpr auto ssize() const noexcept -> std::ptrdiff_t {
      return static_cast<std::ptrdiff_t>(m_worker.bottom) - static_cast<std::ptrdiff_t>(m_top.load(relaxed));
  }

  constexpr void push(T const &val) noexcept {
    if (m_worker.o_allstolen) [[unlikely]] {
      const uint32_t bot = static_cast<uint32_t>(m_worker.bottom);
      (m_array + mask_index(bot))->store(val, relaxed);

      // Order is critical: initialize indices before revealing 'not empty'
      m_top.store(bot, relaxed);
      m_split.store(bot + 1, relaxed);
      m_worker.osplit = m_worker.bottom + 1;
      ++m_worker.bottom;

      m_allstolen.store(false, release);
      m_worker.o_allstolen = false;
      m_splitreq.store(false, relaxed);
      return;
    }

    (m_array + mask_index(m_worker.bottom))->store(val, relaxed);
    ++m_worker.bottom;
    if (m_splitreq.load(relaxed)) [[unlikely]] { grow_shared(); }
  }

  template <std::invocable F = return_nullopt<T>>
  constexpr auto pop(F &&when_empty = {}) -> std::invoke_result_t<F> {
    if (m_worker.bottom > m_worker.osplit) [[likely]] {
      --m_worker.bottom;
      T val = (m_array + mask_index(m_worker.bottom))->load(relaxed);
      if (m_splitreq.load(relaxed)) [[unlikely]] { grow_shared(); }
      return val;
    }
    return pop_cold_path(std::forward<F>(when_empty));
  }

  template <typename F>
  LF_NOINLINE auto pop_cold_path(F &&when_empty) noexcept -> std::invoke_result_t<F> {
    if (m_worker.o_allstolen || m_worker.bottom <= 0) return pop_stolen(std::forward<F>(when_empty));
    if (shrink_shared()) return pop_stolen(std::forward<F>(when_empty));

    --m_worker.bottom;
    if (static_cast<int32_t>(m_top.load(relaxed) - static_cast<uint32_t>(m_worker.bottom)) > 0) {
      return pop_stolen(std::forward<F>(when_empty));
    }

    T val = (m_array + mask_index(m_worker.bottom))->load(relaxed);
    if (m_splitreq.load(relaxed)) grow_shared();
    return val;
  }

  [[nodiscard]] constexpr auto steal() noexcept -> steal_t<T> {
      if (m_allstolen.load(acquire)) return {.code = err::empty};

      uint32_t s = m_split.load(acquire);
      uint32_t t = m_top.load(acquire);

      if (static_cast<int32_t>(s - t) > 0) {
          T tmp = (m_array + mask_index(t))->load(relaxed);
          if (m_top.compare_exchange_strong(t, t + 1, seq_cst, relaxed)) {
              return {.code = err::none, .val = tmp};
          }
          return {.code = err::lost};
      }
      m_splitreq.store(true, release);
      return {.code = err::empty};
  }

 private:
  constexpr auto grow_shared() noexcept -> void {
      uint32_t new_s = static_cast<uint32_t>((m_worker.osplit + m_worker.bottom + 1) >> 1U);
      m_split.store(new_s, release);
      m_worker.osplit = new_s;
      m_splitreq.store(false, relaxed);
  }

  constexpr auto shrink_shared() noexcept -> bool {
    uint32_t t = m_top.load(relaxed);
    uint32_t s = m_split.load(relaxed);
    if (t == s) return true;

    uint32_t new_s = t + ((s - t) >> 1U);
    m_split.store(new_s, release);
    std::atomic_thread_fence(seq_cst); // Dekker Fence

    uint32_t ft = m_top.load(acquire);
    if (static_cast<int32_t>(ft - static_cast<uint32_t>(m_worker.bottom)) >= 0) return true;

    m_worker.osplit = static_cast<std::ptrdiff_t>(new_s);
    if (static_cast<int32_t>(ft - new_s) > 0) m_worker.osplit = static_cast<std::ptrdiff_t>(ft);

    return m_worker.bottom <= m_worker.osplit;
  }

  template <typename F>
  constexpr auto pop_stolen(F&& when_empty) -> std::invoke_result_t<F> {
      m_worker.osplit = m_worker.bottom;
      m_worker.o_allstolen = true;
      m_allstolen.store(true, release);
      return std::invoke(std::forward<F>(when_empty));
  }

  LF_FORCEINLINE auto mask_index(std::ptrdiff_t i) const noexcept { return static_cast<std::size_t>(i & m_mask); }

  std::atomic<T>* m_array;
  const std::ptrdiff_t m_mask, m_capacity;

  alignas(k_cache_line) std::atomic<uint32_t> m_top;
  std::atomic<uint32_t> m_split;
  std::atomic<bool> m_allstolen, m_splitreq;

  struct alignas(k_cache_line) {
    std::ptrdiff_t bottom, osplit;
    bool o_allstolen;
  } m_worker;

  static constexpr auto relaxed = std::memory_order_relaxed;
  static constexpr auto acquire = std::memory_order_acquire;
  static constexpr auto release = std::memory_order_release;
  static constexpr auto seq_cst = std::memory_order_seq_cst;
};

} // namespace ext
} // namespace lf
#endif