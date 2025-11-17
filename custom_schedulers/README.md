# Custom Schedulers for libfork Benchmarks

This directory is for implementing custom work-stealing schedulers that can be benchmarked against libfork's built-in schedulers and other parallel frameworks.

## Overview

The libfork library uses a **scheduler** abstraction that makes it easy to plug in custom work-stealing implementations. Any type that satisfies the `lf::scheduler` concept can be used with libfork's benchmarks.

## The Scheduler Concept

A scheduler must satisfy the following concept:

```cpp
template <typename Sch>
concept scheduler = requires (Sch &&sch, submit_handle handle) {
  std::forward<Sch>(sch).schedule(handle);
};
```

This means your scheduler only needs to implement a single method:

```cpp
void schedule(lf::submit_handle handle);
```

The `schedule` method accepts a suspended task handle and is responsible for:
1. Storing the task in a queue/deque
2. Ensuring some worker thread will eventually call `lf::resume(handle)` on it
3. Providing the strong exception guarantee

## Minimal Example

Here's the simplest possible scheduler (single-threaded):

```cpp
#include <libfork.hpp>
#include <thread>
#include <atomic>

class simple_pool {
  std::atomic_flag m_stop = ATOMIC_FLAG_INIT;
  std::atomic_flag m_ready = ATOMIC_FLAG_INIT;
  lf::worker_context *m_context = nullptr;
  std::thread m_thread;

  static void work(simple_pool *self) {
    lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
    
    self->m_context = me;
    self->m_ready.test_and_set(std::memory_order_release);
    self->m_ready.notify_one();

    while (!self->m_stop.test(std::memory_order_acquire)) {
      if (auto *job = me->try_pop_all()) {
        lf::resume(job);
      }
    }
    
    // Drain the queue
    while (auto *job = me->try_pop_all()) {
      lf::resume(job);
    }
    
    lf::finalize(me);
  }

public:
  simple_pool() : m_thread{work, this} {
    m_ready.wait(false, std::memory_order_acquire);
  }

  void schedule(lf::submit_handle job) { 
    m_context->schedule(job); 
  }

  ~simple_pool() noexcept {
    m_stop.test_and_set(std::memory_order_release);
    m_thread.join();
  }
};
```

## Implementing a Work-Stealing Scheduler

For a multi-threaded work-stealing scheduler, you'll need:

1. **Worker Contexts**: Each thread needs a `lf::worker_context*` 
2. **Work Queues**: Each worker has a double-ended queue (deque) for tasks
3. **Stealing Logic**: Idle workers steal from other workers' deques
4. **Sleep/Wake Mechanism**: Workers sleep when idle and wake when work arrives

### Key Components

#### Worker Context Initialization

Each worker thread must call:

```cpp
lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
```

And finalize when done:

```cpp
lf::finalize(me);
```

#### Task Submission and Execution

The worker context provides methods for task management:

```cpp
// Check if there's work in the local queue
auto *task = worker_ctx->try_pop_all();

// Try to steal work from other workers
auto *stolen = worker_ctx->try_steal();

// Resume a task
if (task) {
  lf::resume(task);
}
```

#### The schedule() Method

Your scheduler's `schedule()` method should submit work to a worker:

```cpp
void schedule(lf::submit_handle handle) {
  // Choose a worker (round-robin, random, least-loaded, etc.)
  auto *worker = choose_worker();
  
  // Submit the task to that worker's queue
  worker->schedule(handle);
  
  // Wake up the worker if it's sleeping
  wake_worker(worker);
}
```

## Example: Lace Work-Stealing Scheduler

Here's a template for implementing a Lace-style work-stealing scheduler:

```cpp
// custom_schedulers/lace_pool.hpp
#ifndef LACE_POOL_HPP
#define LACE_POOL_HPP

#include <libfork.hpp>
#include <atomic>
#include <thread>
#include <vector>
#include <random>

namespace custom {

class lace_pool {
  std::size_t m_num_threads;
  std::atomic_flag m_stop = ATOMIC_FLAG_INIT;
  std::vector<lf::worker_context*> m_workers;
  std::vector<std::thread> m_threads;
  std::atomic<std::size_t> m_next_worker{0};

  static void worker_loop(lace_pool *pool, std::size_t worker_id) {
    // Initialize this worker's context
    lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
    pool->m_workers[worker_id] = me;
    
    // Main work-stealing loop
    while (!pool->m_stop.test(std::memory_order_acquire)) {
      // Try local work first
      if (auto *task = me->try_pop_all()) {
        lf::resume(task);
        continue;
      }
      
      // Try stealing from other workers
      if (auto *stolen = me->try_steal()) {
        lf::resume(stolen);
        continue;
      }
      
      // No work found - could implement sleep/wait here
      std::this_thread::yield();
    }
    
    // Cleanup
    lf::finalize(me);
  }

public:
  explicit lace_pool(std::size_t num_threads = std::thread::hardware_concurrency())
      : m_num_threads(num_threads), 
        m_workers(num_threads, nullptr) {
    
    // Start worker threads
    for (std::size_t i = 0; i < num_threads; ++i) {
      m_threads.emplace_back(worker_loop, this, i);
    }
    
    // Wait for all workers to initialize
    for (auto *worker : m_workers) {
      while (!worker) {
        std::this_thread::yield();
      }
    }
  }

  void schedule(lf::submit_handle handle) {
    // Simple round-robin assignment
    std::size_t idx = m_next_worker.fetch_add(1, std::memory_order_relaxed) % m_num_threads;
    m_workers[idx]->schedule(handle);
  }

  ~lace_pool() noexcept {
    m_stop.test_and_set(std::memory_order_release);
    for (auto &thread : m_threads) {
      thread.join();
    }
  }
};

} // namespace custom

#endif // LACE_POOL_HPP
```

## Using Your Custom Scheduler with Benchmarks

### Step 1: Create Your Scheduler Header

Place your scheduler implementation in `custom_schedulers/your_scheduler.hpp`.

### Step 2: Create Benchmark Files

For each benchmark you want to run (e.g., fibonacci), create a file like:

```cpp
// bench/source/fib/custom_lace.cpp
#include <benchmark/benchmark.h>
#include "../../../../custom_schedulers/lace_pool.hpp"
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

void fib_custom_lace(benchmark::State &state) {
  state.counters["green_threads"] = state.range(0);
  state.counters["fib(n)"] = work;

  custom::lace_pool pool(state.range(0));

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

BENCHMARK(fib_custom_lace)->Apply(targs)->UseRealTime();
```

### Step 3: Update CMakeLists.txt

Add your custom scheduler to the benchmark build:

```cmake
# In bench/CMakeLists.txt, add:

# Custom Lace scheduler
option(ENABLE_LACE_BENCHMARKS "Enable custom Lace scheduler benchmarks" ON)

if(ENABLE_LACE_BENCHMARKS)
  file(GLOB_RECURSE BENCH_CUSTOM_LACE CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/source/**/custom_lace.cpp")
  target_sources(benchmark PRIVATE ${BENCH_CUSTOM_LACE})
  target_include_directories(benchmark PRIVATE "${CMAKE_SOURCE_DIR}/custom_schedulers")
  message(STATUS "Custom Lace scheduler benchmarks enabled")
endif()
```

### Step 4: Build and Run

```bash
cd build
cmake .. -DENABLE_LACE_BENCHMARKS=ON
cmake --build .
./bench/benchmark --benchmark_filter=fib
```

## Advanced Features

### NUMA Awareness

For NUMA systems, you may want to:
- Pin workers to specific NUMA nodes
- Use `lf::numa_topology` to distribute workers
- Implement NUMA-aware stealing (prefer local node)

See `include/libfork/schedule/lazy_pool.hpp` for an example.

### Sleep/Wake Mechanisms

Instead of busy-waiting with `std::this_thread::yield()`, implement:
- Event counts (see `include/libfork/schedule/ext/event_count.hpp`)
- Condition variables
- Futexes (Linux)

This is crucial for production use to avoid wasting CPU.

### Custom Work Distribution

Experiment with different strategies:
- **Random stealing**: Pick victims at random
- **Locality-aware**: Prefer nearby workers (NUMA, same core)
- **Load-based**: Steal from the busiest worker
- **Hierarchical**: Organize workers in a tree structure

## Testing Your Scheduler

Run all benchmarks with your scheduler:

```bash
./bench/benchmark --benchmark_filter="custom_lace"
```

Compare against libfork's schedulers:

```bash
# Run both and compare
./bench/benchmark --benchmark_filter="fib.*lazy_pool"
./bench/benchmark --benchmark_filter="fib.*custom_lace"
```

## Resources

- **libfork scheduler interface**: `include/libfork/core/scheduler.hpp`
- **Example schedulers**: `include/libfork/schedule/`
- **Worker context API**: `include/libfork/core/ext/context.hpp`
- **Lace paper**: [Lace: Non-blocking Split Deque for Work-Stealing](https://link.springer.com/chapter/10.1007/978-3-319-43659-3_44)

## Troubleshooting

### My scheduler crashes

- Ensure every worker calls `lf::worker_init()` and `lf::finalize()`
- Check that `schedule()` never throws exceptions
- Verify worker contexts are properly synchronized during startup

### Poor performance

- Profile to find bottlenecks (stealing overhead, synchronization, etc.)
- Try different stealing strategies
- Implement sleep/wake instead of busy-waiting
- Consider NUMA topology

### Benchmarks show incorrect results

- Verify your work-stealing respects task dependencies
- Check that all tasks are eventually executed
- Ensure proper memory ordering in atomic operations
