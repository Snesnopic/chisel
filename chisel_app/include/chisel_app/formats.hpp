#ifndef CHISEL_APP_FORMATS_HPP
#define CHISEL_APP_FORMATS_HPP

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace chisel_app {

    // ChiselSwift's FileCategory, which groups the supported formats
    enum class FileKind { Archive, Audio, Database, Document, Font, Image, Scientific, Video, Unknown };

    FileKind file_kind(std::string extension); // with or without the dot, any case
    std::string_view kind_name(FileKind kind);

    // extensions by kind, each list sorted; kinds in alphabetical order, Unknown last
    std::map<FileKind, std::vector<std::string>> group_by_kind(const std::vector<std::string>& extensions);

} // namespace chisel_app

#endif // CHISEL_APP_FORMATS_HPP
