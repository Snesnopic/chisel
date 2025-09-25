//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_THREAD_POOL_HPP
#define MONOLITH_THREAD_POOL_HPP

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include "logger.hpp"

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = std::thread::hardware_concurrency() / 2);

    template<class F, class... Args>
    auto enqueue(F &&f, Args &&... args) -> std::future<std::invoke_result_t<F, Args...> > {
        using return_type = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future(); {
            std::unique_lock lock(queue_mutex_);
            if (stop_) {
                Logger::log(LogLevel::ERROR, "ThreadPool error: enqueue on stopped ThreadPool", "ThreadPool");
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return res;
    }

    ~ThreadPool();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()> > tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
};

#endif // MONOLITH_THREAD_POOL_HPP
