//
// Created by Giuseppe Francione on 10/10/25.
//

#ifndef MONOLITH_FLEXIGIF_ENCODER_HPP
#define MONOLITH_FLEXIGIF_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class FlexiGifEncoder final : public IEncoder {
public:
    explicit FlexiGifEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/gif"; }

    [[nodiscard]] std::string name() const override { return "FlexiGifEncoder"; }

private:
    bool preserve_metadata_;
};

#endif // MONOLITH_FLEXIGIF_ENCODER_HPP