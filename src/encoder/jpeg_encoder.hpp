//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_JPEG_ENCODER_HPP
#define MONOLITH_JPEG_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class JpegEncoder : public IEncoder {
public:
    explicit JpegEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    std::string mime_type() const override { return "image/jpeg"; }

    [[nodiscard]] std::string name() const override { return "JpegEncoder"; }

};
#endif //MONOLITH_JPEG_ENCODER_HPP
