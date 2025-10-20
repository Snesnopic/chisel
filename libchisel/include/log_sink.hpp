//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_LOG_SINK_HPP
#define CHISEL_LOG_SINK_HPP

#include <string_view>

/**
 * @brief Severity levels for log messages.
 *
 * These levels indicate the importance and type of a log entry.
 * They can be used by sinks to filter or format output accordingly.
 */
enum class LogLevel {
    Debug,   ///< Detailed diagnostic information, useful for developers
    Info,    ///< General informational messages about normal operation
    Warning, ///< Indications of potential issues or unexpected states
    Error    ///< Errors that require attention or intervention
};

/**
 * @brief Abstract sink interface for logging.
 *
 * Implementations of ILogSink define how log messages are delivered
 * (e.g. console, file, syslog). The Logger class delegates to the
 * currently installed sink.
 */
struct ILogSink {
    virtual ~ILogSink() = default;

    /**
     * @brief Log a message.
     * @param level Severity level of the message.
     * @param message The message text.
     * @param tag Optional tag identifying the source component.
     */
    virtual void log(LogLevel level,
                     std::string_view message,
                     std::string_view tag) = 0;
};

#endif // CHISEL_LOG_SINK_HPP