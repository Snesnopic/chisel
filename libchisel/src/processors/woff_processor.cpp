#include "../../include/woff_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <zlib.h>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstring>


namespace chisel {

static std::vector<uint8_t> inflate_zlib(const uint8_t* src, const std::size_t src_len, const std::size_t expected_len) {
    std::vector<uint8_t> uncompressed(expected_len);
    uLongf dest_len = static_cast<uLongf>(expected_len);
    
    const int res = uncompress(uncompressed.data(), &dest_len, src, static_cast<uLong>(src_len));
    if (res != Z_OK) {
        throw std::runtime_error("zlib decompression failed with code: " + std::to_string(res));
    }
    if (dest_len != expected_len) {
        throw std::runtime_error("WOFF table size mismatch: expected " + std::to_string(expected_len) + " got " + std::to_string(dest_len));
    }
    
    return uncompressed;
}

static std::vector<uint8_t> deflate_zlib(const std::vector<uint8_t>& uncompressed) {
    z_stream strm{};
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK) {
        throw std::runtime_error("failed to init zlib");
    }

    std::vector<uint8_t> compressed;
    std::vector<uint8_t> out_buf(32768); // 32KB buffer

    strm.next_in = const_cast<uint8_t*>(uncompressed.data());
    strm.avail_in = static_cast<uInt>(uncompressed.size());

    int ret;
    do {
        strm.next_out = out_buf.data();
        strm.avail_out = static_cast<uInt>(out_buf.size());
        
        ret = deflate(&strm, Z_FINISH);
        
        const std::size_t have = out_buf.size() - strm.avail_out;
        if (have > 0) {
            compressed.insert(compressed.end(), out_buf.data(), out_buf.data() + have);
        }
    } while (strm.avail_out == 0);

    deflateEnd(&strm);
    
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("zlib compression did not complete");
    }
    
    return compressed;
}

void WoffProcessor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path, const ProcessingOptions& options) {
    Logger::log(LogLevel::Debug, "starting woff recompression for " + input_path.string(), get_name());

    const auto data = read_file(input_path);
    if (data.size() < 44) return; // WOFF header is strictly 44 bytes

    if (read_be32(data.data()) != 0x774F4646) { // 'wOFF'
        Logger::log(LogLevel::Warning, "invalid woff signature", get_name());
        return;
    }

    uint16_t num_tables = read_be16(data.data() + 12);
    if (data.size() < 44 + static_cast<std::size_t>(num_tables) * 20) return;

    std::vector<uint8_t> new_woff;
    new_woff.insert(new_woff.end(), data.begin(), data.begin() + 44);

    constexpr std::size_t dir_offset = 44;
    new_woff.resize(44 + num_tables * 20);

    uint32_t current_offset = static_cast<uint32_t>(new_woff.size());

    try {
        for (uint16_t i = 0; i < num_tables; ++i) {
            std::size_t entry_pos = dir_offset + i * 20;
            uint32_t tag = read_be32(data.data() + entry_pos);
            uint32_t offset = read_be32(data.data() + entry_pos + 4);
            uint32_t comp_len = read_be32(data.data() + entry_pos + 8);
            uint32_t orig_len = read_be32(data.data() + entry_pos + 12);
            uint32_t orig_checksum = read_be32(data.data() + entry_pos + 16);

            if (offset + comp_len > data.size()) throw std::runtime_error("table offset out of bounds");

            std::vector<uint8_t> table_data;
            if (comp_len < orig_len) {
                table_data = inflate_zlib(data.data() + offset, comp_len, orig_len);
            } else {
                table_data.assign(data.begin() + offset, data.begin() + offset + orig_len);
            }

            auto compressed = deflate_zlib(table_data);
            
            // if compression is worse or equal, store uncompressed to save CPU on client
            if (compressed.size() >= table_data.size()) {
                compressed = std::move(table_data);
            }

            uint32_t final_comp_len = static_cast<uint32_t>(compressed.size());
            uint32_t padded_len = align4(final_comp_len);

            // write exact sizes to directory
            uint8_t entry[20];
            write_be32(entry, tag);
            write_be32(entry + 4, current_offset);
            write_be32(entry + 8, final_comp_len);
            write_be32(entry + 12, orig_len);
            write_be32(entry + 16, orig_checksum);
            memcpy(new_woff.data() + entry_pos, entry, 20);

            new_woff.insert(new_woff.end(), compressed.begin(), compressed.end());
            
            if (padded_len > final_comp_len) {
                new_woff.insert(new_woff.end(), padded_len - final_comp_len, 0);
            }
            
            current_offset += padded_len;
        }

        const uint32_t orig_meta_offset = read_be32(data.data() + 24);
        const uint32_t orig_meta_length = read_be32(data.data() + 28);
        const uint32_t orig_meta_orig_length = read_be32(data.data() + 32);
        const uint32_t orig_priv_offset = read_be32(data.data() + 36);
        const uint32_t orig_priv_length = read_be32(data.data() + 40);

        if (!options.preserve_metadata) {
            // correct WOFF header offsets for metadata and private data
            write_be32(new_woff.data() + 24, 0); // metaOffset
            write_be32(new_woff.data() + 28, 0); // metaLength
            write_be32(new_woff.data() + 32, 0); // metaOrigLength
            write_be32(new_woff.data() + 36, 0); // privOffset
            write_be32(new_woff.data() + 40, 0); // privLength
        } else {
            // metadata/private blocks are opaque (metadata already zlib-compressed, private
            // data never compressed per spec) so they're copied verbatim; only their offsets
            // change, since the table data preceding them has been resized by recompression
            if (orig_meta_offset != 0 && orig_meta_length != 0 &&
                static_cast<uint64_t>(orig_meta_offset) + orig_meta_length <= data.size()) {
                const uint32_t new_meta_offset = current_offset;
                new_woff.insert(new_woff.end(), data.begin() + orig_meta_offset,
                                 data.begin() + orig_meta_offset + orig_meta_length);
                current_offset += orig_meta_length;

                write_be32(new_woff.data() + 24, new_meta_offset);
                write_be32(new_woff.data() + 28, orig_meta_length);
                write_be32(new_woff.data() + 32, orig_meta_orig_length);
            } else {
                write_be32(new_woff.data() + 24, 0);
                write_be32(new_woff.data() + 28, 0);
                write_be32(new_woff.data() + 32, 0);
            }

            if (orig_priv_offset != 0 && orig_priv_length != 0 &&
                static_cast<uint64_t>(orig_priv_offset) + orig_priv_length <= data.size()) {
                // privOffset must land on a four-byte boundary
                const uint32_t padded_offset = align4(current_offset);
                if (padded_offset > current_offset) {
                    new_woff.insert(new_woff.end(), padded_offset - current_offset, 0);
                    current_offset = padded_offset;
                }

                const uint32_t new_priv_offset = current_offset;
                new_woff.insert(new_woff.end(), data.begin() + orig_priv_offset,
                                 data.begin() + orig_priv_offset + orig_priv_length);
                current_offset += orig_priv_length;

                write_be32(new_woff.data() + 36, new_priv_offset);
                write_be32(new_woff.data() + 40, orig_priv_length);
            } else {
                write_be32(new_woff.data() + 36, 0);
                write_be32(new_woff.data() + 40, 0);
            }
        }

        // update header length (must run last: metadata/private blocks above extend the file)
        write_be32(new_woff.data() + 8, current_offset);

        std::ofstream out(output_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(new_woff.data()), new_woff.size());
        out.close();

    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "failed to recompress woff: " + std::string(e.what()), get_name());
    }
}

bool WoffProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        if (data_a.size() < 44 || data_b.size() < 44) return false;
        
        const uint16_t num_tables_a = read_be16(data_a.data() + 12);
        const uint16_t num_tables_b = read_be16(data_b.data() + 12);
        
        if (num_tables_a != num_tables_b) return false;

        // compare uncompressed tables one by one
        for (uint16_t i = 0; i < num_tables_a; ++i) {
            const std::size_t entry_a = 44 + i * 20;
            const std::size_t entry_b = 44 + i * 20;

            if (read_be32(data_a.data() + entry_a) != read_be32(data_b.data() + entry_b)) return false; // Tags must match
            
            const uint32_t orig_len_a = read_be32(data_a.data() + entry_a + 12);
            const uint32_t orig_len_b = read_be32(data_b.data() + entry_b + 12);
            if (orig_len_a != orig_len_b) return false;

            const uint32_t comp_len_a = read_be32(data_a.data() + entry_a + 8);
            const uint32_t comp_len_b = read_be32(data_b.data() + entry_b + 8);
            const uint32_t offset_a = read_be32(data_a.data() + entry_a + 4);
            const uint32_t offset_b = read_be32(data_b.data() + entry_b + 4);

            std::vector<uint8_t> table_a;
            if (comp_len_a < orig_len_a) table_a = inflate_zlib(data_a.data() + offset_a, comp_len_a, orig_len_a);
            else table_a.assign(data_a.begin() + offset_a, data_a.begin() + offset_a + orig_len_a);

            std::vector<uint8_t> table_b;
            if (comp_len_b < orig_len_b) table_b = inflate_zlib(data_b.data() + offset_b, comp_len_b, orig_len_b);
            else table_b.assign(data_b.begin() + offset_b, data_b.begin() + offset_b + orig_len_b);

            if (table_a != table_b) return false;
        }

        return true;
    } catch (...) {
        return false;
    }
}

} // namespace chisel