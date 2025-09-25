//
// Created by Giuseppe Francione on 19/09/25.
//

#ifndef MONOLITH_ARCHIVE_HANDLER_HPP
#define MONOLITH_ARCHIVE_HANDLER_HPP

#include <string>
#include <vector>
#include "container.hpp"
#include "../utils/archive_formats.hpp"
#include "../cli/cli_parser.hpp"

class ArchiveHandler: IContainer {
public:
    static void set_user_selected_format(const ContainerFormat fmt) {
        user_selected_format = fmt;
    }

    ContainerJob prepare(const std::string &archive_path) override;

    bool finalize(const ContainerJob &job, Settings& settings) override;

private:
    static inline auto user_selected_format = ContainerFormat::Unknown;

    static ContainerFormat detect_format(const std::string& path);
    static bool extract_with_libarchive(const std::string& archive_path, const std::string& dest_dir);
    static bool create_with_libarchive(const std::string& src_dir, const std::string& out_path, ContainerFormat fmt);
    static bool is_archive_file(const std::string& path, ContainerFormat& fmt_out);
};

#endif // MONOLITH_ARCHIVE_HANDLER_HPP