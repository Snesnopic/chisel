//
// Created by Giuseppe Francione on 01/10/25.
//

#ifndef MONOLITH_SQLITE_ENCODER_HPP
#define MONOLITH_SQLITE_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>
#include <string>

class SqliteEncoder : public IEncoder {
public:
    explicit SqliteEncoder(bool preserve_metadata = true);

    // re-encodes input to output; returns true if successful, false otherwise
    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    // returns supported mime
    std::string mime_type() const override { return "application/x-sqlite3"; }
};

#endif //MONOLITH_SQLITE_ENCODER_HPP