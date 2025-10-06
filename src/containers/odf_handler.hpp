//
// Created by Giuseppe Francione on 06/10/25.
//

#ifndef MONOLITH_ODF_HANDLER_HPP
#define MONOLITH_ODF_HANDLER_HPP

#include <string>
#include "../containers/archive_handler.hpp"
#include "../utils/archive_formats.hpp"

class OdfHandler final: public IContainer {
public:
    explicit OdfHandler(const ContainerFormat fmt) : fmt_(fmt) {}

    [[nodiscard]] ContainerJob prepare(const std::string& path) override;
    bool finalize(const ContainerJob& job, Settings& settings) override;

    static OdfHandler from_path(const std::string& path);
private:
    ContainerFormat fmt_;

    [[nodiscard]] const char* temp_prefix() const;
    [[nodiscard]] const char* output_extension() const;
};
#endif //MONOLITH_ODF_HANDLER_HPP