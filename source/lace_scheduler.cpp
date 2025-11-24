#include "libfork/lace_scheduler.hpp"

#include <functional>
#include <thread>
#include <utility>

#include "lace.h"
#include "libfork/core/ext/context.hpp"
#include "libfork/core/ext/resume.hpp"
#include "libfork/core/ext/tls.hpp"
#include "libfork/core/impl/utility.hpp"

namespace lf {

namespace {
struct lf_lace_job {
  std::function<void()> fn;
};

struct lace_worker_tls {
  lace_worker_tls() {
    if (!lf::impl::tls::has_context) {
      m_ctx = lf::worker_init(lf::nullary_function_t{[] {}});
      m_owns_context = true;
    }
  }

  ~lace_worker_tls() {
    if (m_owns_context && m_ctx != nullptr) {
      lf::finalize(m_ctx);
    }
  }

  lace_worker_tls(lace_worker_tls const &) = delete;
  lace_worker_tls &operator=(lace_worker_tls const &) = delete;

  lace_worker_tls(lace_worker_tls &&) = delete;
  lace_worker_tls &operator=(lace_worker_tls &&) = delete;

private:
  worker_context *m_ctx = nullptr;
  bool m_owns_context = false;
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
    enqueue([job] {
      static thread_local lace_worker_tls tls_guard{};
      lf::ext::resume(non_null(job));
    });
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

void lace_scheduler::schedule(ext::submit_handle job) {
  LF_ASSERT(job != nullptr);
  m_impl->schedule(job);
}

void lace_scheduler::enqueue(std::function<void()> &&func) { m_impl->enqueue(std::move(func)); }

unsigned int lace_scheduler::worker_count() const noexcept { return m_impl->worker_count(); }

} // namespace lf
