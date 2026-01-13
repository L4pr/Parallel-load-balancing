#ifndef C9703881_3D9C_41A5_A7A2_44615C4CFA6A
#define C9703881_3D9C_41A5_A7A2_44615C4CFA6A

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

#include "libfork/core/ext/deque_common.hpp"

#if defined(LF_BENCH_CHASE_LEV)
    #include "libfork/core/ext/deque_chase_lev.hpp"

#elif defined(LF_BENCH_BLOCKING)
    #include "libfork/core/ext/deque_blocking.hpp"

#elif defined(LF_BENCH_LACE) || defined(LF_USE_LACE_DEQUE)
    #include "libfork/core/ext/deque_lace.hpp"

#else
    #include "libfork/core/ext/deque_chase_lev.hpp"
#endif


namespace lf::ext {

#if defined(LF_BENCH_CHASE_LEV)
template <dequeable T>
using deque = chase_lev_deque<T>;

#elif defined(LF_BENCH_BLOCKING)
template <dequeable T>
using deque = blocking_deque<T>;

#elif defined(LF_BENCH_LACE) || defined(LF_USE_LACE_DEQUE)
template <dequeable T>
using deque = lace_deque<T>;

#else
template <dequeable T>
using deque = chase_lev_deque<T>;
#endif

} // namespace lf::ext

#endif /* C9703881_3D9C_41A5_A7A2_44615C4CFA6A */