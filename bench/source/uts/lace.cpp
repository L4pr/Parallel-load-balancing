#include <benchmark/benchmark.h>
#include <iostream>

#include "../util.hpp"
#include "config.hpp"
#include "external/uts.h"

extern "C" {
  void lace_uts_init_bridge(int workers);
  void lace_uts_stop_bridge();
  void lace_uts_bridge(int tree, result* out_res);
}

namespace {

void uts_lace(benchmark::State &state, int tree) {
  state.counters["green_threads"] = state.range(0);

  lace_uts_init_bridge(state.range(0));

  result r;

  for (auto _ : state) {
    lace_uts_bridge(tree, &r);
  }

  lace_uts_stop_bridge();

  if (r != result_tree(tree)) {
    std::cerr << "lace uts " << tree << " failed: "
              << "Size: " << r.size << " != " << result_tree(tree).size
              << std::endl;
  }
}

} // namespace

MAKE_UTS_FOR(uts_lace);