//
// Created by Giuseppe Francione on 20/10/25.
//

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
     * EventBus allows decoupled communication between producers (e.g. ProcessorExecutor)
     * and consumers (e.g. CLI, report generator). Listeners can subscribe to specific
     * event types, and publishers can broadcast events without knowing who is listening.
     *
     * Thread-safety: subscriptions and publications are protected by a mutex.
     */
    class EventBus {
    public:
        EventBus() = default;

        /**
         * @brief Subscribe a handler to a specific event type.
         * @tparam Event The event struct type.
         * @param handler Function to invoke when an event of this type is published.
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
        using Callback = std::function<void(const void*)>;
        std::unordered_map<std::type_index, std::vector<Callback>> subscribers_; ///< Map of event type to callbacks
        std::mutex mtx_; ///< Protects subscriber map
    };

} // namespace chisel

#endif // CHISEL_EVENT_BUS_HPP