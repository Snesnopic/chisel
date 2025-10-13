//
// Created by Giuseppe Francione on 21/09/25.
//

#ifndef MONOLITH_CONTAINER_HPP
#define MONOLITH_CONTAINER_HPP
#include <string>
#include  "../cli/cli_parser.hpp"
#include "../utils/archive_formats.hpp"

struct ContainerJob {
    std::filesystem::path original_path;
    std::filesystem::path temp_dir;
    std::vector<std::filesystem::path> file_list;
    ContainerFormat format = ContainerFormat::Unknown;
    std::vector<ContainerJob> children;
};
class IContainer {
public:
    virtual ~IContainer() = default;

    // read container and extract files; put file names in ContainerJob.children
    virtual ContainerJob prepare(const std::string& path) = 0;

    // write container with re-encoded files in settings.children
    virtual bool finalize(const ContainerJob &job, Settings& settings) = 0;

};

#endif //MONOLITH_CONTAINER_HPP