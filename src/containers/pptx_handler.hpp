//
// Created by Giuseppe Francione on 05/10/25.
//

#ifndef MONOLITH_PPTX_HANDLER_HPP
#define MONOLITH_PPTX_HANDLER_HPP

#include "container.hpp"

class PptxHandler final : public IContainer {
public:
    ContainerJob prepare(const std::string &path) override;

    bool finalize(const ContainerJob &job, Settings &settings) override;
};

#endif //MONOLITH_PPTX_HANDLER_HPP
