#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include "libfork/lace_scheduler.hpp"

int main() {
    lf::lace_scheduler scheduler;

    std::cout << "lace_scheduler started with " << scheduler.worker_count() << " workers.\n";

    for (int i = 0; i < 10; ++i) {
        scheduler.enqueue([i] {
            std::cout << "Task " << i << " running on thread " << std::this_thread::get_id() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Shutting down scheduler.\n";
}
