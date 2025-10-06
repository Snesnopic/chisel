//
// Created by Giuseppe Francione on 05/10/25.
//

#ifndef MONOLITH_MSEED_ENCODER_HPP
#define MONOLITH_MSEED_ENCODER_HPP

#include "encoder.hpp"
#include <string>
#include <filesystem>

#include "libmseed.h"

// encoder for miniSEED files (v2/v3)
// keeps same version, forces STEIM2 for integer data, chooses optimal reclen
class MseedEncoder final : public IEncoder {
public:
    explicit MseedEncoder(bool preserve_metadata = true);

    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    [[nodiscard]] std::string mime_type() const override { return "application/vnd.fdsn.mseed"; }

    [[nodiscard]] std::string name() const override { return "mseed_encoder"; }

private:
    // choose record length
    static int choose_reclen(const MS3Record *msr, size_t sample_count);
};

#endif // MONOLITH_MSEED_ENCODER_HPP