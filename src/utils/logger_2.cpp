//
// Created by Giuseppe Francione on 20/10/25.
//

// logger.cpp
#include "logger_2.hpp"


std::unique_ptr<ILogSink> Logger2::sink_ = nullptr;
std::mutex Logger2::mtx_;

void Logger2::set_sink(std::unique_ptr<ILogSink> sink) {
    std::lock_guard lock(mtx_);
    sink_ = std::move(sink);
}

void Logger2::log(const LogLevel level,
                  const std::string_view msg,
                  const std::string_view tag) {
    std::lock_guard lock(mtx_);
    if (sink_) {
        sink_->log(level, msg, tag);
    }
}