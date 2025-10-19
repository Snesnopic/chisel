//
// Created by Giuseppe Francione on 16/10/25.
//

#ifndef CHISEL_MKV_ENCODER_HPP
#define CHISEL_MKV_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>

extern "C" {
    // yes this is stupid but this took less than a minute and it works
    int mkclean_optimize(int argc, char* argv[]);
}

class MkvEncoder final : public IEncoder {
public:
    explicit MkvEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "video/x-matroska"; }

    [[nodiscard]] std::string name() const override { return "MkvEncoder"; }
};

#endif //CHISEL_MKV_ENCODER_HPP