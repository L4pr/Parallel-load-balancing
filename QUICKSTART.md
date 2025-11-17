# Quick Start: Testing Custom Work-Stealing Schedulers

This guide will help you quickly get started with implementing and benchmarking your own work-stealing scheduler (like Lace) using the libfork benchmark infrastructure.

## Prerequisites

- CMake 3.14+
- C++20 compiler (C++23 recommended)
- vcpkg dependencies already installed in `vcpkg_installed/`

## Step 1: Understand the Scheduler Interface

Any custom scheduler needs only one method:

```cpp
void schedule(lf::submit_handle handle);
```

This method receives a task and ensures it will eventually be executed by calling `lf::resume(handle)`.

## Step 2: Implement Your Scheduler

Create `custom_schedulers/my_scheduler.hpp`:

```cpp
#include <libfork.hpp>
#include <atomic>
#include <thread>
#include <vector>

namespace custom {

class my_scheduler {
  std::size_t m_num_threads;
  std::atomic_flag m_stop = ATOMIC_FLAG_INIT;
  std::vector<lf::worker_context*> m_workers;
  std::vector<std::thread> m_threads;
  std::atomic<std::size_t> m_next_worker{0};

  static void worker_loop(my_scheduler *pool, std::size_t worker_id) {
    lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
    pool->m_workers[worker_id] = me;
    
    while (!pool->m_stop.test(std::memory_order_acquire)) {
      if (auto *task = me->try_pop_all()) {
        lf::resume(task);
        continue;
      }
      if (auto stolen = me->try_steal(); stolen) {
        lf::resume(*stolen);
        continue;
      }
      std::this_thread::yield();
    }
    
    lf::finalize(me);
  }

public:
  explicit my_scheduler(std::size_t num_threads = std::thread::hardware_concurrency())
      : m_num_threads(num_threads), m_workers(num_threads, nullptr) {
    for (std::size_t i = 0; i < num_threads; ++i) {
      m_threads.emplace_back(worker_loop, this, i);
    }
    for (std::size_t i = 0; i < num_threads; ++i) {
      while (!m_workers[i]) std::this_thread::yield();
    }
  }

  void schedule(lf::submit_handle handle) {
    std::size_t idx = m_next_worker.fetch_add(1, std::memory_order_relaxed) % m_num_threads;
    m_workers[idx]->schedule(handle);
  }

  ~my_scheduler() noexcept {
    m_stop.test_and_set(std::memory_order_release);
    for (auto &thread : m_threads) {
      if (thread.joinable()) thread.join();
    }
  }
};

} // namespace custom
```

## Step 3: Create Benchmark Files

Create `bench/source/fib/custom_my.cpp`:

```cpp
#include <benchmark/benchmark.h>
#include <my_scheduler.hpp>
#include "../util.hpp"
#include "config.hpp"

namespace {

constexpr auto fib = [](auto fib, int n) LF_STATIC_CALL -> lf::task<int> {
  if (n < 2) {
    co_return n;
  }
  int a, b;
  co_await lf::fork(&a, fib)(n - 1);
  co_await lf::call(&b, fib)(n - 2);
  co_await lf::join;
  co_return a + b;
};

void fib_custom_my(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["fib(n)"] = work;

  custom::my_scheduler pool(state.range(0));

  volatile int secret = work;
  volatile int output;

  for (auto _ : state) {
    output = lf::sync_wait(pool, fib, secret);
  }

#ifndef LF_NO_CHECK
  if (output != sfib(work)) {
    std::cout << "error" << std::endl;
  }
#endif
}

} // namespace

BENCHMARK(fib_custom_my)->Apply(targs)->UseRealTime();
```

## Step 4: Build

```bash
# Configure
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -Dlibfork_DEV_MODE=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DENABLE_CUSTOM_SCHEDULERS=ON \
  -DCMAKE_PREFIX_PATH="$(pwd)/vcpkg_installed/x64-linux"

# Build
cmake --build build --target benchmark -j$(nproc)
```

## Step 5: Run Benchmarks

```bash
# List your benchmarks
./build/bench/benchmark --benchmark_list_tests | grep custom_my

# Run your benchmarks
./build/bench/benchmark --benchmark_filter=custom_my

# Compare with libfork
./build/bench/benchmark --benchmark_filter="fib.*(lazy|custom_my)"
```

## Common Benchmarks to Implement

Start with these benchmarks (copy from existing `custom_lace.cpp` files):

1. **fibonacci** (`bench/source/fib/custom_my.cpp`) - Tests task creation overhead
2. **nqueens** (`bench/source/nqueens/custom_my.cpp`) - Tests search/backtracking

## Example Output

```
-------------------------------------------------------------------------
Benchmark                            Time             CPU   Iterations
-------------------------------------------------------------------------
fib_custom_my/1/real_time    14788000000 ns        32624 ns            1
fib_custom_my/2/real_time     7500000000 ns        25340 ns            1
fib_custom_my/4/real_time     4200000000 ns        18230 ns            1
```

## Tips

1. **Start Simple**: Use the template in `custom_schedulers/lace_pool.hpp` as a starting point
2. **Test Correctness First**: Run with `-DLFILE_NO_CHECK=OFF` to verify results
3. **Optimize Later**: Profile your scheduler to find bottlenecks
4. **Read the Docs**: See `CUSTOM_SCHEDULERS.md` and `custom_schedulers/README.md` for details

## Troubleshooting

**Build fails**: Make sure your scheduler header is in `custom_schedulers/` and your benchmark files match the pattern `custom_*.cpp`

**Segfault**: Ensure all workers call `lf::worker_init()` and `lf::finalize()`

**Wrong results**: Check that tasks are properly joined before returning

## Next Steps

- Read `custom_schedulers/README.md` for detailed documentation
- Study `custom_schedulers/lace_pool.hpp` for a complete example
- Implement advanced features like NUMA awareness, sleep/wake, etc.
- Benchmark against multiple cores and workloads

## Resources

- **libfork Docs**: https://conorwilliams.github.io/libfork/
- **Lace Paper**: https://link.springer.com/chapter/10.1007/978-3-319-43659-3_44
- **Scheduler Interface**: `include/libfork/core/scheduler.hpp`
- **Example Schedulers**: `include/libfork/schedule/`
