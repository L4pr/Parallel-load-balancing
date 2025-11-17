#include <benchmark/benchmark.h>

#include <lace_pool.hpp>

#include "../util.hpp"
#include "config.hpp"

namespace {

// Fibonacci task using libfork's task interface
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

// Benchmark function for custom Lace scheduler
void fib_custom_lace(benchmark::State &state) {

  state.counters["green_threads"] = state.range(0);
  state.counters["fib(n)"] = work;

  custom::lace_pool pool(state.range(0));

  volatile int secret = work;
  volatile int output;

  for (auto _ : state) {
    output = lf::sync_wait(pool, fib, secret);
  }

#ifndef LF_NO_CHECK
  if (output != sfib(work)) {
    std::cout << "error: fib(" << work << ") = " << output << " != " << sfib(work) << std::endl;
  }
#endif
}

} // namespace

// Register the benchmark
BENCHMARK(fib_custom_lace)->Apply(targs)->UseRealTime();
