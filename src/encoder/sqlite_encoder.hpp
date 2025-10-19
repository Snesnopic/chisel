//
// Created by Giuseppe Francione on 01/10/25.
//

#ifndef CHISEL_SQLITE_ENCODER_HPP
#define CHISEL_SQLITE_ENCODER_HPP

#include "encoder.hpp"
#include <filesystem>
#include <string>

class SqliteEncoder final : public IEncoder {
public:
    explicit SqliteEncoder(bool preserve_metadata = true);

    // re-encodes input to output; returns true if successful, false otherwise
    bool recompress(const std::filesystem::path &input,
                    const std::filesystem::path &output) override;

    // returns supported mime
    [[nodiscard]] std::string mime_type() const override { return "application/x-sqlite3"; }

    [[nodiscard]] std::string name() const override { return "SqliteEncoder"; }

};

#endif //CHISEL_SQLITE_ENCODER_HPP