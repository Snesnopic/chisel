//
// Created by Giuseppe Francione on 30/09/25.
//

#ifndef CHISEL_JXL_ENCODER_HPP
#define CHISEL_JXL_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class JXLEncoder final : public IEncoder{
public:
    explicit JXLEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                           const std::filesystem::path& output) override;

    [[nodiscard]] std::string mime_type() const override { return "image/jxl"; }

    [[nodiscard]] std::string name() const override { return "JXLEncoder"; }


};

#endif //CHISEL_JXL_ENCODER_HPP