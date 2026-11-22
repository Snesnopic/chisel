#ifndef CHISEL_APP_OUTPUT_HPP
#define CHISEL_APP_OUTPUT_HPP

#include "settings.hpp"

#include <filesystem>
#include <string>

namespace chisel_app {

    // a.pdf -> a.compressed.pdf, README -> README.compressed
    std::filesystem::path side_by_side_name(const std::filesystem::path& original);
    // a.pdf -> a.original.pdf
    std::filesystem::path backup_name(const std::filesystem::path& original);
    // the path itself if free, else name-1.ext, name-2.ext... (ChiselSwift's uniqueURL)
    std::filesystem::path unique_path(const std::filesystem::path& wanted);

    // renames without ever replacing an existing file
    std::error_code rename_no_replace(const std::filesystem::path& from, const std::filesystem::path& to);

    struct PlaceResult {
        std::filesystem::path result; // where the smaller file is now
        std::filesystem::path backup; // keepOriginal: where the original went
        std::string error;            // empty on success
    };

    // puts chisel's result for original where SideBySide or KeepOriginal want it; produced is consumed either way
    PlaceResult place_result(OutputMode mode, const std::filesystem::path& produced, const std::filesystem::path& original);

    // OutputFolder: moves produced to target, under a free name if target is taken, with original's metadata
    PlaceResult place_in_folder(const std::filesystem::path& produced, const std::filesystem::path& original,
                                const std::filesystem::path& target);

    // whether a file in this folder may be created: access(W_OK) on posix; on windows only a write test tells, so true
    bool folder_writable(const std::filesystem::path& folder);

    // a new empty folder named after tag in parent, hidden on windows; empty path on failure
    std::filesystem::path make_temp_folder(const std::filesystem::path& parent, const std::string& tag);

} // namespace chisel_app

#endif // CHISEL_APP_OUTPUT_HPP
