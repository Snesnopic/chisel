//
// Created by Giuseppe Francione on 25/09/25.
//

#ifndef MONOLITH_ALAC_ENCODER_HPP
#define MONOLITH_ALAC_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class AlacEncoder : public IEncoder {
public:
    explicit AlacEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    std::string mime_type() const override { return "audio/mp4"; }
};

#endif // MONOLITH_ALAC_ENCODER_HPP