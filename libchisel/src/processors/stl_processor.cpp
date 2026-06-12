//
// Created by Giuseppe Francione on 11/06/26.
//

#include "../../include/stl_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cmath>

/**
 * @file stl_processor.cpp
 * @brief Lossless ASCII→Binary STL converter.
 *
 * STL Binary layout:
 *   80 bytes  — free-form header (often contains authoring software name)
 *    4 bytes  — uint32_le  number of triangles
 *   per triangle (50 bytes):
 *     12 bytes — float32[3]  normal vector (nx, ny, nz)
 *     12 bytes — float32[3]  vertex 1 (x1, y1, z1)
 *     12 bytes — float32[3]  vertex 2 (x2, y2, z2)
 *     12 bytes — float32[3]  vertex 3 (x3, y3, z3)
 *      2 bytes — uint16      attribute byte count (0 = standard)
 *
 * STL ASCII starts with the keyword "solid" (optionally followed by a name).
 * We detect ASCII vs. binary by checking whether the first 5 printable bytes
 * spell "solid" AND the file is not a valid binary STL (binary files can also
 * start with "solid" in the 80-byte header, but in that case the uint32 at
 * offset 80 will correspond to a triangle count that is consistent with the
 * file size).
 */

namespace chisel {

namespace {

#pragma pack(push, 1)
struct StlTriangle {
    float normal[3];
    float v1[3];
    float v2[3];
    float v3[3];
    uint16_t attr;
};
#pragma pack(pop)

static_assert(sizeof(StlTriangle) == 50, "StlTriangle must be 50 bytes");

constexpr size_t kBinaryHeaderSize = 80;
constexpr size_t kBinaryTriangleSize = 50;

/**
 * @brief Heuristic: returns true if the data looks like an ASCII STL.
 *
 * An ASCII STL starts with the token "solid" (case-insensitive, possibly
 * preceded by a BOM or whitespace).  A binary STL can also start with
 * "solid" in its 80-byte header, so we disambiguate by checking the
 * uint32 triangle count at offset 80 against the actual file size.
 */
bool is_ascii_stl(const std::vector<uint8_t>& data) {
    if (data.size() < 5) return false;

    // Check for "solid" at the start (after optional whitespace/BOM)
    size_t start = 0;
    // skip BOM if present
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        start = 3;

    // tolower comparison of first 5 non-space chars
    size_t idx = start;
    while (idx < data.size() && std::isspace(static_cast<unsigned char>(data[idx]))) ++idx;

    const std::string_view kSolid = "solid";
    if (idx + 5 > data.size()) return false;
    for (size_t i = 0; i < 5; ++i) {
        if (std::tolower(static_cast<unsigned char>(data[idx + i])) != kSolid[i])
            return false;
    }

    // File starts with "solid" — now check if it's actually binary
    // A valid binary STL has size == 80 + 4 + N*50
    if (data.size() > kBinaryHeaderSize + 4) {
        uint32_t n_tri;
        std::memcpy(&n_tri, data.data() + kBinaryHeaderSize, 4);
        // On a little-endian host this is straightforward; on big-endian we
        // would need to byte-swap, but STL is defined as little-endian and
        // chisel targets x86/ARM little-endian platforms.
        size_t expected = kBinaryHeaderSize + 4 + static_cast<size_t>(n_tri) * kBinaryTriangleSize;
        if (expected == data.size()) {
            // Perfect binary match — it is binary despite the "solid" header
            return false;
        }
    }

    return true;
}

/**
 * @brief Parse a float from an ASCII token.
 */
float parse_float(const std::string& tok) {
    try { return std::stof(tok); }
    catch (...) {
        throw std::runtime_error("StlProcessor: invalid float token: " + tok);
    }
}

/**
 * @brief Parse an ASCII STL into a list of triangles.
 *
 * The ASCII grammar is:
 *   solid [name]
 *     facet normal nx ny nz
 *       outer loop
 *         vertex x y z
 *         vertex x y z
 *         vertex x y z
 *       endloop
 *     endfacet
 *   ...
 *   endsolid [name]
 */
std::vector<StlTriangle> parse_ascii_stl(const std::vector<uint8_t>& data) {
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream ss(text);
    std::string token;

    // Consume "solid [name]"
    if (!(ss >> token) || token != "solid") // case-sensitive after our heuristic
    {
        // Re-try case-insensitively
        std::string lower_tok = token;
        for (char& c : lower_tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower_tok != "solid")
            throw std::runtime_error("StlProcessor: ASCII STL does not start with 'solid'");
    }

    // Skip remainder of "solid" line (name)
    std::string line;
    std::getline(ss, line);

    std::vector<StlTriangle> triangles;

    while (ss >> token) {
        std::string lower = token;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower == "endsolid") break;

        if (lower != "facet")
            throw std::runtime_error("StlProcessor: expected 'facet', got '" + token + "'");

        StlTriangle tri{};

        // "normal nx ny nz"
        std::string norm_kw;
        ss >> norm_kw >> tri.normal[0] >> tri.normal[1] >> tri.normal[2];

        // "outer loop"
        std::string kw1, kw2;
        ss >> kw1 >> kw2;

        // three vertices
        for (int v = 0; v < 3; ++v) {
            std::string vkw;
            float* dst = (v == 0) ? tri.v1 : (v == 1) ? tri.v2 : tri.v3;
            ss >> vkw >> dst[0] >> dst[1] >> dst[2];
        }

        // "endloop"
        ss >> kw1;
        // "endfacet"
        ss >> kw1;

        tri.attr = 0;
        triangles.push_back(tri);
    }

    if (!ss && triangles.empty())
        throw std::runtime_error("StlProcessor: failed to parse any triangles from ASCII STL");

    return triangles;
}

/**
 * @brief Write a binary STL to a buffer.
 */
std::vector<uint8_t> write_binary_stl(const std::vector<StlTriangle>& triangles,
                                      const uint8_t header[kBinaryHeaderSize],
                                      bool preserve_header) {
    std::vector<uint8_t> out;
    out.reserve(kBinaryHeaderSize + 4 + triangles.size() * kBinaryTriangleSize);

    // 80-byte header
    if (preserve_header && header != nullptr) {
        out.insert(out.end(), header, header + kBinaryHeaderSize);
    } else {
        out.resize(out.size() + kBinaryHeaderSize, 0x00);
    }

    // Triangle count (little-endian uint32)
    uint32_t n = static_cast<uint32_t>(triangles.size());
    out.push_back(static_cast<uint8_t>(n));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n >> 16));
    out.push_back(static_cast<uint8_t>(n >> 24));

    // Triangle data
    for (const auto& tri : triangles) {
        out.insert(out.end(),
            reinterpret_cast<const uint8_t*>(&tri),
            reinterpret_cast<const uint8_t*>(&tri) + sizeof(tri));
    }

    return out;
}

/**
 * @brief Read all triangles from a binary STL.
 */
std::vector<StlTriangle> read_binary_triangles(const std::vector<uint8_t>& data) {
    if (data.size() < kBinaryHeaderSize + 4)
        throw std::runtime_error("StlProcessor: binary STL too small");

    uint32_t n_tri;
    std::memcpy(&n_tri, data.data() + kBinaryHeaderSize, 4);

    size_t expected = kBinaryHeaderSize + 4 + static_cast<size_t>(n_tri) * kBinaryTriangleSize;
    if (data.size() < expected)
        throw std::runtime_error("StlProcessor: truncated binary STL");

    std::vector<StlTriangle> out(n_tri);
    std::memcpy(out.data(), data.data() + kBinaryHeaderSize + 4, n_tri * kBinaryTriangleSize);
    return out;
}

/**
 * @brief Compare two triangle lists for geometric equality.
 *
 * Normals are allowed to differ slightly because the ASCII parser may
 * introduce floating-point rounding, but vertex coordinates must match
 * exactly (they are stored as float32 in ASCII, which round-trips exactly
 * through strtof → float → same bits).
 */
bool triangles_equal(const std::vector<StlTriangle>& a, const std::vector<StlTriangle>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        // Compare vertices exactly
        if (std::memcmp(a[i].v1, b[i].v1, 12) != 0) return false;
        if (std::memcmp(a[i].v2, b[i].v2, 12) != 0) return false;
        if (std::memcmp(a[i].v3, b[i].v3, 12) != 0) return false;
        // Normals: compare with a small ULP tolerance
        for (int j = 0; j < 3; ++j) {
            if (std::fabs(a[i].normal[j] - b[i].normal[j]) > 1e-5f) return false;
        }
    }
    return true;
}

} // namespace

// ─── IProcessor interface ─────────────────────────────────────────────────────

void StlProcessor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions& options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    const auto in_data = read_file(input_path);

    if (is_ascii_stl(in_data)) {
        Logger::log(LogLevel::Debug, "Detected ASCII STL, converting to binary", get_name());
        const auto triangles = parse_ascii_stl(in_data);
        const auto out_data = write_binary_stl(triangles, nullptr, false);

        if (!write_file(output_path, out_data))
            throw std::runtime_error("StlProcessor: failed to write output");

        Logger::log(LogLevel::Debug,
            "ASCII→Binary STL: " + std::to_string(in_data.size()) +
            " → " + std::to_string(out_data.size()) + " bytes (" +
            std::to_string(triangles.size()) + " triangles)",
            get_name());
    } else {
        // Already binary — optionally zero the header metadata field
        if (in_data.size() < kBinaryHeaderSize + 4) {
            throw std::runtime_error("StlProcessor: binary STL is too small to be valid");
        }

        auto out_data = in_data;
        if (!options.preserve_metadata) {
            // Zero the 80-byte free-form header to remove authoring tool info
            std::memset(out_data.data(), 0, kBinaryHeaderSize);
        }

        if (!write_file(output_path, out_data))
            throw std::runtime_error("StlProcessor: failed to write output");

        Logger::log(LogLevel::Debug, "Binary STL header sanitised", get_name());
    }
}

bool StlProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto da = read_file(a);
        const auto db = read_file(b);

        auto get_triangles = [](const std::vector<uint8_t>& data) {
            if (is_ascii_stl(data)) return parse_ascii_stl(data);
            return read_binary_triangles(data);
        };

        const auto ta = get_triangles(da);
        const auto tb = get_triangles(db);
        return triangles_equal(ta, tb);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("raw_equal failed: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel
