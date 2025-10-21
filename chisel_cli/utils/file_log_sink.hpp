//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_FILE_LOG_SINK_HPP
#define CHISEL_FILE_LOG_SINK_HPP

#include "../../libchisel/include/log_sink.hpp"
#include "../../libchisel/include/logger.hpp"
#include <fstream>
#include <mutex>
#include <string>

class FileLogSink final : public ILogSink {
public:
    explicit FileLogSink(const std::string& filename, const bool append = true)
        : out_(filename, append ? std::ios::app : std::ios::trunc) {}

    void log(const LogLevel level,
             const std::string_view message,
             const std::string_view tag) override {
        if (!out_.is_open()) return;

        std::lock_guard lock(mtx_);
        out_ << "[" << Logger::level_to_string(level) << "]";
        if (!tag.empty()) out_ << "[" << tag << "]";
        out_ << " " << message << "\n";
        out_.flush();
    }

private:
    std::ofstream out_;
    std::mutex mtx_;

};

#endif // CHISEL_FILE_LOG_SINK_HPP