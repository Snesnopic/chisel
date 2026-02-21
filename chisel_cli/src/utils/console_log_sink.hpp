//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_CONSOLE_LOG_SINK_HPP
#define CHISEL_CONSOLE_LOG_SINK_HPP

#include "../../libchisel/include/log_sink.hpp"
#include <iostream>

class ConsoleLogSink final : public ILogSink {
public:
    LogLevel log_level = LogLevel::Error;

    void log(const LogLevel level,
             const std::string_view message,
             const std::string_view tag) override {

        // drop messages below threshold or if completely disabled
        if (level < log_level || log_level == LogLevel::Off || level == LogLevel::Off) {
            return;
        }

        switch (level) {
            case LogLevel::Debug:
                std::cerr << "[DEBUG][" << tag << "] " << message << std::endl;
                break;
            case LogLevel::Info:
                std::cerr << "[INFO ][" << tag << "] " << message << std::endl;
                break;
            case LogLevel::Warning:
                std::cerr << "[WARN ][" << tag << "] " << message << std::endl;
                break;
            case LogLevel::Error:
                std::cerr << "[ERROR][" << tag << "] " << message << std::endl;
                break;
            default:
                break;
        }
    }
};

#endif // CHISEL_CONSOLE_LOG_SINK_HPP