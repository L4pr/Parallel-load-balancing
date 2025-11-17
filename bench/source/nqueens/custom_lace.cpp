#include <algorithm>
#include <array>
#include <numeric>
#include <iostream>

#include <benchmark/benchmark.h>

#include <lace_pool.hpp>

#include "../util.hpp"
#include "config.hpp"

namespace {

constexpr auto nqueens = []<std::size_t N>(auto nqueens, int j, std::array<char, N> const &a)
                             LF_STATIC_CALL -> lf::task<int> {
  if (N == j) {
    co_return 1;
  }

  std::array<std::array<char, N>, N> buf;
  std::array<int, N> parts;

  for (int i = 0; i < N; i++) {

    for (int k = 0; k < j; k++) {
      buf[i][k] = a[k];
    }

    buf[i][j] = i;

    if (queens_ok(j + 1, buf[i].data())) {
      co_await lf::fork(&parts[i], nqueens)(j + 1, buf[i]);
    } else {
      parts[i] = 0;
    }
  }

  co_await lf::join;

  co_return std::accumulate(parts.begin(), parts.end(), 0L);
};

void nqueens_custom_lace(benchmark::State &state) {

  state.counters["green_threads"] = state.range(0);
  state.counters["nqueens(n)"] = nqueens_work;

  custom::lace_pool pool(state.range(0));

  volatile int output;

  std::array<char, nqueens_work> buf{};

  for (auto _ : state) {
    output = lf::sync_wait(pool, nqueens, 0, buf);
  }

#ifndef LF_NO_CHECK
  if (output != answers[nqueens_work]) {
    std::cout << "error: nqueens(" << nqueens_work << ") = " << output << " != " << answers[nqueens_work] << std::endl;
  }
#endif
}

} // namespace

BENCHMARK(nqueens_custom_lace)->Apply(targs)->UseRealTime();
