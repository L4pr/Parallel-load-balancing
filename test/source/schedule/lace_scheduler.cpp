// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <atomic>
#include <latch>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include "libfork/lace_scheduler.hpp"

TEST_CASE("lace_scheduler reports worker count", "[schedule][lace]") {
  lf::lace_scheduler scheduler{std::max(1u, std::thread::hardware_concurrency())};
  REQUIRE(scheduler.worker_count() > 0);
}

TEST_CASE("lace_scheduler executes enqueued work", "[schedule][lace]") {
  constexpr int kTasks = 6;
  std::latch latch{kTasks};
  std::atomic<int> counter{0};

  lf::lace_scheduler scheduler{std::max(2u, std::thread::hardware_concurrency())};

  for (int i = 0; i < kTasks; ++i) {
    scheduler.enqueue([&] {
      counter.fetch_add(1, std::memory_order_relaxed);
      latch.count_down();
    });
  }

  latch.wait();
  REQUIRE(counter.load(std::memory_order_relaxed) == kTasks);
}
