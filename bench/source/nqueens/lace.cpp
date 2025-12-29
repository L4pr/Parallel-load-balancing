#include <benchmark/benchmark.h>
#include <iostream>

#include "../util.hpp"
#include "config.hpp"

extern "C" {
  void lace_nqueens_init_bridge(int workers);
  void lace_nqueens_stop_bridge();
  int lace_nqueens_bridge();
}

namespace {

void nqueens_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["nqueens(n)"] = nqueens_work;

  lace_nqueens_init_bridge(state.range(0));

  volatile int output;

  for (auto _ : state) {
    output = lace_nqueens_bridge();
  }

  lace_nqueens_stop_bridge();

  if (output != answers[nqueens_work]) {
    std::cerr << "lace wrong answer: " << output << " != " << answers[nqueens_work] << std::endl;
  }
}

} // namespace

BENCHMARK(nqueens_lace)->Apply(targs)->UseRealTime();