//
// Created by Giuseppe Francione on 11/10/25.
//

#ifndef MONOLITH_MIME_DETECTOR_HPP
#define MONOLITH_MIME_DETECTOR_HPP
#include <filesystem>

class MimeDetector {
public:
    static std::string detect(const std::filesystem::path& path);
    static bool is_mpeg1_layer3(const std::filesystem::path& path);
};

#endif //MONOLITH_MIME_DETECTOR_HPP