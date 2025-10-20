//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef CHISEL_THREAD_POOL_HPP
#define CHISEL_THREAD_POOL_HPP

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stop_token>
#include <thread>

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = std::thread::hardware_concurrency() / 2);
    ~ThreadPool();

    template<class F>
    auto enqueue(F&& f) -> std::future<std::invoke_result_t<F, std::stop_token>> {
        using return_type = std::invoke_result_t<F, std::stop_token>;
        auto task = std::make_shared<std::packaged_task<return_type(std::stop_token)>>(
            std::forward<F>(f)
        );
        {
            std::unique_lock lock(queue_mutex_);
            if (stop_) throw std::runtime_error("enqueue on stopped ThreadPool");
            ++pending_;
            tasks_.emplace([task](std::stop_token st) { (*task)(st); });
        }
        condition_.notify_one();
        return task->get_future();
    }

    void wait_idle();
    void request_stop();

private:
    std::mutex queue_mutex_;
    std::condition_variable_any condition_;
    std::condition_variable idle_cv_;
    std::queue<std::function<void(std::stop_token)>> tasks_;
    bool stop_{false};
    size_t pending_{0};
    std::vector<std::jthread> workers_;
};

#endif // CHISEL_THREAD_POOL_HPP