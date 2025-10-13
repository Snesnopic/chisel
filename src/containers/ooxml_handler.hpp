//
// Created by Giuseppe Francione on 06/10/25.
//

#ifndef MONOLITH_OOXML_HANDLER_HPP
#define MONOLITH_OOXML_HANDLER_HPP

#include "../containers/archive_handler.hpp"

class OoxmlHandler final :public IContainer {
public:
    explicit OoxmlHandler(const ContainerFormat fmt) : fmt_(fmt) {}

    ContainerJob prepare(const std::filesystem::path &path) override;

    bool finalize(const ContainerJob &job, Settings &settings) override;

private:
    ContainerFormat fmt_;
};
#endif //MONOLITH_OOXML_HANDLER_HPP