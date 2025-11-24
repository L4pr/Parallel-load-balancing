#include "libfork/lace_scheduler.hpp"

#include <thread>
#include <utility>
#include <functional>

#include "lace.h"
#include "libfork/core/ext/resume.hpp"

namespace lf {

namespace {
struct lf_lace_job {
  std::function<void()> fn;
};

VOID_TASK_1(run_lace_job, lf_lace_job *, job);

void run_lace_job_CALL(lace_worker *, lf_lace_job *job) {
  try {
    job->fn();
  } catch (...) {
    // Swallow exceptions to avoid unwinding across C boundary.
  }
  delete job;
}
} // namespace

struct lace_scheduler::impl {
  impl(unsigned int n_workers, std::size_t dequesize) {
    if (n_workers == 0) {
      n_workers = std::max(1u, std::thread::hardware_concurrency());
    }
    if (!lace_is_running()) {
      lace_start(n_workers, dequesize, 0);
    }
  }

  ~impl() {
    if (lace_is_running()) {
      lace_stop();
    }
  }

  void schedule(ext::submit_handle job) {
    enqueue([job] { ext::resume(job); });
  }

  void enqueue(std::function<void()> &&func) {
    auto *task = new lf_lace_job{std::move(func)};
    run_lace_job(task);
  }

  [[nodiscard]] unsigned int worker_count() const noexcept { return lace_worker_count(); }
};

lace_scheduler::lace_scheduler(unsigned int n_workers, std::size_t dequesize)
    : m_impl(std::make_unique<impl>(n_workers, dequesize)) {}

lace_scheduler::~lace_scheduler() = default;

void lace_scheduler::schedule(ext::submit_handle job) { m_impl->schedule(job); }

void lace_scheduler::enqueue(std::function<void()> &&func) { m_impl->enqueue(std::move(func)); }

unsigned int lace_scheduler::worker_count() const noexcept { return m_impl->worker_count(); }

} // namespace lf
