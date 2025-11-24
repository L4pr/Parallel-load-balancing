// Copyright © Conor Williams <conorwilliams@outlook.com>

// SPDX-License-Identifier: MPL-2.0

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <atomic>
#include <latch>
#include <thread>

#include <libfork.hpp>
#include <catch2/catch_test_macros.hpp>
#include "libfork/lace_scheduler.hpp"

namespace {
constexpr auto fib_task = [](auto fib_task, int n) -> lf::task<int> {
  if (n < 2) {
    co_return n;
  }

  int a = 0;
  int b = 0;
  co_await lf::fork(&a, fib_task)(n - 1);
  co_await lf::call(&b, fib_task)(n - 2);
  co_await lf::join;
  co_return a + b;
};
}

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

TEST_CASE("lace_scheduler computes Fibonacci", "[schedule][lace]") {
  constexpr int kN = 12;
  constexpr int kExpected = 144;
  lf::lace_scheduler scheduler{std::max(2u, std::thread::hardware_concurrency())};
  auto result = lf::sync_wait(scheduler, fib_task, kN);
  REQUIRE(result == kExpected);
}
