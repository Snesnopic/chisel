//
// Created by Giuseppe Francione on 31/05/26.
//

#include "../../include/vcf_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_type.hpp"
#include "tbytevector.h"
#include "file_type.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <string_view>

namespace chisel {

namespace {
    bool ci_compare(const char a, const char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    }

    std::size_t find_case_insensitive(const std::string_view haystack, const std::string_view needle, const std::size_t pos = 0) {
        if (pos >= haystack.size()) return std::string_view::npos;
        const auto it = std::search(haystack.begin() + pos, haystack.end(),
                              needle.begin(), needle.end(), ci_compare);
        if (it == haystack.end()) return std::string_view::npos;
        return std::distance(haystack.begin(), it);
    }
}

std::optional<ExtractedContent> VcfProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "starting vcf extraction for " + input_path.string(), get_name());

    const auto raw_data = read_file(input_path);
    // Zero-copy: map string_view over the raw data vector
    std::string_view content_view(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "vcf");
    content.format = ContainerFormat::Vcf;

    std::vector<PhotoPosition> positions;
    std::size_t last_search_pos = 0;

    constexpr std::string_view photo_tag = "PHOTO;";

    while (true) {
        std::size_t photo_pos = find_case_insensitive(content_view, photo_tag, last_search_pos);
        if (photo_pos == std::string_view::npos) break;

        std::size_t colon_pos = content_view.find(':', photo_pos);
        if (colon_pos == std::string_view::npos) break;

        std::string_view header = content_view.substr(photo_pos, colon_pos - photo_pos);
        
        // ensure it's base64 encoded
        auto contains_ci = [](const std::string_view haystack, const std::string_view needle) {
            return find_case_insensitive(haystack, needle) != std::string_view::npos;
        };

        if (!contains_ci(header, "encoding=b") && !contains_ci(header, "encoding=base64")) {
            last_search_pos = colon_pos + 1;
            continue;
        }

        // find end of base64 data (first line that doesn't start with space/tab)
        std::size_t data_start = colon_pos + 1;
        std::size_t data_end = data_start;
        
        while (data_end < content_view.size()) {
            std::size_t next_newline = content_view.find('\n', data_end);
            if (next_newline == std::string_view::npos) {
                data_end = content_view.size();
                break;
            }
            
            std::size_t line_after = next_newline + 1;
            if (line_after < content_view.size() && 
                content_view[line_after] != ' ' && 
                content_view[line_after] != '\t') {
                data_end = next_newline;
                if (data_end > 0 && content_view[data_end - 1] == '\r') {
                    data_end--;
                }
                break;
            }
            data_end = next_newline + 1;
        }

        std::string b64_raw(content_view.substr(data_start, data_end - data_start));
        std::string b64_clean;
        b64_clean.reserve(b64_raw.size());
        for (char c : b64_raw) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                b64_clean.push_back(c);
            }
        }

        TagLib::ByteVector bv = TagLib::ByteVector::fromBase64(b64_clean.c_str());
        if (!bv.isEmpty()) {
            std::string ext = ".bin";
            if (bv.size() > 2 && static_cast<uint8_t>(bv[0]) == 0x89 && bv[1] == 'P') ext = ".png";
            else if (bv.size() > 2 && static_cast<uint8_t>(bv[0]) == 0xFF && static_cast<uint8_t>(bv[1]) == 0xD8) ext = ".jpg";
            else if (bv.size() > 2 && bv[0] == 'G' && bv[1] == 'I' && bv[2] == 'F') ext = ".gif";

            std::filesystem::path inner_path = content.temp_dir / ("photo_" + std::to_string(positions.size()) + ext);
            std::ofstream out_file(inner_path, std::ios::binary);
            out_file.write(bv.data(), bv.size());
            out_file.close();

            content.extracted_files.push_back(inner_path);
            
            PhotoPosition pos;
            pos.start = data_start;
            pos.end = data_end;
            pos.prefix = std::string(content_view.substr(photo_pos, data_start - photo_pos));
            positions.push_back(pos);
        }

        last_search_pos = data_end;
    }

    if (content.extracted_files.empty()) {
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    content.extras = std::make_any<std::vector<PhotoPosition>>(positions);
    return content;
}

std::filesystem::path VcfProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "starting vcf finalization for " + content.original_path.string(), get_name());

    const auto raw_data = read_file(content.original_path);
    std::string_view content_view(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    auto positions = std::any_cast<std::vector<PhotoPosition>>(content.extras);

    std::string new_content;
    new_content.reserve(raw_data.size()); 

    std::size_t last_pos = 0;

    for (size_t i = 0; i < positions.size(); ++i) {
        // text before this photo
        std::size_t photo_entry_start = positions[i].start - positions[i].prefix.length();
        new_content.append(content_view.substr(last_pos, photo_entry_start - last_pos));
        
        // prefix (PHOTO;...)
        new_content.append(positions[i].prefix);

        // get optimized data and encode to base64
        const auto opt_data = read_file(content.extracted_files[i]);
        TagLib::ByteVector bv(reinterpret_cast<const char*>(opt_data.data()), static_cast<unsigned int>(opt_data.size()));
        TagLib::ByteVector b64_bv = bv.toBase64();
        std::string b64(b64_bv.data(), b64_bv.size());

        std::size_t current_line_len = positions[i].prefix.length();
        
        for (char c : b64) {
            if (current_line_len >= 74) {
                new_content.append("\r\n ");
                current_line_len = 1; 
            }
            new_content.push_back(c);
            current_line_len++;
        }

        last_pos = positions[i].end;
    }

    new_content.append(content_view.substr(last_pos));

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".vcf");

    std::ofstream out_file(output_path, std::ios::binary);
    out_file.write(new_content.data(), new_content.size());
    out_file.close();

    cleanup_temp_dir(content.temp_dir, get_name());
    return output_path;
}

bool VcfProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    return read_file(a) == read_file(b);
}

std::string VcfProcessor::get_raw_checksum(const std::filesystem::path&) const {
    return "";
}

} // namespace chisel
