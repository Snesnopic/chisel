//
// Created by Giuseppe Francione on 30/09/25.
//

#ifndef MONOLITH_TIFF_ENCODER_HPP
#define MONOLITH_TIFF_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

// tiff encoder using libtiff
class TiffEncoder : public IEncoder {
public:
    explicit TiffEncoder(bool preserve_metadata = false);

    bool recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output) override;

    std::string mime_type() const override { return "image/tiff"; }

};

#endif // MONOLITH_TIFF_ENCODER_HPP