//
// Created by Giuseppe Francione on 07/10/25.
//

#ifndef MONOLITH_GIF_ENCODER_HPP
#define MONOLITH_GIF_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class GifEncoder final : public IEncoder {
public:
    explicit GifEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/gif"; }

    [[nodiscard]] std::string name() const override { return "GifEncoder"; }

};

#endif //MONOLITH_GIF_ENCODER_HPP