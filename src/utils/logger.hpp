//
// Created by Giuseppe Francione on 19/09/25.
//

#ifndef MONOLITH_LOGGER_HPP
#define MONOLITH_LOGGER_HPP

#include <string>
#include <mutex>
#include <iostream>

enum class LogLevel { DEBUG, INFO, WARNING, ERROR, NONE };

class Logger {
public:
    static void set_level(LogLevel level);

    static void enable(bool enabled);

    static void log(LogLevel level, const std::string &msg, const std::string &source = "");

private:
    Logger() = default;

    static Logger &instance();

    static const char *level_to_string(LogLevel level);

    LogLevel level_ = LogLevel::ERROR;
    bool enabled_ = true;
    std::mutex mutex_;
};

#endif // MONOLITH_LOGGER_HPP
