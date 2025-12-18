#include <lace.h>
#include <type_traits> // Required for std::true_type

#include "../util.hpp" // Contains the C++ 'multiply' template
#include "config.hpp"

// 1. Declare the Task
// We use 'int' return type to satisfy the macro requirements
TASK_6(int, matmul_task, float*, A, float*, B, float*, R, unsigned, n, unsigned, s, int, add)

// 2. Implement the Task Logic
int matmul_task_CALL(lace_worker *worker, float* A, float* B, float* R, unsigned n, unsigned s, int add)
{
  // Base Case
  if (n * sizeof(float) <= lf::impl::k_cache_line) {
    if (add) {
      multiply(A, B, R, n, s, std::true_type{});
    } else {
      multiply(A, B, R, n, s, std::false_type{});
    }
    return 0;
  }

  unsigned m = n / 2;

  // Offsets
  unsigned o00 = 0;
  unsigned o01 = m;
  unsigned o10 = m * s;
  unsigned o11 = m * s + m;

  // --- PHASE 1 ---
  matmul_task_SPAWN(worker, A + o00, B + o00, R + o00, m, s, add);
  matmul_task_SPAWN(worker, A + o00, B + o01, R + o01, m, s, add);
  matmul_task_SPAWN(worker, A + o10, B + o00, R + o10, m, s, add);
  matmul_task_SPAWN(worker, A + o10, B + o01, R + o11, m, s, add);

  // Sync 4 times
  matmul_task_SYNC(worker);
  matmul_task_SYNC(worker);
  matmul_task_SYNC(worker);
  matmul_task_SYNC(worker);

  // --- PHASE 2 ---
  matmul_task_SPAWN(worker, A + o01, B + o10, R + o00, m, s, 1);
  matmul_task_SPAWN(worker, A + o01, B + o11, R + o01, m, s, 1);
  matmul_task_SPAWN(worker, A + o11, B + o10, R + o10, m, s, 1);
  matmul_task_SPAWN(worker, A + o11, B + o11, R + o11, m, s, 1);

  // Sync 4 times
  matmul_task_SYNC(worker);
  matmul_task_SYNC(worker);
  matmul_task_SYNC(worker);
  matmul_task_SYNC(worker);

  return 0;
}

// 3. Export C-compatible Bridge Functions
extern "C" {

void lace_matmul_init_bridge(int workers) {
  // 0 for dqsize/stacksize uses defaults
  lace_start(workers, 0, 0);
}

void lace_matmul_stop_bridge() {
  lace_stop();
}

void lace_matmul_bridge(float* A, float* B, float* R, unsigned n) {
  // Call the generated wrapper. add=0 (overwrite)
  matmul_task(A, B, R, n, n, 0);
}

}