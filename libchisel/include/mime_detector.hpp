//
// Created by Giuseppe Francione on 11/10/25.
//

/**
 * @file mime_detector.hpp
 * @brief Utility definitions for MIME_DETECTOR.
 */

#ifndef CHISEL_MIME_DETECTOR_HPP
#define CHISEL_MIME_DETECTOR_HPP

#include <filesystem>
#include <string>

namespace chisel {

    /**
     * @brief Provides cross-platform file type detection.
     *
     * This class abstracts the underlying mechanism for detecting MIME types.
     */
    class MimeDetector {
    public:
        /**
         * @brief Detect the MIME type of a file.
         *
         * @param path The filesystem path to the file.
         * @return A string representing the MIME type (e.g., "image/jpeg").
         */
        static std::string detect(const std::filesystem::path& path);
    };

} // namespace chisel
#endif //CHISEL_MIME_DETECTOR_HPP
