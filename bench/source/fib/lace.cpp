#include <benchmark/benchmark.h>
#include <algorithm>

#include <libfork.hpp>
#include <libfork/lace_scheduler.hpp>

#include "../util.hpp"
#include "config.hpp"

namespace {

constexpr auto fib_task = [](auto fib_task, int n) -> lf::task<int> {
  if (n < 2) {
    co_return n;
  }

  int a = 0;
  int b = 0;
  co_await lf::fork(&a, fib_task)(n - 1);
  co_await lf::call(&b, fib_task)(n - 2);
  co_await lf::join;
  co_return a + b;
};

void fib_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["fib(n)"] = work;

  const auto workers = static_cast<unsigned>(std::max<int64_t>(1, state.range(0)));
  lf::lace_scheduler scheduler{workers};
  volatile int result = 0;

  for (auto _ : state) {
    result = lf::sync_wait(scheduler, fib_task, work);
  }

#ifndef LF_NO_CHECK
  if (result != sfib(work)) {
    std::cout << "lace fib mismatch" << std::endl;
  }
#endif
}

} // namespace

BENCHMARK(fib_lace)->Apply(targs)->UseRealTime();
