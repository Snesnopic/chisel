//
// Created by Giuseppe Francione on 30/09/25.
//

#ifndef CHISEL_TIFF_ENCODER_HPP
#define CHISEL_TIFF_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

// tiff encoder using libtiff
class TiffEncoder final : public IEncoder {
public:
    explicit TiffEncoder(bool preserve_metadata = false);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/tiff"; }

    [[nodiscard]] std::string name() const override { return "TiffEncoder"; }

};

#endif // CHISEL_TIFF_ENCODER_HPP