//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_LOGGER_HPP
#define CHISEL_LOGGER_HPP

#include "log_sink.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Static logging facade for chisel.
 *
 * Logger provides a global entry point for logging. It delegates
 * to a configurable ILogSink implementation. Thread-safe.
 */
class Logger {
public:
    /**
     * @brief Add a new log sink to the logger.
     * @param sink Unique pointer to a sink implementation.
     */
    static void add_sink(std::unique_ptr<ILogSink> sink);

    /**
     * @brief Remove all configured sinks.
     */
    static void clear_sinks();

    /**
     * @brief Log a message.
     * @param level Severity level.
     * @param msg Message text.
     * @param tag Optional tag (default: "chisel").
     */
    static void log(LogLevel level,
                    std::string_view msg,
                    std::string_view tag = "chisel");

    static const char* level_to_string(const LogLevel level) {
        switch (level) {
            case LogLevel::Debug:   return "DEBUG";
            case LogLevel::Info:    return "INFO";
            case LogLevel::Warning: return "WARN";
            case LogLevel::Error:   return "ERROR";
        }
        return "";
    }

    static LogLevel string_to_level(const std::string& level) {
        if (level == "DEBUG")
            return LogLevel::Debug;
        if (level == "INFO")
            return LogLevel::Info;
        if (level == "WARNING")
            return LogLevel::Warning;
        return LogLevel::Error;
    }
private:
    static std::vector<std::unique_ptr<ILogSink>> sinks_;
    static std::mutex mtx_;                               ///< Protects sink access
};

#endif //CHISEL_LOGGER_HPP