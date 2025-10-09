//
// Created by Giuseppe Francione on 09/10/25.
//

#ifndef MONOLITH_APE_ENCODER_HPP
#define MONOLITH_APE_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

class ApeEncoder final : public IEncoder {
public:
    explicit ApeEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "audio/ape"; }

    [[nodiscard]] std::string name() const override { return "ApeEncoder"; }
};

#endif //MONOLITH_APE_ENCODER_HPP
