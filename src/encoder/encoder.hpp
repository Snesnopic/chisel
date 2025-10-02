//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_ENCODER_HPP
#define MONOLITH_ENCODER_HPP

#include <string>
#include <filesystem>

class IEncoder {
public:
    virtual ~IEncoder() = default;

    // re-encodes input to output; returns true if successful, false otherwise
    virtual bool recompress(const std::filesystem::path &input,
                            const std::filesystem::path &output) = 0;

    // returns supported MIME (es. "audio/flac")
    [[nodiscard]] virtual std::string mime_type() const = 0;

    // returns encoder name for logging purposes
    [[nodiscard]] virtual std::string name() const = 0;

    // enables/disables metadata preservation
    virtual void set_preserve_metadata(const bool preserve) { preserve_metadata_ = preserve; }
    [[nodiscard]] virtual bool preserve_metadata() const { return preserve_metadata_; }

protected:
    bool preserve_metadata_ = true;
};
#endif //MONOLITH_ENCODER_HPP
