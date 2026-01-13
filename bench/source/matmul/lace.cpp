#include <benchmark/benchmark.h>
#include <iostream>

#include "../util.hpp"
#include "config.hpp"

// Define the Bridge Interface (C linkage)
extern "C" {
  void lace_matmul_init_bridge(int workers);
  void lace_matmul_stop_bridge();
  void lace_matmul_bridge(float* A, float* B, float* R, unsigned n);
}

namespace {

void matmul_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["mat NxN"] = matmul_work;

  // 1. Initialize
  lace_matmul_init_bridge(state.range(0));

  matmul_args args = matmul_init(matmul_work);

  auto *A = args.A.get();
  auto *B = args.B.get();
  auto *C1 = args.C1.get();
  auto *C2 = args.C2.get();
  auto n = args.n;

  for (auto _ : state) {
    state.PauseTiming();
    zero(C1, n);
    state.ResumeTiming();

    // 2. Run Task
    lace_matmul_bridge(A, B, C1, n);
  }

  // 3. Stop
  lace_matmul_stop_bridge();

  #ifndef LF_NO_CHECK
  iter_matmul(A, B, C2, n);
  if (maxerror(C1, C2, n) > 1e-6f) {
    std::cout << "lace maxerror: " << maxerror(C1, C2, n) << std::endl;
  }
  #endif
}

} // namespace

BENCHMARK(matmul_lace)->Apply(targs)->UseRealTime();