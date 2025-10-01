//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_FLAC_ENCODER_HPP
#define MONOLITH_FLAC_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class FlacEncoder : public IEncoder {
public:
    explicit FlacEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    std::string mime_type() const override { return "audio/flac"; }

    [[nodiscard]] std::string name() const override { return "FlacEncoder"; }
};

#endif //MONOLITH_FLAC_ENCODER_HPP
