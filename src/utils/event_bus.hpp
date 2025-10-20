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

    class EventBus {
    public:
        EventBus() = default;

        // Registrazione di un listener per un certo tipo di evento
        template <typename Event>
        void subscribe(std::function<void(const Event&)> handler) {
            std::lock_guard lock(mtx_);
            auto& vec = subscribers_[std::type_index(typeid(Event))];
            vec.push_back([handler](const void* e) {
                handler(*static_cast<const Event*>(e));
            });
        }

        // Pubblicazione di un evento
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
        std::unordered_map<std::type_index, std::vector<Callback>> subscribers_;
        std::mutex mtx_;
    };

} // namespace chisel

#endif // CHISEL_EVENT_BUS_HPP