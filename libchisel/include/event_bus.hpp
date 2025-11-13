//
// Created by Giuseppe Francione on 20/10/25.
//

/**
 * @file event_bus.hpp
 * @brief Defines a simple, thread-safe publish/subscribe event bus.
 */

#ifndef CHISEL_EVENT_BUS_HPP
#define CHISEL_EVENT_BUS_HPP

#include <functional>
#include <unordered_map>
#include <typeindex>
#include <vector>
#include <mutex>
#include <memory>

namespace chisel {

    /**
     * @brief Simple type-safe publish/subscribe event bus.
     *
     * @details EventBus allows decoupled communication between components.
     * Producers (e.g. ProcessorExecutor) can broadcast events without
     * knowing who is listening. Consumers (e.g. CLI, report generator)
     * can subscribe to specific event types to receive notifications.
     *
     * This class is thread-safe. Subscriptions and publications
     * are protected by a mutex.
     */
    class EventBus {
    public:
        EventBus() = default;

        /**
         * @brief Subscribe a handler to a specific event type.
         * @tparam Event The event struct type (e.g., FileProcessCompleteEvent).
         * @param handler Function to invoke when an event of this type is published.
         * The handler will receive a const reference to the event.
         */
        template <typename Event>
        void subscribe(std::function<void(const Event&)> handler) {
            std::lock_guard lock(mtx_);
            auto& vec = subscribers_[std::type_index(typeid(Event))];
            vec.push_back([handler](const void* e) {
                handler(*static_cast<const Event*>(e));
            });
        }

        /**
         * @brief Publish an event to all subscribers of its type.
         * @tparam Event The event struct type.
         * @param event The event instance to publish.
         */
        template <typename Event>
        void publish(const Event& event) {
            std::lock_guard lock(mtx_);
            auto it = subscribers_.find(std::type_index(typeid(Event)));
            if (it != subscribers_.end()) {
                for (auto& fn : it->second) {
                    fn(&event);
                }
            }
        }

    private:
        ///< Type alias for the internal type-erased callback.
        using Callback = std::function<void(const void*)>;
        ///< Map of event type_index to a vector of callbacks.
        std::unordered_map<std::type_index, std::vector<Callback>> subscribers_;
        ///< Protects subscriber map during read/write.
        std::mutex mtx_;
    };

} // namespace chisel

#endif // CHISEL_EVENT_BUS_HPP