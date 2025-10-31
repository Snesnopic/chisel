//
// Created by Giuseppe Francione on 20/10/25.
//

#include "../../include/logger.hpp"
#include <vector>

std::vector<std::unique_ptr<ILogSink>> Logger::sinks_;
std::mutex Logger::mtx_;

void Logger::add_sink(std::unique_ptr<ILogSink> sink) {
    std::lock_guard lock(mtx_);
    if (sink) {
        sinks_.push_back(std::move(sink));
    }
}

void Logger::clear_sinks() {
    std::lock_guard lock(mtx_);
    sinks_.clear();
}

void Logger::log(const LogLevel level,
                  const std::string_view msg,
                  const std::string_view tag) {
    std::lock_guard lock(mtx_);
    for (const auto& sink : sinks_) { // Modificato
        if (sink) {
            sink->log(level, msg, tag);
        }
    }
}