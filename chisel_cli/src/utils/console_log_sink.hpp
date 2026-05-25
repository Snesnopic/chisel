//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_CONSOLE_LOG_SINK_HPP
#define CHISEL_CONSOLE_LOG_SINK_HPP

#include "../../libchisel/include/log_sink.hpp"
#include <iostream>

class ConsoleLogSink final : public chisel::ILogSink {
public:
    chisel::LogLevel log_level = chisel::LogLevel::Error;

    void log(const chisel::LogLevel level,
             const std::string_view message,
             const std::string_view tag) override {

        // drop messages below threshold or if completely disabled
        if (level < log_level || log_level == chisel::LogLevel::Off || level == chisel::LogLevel::Off) {
            return;
        }

        std::cerr << "\r\033[K";

        switch (level) {
            case chisel::LogLevel::Debug:
                std::cerr << "[DEBUG][" << tag << "] " << message << std::endl;
                break;
            case chisel::LogLevel::Info:
                std::cerr << "[INFO ][" << tag << "] " << message << std::endl;
                break;
            case chisel::LogLevel::Warning:
                std::cerr << "[WARN ][" << tag << "] " << message << std::endl;
                break;
            case chisel::LogLevel::Error:
                std::cerr << "[ERROR][" << tag << "] " << message << std::endl;
                break;
            default:
                break;
        }
    }
};

#endif // CHISEL_CONSOLE_LOG_SINK_HPP