//
// Created by Giuseppe Francione on 25/09/25.
//

#ifndef MONOLITH_WEBP_ENCODER_HPP
#define MONOLITH_WEBP_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class WebpEncoder final : public IEncoder {
public:
    explicit WebpEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/webp"; }

    [[nodiscard]] std::string name() const override { return "WebpEncoder"; }

};

#endif //MONOLITH_WEBP_ENCODER_HPP