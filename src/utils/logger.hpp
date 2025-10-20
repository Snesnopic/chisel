//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_LOGGER_HPP
#define CHISEL_LOGGER_HPP

#include "log_sink.hpp"
#include <memory>
#include <mutex>

class Logger {
public:
    static void set_sink(std::unique_ptr<ILogSink> sink);
    static void log(LogLevel level,
                    std::string_view msg,
                    std::string_view tag = "chisel");

private:
    static std::unique_ptr<ILogSink> sink_;
    static std::mutex mtx_;
};


#endif //CHISEL_LOGGER_HPP