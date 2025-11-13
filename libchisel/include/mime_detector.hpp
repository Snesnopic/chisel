//
// Created by Giuseppe Francione on 11/10/25.
//

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
         *
         * @note On Linux/macOS, this uses libmagic for accurate detection.
         * @note On Windows, this currently falls back to a simple
         * map of file extensions to MIME types.
         */
        static std::string detect(const std::filesystem::path& path);

        /**
         * @brief Specifically checks if a file is MPEG-1 Layer 3 (MP3).
         * * Used for formats where MIME detection alone might be ambiguous.
         * * @param path The filesystem path to the file.
         * @return true if the file is identified as MP3, false otherwise.
         */
        static bool is_mpeg1_layer3(const std::filesystem::path& path);

        /**
         * @brief Ensures the libmagic database (.mgc) is installed.
         *
         * On first run (non-Windows), this extracts the embedded magic file
         * to the user's data directory.
         */
        static void ensure_magic_installed();

        /**
         * @brief Gets the platform-specific path for the magic.mgc file.
         */
        static std::filesystem::path get_magic_file_path();
    };

} // namespace chisel
#endif //CHISEL_MIME_DETECTOR_HPP