//
// Created by Giuseppe Francione on 25/09/25.
//

#ifndef MONOLITH_WEBP_ENCODER_HPP
#define MONOLITH_WEBP_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

// forward declaration not needed for libwebp, we will include headers in cpp

class WebpEncoder: public IEncoder {
public:
    explicit WebpEncoder(bool preserve_metadata = true);

    // recompress a webp file into webp lossless with maximum compression
    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    // return mime type handled by this encoder
    std::string mime_type() const override { return "image/webp"; }
};

#endif //MONOLITH_WEBP_ENCODER_HPP