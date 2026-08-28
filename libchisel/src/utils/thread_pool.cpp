//
// Created by Giuseppe Francione on 19/09/25.
//

#include "../../include/thread_pool.hpp"
#include "logger.hpp"
#include "log_sink.hpp"


namespace chisel {

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this]() {
            const stop_token st(&stop_flag_);
            for (;;) {
                std::function<void(stop_token)> task;
                {
                    std::unique_lock lock(queue_mutex_);
                    condition_.wait(lock, [this] {
                        return stop_flag_.load(std::memory_order_acquire) || !tasks_.empty();
                    });

                    if (stop_flag_.load(std::memory_order_acquire) && tasks_.empty()) {
                        return;
                    }
                    if (tasks_.empty()) {
                        continue;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                struct PendingGuard {
                    std::size_t &pending;
                    std::mutex &mtx;
                    std::condition_variable &cv;
                    ~PendingGuard() {
                        std::scoped_lock lock(mtx);
                        if (pending > 0) --pending;
                        cv.notify_all();
                    }
                } guard{.pending=pending_, .mtx=queue_mutex_, .cv=idle_cv_};

                try {
                    task(st);
                } catch (const std::exception& e) {
                    Logger::log(LogLevel::Error, std::string("Unhandled exception in thread pool: ") + e.what());
                } catch (...) {
                    Logger::log(LogLevel::Error, "Unhandled non-standard exception in thread pool");
                }
            }
        });
    }
}

void ThreadPool::request_stop() {
    {
        std::unique_lock lock(queue_mutex_);
        stop_flag_.store(true, std::memory_order_release);
        while (!tasks_.empty()) {
            tasks_.pop();
            if (pending_ > 0) {
                pending_--;
            }
        }
    }
    condition_.notify_all();
    idle_cv_.notify_all();
}

void ThreadPool::wait_idle() {
    std::unique_lock lock(queue_mutex_);
    idle_cv_.wait(lock, [this] {
        return pending_ == 0 && tasks_.empty();
    });
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock lock(queue_mutex_);
        stop_flag_.store(true, std::memory_order_release);
    }
    condition_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace chisel