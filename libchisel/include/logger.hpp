//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_LOGGER_HPP
#define CHISEL_LOGGER_HPP

#include "log_sink.hpp"
#include <memory>
#include <mutex>

/**
 * @brief Static logging facade for chisel.
 *
 * Logger provides a global entry point for logging. It delegates
 * to a configurable ILogSink implementation. Thread-safe.
 */
class Logger {
public:
    /**
     * @brief Set the active log sink.
     * @param sink Unique pointer to a sink implementation.
     */
    static void set_sink(std::unique_ptr<ILogSink> sink);

    /**
     * @brief Log a message.
     * @param level Severity level.
     * @param msg Message text.
     * @param tag Optional tag (default: "chisel").
     */
    static void log(LogLevel level,
                    std::string_view msg,
                    std::string_view tag = "chisel");

private:
    static std::unique_ptr<ILogSink> sink_; ///< Active sink
    static std::mutex mtx_;                 ///< Protects sink access
};

#endif //CHISEL_LOGGER_HPP