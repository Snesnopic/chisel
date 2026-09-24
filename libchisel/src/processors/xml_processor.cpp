//
// Created by Giuseppe Francione on 22/02/26.
//

/**
 * @file xml_processor.cpp
 * @brief implementation of the XML processor class.
 */

#include "../../include/xml_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_type.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/data_uri.hpp"
#include <pugixml.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chisel {

namespace {

namespace fs = std::filesystem;
constexpr auto npos = std::string_view::npos;

constexpr bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// bytes that can't be part of a tag or attribute name
constexpr bool ends_name(const char c) {
    return is_space(c) || c == '/' || c == '>' || c == '=' || c == '<' || c == '"' || c == '\'';
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(in), {}};
    if (in.bad()) return std::nullopt;
    return text;
}

bool write_bytes(const fs::path& path, const std::string_view data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    return static_cast<bool>(out);
}

// true when <, >, =, /, quotes and whitespace can't be bytes of a multibyte character
bool ascii_compatible(std::string_view xml) {
    if (xml.starts_with("\xEF\xBB\xBF")) {
        xml.remove_prefix(3);
    } else if (xml.size() >= 2 && (xml[0] == '\0' || xml[1] == '\0' ||
                                   xml.starts_with("\xFE\xFF") || xml.starts_with("\xFF\xFE"))) {
        // utf-16 and utf-32, with or without a bom
        return false;
    }
    if (!xml.starts_with("<?xml")) return true;
    const auto declaration_end = xml.find("?>");
    if (declaration_end == npos) return false;
    const auto declaration = xml.substr(0, declaration_end);
    const auto key = declaration.find("encoding");
    if (key == npos) return true;
    const auto open = declaration.find_first_of("\"'", key);
    const auto close = open == npos ? npos : declaration.find(declaration[open], open + 1);
    if (close == npos) return false;
    const auto encoding = to_lower_copy(std::string(declaration.substr(open + 1, close - open - 1)));
    static constexpr std::array<std::string_view, 20> kAsciiSupersets = {
        "utf-8", "utf8", "us-ascii", "ascii", "iso-8859-", "iso8859-", "iso_8859-", "latin",
        "windows-125", "cp125", "shift_jis", "shift-jis", "sjis", "euc-", "gb2312", "gbk",
        "gb18030", "big5", "koi8-", "macintosh"
    };
    return std::ranges::any_of(kAsciiSupersets, [&](const std::string_view prefix) {
        return encoding.starts_with(prefix);
    });
}

std::size_t skip_spaces(const std::string_view in, std::size_t pos) {
    while (pos < in.size() && is_space(in[pos])) ++pos;
    return pos;
}

// position right after a quoted literal starting at pos, or npos
std::size_t literal_end(const std::string_view in, const std::size_t pos) {
    if (pos >= in.size() || (in[pos] != '"' && in[pos] != '\'')) return npos;
    const auto close = in.find(in[pos], pos + 1);
    return close == npos ? npos : close + 1;
}

// position right after the internal subset whose '[' is at pos, or npos
std::size_t internal_subset_end(const std::string_view in, std::size_t pos) {
    for (++pos; pos < in.size();) {
        const char c = in[pos];
        if (c == '"' || c == '\'') {
            pos = literal_end(in, pos);
        } else if (in.compare(pos, 4, "<!--") == 0) {
            pos = in.find("-->", pos + 4);
            if (pos != npos) pos += 3;
        } else if (in.compare(pos, 2, "<?") == 0) {
            pos = in.find("?>", pos + 2);
            if (pos != npos) pos += 2;
        } else if (c == ']') {
            return pos + 1;
        } else if (c == '[') {
            // conditional sections only belong to external subsets
            return npos;
        } else {
            ++pos;
        }
        if (pos == npos) return npos;
    }
    return npos;
}

// position right after the xml declaration starting at pos, or npos if it doesn't follow the grammar
std::size_t declaration_end(const std::string_view in, const std::size_t pos) {
    auto cursor = pos + 5;
    for (const std::string_view key : {"version", "encoding", "standalone"}) {
        const auto next = skip_spaces(in, cursor);
        if (in.compare(next, key.size(), key) != 0) {
            if (key == "version") return npos;
            continue;
        }
        const auto equals = skip_spaces(in, next + key.size());
        if (next == cursor || equals >= in.size() || in[equals] != '=') return npos;
        const auto open = skip_spaces(in, equals + 1);
        cursor = literal_end(in, open);
        if (cursor == npos) return npos;
        const auto value = in.substr(open + 1, cursor - open - 2);
        if (value.empty() || !std::ranges::all_of(value, [](const char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_' || c == '-';
            })) {
            return npos;
        }
    }
    cursor = skip_spaces(in, cursor);
    return in.compare(cursor, 2, "?>") == 0 ? cursor + 2 : npos;
}

// position right after the processing instruction starting at pos, or npos if it is malformed or named xml
std::size_t processing_instruction_end(const std::string_view in, const std::size_t pos) {
    auto cursor = pos + 2;
    while (cursor < in.size() && !ends_name(in[cursor]) && in[cursor] != '?') ++cursor;
    const auto target = to_lower_copy(std::string(in.substr(pos + 2, cursor - pos - 2)));
    if (target.empty() || target == "xml") return npos;
    if (cursor >= in.size() || (!is_space(in[cursor]) && in.compare(cursor, 2, "?>") != 0)) return npos;
    const auto end = in.find("?>", cursor);
    return end == npos ? npos : end + 2;
}

// position right after the doctype declaration starting at pos, or npos if it doesn't follow the grammar
std::size_t doctype_end(const std::string_view in, const std::size_t pos) {
    auto cursor = pos + 9;
    if (cursor >= in.size() || !is_space(in[cursor])) return npos;
    cursor = skip_spaces(in, cursor);
    const auto name_start = cursor;
    while (cursor < in.size() && !ends_name(in[cursor]) && in[cursor] != '[') ++cursor;
    if (cursor == name_start) return npos;

    auto next = skip_spaces(in, cursor);
    const int literals = in.compare(next, 6, "SYSTEM") == 0 ? 1 : in.compare(next, 6, "PUBLIC") == 0 ? 2 : 0;
    if (literals > 0) {
        if (next == cursor) return npos;
        cursor = next + 6;
        for (int i = 0; i < literals; ++i) {
            next = skip_spaces(in, cursor);
            if (next == cursor) return npos;
            cursor = literal_end(in, next);
            if (cursor == npos) return npos;
        }
        next = skip_spaces(in, cursor);
    }
    if (next < in.size() && in[next] == '[') {
        next = internal_subset_end(in, next);
        if (next == npos) return npos;
        next = skip_spaces(in, next);
    }
    return next < in.size() && in[next] == '>' ? next + 1 : npos;
}

/**
 * @brief Minifies a document with a single root element by dropping the whitespace inside
 * tags and outside the root element; everything else is copied byte for byte.
 * @return The minified document, or std::nullopt if it isn't well-formed enough to be read
 * unambiguously.
 */
std::optional<std::string> minify_xml(const std::string_view in) {
    // control characters other than tab, cr and lf can't appear in a well-formed document
    if (std::ranges::any_of(in, [](const char c) {
            const auto u = static_cast<unsigned char>(c);
            return u < 0x20 && u != '\t' && u != '\n' && u != '\r';
        })) {
        return std::nullopt;
    }
    std::string minified;
    minified.reserve(in.size());
    std::vector<std::string_view> open;
    bool root_seen = false;
    bool doctype_seen = false;
    std::size_t pos = 0;
    const auto emit = [&](const std::string_view s) { minified.append(s); };
    const auto name_at = [&](const std::size_t from) {
        std::size_t end = from;
        while (end < in.size() && !ends_name(in[end])) ++end;
        return in.substr(from, end - from);
    };
    // copies a construct through its terminator
    const auto copy_through = [&](const std::string_view terminator, const std::size_t search_from) {
        const auto end = in.find(terminator, search_from);
        if (end == npos) return false;
        emit(in.substr(pos, end + terminator.size() - pos));
        pos = end + terminator.size();
        return true;
    };

    if (in.starts_with("\xEF\xBB\xBF")) {
        emit(in.substr(0, 3));
        pos = 3;
    }
    const auto document_start = pos;
    while (pos < in.size()) {
        if (in[pos] != '<') {
            const auto next = std::min(in.find('<', pos), in.size());
            const auto text = in.substr(pos, next - pos);
            if (!open.empty()) {
                emit(text);
            } else if (!std::ranges::all_of(text, is_space)) {
                return std::nullopt;
            }
            pos = next;
        } else if (in.compare(pos, 4, "<!--") == 0) {
            if (!copy_through("-->", pos + 4)) return std::nullopt;
        } else if (in.compare(pos, 9, "<![CDATA[") == 0) {
            if (open.empty() || !copy_through("]]>", pos + 9)) return std::nullopt;
        } else if (in.compare(pos, 2, "<?") == 0) {
            // the declaration can only open the document
            const bool declaration = pos == document_start && in.compare(pos, 5, "<?xml") == 0 &&
                                     pos + 5 < in.size() && (is_space(in[pos + 5]) || in[pos + 5] == '?');
            const auto end = declaration ? declaration_end(in, pos) : processing_instruction_end(in, pos);
            if (end == npos) return std::nullopt;
            emit(in.substr(pos, end - pos));
            pos = end;
        } else if (in.compare(pos, 9, "<!DOCTYPE") == 0) {
            const auto end = root_seen || doctype_seen ? npos : doctype_end(in, pos);
            if (end == npos) return std::nullopt;
            doctype_seen = true;
            emit(in.substr(pos, end - pos));
            pos = end;
        } else if (in.compare(pos, 2, "</") == 0) {
            const auto name = name_at(pos + 2);
            const auto end = skip_spaces(in, pos + 2 + name.size());
            if (name.empty() || open.empty() || open.back() != name || end >= in.size() || in[end] != '>') {
                return std::nullopt;
            }
            open.pop_back();
            emit("</");
            emit(name);
            emit(">");
            pos = end + 1;
        } else {
            const auto name = name_at(pos + 1);
            if (name.empty() || name.front() == '!' || (root_seen && open.empty())) return std::nullopt;
            emit("<");
            emit(name);
            auto cursor = pos + 1 + name.size();
            bool empty_element = false;
            for (;;) {
                const auto next = skip_spaces(in, cursor);
                if (next >= in.size()) return std::nullopt;
                if (in[next] == '>') {
                    cursor = next + 1;
                    break;
                }
                if (in.compare(next, 2, "/>") == 0) {
                    cursor = next + 2;
                    empty_element = true;
                    break;
                }
                // an attribute must be preceded by whitespace
                const auto attribute = name_at(next);
                if (next == cursor || attribute.empty()) return std::nullopt;
                const auto equals = skip_spaces(in, next + attribute.size());
                if (equals >= in.size() || in[equals] != '=') return std::nullopt;
                const auto open_quote = skip_spaces(in, equals + 1);
                if (open_quote >= in.size() || (in[open_quote] != '"' && in[open_quote] != '\'')) return std::nullopt;
                const auto close_quote = in.find(in[open_quote], open_quote + 1);
                if (close_quote == npos) return std::nullopt;
                const auto value = in.substr(open_quote + 1, close_quote - open_quote - 1);
                if (value.find('<') != npos) return std::nullopt;
                emit(" ");
                emit(attribute);
                emit("=");
                emit(in.substr(open_quote, close_quote - open_quote + 1));
                cursor = close_quote + 1;
            }
            emit(empty_element ? "/>" : ">");
            if (!empty_element) open.push_back(name);
            root_seen = true;
            pos = cursor;
        }
    }
    if (!root_seen || !open.empty()) return std::nullopt;
    return minified;
}

bool same_node(const pugi::xml_node& a, const pugi::xml_node& b) {
    if (a.type() != b.type() || std::strcmp(a.name(), b.name()) != 0 || std::strcmp(a.value(), b.value()) != 0) {
        return false;
    }
    auto attribute_b = b.first_attribute();
    for (auto attribute_a = a.first_attribute(); attribute_a; attribute_a = attribute_a.next_attribute()) {
        if (!attribute_b || std::strcmp(attribute_a.name(), attribute_b.name()) != 0 ||
            std::strcmp(attribute_a.value(), attribute_b.value()) != 0) {
            return false;
        }
        attribute_b = attribute_b.next_attribute();
    }
    return !attribute_b;
}

// nodes in document order with their depth
std::vector<std::pair<int, pugi::xml_node>> flatten(pugi::xml_document& doc) {
    struct Walker : pugi::xml_tree_walker {
        std::vector<std::pair<int, pugi::xml_node>> nodes;
        bool for_each(pugi::xml_node& node) override {
            nodes.emplace_back(depth(), node);
            return true;
        }
    } walker;
    doc.traverse(walker);
    return std::move(walker.nodes);
}

} // namespace

void XmlProcessor::recompress(const std::filesystem::path& input_path,
                              const std::filesystem::path& output_path, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.filename().string(), get_name());

    const auto text = read_text(input_path);
    if (!text) {
        throw std::runtime_error("can't read " + input_path.string());
    }
    const auto minified = ascii_compatible(*text) ? minify_xml(*text) : std::nullopt;
    if (!minified) {
        Logger::log(LogLevel::Debug, "Not a well-formed ASCII-compatible document, left as is", get_name());
    }
    if (!write_bytes(output_path, minified ? *minified : *text)) {
        throw std::runtime_error("can't write " + output_path.string());
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output_path.filename().string(), get_name());
}

std::optional<ExtractedContent> XmlProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.format = ContainerFormat::Xml;
    std::vector<DataUriAsset> assets;
    if (const auto text = read_text(input_path)) {
        assets = extract_data_uris(*text, [&] {
            content.temp_dir = make_temp_dir_for(input_path, "XML");
            return content.temp_dir;
        });
    }
    for (const auto& asset : assets) {
        content.extracted_files.push_back(asset.file);
    }
    Logger::log(LogLevel::Debug, "Extracted " + std::to_string(assets.size()) + " data URIs", get_name());
    content.extras = std::move(assets);

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.filename().string(), get_name());
    return content;
}

std::filesystem::path XmlProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    const auto* assets = std::any_cast<std::vector<DataUriAsset>>(&content.extras);
    // phase 2 may have minified the file since, which leaves the data uris as they were
    const auto text = assets != nullptr && !assets->empty() ? read_text(content.original_path) : std::nullopt;
    const auto rebuilt = text ? reinsert_data_uris(*text, *assets) : std::nullopt;
    if (!content.temp_dir.empty()) {
        cleanup_temp_dir(content.temp_dir, get_name());
    }
    if (!rebuilt) {
        Logger::log(LogLevel::Debug, "No data URI got smaller", get_name());
        return {};
    }

    const fs::path out_path = fs::temp_directory_path() /
                              (content.original_path.stem().string() + "_tmp" + RandomUtils::random_suffix() +
                               content.original_path.extension().string());
    if (!write_bytes(out_path, *rebuilt)) {
        Logger::log(LogLevel::Error, "Can't write " + out_path.string(), get_name());
        std::error_code ec;
        fs::remove(out_path, ec);
        return {};
    }

    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + out_path.filename().string(), get_name());
    return out_path;
}

std::string XmlProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // not meaningful for XML, semantic equality is handled by raw_equal
    return "";
}

bool XmlProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    // documents left as is may not parse at all
    if (const auto text_a = read_text(a), text_b = read_text(b); text_a && text_b && *text_a == *text_b) return true;

    // values are compared as written: no escapes, line endings or attribute whitespace get normalized
    constexpr unsigned int kVerbatim = pugi::parse_cdata | pugi::parse_pi | pugi::parse_comments |
                                       pugi::parse_declaration | pugi::parse_doctype | pugi::parse_ws_pcdata;
    pugi::xml_document doc_a;
    pugi::xml_document doc_b;
    if (!doc_a.load_file(a.c_str(), kVerbatim) || !doc_b.load_file(b.c_str(), kVerbatim)) {
        return false;
    }

    const auto nodes_a = flatten(doc_a);
    const auto nodes_b = flatten(doc_b);
    return std::ranges::equal(nodes_a, nodes_b, [](const auto& x, const auto& y) {
        return x.first == y.first && same_node(x.second, y.second);
    });
}

bool XmlProcessor::is_signed(const std::filesystem::path& file_path) const {
    return file_contains(file_path, {"http://www.w3.org/2000/09/xmldsig#", "http://c2pa.org/manifest"});
}

} // namespace chisel
