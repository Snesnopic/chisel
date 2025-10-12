//
// Created by Giuseppe Francione on 19/09/25.
//

#include "logger.hpp"

void Logger::set_level(const LogLevel level) {
    instance().level_ = level;
}

void Logger::enable(const bool enabled) {
    instance().enabled_ = enabled;
}

void Logger::log(const LogLevel level, const std::string &msg, const std::string &source) {
    auto &inst = instance();
    if (!inst.enabled_ || level < inst.level_) return;

    std::scoped_lock lock(inst.mutex_);
    std::ostream &out = (level == LogLevel::Error) ? std::cerr : std::cout;
    out << "[" << level_to_string(level) << "]";
    if (!source.empty()) out << "[" << source << "]";
    out << " " << msg << "\n";
}

Logger &Logger::instance() {
    static Logger inst;
    return inst;
}

const char *Logger::level_to_string(const LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "";
    }
}
