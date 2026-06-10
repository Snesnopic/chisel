//
// Created by Giuseppe Francione on 11/10/25.
//

#include "../../include/mime_detector.hpp"
#include "../../include/logger.hpp"
#include <qadmimes.hpp>
#include <filesystem>
#include <string>

namespace chisel {

std::string MimeDetector::detect(const std::filesystem::path& path)
{
    try {
        return std::string(qadmimes::MimeDetector::detect(path));
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Detection error: " + std::string(e.what()), "MimeDetector");
        return "application/octet-stream";
    }
}

} // namespace chisel