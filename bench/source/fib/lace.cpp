#include <benchmark/benchmark.h>
#include <iostream>

#include "../util.hpp"
#include "config.hpp"

extern "C" {
  int lace_fib_entry(int n);

  void lace_init_bridge(int workers);
  void lace_stop_bridge();
}

namespace {

void fib_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["fib(n)"] = work;

  lace_init_bridge(state.range(0));

  volatile int output;

  for (auto _ : state) {
    output = lace_fib_entry(work);
  }

  lace_stop_bridge();

  #ifndef LF_NO_CHECK
  if (output != sfib(work)) {
    std::cout << "error: expected " << sfib(work) << " got " << output << std::endl;
  }
  #endif
}

} // namespace

BENCHMARK(fib_lace)->Apply(targs)->UseRealTime();