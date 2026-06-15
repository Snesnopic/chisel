#include "../../include/mime_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/base64_utils.hpp"
#include "../../include/random_utils.hpp"
#include <fstream>
#include <sstream>
#include <regex>

namespace chisel {

namespace {

struct MemChunk {
    std::variant<std::string, std::vector<uint8_t>> data;
};

std::vector<MemChunk> parse_mime_memory(const std::filesystem::path& input_path) {
    std::vector<MemChunk> chunks;
    std::ifstream in(input_path, std::ios::binary);
    if (!in.is_open()) return chunks;

    std::string line;
    bool in_headers = true;
    bool is_base64 = false;
    std::string b64_buffer;
    std::string current_text;

    std::regex b64_regex(R"(^\s*Content-Transfer-Encoding:\s*base64\s*$)", std::regex_constants::icase);

    auto flush_b64 = [&]() {
        if (!b64_buffer.empty()) {
            std::string clean_b64;
            for (const char c : b64_buffer) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    clean_b64.push_back(c);
                }
            }
            if (!clean_b64.empty()) {
                if (!current_text.empty()) {
                    chunks.push_back(MemChunk{current_text});
                    current_text.clear();
                }
                chunks.push_back(MemChunk{Base64Utils::decode(clean_b64)});
            } else {
                current_text += b64_buffer;
            }
            b64_buffer.clear();
        }
    };

    while (std::getline(in, line)) {
        std::string raw_line = line + "\n";
        std::string clean_line = line;
        if (!clean_line.empty() && clean_line.back() == '\r') {
            clean_line.pop_back();
        }

        if (clean_line.size() >= 2 && clean_line[0] == '-' && clean_line[1] == '-') {
            flush_b64();
            current_text += raw_line;
            in_headers = true;
            is_base64 = false;
            continue;
        }

        if (in_headers) {
            current_text += raw_line;
            if (clean_line.empty()) {
                in_headers = false;
            } else if (std::regex_match(clean_line, b64_regex)) {
                is_base64 = true;
            }
        } else {
            if (is_base64) {
                b64_buffer += raw_line;
            } else {
                current_text += raw_line;
            }
        }
    }

    flush_b64();
    if (!current_text.empty()) {
        chunks.push_back(MemChunk{current_text});
    }

    return chunks;
}

} // namespace

std::optional<ExtractedContent> MimeProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    std::ifstream in(input_path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = chisel::make_temp_dir_for(input_path, "MIME");
    auto state = std::make_shared<MimeState>();

    std::string line;
    bool in_headers = true;
    bool is_base64 = false;
    std::string b64_buffer;
    std::string current_text;

    std::regex b64_regex(R"(^\s*Content-Transfer-Encoding:\s*base64\s*$)", std::regex_constants::icase);

    auto flush_b64 = [&]() {
        if (!b64_buffer.empty()) {
            std::string clean_b64;
            for (const char c : b64_buffer) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    clean_b64.push_back(c);
                }
            }

            if (!clean_b64.empty()) {
                const std::vector<uint8_t> binary_data = Base64Utils::decode(clean_b64);
                const std::filesystem::path tmp_file = content.temp_dir /
                                                ("mime_asset_" + RandomUtils::random_suffix() + ".bin");
                chisel::write_file(tmp_file, binary_data);

                if (!current_text.empty()) {
                    state->chunks.push_back(TextChunk{current_text});
                    current_text.clear();
                }

                state->chunks.push_back(Base64Chunk{content.extracted_files.size()});
                content.extracted_files.push_back(tmp_file);
            } else {
                current_text += b64_buffer;
            }
            b64_buffer.clear();
        }
    };

    while (std::getline(in, line)) {
        std::string raw_line = line + "\n";
        std::string clean_line = line;
        if (!clean_line.empty() && clean_line.back() == '\r') {
            clean_line.pop_back();
        }

        if (clean_line.size() >= 2 && clean_line[0] == '-' && clean_line[1] == '-') {
            flush_b64();
            current_text += raw_line;
            in_headers = true;
            is_base64 = false;
            continue;
        }

        if (in_headers) {
            current_text += raw_line;
            if (clean_line.empty()) {
                in_headers = false;
            } else if (std::regex_match(clean_line, b64_regex)) {
                is_base64 = true;
            }
        } else {
            if (is_base64) {
                b64_buffer += raw_line;
            } else {
                current_text += raw_line;
            }
        }
    }

    flush_b64();
    if (!current_text.empty()) {
        state->chunks.push_back(TextChunk{current_text});
    }

    if (content.extracted_files.empty()) {
        chisel::cleanup_temp_dir(content.temp_dir);
        return std::nullopt;
    }

    content.extras = state;
    return content;
}

std::filesystem::path MimeProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction", get_name());

    std::filesystem::path output_file = content.temp_dir / ("final_" + RandomUtils::random_suffix() + ".mime");
    std::ofstream out(output_file, std::ios::binary);

    if (!content.extras.has_value()) {
        return output_file;
    }

    auto state = std::any_cast<std::shared_ptr<MimeState>>(content.extras);

    for (const auto& chunk : state->chunks) {
        if (std::holds_alternative<TextChunk>(chunk)) {
            out << std::get<TextChunk>(chunk).text;
        } else if (std::holds_alternative<Base64Chunk>(chunk)) {
            std::size_t idx = std::get<Base64Chunk>(chunk).file_index;
            const auto& file_path = content.extracted_files[idx];
            
            std::vector<uint8_t> opt_data = chisel::read_file(file_path);
            std::string new_b64 = Base64Utils::encode(opt_data);
            
            for (size_t i = 0; i < new_b64.length(); i += 76) {
                out << new_b64.substr(i, 76) << "\r\n";
            }
        }
    }
    out.close();

    return output_file;
}

std::string MimeProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // Semantic equality handled by raw_equal
    return "";
}

bool MimeProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    const auto chunks_a = parse_mime_memory(a);
    const auto chunks_b = parse_mime_memory(b);

    if (chunks_a.size() != chunks_b.size()) {
        return false;
    }

    for (size_t i = 0; i < chunks_a.size(); ++i) {
        if (chunks_a[i].data.index() != chunks_b[i].data.index()) {
            return false;
        }

        if (std::holds_alternative<std::string>(chunks_a[i].data)) {
            if (std::get<std::string>(chunks_a[i].data) != std::get<std::string>(chunks_b[i].data)) {
                return false;
            }
        } else {
            if (std::get<std::vector<uint8_t>>(chunks_a[i].data) != std::get<std::vector<uint8_t>>(chunks_b[i].data)) {
                return false;
            }
        }
    }

    return true;
}

} // namespace chisel
