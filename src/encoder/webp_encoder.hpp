//
// Created by Giuseppe Francione on 25/09/25.
//

#ifndef MONOLITH_WEBP_ENCODER_HPP
#define MONOLITH_WEBP_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class WebpEncoder: public IEncoder {
public:
    explicit WebpEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    std::string mime_type() const override { return "image/webp"; }
};

#endif //MONOLITH_WEBP_ENCODER_HPP