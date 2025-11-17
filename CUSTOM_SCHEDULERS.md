# Adding Custom Work-Stealing Schedulers to libfork Benchmarks

This guide explains how to implement and benchmark your own work-stealing algorithm (such as Lace) using the libfork benchmark infrastructure.

## Quick Start

### 1. Understand the Scheduler Interface

Any custom scheduler must implement a single method that satisfies the `lf::scheduler` concept:

```cpp
void schedule(lf::submit_handle handle);
```

This method receives a suspended task and must ensure it will eventually be resumed by calling `lf::resume(handle)`.

### 2. Create Your Scheduler

Create your scheduler implementation in the `custom_schedulers/` directory:

```cpp
// custom_schedulers/my_scheduler.hpp
#include <libfork.hpp>

namespace custom {

class my_scheduler {
  // Your implementation here
  
public:
  void schedule(lf::submit_handle handle) {
    // Schedule the task for execution
  }
};

} // namespace custom
```

See `custom_schedulers/README.md` for detailed documentation and `custom_schedulers/lace_pool.hpp` for a complete example.

### 3. Create Benchmark Files

For each benchmark you want to run (e.g., fibonacci, nqueens), create a file named `custom_<name>.cpp`:

```bash
# Example for fibonacci benchmark
cp bench/source/fib/libfork.cpp bench/source/fib/custom_lace.cpp
```

Then modify it to use your custom scheduler:

```cpp
#include "../../../../custom_schedulers/my_scheduler.hpp"

void my_benchmark(benchmark::State &state) {
  custom::my_scheduler pool(state.range(0));
  // ... rest of benchmark code using pool
  output = lf::sync_wait(pool, fib, secret);
}

BENCHMARK(my_benchmark)->Apply(targs)->UseRealTime();
```

### 4. Build and Run

```bash
# Configure with custom schedulers enabled (default: ON)
cmake -B build -S . -DENABLE_CUSTOM_SCHEDULERS=ON

# Build
cmake --build build

# Run your benchmark
./build/bench/benchmark --benchmark_filter=custom_lace
```

## Example: Implementing Lace

A template Lace implementation is provided in `custom_schedulers/lace_pool.hpp`. To test it:

```bash
# Build
cmake -B build -S .
cmake --build build

# Run Lace benchmarks
./build/bench/benchmark --benchmark_filter=custom_lace

# Compare with libfork's lazy_pool
./build/bench/benchmark --benchmark_filter="fib.*(lazy_pool|custom_lace)"
```

## Directory Structure

```
Parallel-load-balancing/
├── custom_schedulers/          # Your custom scheduler implementations
│   ├── README.md              # Detailed documentation
│   ├── lace_pool.hpp          # Example Lace scheduler
│   └── your_scheduler.hpp     # Your implementation
│
├── bench/
│   └── source/
│       ├── fib/
│       │   ├── libfork.cpp          # libfork baseline
│       │   ├── tbb.cpp              # TBB comparison
│       │   └── custom_lace.cpp      # Your custom scheduler
│       │
│       └── nqueens/
│           ├── libfork.cpp
│           └── custom_lace.cpp
│
└── CMakeLists.txt             # Build configuration
```

## Available Benchmarks

You can create custom scheduler versions for any of these benchmarks:

- `fib/` - Recursive Fibonacci (task creation overhead)
- `nqueens/` - N-Queens problem (search/backtracking)
- `fold/` - Parallel reduction
- `integrate/` - Numerical integration
- `matmul/` - Matrix multiplication
- `primes/` - Prime number sieving
- `scan/` - Parallel prefix sum
- `uts/` - Unbalanced Tree Search

## Running Benchmarks

### Run all custom scheduler benchmarks
```bash
./build/bench/benchmark --benchmark_filter=custom
```

### Run specific benchmark
```bash
./build/bench/benchmark --benchmark_filter=fib_custom_lace
```

### Compare implementations
```bash
# Compare Lace vs libfork lazy_pool on fibonacci
./build/bench/benchmark --benchmark_filter="fib.*(lazy|custom_lace)" \
  --benchmark_repetitions=5
```

### Export results
```bash
# JSON format
./build/bench/benchmark --benchmark_out=results.json \
  --benchmark_out_format=json

# CSV format
./build/bench/benchmark --benchmark_out=results.csv \
  --benchmark_out_format=csv
```

## CMake Options

```bash
# Enable/disable custom scheduler benchmarks
-DENABLE_CUSTOM_SCHEDULERS=ON/OFF  # Default: ON

# Disable result checking for faster benchmarks
-DLF_NO_CHECK=ON

# Build type
-DCMAKE_BUILD_TYPE=Release  # Important for performance testing
```

## Troubleshooting

### Build errors

**Error**: `cannot find lace_pool.hpp`
- Make sure your scheduler is in `custom_schedulers/` directory
- Check that CMake includes the directory: `target_include_directories(benchmark PRIVATE "${CMAKE_SOURCE_DIR}/custom_schedulers")`

**Error**: `schedule() not found`
- Verify your scheduler implements `void schedule(lf::submit_handle)`
- Check that the method is public

### Runtime errors

**Segmentation fault**
- Ensure all workers call `lf::worker_init()` and `lf::finalize()`
- Check that workers are properly synchronized during pool construction
- Verify that tasks are not accessed after being resumed

**Incorrect results**
- Enable checking: build without `-DLF_NO_CHECK=ON`
- Verify your work-stealing respects task dependencies
- Check that all forked tasks are joined before parent returns

### Performance issues

**Slower than expected**
- Compile with optimizations: `-DCMAKE_BUILD_TYPE=Release`
- Profile to identify bottlenecks (stealing overhead, synchronization)
- Consider NUMA-aware work distribution
- Implement sleep/wake instead of busy-waiting

## Resources

- **libfork documentation**: https://conorwilliams.github.io/libfork/
- **Scheduler concept**: `include/libfork/core/scheduler.hpp`
- **Example schedulers**: `include/libfork/schedule/`
- **Worker context API**: `include/libfork/core/ext/context.hpp`
- **Custom schedulers guide**: `custom_schedulers/README.md`

## Contributing

If you implement an interesting work-stealing algorithm:

1. Add it to `custom_schedulers/` with documentation
2. Create benchmark files for at least `fib` and `nqueens`
3. Include performance comparisons in the PR description
4. Document any unique features or optimizations

## Examples from Literature

Consider implementing these work-stealing algorithms:

- **Lace**: Non-blocking split deque ([paper](https://link.springer.com/chapter/10.1007/978-3-319-43659-3_44))
- **ABP**: Arora-Blumofe-Plaxton work-stealing ([paper](https://doi.org/10.1145/277651.277678))
- **NUMA-aware**: Locality-conscious stealing
- **Hierarchical**: Tree-structured work distribution
- **Adaptive**: Dynamic victim selection strategies
