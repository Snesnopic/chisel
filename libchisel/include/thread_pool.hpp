//
// Created by Giuseppe Francione on 18/09/25.
//

/**
 * @file thread_pool.hpp
 * @brief Defines a simple, modern, and thread-safe thread pool.
 *
 * This file contains the ThreadPool class used by ProcessorExecutor
 * to parallelize file processing tasks.
 */

#ifndef CHISEL_THREAD_POOL_HPP
#define CHISEL_THREAD_POOL_HPP

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stop_token> // Used by the implementation (std::jthread)
#include <thread>

/**
 * @brief A simple fixed-size thread pool for executing tasks concurrently.
 *
 * @details This pool uses std::jthread (C++20) internally, which automatically
 * handles joining on destruction and supports cooperative cancellation
 * via std::stop_token. Tasks enqueued should accept a
 * `std::stop_token` as their argument.
 */
class ThreadPool {
public:
    /**
     * @brief Constructs the thread pool and starts worker threads.
     * @param threads The number of worker threads to create. Defaults to
     * half the hardware concurrency, or 1 if that's zero.
     */
    explicit ThreadPool(unsigned threads = std::thread::hardware_concurrency() / 2);

    /**
     * @brief Destructor.
     * Automatically requests stop and joins all worker threads.
     */
    ~ThreadPool();

    /**
     * @brief Enqueues a task to be executed by a worker thread.
     *
     * The task must be a callable that accepts a `std::stop_token`.
     *
     * @tparam F The type of the callable task.
     * @param f The task to execute.
     * @return A std::future representing the eventual result of the task.
     * @throws std::runtime_error if enqueue is called on a stopped pool.
     */
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

    /**
     * @brief Blocks the calling thread until all pending tasks are complete.
     */
    void wait_idle();

    /**
     * @brief Requests all worker threads to stop and clears the task queue.
     *
     * Pending tasks that have not started will be discarded.
     * Tasks currently running will be notified via their stop_token.
     */
    void request_stop();

private:
    std::mutex queue_mutex_;                ///< Protects tasks_, stop_, and pending_
    std::condition_variable_any condition_; ///< Notifies workers of new tasks or stop requests
    std::condition_variable idle_cv_;       ///< Notifies wait_idle() when pending_ is zero
    std::queue<std::function<void(std::stop_token)>> tasks_; ///< The queue of tasks
    bool stop_{false};                      ///< Flag to signal workers to stop
    size_t pending_{0};                     ///< Number of tasks enqueued or running
    std::vector<std::jthread> workers_;     ///< The worker threads
};

#endif // CHISEL_THREAD_POOL_HPP