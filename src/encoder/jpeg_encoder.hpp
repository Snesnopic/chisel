//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef CHISEL_JPEG_ENCODER_HPP
#define CHISEL_JPEG_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class JpegEncoder final : public IEncoder {
public:
    explicit JpegEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/jpeg"; }

    [[nodiscard]] std::string name() const override { return "JpegEncoder"; }

};
#endif //CHISEL_JPEG_ENCODER_HPP
