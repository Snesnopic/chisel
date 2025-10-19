//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef CHISEL_FLAC_ENCODER_HPP
#define CHISEL_FLAC_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class FlacEncoder final : public IEncoder {
public:
    explicit FlacEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "audio/flac"; }

    [[nodiscard]] std::string name() const override { return "FlacEncoder"; }
};

#endif //CHISEL_FLAC_ENCODER_HPP
