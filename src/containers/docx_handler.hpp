//
// Created by Giuseppe Francione on 04/10/25.
//

#ifndef MONOLITH_DOCX_CONTAINER_HPP
#define MONOLITH_DOCX_CONTAINER_HPP

#include "container.hpp"

class DocxHandler final : public IContainer {
public:
    ContainerJob prepare(const std::string &path) override;

    bool finalize(const ContainerJob &job, Settings &settings) override;
};
#endif //MONOLITH_DOCX_CONTAINER_HPP
