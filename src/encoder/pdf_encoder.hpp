//
// Created by Giuseppe Francione on 28/09/25.
//

#ifndef MONOLITH_PDF_ENCODER_HPP
#define MONOLITH_PDF_ENCODER_HPP

#include <filesystem>
#include "encoder.hpp"

class PdfEncoder final : public IEncoder{
public:
    explicit PdfEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path& input,
                           const std::filesystem::path& output) override;

    [[nodiscard]] std::string mime_type() const override { return "application/pdf"; }

    [[nodiscard]] std::string name() const override { return "PdfEncoder"; }

};

#endif //MONOLITH_PDF_ENCODER_HPP