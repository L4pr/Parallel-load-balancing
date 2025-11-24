#include <benchmark/benchmark.h>

#include <libfork/lace_scheduler.hpp>
#include <libfork.hpp>

#include "../util.hpp"
#include "config.hpp"

namespace {

inline constexpr auto matmul = [](auto matmul, float *A, float *B, float *R, unsigned n, unsigned s,
                                   auto add) -> lf::task<> {
  if (n * sizeof(float) <= lf::impl::k_cache_line) {
    co_return multiply(A, B, R, n, s, add);
  }

  unsigned m = n / 2;
  unsigned o00 = 0;
  unsigned o01 = m;
  unsigned o10 = m * s;
  unsigned o11 = m * s + m;

  co_await lf::fork(matmul)(A + o00, B + o00, R + o00, m, s, add);
  co_await lf::fork(matmul)(A + o00, B + o01, R + o01, m, s, add);
  co_await lf::fork(matmul)(A + o10, B + o00, R + o10, m, s, add);
  co_await lf::call(matmul)(A + o10, B + o01, R + o11, m, s, add);

  co_await lf::join;

  co_await lf::fork(matmul)(A + o01, B + o10, R + o00, m, s, std::true_type{});
  co_await lf::fork(matmul)(A + o01, B + o11, R + o01, m, s, std::true_type{});
  co_await lf::fork(matmul)(A + o11, B + o10, R + o10, m, s, std::true_type{});
  co_await lf::call(matmul)(A + o11, B + o11, R + o11, m, s, std::true_type{});

  co_await lf::join;
};

void matmul_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["mat NxN"] = matmul_work;

  lf::lace_scheduler sch{static_cast<unsigned>(state.range(0))};
  auto [A, B, C1, C2, n] = lf::sync_wait(sch, lf::lift, matmul_init, matmul_work);

  for (auto _ : state) {
    lf::sync_wait(sch, matmul, A.get(), B.get(), C1.get(), n, n, std::false_type{});
  }

#ifndef LF_NO_CHECK
  iter_matmul(A.get(), B.get(), C2.get(), n);
  if (maxerror(C1.get(), C2.get(), n) > 1e-6) {
    std::cout << "lace maxerror: " << maxerror(C1.get(), C2.get(), n) << std::endl;
  }
#endif
}

} // namespace

BENCHMARK(matmul_lace)->Apply(targs)->UseRealTime();

