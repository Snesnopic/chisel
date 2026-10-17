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
#include <thread>
#include <atomic>
#include <exception>

namespace chisel {

// custom stop token to replace std::stop_token
/**
 * @brief Represents stop_token.
 */
class stop_token {
    const std::atomic<bool>* stop_flag_{nullptr};
public:
    stop_token() = default;
    explicit stop_token(const std::atomic<bool>* flag) : stop_flag_(flag) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_flag_ && stop_flag_->load(std::memory_order_acquire);
    }
};

/**
 * @brief A simple fixed-size thread pool for executing tasks concurrently.
 *
 * @details This pool uses std::thread internally. It handles joining on
 * destruction and supports cooperative cancellation via a custom stop_token.
 * Tasks enqueued should accept a `stop_token` as their argument. A running
 * task can split its own work with parallel_for(): workers with no task waiting
 * pick up those iterations, since separate tasks keep the workers busier than
 * the parts of one.
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
     * The task must be a callable that accepts a `stop_token`.
     *
     * @tparam F The type of the callable task.
     * @param f The task to execute.
     * @return A std::future representing the eventual result of the task.
     * @throws std::runtime_error if enqueue is called on a stopped pool.
     */
    template<class F>
    auto enqueue(F&& f) -> std::future<std::invoke_result_t<F, stop_token>> {
        using return_type = std::invoke_result_t<F, stop_token>;
        auto task = std::make_shared<std::packaged_task<return_type(stop_token)>>(
            std::forward<F>(f)
        );
        {
            std::unique_lock lock(queue_mutex_);
            if (stop_flag_.load(std::memory_order_acquire)) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            ++pending_;
            tasks_.emplace([task](stop_token st) { (*task)(st); });
        }
        condition_.notify_one();
        return task->get_future();
    }

    /**
     * @brief Blocks the calling thread until all pending tasks are complete.
     */
    void wait_idle();

    /**
     * @brief Runs @p body(i) for every i in [0, n) and returns once all of them are done.
     *
     * @details Called from one of this pool's tasks, the iterations are shared with idle workers, and the
     * caller runs iterations itself rather than block while any are left; it can be nested. Called from
     * anywhere else, the iterations run in order on the calling thread. The first exception an iteration
     * throws is rethrown after all of them are done.
     */
    void parallel_for(std::size_t n, const std::function<void(std::size_t)>& body);

    /**
     * @brief The pool whose worker is running the calling thread, or nullptr.
     */
    [[nodiscard]] static ThreadPool* current() noexcept;

    /**
     * @brief Number of worker threads.
     */
    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

    /**
     * @brief Requests all worker threads to stop and clears the task queue.
     *
     * Pending tasks that have not started will be discarded.
     * Tasks currently running will be notified via their stop_token.
     */
    void request_stop();

private:
    /**
     * @brief The iterations of one parallel_for() call, owned by its caller's stack.
     */
    struct LoopGroup {
        const std::function<void(std::size_t)>* body = nullptr;
        std::size_t n = 0;
        std::atomic<std::size_t> next{0}; ///< Next iteration to claim
        std::size_t done = 0;             ///< Finished iterations, guarded by queue_mutex_
        std::exception_ptr error;         ///< First exception thrown, guarded by queue_mutex_
    };

    /**
     * @brief Runs one unclaimed iteration of the newest open loop, if there is one.
     * @param lock A lock on queue_mutex_, released while the iteration runs.
     * @return true if an iteration ran.
     */
    bool help_one(std::unique_lock<std::mutex>& lock);

    std::mutex queue_mutex_;                ///< Protects tasks_, pending_ and groups_
    std::condition_variable condition_;     ///< Notifies workers of new tasks or stop requests
    std::condition_variable idle_cv_;       ///< Notifies wait_idle() when pending_ is zero
    std::queue<std::function<void(stop_token)>> tasks_; ///< The queue of tasks
    std::vector<std::thread> workers_;      ///< The worker threads
    std::size_t pending_{0};                     ///< Number of tasks enqueued or running
    std::atomic<bool> stop_flag_{false};    ///< Flag to signal workers to stop
    std::vector<LoopGroup*> groups_;        ///< Open parallel_for() loops, newest last
    std::condition_variable group_cv_;      ///< Notifies parallel_for() callers of finished or new iterations
};

} // namespace chisel

#endif // CHISEL_THREAD_POOL_HPP