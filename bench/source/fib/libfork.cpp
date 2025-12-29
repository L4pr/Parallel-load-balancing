#include <benchmark/benchmark.h>

#include <libfork.hpp>

#include "../util.hpp"
#include "config.hpp"

#if defined(LF_BENCH_CHASE_LEV)
  #define ALGO_NAME "ChaseLev"
#elif defined(LF_BENCH_BLOCKING)
  #define ALGO_NAME "Blocking"
#elif defined(LF_BENCH_LACE)
  #define ALGO_NAME "Lace"
#else
  #define ALGO_NAME "ChaseLev"
#endif

namespace {

constexpr auto fib = [](auto fib, int n) LF_STATIC_CALL -> lf::task<int> {
  if (n < 2) {
    co_return n;
  }

  int a, b;

  co_await lf::fork(&a, fib)(n - 1);
  co_await lf::call(&b, fib)(n - 2);

  co_await lf::join;

  co_return a + b;
};

template <lf::scheduler Sch, lf::numa_strategy Strategy>
void fib_libfork(benchmark::State &state) {

  state.counters["green_threads"] = state.range(0);
  state.counters["fib(n)"] = work;

  Sch sch = [&] {
    if constexpr (std::constructible_from<Sch, int>) {
      return Sch(state.range(0));
    } else {
      return Sch{};
    }
  }();

  volatile int secret = work;
  volatile int output;

  for (auto _ : state) {
    output = lf::sync_wait(sch, fib, secret);
  }

#ifndef LF_NO_CHECK
  if (output != sfib(work)) {
    std::cout << "error" << std::endl;
  }
#endif
}

} // namespace

using namespace lf;

// BENCHMARK(fib_libfork<unit_pool, numa_strategy::seq>)->DenseRange(1, 1)->UseRealTime();
// BENCHMARK(fib_libfork<debug_pool, numa_strategy::seq>)->DenseRange(1, 1)->UseRealTime();

BENCHMARK(fib_libfork<lazy_pool, numa_strategy::seq>)
    ->Apply(targs)
    ->UseRealTime()
    ->Label(ALGO_NAME);

BENCHMARK(fib_libfork<lazy_pool, numa_strategy::fan>)
    ->Apply(targs)
    ->UseRealTime()
    ->Label(ALGO_NAME);

BENCHMARK(fib_libfork<busy_pool, numa_strategy::seq>)
    ->Apply(targs)
    ->UseRealTime()
    ->Label(ALGO_NAME);

BENCHMARK(fib_libfork<busy_pool, numa_strategy::fan>)
    ->Apply(targs)
    ->UseRealTime()
    ->Label(ALGO_NAME);