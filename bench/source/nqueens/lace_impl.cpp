#include <lace.h>
#include <array>
#include <vector>

#include "../util.hpp"
#include "config.hpp"

struct QueensBoard {
  std::array<char, nqueens_work> data;
};

TASK_3(int, nqueens_task, int, j, QueensBoard, b, int *, res)

int nqueens_task_CALL(lace_worker *worker, int j, QueensBoard b, int *res) {
  if (j == nqueens_work) {
    *res = 1;
    return 0;
  }

  int sub_results[nqueens_work];
  bool spawned[nqueens_work];
  QueensBoard next_b;

  for (int i = 0; i < nqueens_work; ++i) {
    spawned[i] = false;

    next_b = b;
    next_b.data[j] = (char)i;

    if (queens_ok(j + 1, next_b.data.data())) {
      nqueens_task_SPAWN(worker, j + 1, next_b, &sub_results[i]);
      spawned[i] = true;
    }
  }

  for (int i = 0; i < nqueens_work; ++i) {
    if (spawned[i]) {
      nqueens_task_SYNC(worker);
    }
  }

  int total = 0;
  for (int i = 0; i < nqueens_work; ++i) {
    if (spawned[i]) {
      total += sub_results[i];
    }
  }

  *res = total;
  return 0;
}

extern "C" {

  void lace_nqueens_init_bridge(int workers) {
    lace_start(workers, 0, 0);
  }

  void lace_nqueens_stop_bridge() {
    lace_stop();
  }

  int lace_nqueens_bridge() {
    QueensBoard b{};
    int result = 0;

    nqueens_task(0, b, &result);

    return result;
  }

}