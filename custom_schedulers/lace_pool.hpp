#ifndef LACE_POOL_HPP
#define LACE_POOL_HPP

// Template implementation of a Lace-style work-stealing scheduler for libfork
// This is a starting point for implementing the full Lace algorithm.
//
// Reference: "Lace: Non-blocking Split Deque for Work-Stealing"
// https://link.springer.com/chapter/10.1007/978-3-319-43659-3_44

#include <libfork.hpp>

#include <atomic>
#include <thread>
#include <vector>
#include <random>
#include <chrono>

namespace custom {

/**
 * @brief A work-stealing thread pool implementing the Lace algorithm.
 * 
 * This is a template/starting point for implementing Lace's work-stealing.
 * The core Lace features to implement include:
 * 
 * 1. Non-blocking split deque per worker
 * 2. Lock-free task stealing
 * 3. Efficient task distribution
 * 
 * Current implementation uses libfork's built-in worker contexts which
 * already provide work-stealing deques. To implement true Lace, you would
 * need to replace the deque implementation with Lace's split deque.
 */
class lace_pool {
  std::size_t m_num_threads;
  std::atomic_flag m_stop = ATOMIC_FLAG_INIT;
  std::vector<lf::worker_context*> m_workers;
  std::vector<std::thread> m_threads;
  std::atomic<std::size_t> m_next_worker{0};
  
  // For randomized work stealing
  std::vector<std::mt19937> m_rngs;

  /**
   * @brief Main worker loop for each thread.
   * 
   * Each worker:
   * 1. Processes local work first (LIFO from own deque)
   * 2. Steals from other workers when idle (FIFO from victim's deque)
   * 3. Yields when no work is available
   */
  static void worker_loop(lace_pool *pool, std::size_t worker_id) {
    // Initialize this worker's context with libfork
    lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
    
    // Register this worker in the pool
    pool->m_workers[worker_id] = me;
    
    // Random number generator for stealing victim selection
    auto &rng = pool->m_rngs[worker_id];
    std::uniform_int_distribution<std::size_t> dist(0, pool->m_num_threads - 1);
    
    // Main work-stealing loop
    while (!pool->m_stop.test(std::memory_order_acquire)) {
      // Phase 1: Try to execute local work (LIFO order for cache locality)
      if (auto *task = me->try_pop_all()) {
        lf::resume(task);
        continue;
      }
      
      // Phase 2: Try stealing from other workers (FIFO order)
      // In true Lace, this would use a randomized victim selection
      // and a non-blocking split deque protocol
      if (auto stolen = me->try_steal(); stolen) {
        lf::resume(*stolen);
        continue;
      }
      
      // Phase 3: No work found
      // TODO: Implement proper sleep/wake mechanism instead of yielding
      // Lace uses a work-requesting protocol where idle workers can
      // explicitly request work from others
      std::this_thread::yield();
    }
    
    // Drain any remaining work before shutdown
    while (auto *task = me->try_pop_all()) {
      lf::resume(task);
    }
    
    // Cleanup worker context
    lf::finalize(me);
  }

public:
  /**
   * @brief Construct a new lace pool with the specified number of workers.
   * 
   * @param num_threads Number of worker threads (default: hardware concurrency)
   */
  explicit lace_pool(std::size_t num_threads = std::thread::hardware_concurrency())
      : m_num_threads(num_threads), 
        m_workers(num_threads, nullptr),
        m_rngs(num_threads) {
    
    // Initialize random number generators for each worker
    std::random_device rd;
    for (auto &rng : m_rngs) {
      rng.seed(rd());
    }
    
    // Start all worker threads
    for (std::size_t i = 0; i < num_threads; ++i) {
      m_threads.emplace_back(worker_loop, this, i);
    }
    
    // Wait for all workers to initialize their contexts
    // This ensures all workers are ready before we start scheduling work
    for (std::size_t i = 0; i < num_threads; ++i) {
      while (!m_workers[i]) {
        std::this_thread::yield();
      }
    }
  }

  /**
   * @brief Schedule a task for execution.
   * 
   * This is the core scheduler interface required by libfork.
   * 
   * Current implementation uses simple round-robin distribution.
   * 
   * TODO: Implement Lace's work distribution strategy:
   * - Could use thread affinity
   * - Could use locality hints
   * - Could use load balancing
   * 
   * @param handle The suspended task to schedule
   */
  void schedule(lf::submit_handle handle) {
    // Simple round-robin assignment to workers
    // This could be improved with:
    // 1. Affinity-based scheduling (prefer same worker)
    // 2. Load-aware scheduling (prefer less busy workers)
    // 3. NUMA-aware scheduling (prefer local node)
    std::size_t idx = m_next_worker.fetch_add(1, std::memory_order_relaxed) % m_num_threads;
    m_workers[idx]->schedule(handle);
  }

  /**
   * @brief Destructor - stops all workers and waits for completion.
   */
  ~lace_pool() noexcept {
    // Signal all workers to stop
    m_stop.test_and_set(std::memory_order_release);
    
    // Wait for all workers to finish
    for (auto &thread : m_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  // Disable copy/move semantics
  lace_pool(const lace_pool&) = delete;
  lace_pool& operator=(const lace_pool&) = delete;
  lace_pool(lace_pool&&) = delete;
  lace_pool& operator=(lace_pool&&) = delete;

  /**
   * @brief Get the number of worker threads in this pool.
   */
  std::size_t num_workers() const noexcept { return m_num_threads; }
};

} // namespace custom

#endif // LACE_POOL_HPP
