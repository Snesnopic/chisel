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
#include "../../include/base64_utils.hpp"
#include <fstream>
#include <regex>


namespace {



} // namespace

namespace chisel {

void XmlProcessor::recompress(const std::filesystem::path& input_path,
                              const std::filesystem::path& output_path, const ProcessingOptions &options) {
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

    const auto doc = std::make_shared<pugi::xml_document>();
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

                    std::vector<uint8_t> binary_data = Base64Utils::decode(b64_data);

                    std::filesystem::path tmp_file = content_ref.temp_dir /
                                                    ("asset_" + RandomUtils::random_suffix() + ext);

                    std::ofstream out(tmp_file, std::ios::binary);
                    if (out) {
                        if (binary_data.size() > 0) {
                            out.write(reinterpret_cast<const char*>(binary_data.data()), binary_data.size());
                        }
                        out.close();

                        state_ref.mappings.emplace_back(tmp_file, attr);
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
        Logger::log(LogLevel::Debug, "No base64 assets found, will just minify XML", get_name());
    }

    Logger::log(LogLevel::Info, "Extracted " + std::to_string(content.extracted_files.size()) + " base64 assets from XML", get_name());

    content.extras = std::make_any<xml_state>(std::move(state));
    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.filename().string(), get_name());

    return content;
}

std::filesystem::path XmlProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    if (!content.extras.has_value()) {
        Logger::log(LogLevel::Error, "Missing XML state in finalize_extraction", get_name());
        chisel::cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    auto state = std::any_cast<xml_state>(content.extras);

    for (auto& [file_path, attr] : state.mappings) {
        if (std::filesystem::exists(file_path)) {
            std::ifstream in(file_path, std::ios::binary | std::ios::ate);
            if (in) {
                std::streamsize size = in.tellg();
                in.seekg(0, std::ios::beg);

                std::vector<uint8_t> opt_data(size);
                bool read_ok = false;
                if (size == 0) {
                    read_ok = true;
                } else if (size > 0 && in.read(reinterpret_cast<char*>(opt_data.data()), size)) {
                    read_ok = true;
                }

                if (read_ok) {
                    std::string new_b64 = Base64Utils::encode(opt_data);
                    std::string ext = file_path.extension().string().substr(1);
                    std::string new_val = "data:image/" + ext + ";base64," + new_b64;

                    attr.set_value(new_val.c_str());
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
        void write(const void* data, const std::size_t size) override {
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