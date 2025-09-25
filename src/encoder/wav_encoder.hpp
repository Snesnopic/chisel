//
// Created by Giuseppe Francione on 25/09/25.
//

#ifndef MONOLITH_WAV_ENCODER_HPP
#define MONOLITH_WAV_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

// wavpack forward declaration
struct WavpackContext;

class WavEncoder: public IEncoder {
public:
    explicit WavEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    std::string mime_type() const override { return "audio/wav"; }

};

#endif //MONOLITH_WAV_ENCODER_HPP