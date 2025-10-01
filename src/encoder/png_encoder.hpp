//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_PNG_ENCODER_HPP
#define MONOLITH_PNG_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class PngEncoder : public IEncoder {
public:
    explicit PngEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    std::string mime_type() const override { return "image/png"; }

    [[nodiscard]] std::string name() const override { return "PngEncoder"; }

};

#endif //MONOLITH_PNG_ENCODER_HPP
