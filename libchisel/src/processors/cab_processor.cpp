//
// Created by Giuseppe Francione on 11/06/26.
//

#include "../../include/cab_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <libdeflate.h>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdint>

/**
 * @file cab_processor.cpp
 * @brief MSZIP block recompressor for Microsoft Cabinet (.cab) files.
 *
 * CAB binary layout (all integers little-endian):
 *
 *   CFHEADER (36 bytes unless flags & 0x0004):
 *     uint8[4]  signature   "MSCF"
 *     uint8     reserved1   (0)
 *     uint32    cbCabinet   total file size
 *     uint32    reserved2   (0)
 *     uint32    coffFiles   offset to first CFFILE
 *     uint32    reserved3   (0)
 *     uint8     versionMinor
 *     uint8     versionMajor
 *     uint16    cFolders    number of CFFOLDER entries
 *     uint16    cFiles      number of CFFILE entries
 *     uint16    flags
 *     uint16    setID
 *     uint16    iCabinet    cabinet index in set
 *
 *   If flags & 0x0004 (RESERVE_PRESENT), CFHEADER_EXTENDED follows:
 *     uint16    cbCFHeader  extra bytes after CFHEADER
 *     uint8     cbCFFolder  extra bytes per CFFOLDER
 *     uint8     cbCFData    extra bytes per CFDATA
 *   Followed by cbCFHeader extra bytes.
 *
 *   CFFOLDER[cFolders] (8 + cbCFFolder bytes each):
 *     uint32    coffCabStart  offset of first CFDATA block
 *     uint16    cCFDATA       number of CFDATA blocks
 *     uint16    typeCompress  compression type (0=none, 1=MSZIP, 2=Quantum, 3=LZX)
 *
 *   CFFILE[cFiles] (variable, starts at coffFiles):
 *     uint32    cbFile        uncompressed size
 *     uint32    cbFolderStart offset within uncompressed folder data
 *     uint16    iFolder       folder index
 *     uint16    date, time, attribs
 *     char[]    szName        NUL-terminated filename
 *
 *   CFDATA blocks (referenced by each CFFOLDER):
 *     uint32    csum          Adler-style checksum (or 0)
 *     uint16    cbData        compressed data size
 *     uint16    cbUncomp      uncompressed data size
 *     uint8[cbData] ab        compressed payload
 *
 *   For MSZIP (typeCompress == 0x0001), each CFDATA block's payload starts
 *   with the two-byte magic "CK" followed by a raw DEFLATE stream
 *   (i.e. zlib without the zlib wrapper, just raw deflate bytes).
 *
 * Strategy:
 *   1. Parse header, extended header (if any), folders, files.
 *   2. For each MSZIP folder, read all CFDATA blocks, decompress the raw
 *      DEFLATE payload (stripping the "CK" magic), re-compress with
 *      libdeflate at level 12 (maximum), prepend the "CK" magic again.
 *   3. Rebuild the CAB: recompute offsets, update cbCabinet, strip any
 *      reserved extension in the header (they indicated unsigned / empty
 *      signing space — we already skip signed files), clear flags & 0x0004.
 *   4. The CFFILE table is copied verbatim (file metadata is preserved).
 *   5. CFDATA checksums: the CAB spec defines a specific checksum algorithm.
 *      Setting it to 0 is explicitly valid ("checksum omitted") and is what
 *      most modern CAB writers do for MSZIP blocks.
 */

namespace chisel {

namespace {

// ─── CAB constants ────────────────────────────────────────────────────────────
constexpr uint8_t  kMscfSig[4]    = { 'M', 'S', 'C', 'F' };
constexpr uint16_t kFlagReserve   = 0x0004; ///< RESERVE_PRESENT
constexpr uint16_t kFlagPrevCab   = 0x0001; ///< PREV_CABINET
constexpr uint16_t kFlagNextCab   = 0x0002; ///< NEXT_CABINET
constexpr uint16_t kCompNone      = 0x0000;
constexpr uint16_t kCompMszip     = 0x0001;
constexpr uint8_t  kMszipMagic[2] = { 'C', 'K' };
constexpr int      kDeflateLevel  = 12; ///< libdeflate max level

// ─── layout structs ──────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct CfHeader {
    uint8_t  signature[4];
    uint32_t reserved1;
    uint32_t cbCabinet;
    uint32_t reserved2;
    uint32_t coffFiles;
    uint32_t reserved3;
    uint8_t  versionMinor;
    uint8_t  versionMajor;
    uint16_t cFolders;
    uint16_t cFiles;
    uint16_t flags;
    uint16_t setID;
    uint16_t iCabinet;
};

struct CfHeaderExt {
    uint16_t cbCFHeader;
    uint8_t  cbCFFolder;
    uint8_t  cbCFData;
};

struct CfFolder {
    uint32_t coffCabStart;
    uint16_t cCFDATA;
    uint16_t typeCompress;
};

struct CfData {
    uint32_t csum;
    uint16_t cbData;
    uint16_t cbUncomp;
};
#pragma pack(pop)

// ─── safe reader ─────────────────────────────────────────────────────────────

struct Span {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    void read(void* dst, size_t n) {
        if (pos + n > size) throw std::runtime_error("CabProcessor: unexpected end of data");
        std::memcpy(dst, data + pos, n);
        pos += n;
    }

    void skip(size_t n) {
        if (pos + n > size) throw std::runtime_error("CabProcessor: unexpected end of data");
        pos += n;
    }

    const uint8_t* cur() const { return data + pos; }
};

// ─── MSZIP decompress ─────────────────────────────────────────────────────────

/**
 * @brief Decompress a single MSZIP CFDATA payload.
 *
 * MSZIP payload = "CK" + raw DEFLATE stream.  The uncompressed size is
 * given by the CFDATA.cbUncomp field.
 */
std::vector<uint8_t> mszip_decompress(const uint8_t* src, size_t src_len, size_t uncomp_len) {
    if (src_len < 2 || src[0] != 'C' || src[1] != 'K')
        throw std::runtime_error("CabProcessor: missing MSZIP 'CK' magic");

    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) throw std::runtime_error("CabProcessor: libdeflate_alloc_decompressor failed");

    std::vector<uint8_t> out(uncomp_len);
    size_t actual = 0;
    libdeflate_result res = libdeflate_deflate_decompress(
        dec,
        src + 2, src_len - 2,  // skip "CK"
        out.data(), uncomp_len,
        &actual
    );
    libdeflate_free_decompressor(dec);

    if (res != LIBDEFLATE_SUCCESS)
        throw std::runtime_error("CabProcessor: DEFLATE decompression failed (code " + std::to_string(res) + ")");
    if (actual != uncomp_len)
        throw std::runtime_error("CabProcessor: decompressed size mismatch");

    return out;
}

/**
 * @brief Compress a raw buffer into an MSZIP CFDATA payload ("CK" + raw DEFLATE).
 */
std::vector<uint8_t> mszip_compress(libdeflate_compressor* comp,
                                    const uint8_t* src, size_t src_len) {
    size_t bound = libdeflate_deflate_compress_bound(comp, src_len);
    std::vector<uint8_t> out(2 + bound);
    out[0] = 'C';
    out[1] = 'K';
    size_t actual = libdeflate_deflate_compress(comp, src, src_len, out.data() + 2, bound);
    if (actual == 0)
        throw std::runtime_error("CabProcessor: libdeflate_deflate_compress failed");
    out.resize(2 + actual);
    return out;
}

// ─── CAB safety checks ────────────────────────────────────────────────────────

/**
 * @brief Inspect the CAB header and return false if the file should be skipped.
 *
 * Skips: multi-volume CABs, signed CABs (non-null reserved data), files with
 * per-folder or per-data reserved extensions.
 */
bool is_safe_to_recompress(const CfHeader& hdr, const CfHeaderExt& ext,
                            const uint8_t* reserve_data) {
    // Multi-volume
    if (hdr.flags & (kFlagPrevCab | kFlagNextCab)) return false;

    if (hdr.flags & kFlagReserve) {
        // Per-folder or per-data extensions → leave alone
        if (ext.cbCFFolder != 0 || ext.cbCFData != 0) return false;
        // Non-null reserved header data → likely signed
        if (reserve_data && ext.cbCFHeader > 0) {
            for (uint16_t i = 0; i < ext.cbCFHeader; ++i) {
                if (reserve_data[i] != 0) return false;
            }
        }
    }
    return true;
}

} // namespace

// ─── IProcessor interface ─────────────────────────────────────────────────────

void CabProcessor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions& /*options*/) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    const auto in_data = read_file(input_path);
    if (in_data.size() < sizeof(CfHeader))
        throw std::runtime_error("CabProcessor: file too small");

    Span r{ in_data.data(), in_data.size() };

    // ── Parse CFHEADER ────────────────────────────────────────────────────────
    CfHeader hdr{};
    r.read(&hdr, sizeof(hdr));

    if (std::memcmp(hdr.signature, kMscfSig, 4) != 0) {
        // Installshield or unrelated CAB variant — not a standard MSCF file
        throw std::runtime_error("CabProcessor: not an MSCF cabinet");
    }

    CfHeaderExt ext{};
    std::vector<uint8_t> reserve_hdr;
    if (hdr.flags & kFlagReserve) {
        r.read(&ext, sizeof(ext));
        if (ext.cbCFHeader > 0) {
            reserve_hdr.resize(ext.cbCFHeader);
            r.read(reserve_hdr.data(), ext.cbCFHeader);
        }
    }

    if (!is_safe_to_recompress(hdr, ext, reserve_hdr.empty() ? nullptr : reserve_hdr.data())) {
        Logger::log(LogLevel::Info,
            "Skipping " + input_path.filename().string() +
            ": multi-volume, signed, or extended CAB", get_name());
        // Copy input to output unchanged
        if (!write_file(output_path, in_data))
            throw std::runtime_error("CabProcessor: failed to copy input to output");
        return;
    }

    // ── Parse CFFFOLDERs ──────────────────────────────────────────────────────
    std::vector<CfFolder> folders(hdr.cFolders);
    for (uint16_t i = 0; i < hdr.cFolders; ++i) {
        r.read(&folders[i], sizeof(CfFolder));
        if (ext.cbCFFolder > 0) r.skip(ext.cbCFFolder);
    }

    // ── Read CFFILE table verbatim ────────────────────────────────────────────
    // The file table starts at coffFiles and extends to the first folder data.
    // We copy it exactly.
    size_t files_offset = hdr.coffFiles;
    size_t files_end = in_data.size(); // will be tightened below

    // Find the earliest folder data start to bound the file table
    for (const auto& f : folders) {
        if (f.coffCabStart > 0 && f.coffCabStart < files_end)
            files_end = f.coffCabStart;
    }
    if (files_offset >= in_data.size() || files_end < files_offset)
        throw std::runtime_error("CabProcessor: invalid coffFiles offset");

    std::vector<uint8_t> file_table(
        in_data.begin() + static_cast<ptrdiff_t>(files_offset),
        in_data.begin() + static_cast<ptrdiff_t>(files_end));

    // ── Recompress MSZIP folders ──────────────────────────────────────────────
    libdeflate_compressor* comp = libdeflate_alloc_compressor(kDeflateLevel);
    if (!comp) throw std::runtime_error("CabProcessor: libdeflate_alloc_compressor failed");

    struct RecompressedFolder {
        std::vector<std::vector<uint8_t>> blocks; ///< recompressed CFDATA payloads
        std::vector<uint16_t> uncomp_sizes;         ///< original uncompressed sizes per block
        uint16_t type_compress;
    };

    std::vector<RecompressedFolder> result_folders(hdr.cFolders);
    bool any_improvement = false;

    for (uint16_t fi = 0; fi < hdr.cFolders; ++fi) {
        const auto& folder = folders[fi];
        result_folders[fi].type_compress = folder.typeCompress;

        if (folder.typeCompress != kCompMszip && folder.typeCompress != kCompNone) {
            // Quantum / LZX — not handled; copy blocks as-is
            Span fr{ in_data.data(), in_data.size() };
            fr.skip(folder.coffCabStart);
            for (uint16_t bi = 0; bi < folder.cCFDATA; ++bi) {
                CfData block{};
                fr.read(&block, sizeof(block));
                if (ext.cbCFData > 0) fr.skip(ext.cbCFData);
                std::vector<uint8_t> payload(block.cbData);
                fr.read(payload.data(), block.cbData);
                result_folders[fi].blocks.push_back(std::move(payload));
                result_folders[fi].uncomp_sizes.push_back(block.cbUncomp);
            }
            continue;
        }

        Span fr{ in_data.data(), in_data.size() };
        fr.skip(folder.coffCabStart);

        for (uint16_t bi = 0; bi < folder.cCFDATA; ++bi) {
            CfData block{};
            fr.read(&block, sizeof(block));
            if (ext.cbCFData > 0) fr.skip(ext.cbCFData);

            std::vector<uint8_t> old_payload(block.cbData);
            fr.read(old_payload.data(), block.cbData);

            if (folder.typeCompress == kCompNone) {
                // Stored blocks — just copy
                result_folders[fi].blocks.push_back(old_payload);
                result_folders[fi].uncomp_sizes.push_back(block.cbUncomp);
                continue;
            }

            // MSZIP: decompress → recompress
            auto raw = mszip_decompress(old_payload.data(), old_payload.size(), block.cbUncomp);
            auto new_payload = mszip_compress(comp, raw.data(), raw.size());

            if (new_payload.size() < old_payload.size())
                any_improvement = true;

            result_folders[fi].blocks.push_back(std::move(new_payload));
            result_folders[fi].uncomp_sizes.push_back(block.cbUncomp);
        }
    }

    libdeflate_free_compressor(comp);

    if (!any_improvement) {
        Logger::log(LogLevel::Debug, "No improvement from MSZIP recompression, keeping original", get_name());
        if (!write_file(output_path, in_data))
            throw std::runtime_error("CabProcessor: failed to write output");
        return;
    }

    // ── Rebuild CAB ───────────────────────────────────────────────────────────
    // Layout: CFHEADER + CFFOLDER[n] + CFFILE_table + CFDATA_blocks…
    // (We strip the RESERVE extension to remove any empty signing space.)

    // Step 1: calculate folder coffCabStart offsets
    size_t base_hdr_size = sizeof(CfHeader) + hdr.cFolders * sizeof(CfFolder);
    size_t files_size = file_table.size();
    size_t data_start = base_hdr_size + files_size;

    std::vector<uint32_t> new_folder_offsets(hdr.cFolders);
    size_t cur_offset = data_start;
    for (uint16_t fi = 0; fi < hdr.cFolders; ++fi) {
        new_folder_offsets[fi] = static_cast<uint32_t>(cur_offset);
        for (size_t bi = 0; bi < result_folders[fi].blocks.size(); ++bi) {
            cur_offset += sizeof(CfData) + result_folders[fi].blocks[bi].size();
        }
    }
    size_t total_size = cur_offset;

    // Step 2: assemble output buffer
    std::vector<uint8_t> out;
    out.reserve(total_size);

    // CFHEADER (with updated fields, RESERVE stripped)
    CfHeader new_hdr = hdr;
    new_hdr.cbCabinet = static_cast<uint32_t>(total_size);
    new_hdr.coffFiles = static_cast<uint32_t>(base_hdr_size);
    new_hdr.flags    = static_cast<uint16_t>(hdr.flags & ~kFlagReserve); // strip RESERVE

    out.insert(out.end(),
        reinterpret_cast<const uint8_t*>(&new_hdr),
        reinterpret_cast<const uint8_t*>(&new_hdr) + sizeof(new_hdr));

    // CFFFOLDERs (updated coffCabStart)
    for (uint16_t fi = 0; fi < hdr.cFolders; ++fi) {
        CfFolder new_folder = folders[fi];
        new_folder.coffCabStart = new_folder_offsets[fi];
        out.insert(out.end(),
            reinterpret_cast<const uint8_t*>(&new_folder),
            reinterpret_cast<const uint8_t*>(&new_folder) + sizeof(new_folder));
    }

    // CFFILE table verbatim
    out.insert(out.end(), file_table.begin(), file_table.end());

    // CFDATA blocks
    for (uint16_t fi = 0; fi < hdr.cFolders; ++fi) {
        for (size_t bi = 0; bi < result_folders[fi].blocks.size(); ++bi) {
            const auto& payload = result_folders[fi].blocks[bi];
            CfData new_block{};
            new_block.csum    = 0; // omitted checksum
            new_block.cbData  = static_cast<uint16_t>(payload.size());
            new_block.cbUncomp = result_folders[fi].uncomp_sizes[bi];

            out.insert(out.end(),
                reinterpret_cast<const uint8_t*>(&new_block),
                reinterpret_cast<const uint8_t*>(&new_block) + sizeof(new_block));
            out.insert(out.end(), payload.begin(), payload.end());
        }
    }

    if (!write_file(output_path, out))
        throw std::runtime_error("CabProcessor: failed to write output");

    Logger::log(LogLevel::Debug,
        "CAB recompressed: " + std::to_string(in_data.size()) +
        " → " + std::to_string(out.size()) + " bytes",
        get_name());
}


} // namespace chisel
