//
// Created by Giuseppe Francione on 11/10/25.
//

#ifndef CHISEL_MIME_DETECTOR_HPP
#define CHISEL_MIME_DETECTOR_HPP
#include <filesystem>

class MimeDetector {
public:
    static std::string detect(const std::filesystem::path& path);
    static bool is_mpeg1_layer3(const std::filesystem::path& path);
    static void ensure_magic_installed();
    static std::filesystem::path get_magic_file_path();
};

#endif //CHISEL_MIME_DETECTOR_HPP