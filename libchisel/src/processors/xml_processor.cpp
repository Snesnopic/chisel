//
// Created by Giuseppe Francione on 22/02/26.
//

/**
 * @file xml_processor.cpp
 * @brief implementation of the XML processor class.
 */

#include "../../include/xml_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <fstream>
#include <regex>

namespace {

// base64 decode/encode utilities
const std::string b64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

/**
 * @brief checks if a given character belongs to the base64 alphabet.
 * @param c the character to check.
 * @return true if valid base64 character.
 */
inline bool is_base64(unsigned char c) {
    return (isalnum(c) || (c == '+') || (c == '/'));
}

/**
 * @brief decodes a base64 encoded string into raw binary data.
 * @param encoded_string string view containing base64 data.
 * @return vector of decoded bytes.
 */
std::vector<uint8_t> base64_decode(std::string_view encoded_string) {
    int in_len = encoded_string.size();
    int i = 0, j = 0, in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    std::vector<uint8_t> ret;

    while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++)
                char_array_4[i] = b64_chars.find(char_array_4[i]);
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (i = 0; (i < 3); i++) ret.push_back(char_array_3[i]);
            i = 0;
        }
    }
    if (i) {
        for (j = i; j < 4; j++) char_array_4[j] = 0;
        for (j = 0; j < 4; j++) char_array_4[j] = b64_chars.find(char_array_4[j]);
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        for (j = 0; (j < i - 1); j++) ret.push_back(char_array_3[j]);
    }
    return ret;
}

/**
 * @brief encodes raw binary data into a base64 string.
 * @param bytes_to_encode vector of bytes to encode.
 * @return base64 encoded string.
 */
std::string base64_encode(const std::vector<uint8_t>& bytes_to_encode) {
    std::string ret;
    int i = 0, j = 0;
    unsigned char char_array_3[3], char_array_4[4];
    size_t in_len = bytes_to_encode.size();
    const uint8_t* buf = bytes_to_encode.data();

    while (in_len--) {
        char_array_3[i++] = *(buf++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (i = 0; (i < 4); i++) ret += b64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for (j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) ret += b64_chars[char_array_4[j]];
        while ((i++ < 3)) ret += '=';
    }
    return ret;
}

} // namespace

namespace chisel {

void XmlProcessor::recompress(const std::filesystem::path& input_path,
                              const std::filesystem::path& output_path,
                              bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.filename().string(), get_name());

    pugi::xml_document doc;
    if (!doc.load_file(input_path.c_str())) {
        Logger::log(LogLevel::Error, "Failed to load XML file for recompression", get_name());
        return;
    }

    if (!doc.save_file(output_path.c_str(), "", pugi::format_raw)) {
        Logger::log(LogLevel::Error, "Failed to save minified XML file", get_name());
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output_path.filename().string(), get_name());
}

std::optional<ExtractedContent> XmlProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    auto doc = std::make_shared<pugi::xml_document>();
    if (!doc->load_file(input_path.c_str())) {
        Logger::log(LogLevel::Error, "Failed to parse XML for extraction", get_name());
        return std::nullopt;
    }

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = chisel::make_temp_dir_for(input_path, "XML");

    xml_state state;
    state.doc = doc;

    const std::regex data_uri_regex(R"(^data:image/([^;]+);base64,(.*)$)");

    struct Walker : pugi::xml_tree_walker {
        ExtractedContent& content_ref;
        xml_state& state_ref;
        const std::regex& reg;

        Walker(ExtractedContent& c, xml_state& s, const std::regex& r)
            : content_ref(c), state_ref(s), reg(r) {}

        bool for_each(pugi::xml_node& node) override {
            for (pugi::xml_attribute attr : node.attributes()) {
                std::string val = attr.value();
                std::smatch match;

                if (std::regex_match(val, match, reg) && match.size() == 3) {
                    std::string ext = "." + match[1].str();
                    std::string b64_data = match[2].str();

                    std::vector<uint8_t> binary_data = base64_decode(b64_data);

                    std::filesystem::path tmp_file = content_ref.temp_dir /
                                                    ("asset_" + RandomUtils::random_suffix() + ext);

                    std::ofstream out(tmp_file, std::ios::binary);
                    if (out) {
                        out.write(reinterpret_cast<const char*>(binary_data.data()), binary_data.size());
                        out.close();

                        state_ref.mappings.emplace_back(tmp_file, node);
                        content_ref.extracted_files.push_back(tmp_file);
                    } else {
                        Logger::log(LogLevel::Error, "Failed to write extracted base64 data", "XmlProcessor");
                    }
                }
            }
            return true;
        }
    } walker(content, state, data_uri_regex);

    doc->traverse(walker);

    if (content.extracted_files.empty()) {
        chisel::cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    Logger::log(LogLevel::Info, "Extracted " + std::to_string(content.extracted_files.size()) + " base64 assets from XML", get_name());

    content.extras = std::make_any<xml_state>(std::move(state));
    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.filename().string(), get_name());

    return content;
}

std::filesystem::path XmlProcessor::finalize_extraction(const ExtractedContent& content) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    if (!content.extras.has_value()) {
        Logger::log(LogLevel::Error, "Missing XML state in finalize_extraction", get_name());
        chisel::cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    auto state = std::any_cast<xml_state>(content.extras);

    for (const auto& [file_path, node] : state.mappings) {
        if (std::filesystem::exists(file_path)) {
            std::ifstream in(file_path, std::ios::binary | std::ios::ate);
            if (in) {
                std::streamsize size = in.tellg();
                in.seekg(0, std::ios::beg);

                std::vector<uint8_t> opt_data(size);
                if (in.read(reinterpret_cast<char*>(opt_data.data()), size)) {
                    std::string new_b64 = base64_encode(opt_data);
                    std::string ext = file_path.extension().string().substr(1);
                    std::string new_val = "data:image/" + ext + ";base64," + new_b64;

                    for (pugi::xml_attribute attr : node.attributes()) {
                        if (std::string_view(attr.value()).starts_with("data:image/")) {
                            attr.set_value(new_val.c_str());
                            break;
                        }
                    }
                }
            }
        }
    }

    std::filesystem::path out_path = std::filesystem::temp_directory_path() /
                                     (content.original_path.stem().string() + "_tmp" +
                                      RandomUtils::random_suffix() +
                                      content.original_path.extension().string());

    if (!state.doc->save_file(out_path.c_str(), "", pugi::format_raw)) {
        Logger::log(LogLevel::Error, "Failed to save minified XML in finalize_extraction", get_name());
        chisel::cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    Logger::log(LogLevel::Info, "Successfully rebuilt and minified XML dom", get_name());
    chisel::cleanup_temp_dir(content.temp_dir, get_name());

    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + out_path.filename().string(), get_name());
    return out_path;
}

std::string XmlProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // not meaningful for XML, semantic equality is handled by raw_equal
    return "";
}

bool XmlProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    pugi::xml_document doc_a;
    pugi::xml_document doc_b;

    if (!doc_a.load_file(a.c_str()) || !doc_b.load_file(b.c_str())) {
        return false;
    }

    // helper to save XML to string
    struct xml_string_writer : pugi::xml_writer {
        std::string result;
        void write(const void* data, size_t size) override {
            result.append(static_cast<const char*>(data), size);
        }
    };

    // serialize both in raw format to strip whitespace/indentation differences
    xml_string_writer writer_a;
    doc_a.save(writer_a, "", pugi::format_raw);

    xml_string_writer writer_b;
    doc_b.save(writer_b, "", pugi::format_raw);

    return writer_a.result == writer_b.result;
}

} // namespace chisel