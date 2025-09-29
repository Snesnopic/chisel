//
// Created by Giuseppe Francione on 28/09/25.
//

#ifndef MONOLITH_ZOPFLIPNG_ENCODER_HPP
#define MONOLITH_ZOPFLIPNG_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class ZopfliPngEncoder : public IEncoder {
public:
    explicit ZopfliPngEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/png"; }

};

#endif //MONOLITH_ZOPFLIPNG_ENCODER_HPP