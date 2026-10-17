//
// Created by Giuseppe Francione on 19/09/25.
//

#include "../../include/thread_pool.hpp"
#include "logger.hpp"
#include "log_sink.hpp"
#include <algorithm>


namespace chisel {

namespace {
thread_local ThreadPool* t_current_pool = nullptr;
}

ThreadPool* ThreadPool::current() noexcept {
    return t_current_pool;
}

ThreadPool::ThreadPool(unsigned threads) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        workers_.emplace_back([this]() {
            t_current_pool = this;
            const stop_token st(&stop_flag_);
            for (;;) {
                std::function<void(stop_token)> task;
                {
                    std::unique_lock lock(queue_mutex_);
                    condition_.wait(lock, [this] {
                        return stop_flag_.load(std::memory_order_acquire) || !tasks_.empty() || !groups_.empty();
                    });

                    // waiting tasks come first, so that each gets going; loops get the workers left over
                    if (tasks_.empty() && help_one(lock)) {
                        continue;
                    }

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

bool ThreadPool::help_one(std::unique_lock<std::mutex>& lock) {
    while (!groups_.empty()) {
        LoopGroup* group = groups_.back();
        const std::size_t i = group->next.fetch_add(1);
        if (i >= group->n) {
            groups_.pop_back();
            continue;
        }
        // the caller keeps the group alive until this iteration counts as done
        lock.unlock();
        std::exception_ptr error;
        try {
            (*group->body)(i);
        } catch (...) {
            error = std::current_exception();
        }
        lock.lock();
        if (error && !group->error) group->error = error;
        ++group->done;
        group_cv_.notify_all();
        return true;
    }
    return false;
}

void ThreadPool::parallel_for(const std::size_t n, const std::function<void(std::size_t)>& body) {
    if (current() != this || workers_.size() < 2 || n < 2) {
        for (std::size_t i = 0; i < n; ++i) body(i);
        return;
    }

    LoopGroup group;
    group.body = &body;
    group.n = n;
    std::unique_lock lock(queue_mutex_);
    groups_.push_back(&group);
    condition_.notify_all();
    group_cv_.notify_all();

    const auto claimable = [this] {
        return std::ranges::any_of(groups_, [](const LoopGroup* g) { return g->next.load() < g->n; });
    };
    while (group.done < n) {
        if (const std::size_t i = group.next.fetch_add(1); i < n) {
            lock.unlock();
            std::exception_ptr error;
            try {
                body(i);
            } catch (...) {
                error = std::current_exception();
            }
            lock.lock();
            if (error && !group.error) group.error = error;
            ++group.done;
            continue;
        }
        // the last iterations run elsewhere: help other loops meanwhile, never new tasks
        if (help_one(lock)) continue;
        group_cv_.wait(lock, [&] { return group.done == n || claimable(); });
    }
    std::erase(groups_, &group);
    lock.unlock();
    if (group.error) std::rethrow_exception(group.error);
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