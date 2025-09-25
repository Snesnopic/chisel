//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_MKV_HANDLER_HPP
#define MONOLITH_MKV_HANDLER_HPP

#include "../containers/container.hpp"
#include "../utils/logger.hpp"
#include <string>

class MKVHandler : public IContainer {
public:
    MKVHandler() = default;
    ~MKVHandler() override = default;

    // IContainer
    ContainerJob prepare(const std::string& path) override;
    bool finalize(const ContainerJob &job, Settings& settings) override;
};

#endif // MONOLITH_MKV_HANDLER_HPP
