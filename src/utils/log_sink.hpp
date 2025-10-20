//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_LOG_SINK_HPP
#define CHISEL_LOG_SINK_HPP

#include <string_view>

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

struct ILogSink {
    virtual ~ILogSink() = default;
    virtual void log(LogLevel level,
                     std::string_view message,
                     std::string_view tag) = 0;
};


#endif //CHISEL_LOG_SINK_HPP