//
// Created by Giuseppe Francione on 19/09/25.
//

#include "../../include/thread_pool.hpp"
#include "logger.hpp"
#include "log_sink.hpp"

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this](const std::stop_token& st) {
            for (;;) {
                std::function<void(std::stop_token)> task;
                {
                    std::unique_lock lock(queue_mutex_);
                    condition_.wait(lock, st, [this] {
                        return stop_ || !tasks_.empty();
                    });
                    if ((stop_ && tasks_.empty()) || st.stop_requested())
                        return;
                    if (tasks_.empty())
                        continue;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                struct PendingGuard {
                    size_t &pending;
                    std::mutex &mtx;
                    std::condition_variable &cv;
                    ~PendingGuard() {
                        std::lock_guard lock(mtx);
                        if (pending > 0) --pending;
                        cv.notify_all();
                    }
                } guard{pending_, queue_mutex_, idle_cv_};
                try {
                    task(st);
                } catch (const std::exception& e) {
                    Logger::log(LogLevel::Error, std::string("Unhandled exception in thread pool: ") + e.what());
                }
            }
        });
    }
}

void ThreadPool::request_stop() {
    {
        std::unique_lock lock(queue_mutex_);
        stop_ = true;
        while (!tasks_.empty()) {
            tasks_.pop();
            if (pending_ > 0) {
                pending_--;
            }
        }
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        worker.request_stop();
    }
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
        stop_ = true;
    }
    condition_.notify_all();
}
