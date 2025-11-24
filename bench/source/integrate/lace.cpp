#include <benchmark/benchmark.h>

#include <libfork/lace_scheduler.hpp>

#include "../util.hpp"
#include "config.hpp"

namespace {

constexpr auto integrate = [](auto integrate, double x1, double y1, double x2, double y2, double area)
                               -> lf::task<double> {
  double half = (x2 - x1) / 2;
  double x0 = x1 + half;
  double y0 = fn(x0);

  double area_x1x0 = (y1 + y0) / 2 * half;
  double area_x0x2 = (y0 + y2) / 2 * half;
  double area_x1x2 = area_x1x0 + area_x0x2;

  if (std::abs(area_x1x2 - area) < epsilon) {
    co_return area_x1x2;
  }

  co_await lf::fork(&area_x1x0, integrate)(x1, y1, x0, y0, area_x1x0);
  co_await lf::call(&area_x0x2, integrate)(x0, y0, x2, y2, area_x0x2);
  co_await lf::join;

  co_return area_x1x0 + area_x0x2;
};

void integrate_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["integrate_n"] = n;
  state.counters["integrate_epsilon"] = epsilon;

  lf::lace_scheduler sch{static_cast<unsigned>(state.range(0))};
  volatile double out = 0.0;

  for (auto _ : state) {
    out = lf::sync_wait(sch, integrate, 0, fn(0), n, fn(n), 0);
  }

#ifndef LF_NO_CHECK
  double expect = integral_fn(0, n);
  if (std::abs(out - expect) >= epsilon) {
    std::cout << "lace error: " << out << " != " << expect << std::endl;
  }
#endif
}

} // namespace

BENCHMARK(integrate_lace)->Apply(targs)->UseRealTime();

