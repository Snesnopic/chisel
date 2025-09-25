//
// Created by Giuseppe Francione on 20/09/25.
//

#include "mkv_handler.hpp"
#include "../containers/container.hpp"
#include <map>
#include <string>


ContainerJob MKVHandler::prepare(const std::string& mkv_path) {
    ContainerJob job;
    return job;
}

bool MKVHandler::finalize(const ContainerJob &job, Settings& settings) {
    return false;
}
