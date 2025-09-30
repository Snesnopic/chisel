//
// Created by Giuseppe Francione on 19/09/25.
//

#include "thread_pool.hpp"

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = 1;
    stop_ = false;
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> task; {
                    std::unique_lock lock(queue_mutex_);
                    condition_.wait(lock, [this] {
                        return stop_ || !tasks_.empty();
                    });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() { {
        const std::unique_lock lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (auto &w: workers_) w.join();
}
