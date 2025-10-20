//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_CONSOLE_LOG_SINK_HPP
#define CHISEL_CONSOLE_LOG_SINK_HPP

#include "../../libchisel/include/log_sink.hpp"
#include <iostream>

class ConsoleLogSink final : public ILogSink {
public:
    void log(const LogLevel level,
             const std::string_view message,
             const std::string_view tag) override {
        switch (level) {
            case LogLevel::Debug:
                //std::cout << "[DEBUG][" << tag << "] " << message << std::endl;
                break;
            case LogLevel::Info:
                //std::cout << "[INFO ][" << tag << "] " << message << std::endl;
                break;
            case LogLevel::Warning:
                //std::cerr << "[WARN ][" << tag << "] " << message << std::endl;
                break;
            case LogLevel::Error:
                std::cerr << "[ERROR][" << tag << "] " << message << std::endl;
                break;
        }
    }
};

#endif // CHISEL_CONSOLE_LOG_SINK_HPP