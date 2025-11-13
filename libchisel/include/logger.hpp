//
// Created by Giuseppe Francione on 20/10/25.
//

/**
 * @file logger.hpp
 * @brief Provides a static, thread-safe logging facade.
 *
 * This file defines the Logger class, which serves as the global entry
 * point for all logging within the library. It delegates log messages
 * to one or more registered ILogSink implementations.
 */

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
 * Provides a global, thread-safe entry point for logging. It delegates
 * log messages to all registered ILogSink implementations.
 */
class Logger {
public:
    /**
     * @brief Add a new log sink to the logger.
     * The Logger takes ownership of the sink.
     * This operation is thread-safe.
     * @param sink Unique pointer to a sink implementation.
     */
    static void add_sink(std::unique_ptr<ILogSink> sink);

    /**
     * @brief Remove all configured sinks.
     * This operation is thread-safe.
     */
    static void clear_sinks();

    /**
     * @brief Log a message to all registered sinks.
     * This operation is thread-safe.
     * @param level Severity level.
     * @param msg Message text.
     * @param tag Optional tag (default: "chisel").
     */
    static void log(LogLevel level,
                    std::string_view msg,
                    std::string_view tag = "chisel");

    /**
     * @brief Converts a LogLevel enum to its string representation.
     * @param level The enum value.
     * @return A constant string (e.g., "DEBUG", "INFO").
     */
    static const char* level_to_string(const LogLevel level) {
        switch (level) {
            case LogLevel::Debug:   return "DEBUG";
            case LogLevel::Info:    return "INFO";
            case LogLevel::Warning: return "WARN";
            case LogLevel::Error:   return "ERROR";
        }
        return "";
    }

    /**
     * @brief Converts a string to its LogLevel enum representation.
     * Case-sensitive. Returns LogLevel::Error if not matched.
     * @param level The string value (e.g., "DEBUG", "INFO").
     * @return The corresponding LogLevel enum.
     */
    static LogLevel string_to_level(const std::string& level) {
        if (level == "DEBUG")
            return LogLevel::Debug;
        if (level == "INFO")
            return LogLevel::Info;
        if (level == "WARNING")
            return LogLevel::Warning;
        return LogLevel::Error; // Default fallback
    }
private:
    ///< List of all registered sink implementations.
    static std::vector<std::unique_ptr<ILogSink>> sinks_;
    ///< Protects access to the sinks_ vector.
    static std::mutex mtx_;
};

#endif //CHISEL_LOGGER_HPP