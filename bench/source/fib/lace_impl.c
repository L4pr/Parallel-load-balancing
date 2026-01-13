#include <lace.h>

TASK_1(int, pfib, int, n)

int pfib_CALL(lace_worker *worker, int n)
{
  if (n < 2) {
    return n;
  } else {
    pfib_SPAWN(worker, n - 1);
    int k = pfib_CALL(worker, n - 2);
    int m = pfib_SYNC(worker);
    return m + k;
  }
}

void lace_init_bridge(int workers) {
  lace_start(workers, 1000000, 0);
}

void lace_stop_bridge() {
  lace_stop();
}

int lace_fib_entry(int n) {
  return pfib(n);
}