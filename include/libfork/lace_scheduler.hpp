#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "libfork/core/ext/handles.hpp"

namespace lf {

class lace_scheduler {
 public:
  explicit lace_scheduler(unsigned int n_workers = 0, std::size_t dequesize = 0);
  ~lace_scheduler();

  lace_scheduler(lace_scheduler const &) = delete;
  lace_scheduler &operator=(lace_scheduler const &) = delete;
  lace_scheduler(lace_scheduler &&) = delete;
  lace_scheduler &operator=(lace_scheduler &&) = delete;

  void schedule(ext::submit_handle job);
  void enqueue(std::function<void()> &&func);
  [[nodiscard]] unsigned int worker_count() const noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> m_impl;
};

} // namespace lf
