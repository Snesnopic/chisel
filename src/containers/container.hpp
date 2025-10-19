//
// Created by Giuseppe Francione on 21/09/25.
//

#ifndef CHISEL_CONTAINER_HPP
#define CHISEL_CONTAINER_HPP

#include  "../cli/cli_parser.hpp"
#include "../utils/file_type.hpp"

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
    virtual ContainerJob prepare(const std::filesystem::path& path) = 0;

    // write container with re-encoded files in settings.children
    virtual bool finalize(const ContainerJob &job, Settings& settings) = 0;

};

#endif //CHISEL_CONTAINER_HPP