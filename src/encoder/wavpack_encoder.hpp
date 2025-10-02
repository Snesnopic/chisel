//
// Created by Giuseppe Francione on 25/09/25.
//

#ifndef MONOLITH_WAVPACK_ENCODER_HPP
#define MONOLITH_WAVPACK_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

// wavpack forward declaration
struct WavpackContext;

class WavpackEncoder final : public IEncoder {
public:
    explicit WavpackEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    [[nodiscard]] std::string mime_type() const override { return "audio/x-wavpack"; }

    [[nodiscard]] std::string name() const override { return "WavpackEncoder"; }
};

#endif //MONOLITH_WAVPACK_ENCODER_HPP