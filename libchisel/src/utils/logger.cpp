//
// Created by Giuseppe Francione on 20/10/25.
//

#include "../../include/logger.hpp"

std::unique_ptr<ILogSink> Logger::sink_ = nullptr;
std::mutex Logger::mtx_;

void Logger::set_sink(std::unique_ptr<ILogSink> sink) {
    std::lock_guard lock(mtx_);
    sink_ = std::move(sink);
}

void Logger::log(const LogLevel level,
                  const std::string_view msg,
                  const std::string_view tag) {
    std::lock_guard lock(mtx_);
    if (sink_) {
        sink_->log(level, msg, tag);
    }
}