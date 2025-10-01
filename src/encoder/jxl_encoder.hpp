//
// Created by Giuseppe Francione on 30/09/25.
//

#ifndef MONOLITH_JXL_ENCODER_HPP
#define MONOLITH_JXL_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class JXLEncoder: public IEncoder{
public:
    explicit JXLEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                           const std::filesystem::path& output) override;

    std::string mime_type() const override { return "image/jxl"; }

    [[nodiscard]] std::string name() const override { return "JXLEncoder"; }


};

#endif //MONOLITH_JXL_ENCODER_HPP