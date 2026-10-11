//
// Created by Giuseppe Francione on 10/10/26.
//

#ifndef CHISEL_TRACE_RECORDER_HPP
#define CHISEL_TRACE_RECORDER_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace chisel { class EventBus; }

/**
 * @brief Records when each analysis, recompression and finalization task ran and on which thread, and writes
 *        it as a Chrome trace (viewable in Perfetto or chrome://tracing).
 */
class TraceRecorder {
public:
    /** @brief Records the tasks announced on @p bus from now on; the bus must call its handlers on the task's thread. */
    void subscribe(chisel::EventBus& bus);

    /** @brief Writes the finished tasks to @p path, false if it can't be written. */
    [[nodiscard]] bool write(const std::filesystem::path& path) const;

private:
    struct Span {
        std::string phase;
        std::filesystem::path path;
        unsigned thread = 0;
        std::int64_t start_us = 0;
        std::int64_t duration_us = 0;
        std::uintmax_t size = 0;
        std::optional<std::uintmax_t> new_size;
        std::optional<std::filesystem::path> parent;
        std::string outcome;
    };

    void begin(const char* phase, const std::filesystem::path& path,
               std::optional<std::filesystem::path> parent = std::nullopt);
    void end(const char* phase, const std::filesystem::path& path, std::string outcome,
             std::optional<std::uintmax_t> new_size = std::nullopt,
             std::optional<std::filesystem::path> parent = std::nullopt);
    [[nodiscard]] std::int64_t now_us() const;

    const std::chrono::steady_clock::time_point origin_ = std::chrono::steady_clock::now();
    mutable std::mutex mtx_;
    std::map<std::thread::id, unsigned> threads_;
    std::map<std::pair<std::string, std::filesystem::path>, Span> open_;
    std::vector<Span> spans_;
};

#endif //CHISEL_TRACE_RECORDER_HPP
