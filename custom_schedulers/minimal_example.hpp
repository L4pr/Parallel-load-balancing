// Minimal example of a custom work-stealing scheduler for libfork
// This demonstrates the bare minimum needed to create a functioning scheduler

#ifndef MINIMAL_SCHEDULER_HPP
#define MINIMAL_SCHEDULER_HPP

#include <libfork.hpp>
#include <atomic>
#include <thread>
#include <vector>

namespace custom {

/**
 * @brief Minimal single-threaded scheduler.
 * 
 * This is the absolute minimum needed to satisfy the lf::scheduler concept.
 * It runs all work on a single worker thread.
 */
class minimal_single_threaded {
  std::atomic_flag m_stop = ATOMIC_FLAG_INIT;
  std::atomic_flag m_ready = ATOMIC_FLAG_INIT;
  lf::worker_context *m_context = nullptr;
  std::thread m_thread;

  static void worker_loop(minimal_single_threaded *self) {
    // Every worker must initialize a context
    lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
    
    // Store context so main thread can use it
    self->m_context = me;
    self->m_ready.test_and_set(std::memory_order_release);
    self->m_ready.notify_one();

    // Work loop: execute tasks until stopped
    while (!self->m_stop.test(std::memory_order_acquire)) {
      if (auto *task = me->try_pop_all()) {
        lf::resume(task);
      }
    }
    
    // Drain remaining work
    while (auto *task = me->try_pop_all()) {
      lf::resume(task);
    }
    
    // Every worker must finalize its context
    lf::finalize(me);
  }

public:
  minimal_single_threaded() : m_thread{worker_loop, this} {
    // Wait for worker to initialize
    m_ready.wait(false, std::memory_order_acquire);
  }

  // This is the only method required by lf::scheduler concept
  void schedule(lf::submit_handle handle) { 
    m_context->schedule(handle); 
  }

  ~minimal_single_threaded() noexcept {
    m_stop.test_and_set(std::memory_order_release);
    m_thread.join();
  }
};

/**
 * @brief Minimal multi-threaded work-stealing scheduler.
 * 
 * This adds work-stealing across multiple threads.
 */
class minimal_work_stealing {
  std::size_t m_num_threads;
  std::atomic_flag m_stop = ATOMIC_FLAG_INIT;
  std::vector<lf::worker_context*> m_workers;
  std::vector<std::thread> m_threads;
  std::atomic<std::size_t> m_next{0};

  static void worker_loop(minimal_work_stealing *pool, std::size_t id) {
    lf::worker_context *me = lf::worker_init(lf::nullary_function_t{[]() {}});
    pool->m_workers[id] = me;
    
    while (!pool->m_stop.test(std::memory_order_acquire)) {
      // Try local work first
      if (auto *task = me->try_pop_all()) {
        lf::resume(task);
        continue;
      }
      
      // Try stealing from others
      if (auto stolen = me->try_steal(); stolen) {
        lf::resume(*stolen);
        continue;
      }
      
      // Yield if no work found
      std::this_thread::yield();
    }
    
    lf::finalize(me);
  }

public:
  explicit minimal_work_stealing(std::size_t n = std::thread::hardware_concurrency())
      : m_num_threads(n), m_workers(n, nullptr) {
    
    // Start worker threads
    for (std::size_t i = 0; i < n; ++i) {
      m_threads.emplace_back(worker_loop, this, i);
    }
    
    // Wait for all workers to initialize
    for (std::size_t i = 0; i < n; ++i) {
      while (!m_workers[i]) {
        std::this_thread::yield();
      }
    }
  }

  void schedule(lf::submit_handle handle) {
    // Simple round-robin distribution
    std::size_t idx = m_next.fetch_add(1, std::memory_order_relaxed) % m_num_threads;
    m_workers[idx]->schedule(handle);
  }

  ~minimal_work_stealing() noexcept {
    m_stop.test_and_set(std::memory_order_release);
    for (auto &t : m_threads) {
      if (t.joinable()) t.join();
    }
  }
};

} // namespace custom

#endif // MINIMAL_SCHEDULER_HPP
