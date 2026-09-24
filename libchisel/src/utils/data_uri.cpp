//
// Created by Giuseppe Francione on 24/09/26.
//

#include "../../include/data_uri.hpp"
#include "../../include/base64_utils.hpp"
#include "../../include/file_utils.hpp"
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace chisel {

// named, so unity builds don't merge these with other files' helpers
namespace data_uri_scan {
namespace {

constexpr auto npos = std::string_view::npos;

constexpr bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

constexpr bool is_alnum(const char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

constexpr bool is_base64(const char c) {
    return is_alnum(c) || c == '+' || c == '/' || c == '=';
}

// characters of media types and their parameters
constexpr bool is_token(const char c) {
    return is_alnum(c) || c == '!' || c == '#' || c == '$' || c == '&' || c == '^' || c == '_' ||
           c == '.' || c == '+' || c == '-' || c == '=';
}

/// @brief A base64 data URI: its media type, and where its payload lies in the text.
struct DataUri {
    std::string_view media_type;
    std::size_t offset = 0;
    std::size_t length = 0;
};

std::vector<DataUri> find_data_uris(const std::string_view text) {
    std::vector<DataUri> found;
    for (auto pos = text.find("data:"); pos != npos; pos = text.find("data:", pos + 1)) {
        // quoted, or the argument of url(
        const char opener = pos > 0 ? text[pos - 1] : '\0';
        if (opener != '"' && opener != '\'' && opener != '(') continue;

        auto header_end = pos + 5;
        while (header_end < text.size() && (is_token(text[header_end]) || text[header_end] == '/' || text[header_end] == ';')) {
            ++header_end;
        }
        if (header_end >= text.size() || text[header_end] != ',') continue;
        const auto header = text.substr(pos + 5, header_end - pos - 5);
        const auto media_type = header.substr(0, header.find(';'));
        const auto slash = media_type.find('/');
        if (!header.ends_with(";base64") || slash == npos || slash == 0 || slash + 1 == media_type.size()) continue;

        auto start = header_end + 1;
        const bool quoted = opener != '(';
        auto end = start;
        while (end < text.size() && (is_base64(text[end]) || (quoted && is_space(text[end])))) ++end;
        auto closer = end;
        while (!quoted && closer < text.size() && is_space(text[closer])) ++closer;
        if (closer >= text.size() || text[closer] != (quoted ? opener : ')')) continue;
        while (start < end && is_space(text[start])) ++start;
        while (end > start && is_space(text[end - 1])) --end;
        if (end == start) continue;

        found.push_back({.media_type = media_type, .offset = start, .length = end - start});
        pos = closer;
    }
    return found;
}

std::string without_spaces(const std::string_view payload) {
    std::string compact;
    compact.reserve(payload.size());
    std::ranges::copy_if(payload, std::back_inserter(compact), [](const char c) { return !is_space(c); });
    return compact;
}

// decodes base64 only when encoding the result gives back the same text
std::optional<std::vector<uint8_t>> decode_canonical(const std::string& compact) {
    if (compact.size() % 4 != 0) return std::nullopt;
    auto bytes = Base64Utils::decode(compact);
    if (bytes.empty() || Base64Utils::encode(bytes) != compact) return std::nullopt;
    return bytes;
}

// extension of the extracted file, or empty for media types chisel has no use for
std::string extension_for(const std::string_view media_type) {
    const auto slash = media_type.find('/');
    const auto type = media_type.substr(0, slash);
    auto subtype = media_type.substr(slash + 1);
    const bool font = type == "font" || (type == "application" && subtype.find("font") != npos);
    if (type != "image" && !font) return {};

    for (const std::string_view prefix : {"x-", "font-"}) {
        if (subtype.starts_with(prefix)) subtype.remove_prefix(prefix.size());
    }
    subtype = subtype.substr(0, subtype.find('+'));
    std::string extension = ".";
    std::ranges::copy_if(subtype, std::back_inserter(extension), is_alnum);
    return extension.size() > 1 ? extension : ".bin";
}

} // namespace
} // namespace data_uri_scan

std::vector<DataUriAsset> extract_data_uris(const std::string_view text,
                                            const std::function<std::filesystem::path()>& make_dir) {
    using namespace data_uri_scan;
    std::vector<DataUriAsset> assets;
    std::filesystem::path dir;
    for (const auto& uri : find_data_uris(text)) {
        const auto extension = extension_for(uri.media_type);
        if (extension.empty()) continue;
        const auto compact = without_spaces(text.substr(uri.offset, uri.length));
        const auto hash = std::hash<std::string>{}(compact);
        if (std::ranges::any_of(assets, [&](const DataUriAsset& a) {
                return a.payload_hash == hash && a.payload_length == compact.size();
            })) {
            continue;
        }
        const auto bytes = decode_canonical(compact);
        if (!bytes) continue;

        if (dir.empty()) {
            dir = make_dir();
            if (dir.empty()) return assets;
        }
        const auto file = dir / ("asset_" + std::to_string(assets.size()) + extension);
        if (!write_file(file, *bytes)) continue;
        assets.push_back({.payload_hash = hash, .payload_length = compact.size(), .decoded_size = bytes->size(), .file = file});
    }
    return assets;
}

std::optional<std::string> reinsert_data_uris(const std::string_view text, const std::vector<DataUriAsset>& assets) {
    using namespace data_uri_scan;
    // base64 of each asset that got smaller, with the length of the text it replaces
    std::unordered_map<std::size_t, std::pair<std::size_t, std::string>> smaller;
    for (const auto& asset : assets) {
        std::vector<uint8_t> bytes;
        if (read_file(asset.file, bytes) && !bytes.empty() && bytes.size() < asset.decoded_size) {
            smaller.emplace(asset.payload_hash, std::pair{asset.payload_length, Base64Utils::encode(bytes)});
        }
    }
    if (smaller.empty()) return std::nullopt;

    std::string rebuilt;
    std::size_t copied = 0;
    for (const auto& uri : find_data_uris(text)) {
        const auto compact = without_spaces(text.substr(uri.offset, uri.length));
        const auto it = smaller.find(std::hash<std::string>{}(compact));
        if (it == smaller.end() || it->second.first != compact.size()) continue;
        rebuilt.append(text.substr(copied, uri.offset - copied));
        rebuilt.append(it->second.second);
        copied = uri.offset + uri.length;
    }
    if (copied == 0) return std::nullopt;
    rebuilt.append(text.substr(copied));
    return rebuilt;
}

} // namespace chisel
