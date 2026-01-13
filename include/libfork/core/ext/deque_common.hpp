#ifndef LIBFORK_CORE_DEQUE_COMMON_HPP
#define LIBFORK_CORE_DEQUE_COMMON_HPP

#include <atomic>      // for atomic
#include <concepts>    // for default_initializable, etc
#include <cstddef>     // for size_t
#include <optional>    // for optional
#include <type_traits> // for is_trivially_copyable_v, etc
#include <utility>     // for addressof

#include "libfork/core/macro.hpp" // for LF_ASSERT, LF_STATIC_CALL

namespace lf {

inline namespace ext {

/**
 * @brief Verify a type is suitable for use with `std::atomic`
 *
 * This requires a `TriviallyCopyable` type satisfying both `CopyConstructible` and `CopyAssignable`.
 */
template <typename T>
concept atomicable = std::is_trivially_copyable_v<T> && //
                     std::is_copy_constructible_v<T> && //
                     std::is_move_constructible_v<T> && //
                     std::is_copy_assignable_v<T> &&    //
                     std::is_move_assignable_v<T>;      //

/**
 * @brief A concept that verifies a type is lock-free when used with `std::atomic`.
 */
template <typename T>
concept lock_free = atomicable<T> && std::atomic<T>::is_always_lock_free;

/**
 * @brief Test is a type is suitable for use with `lf::deque`.
 *
 * This requires it to be `lf::ext::lock_free` and `std::default_initializable`.
 */
template <typename T>
concept dequeable = lock_free<T> && std::default_initializable<T>;

/**
 * @brief Error codes for ``deque`` 's ``steal()`` operation.
 */
enum class err : int {
  /**
   * @brief The ``steal()`` operation succeeded.
   */
  none = 0,
  /**
   * @brief  Lost the ``steal()`` race hence, the ``steal()`` operation failed.
   */
  lost,
  /**
   * @brief The deque is empty and hence, the ``steal()`` operation failed.
   */
  empty,
};

/**
 * @brief The return type of a `lf::deque` `steal()` operation.
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

} // namespace ext

} // namespace lf

#endif // LIBFORK_CORE_DEQUE_COMMON_HPP