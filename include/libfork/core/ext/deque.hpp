#ifndef C9703881_3D9C_41A5_A7A2_44615C4CFA6A
#define C9703881_3D9C_41A5_A7A2_44615C4CFA6A

// Copyright © Conor Williams <conorwilliams@outlook.com>
// SPDX-License-Identifier: MPL-2.0

// This file acts as a "Selector" to switch implementations at compile-time.

// 1. Include Common Types (Global Scope)
#include "libfork/core/ext/deque_common.hpp"

// 2. Include Specific Implementations (Global Scope)
//    We include them HERE, outside of any 'namespace', because these files
//    already define their own 'namespace lf { namespace ext { ... } }'.

#if defined(LF_BENCH_CHASE_LEV)
    #include "libfork/core/ext/deque_chase_lev.hpp"

#elif defined(LF_BENCH_BLOCKING)
    #include "libfork/core/ext/deque_blocking.hpp"

#elif defined(LF_BENCH_LACE) || defined(LF_USE_LACE_DEQUE)
    #include "libfork/core/ext/deque_lace.hpp"

#else
    // Default Fallback
    #include "libfork/core/ext/deque_chase_lev.hpp"
#endif

// 3. Define the Alias (Inside Namespace)
//    Now we open the namespace just to map the generic name 'deque'
//    to the specific class we just included.

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