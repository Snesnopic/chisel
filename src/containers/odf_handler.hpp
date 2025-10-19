//
// Created by Giuseppe Francione on 06/10/25.
//

#ifndef CHISEL_ODF_HANDLER_HPP
#define CHISEL_ODF_HANDLER_HPP

#include <string>
#include "../containers/archive_handler.hpp"

class OdfHandler final: public IContainer {
public:
    explicit OdfHandler(const ContainerFormat fmt) : fmt_(fmt) {}

    [[nodiscard]] ContainerJob prepare(const std::filesystem::path& path) override;
    bool finalize(const ContainerJob& job, Settings& settings) override;

private:
    ContainerFormat fmt_;
};
#endif //CHISEL_ODF_HANDLER_HPP