//
// Created by Giuseppe Francione on 24/09/26.
//

/**
 * @file c2pa_manifest.hpp
 * @brief Detection of C2PA manifests embedded where each container format keeps them.
 */

#ifndef CHISEL_C2PA_MANIFEST_HPP
#define CHISEL_C2PA_MANIFEST_HPP

#include <filesystem>

namespace chisel::c2pa {

/**
 * @brief Checks an ISO BMFF file (MP4, MOV, M4A, HEIF...) for a top-level C2PA uuid box.
 */
bool bmff_has_manifest(const std::filesystem::path& path);

/**
 * @brief Checks a RIFF file (WAV, AVI, WebP) for a top-level C2PA chunk.
 */
bool riff_has_manifest(const std::filesystem::path& path);

/**
 * @brief Checks a TIFF or BigTIFF file for the C2PA tag (52545) in any IFD of its main chain.
 */
bool tiff_has_manifest(const std::filesystem::path& path);

/**
 * @brief Checks a JPEG XL container for a JUMBF box holding a C2PA manifest store.
 */
bool jxl_has_manifest(const std::filesystem::path& path);

/**
 * @brief Checks the ID3v2 tag at the start of a file (e.g. MP3) for a C2PA GEOB frame.
 */
bool id3_has_manifest(const std::filesystem::path& path);

/**
 * @brief Checks a GIF file for the C2PA application extension.
 */
bool gif_has_manifest(const std::filesystem::path& path);

} // namespace chisel::c2pa

#endif // CHISEL_C2PA_MANIFEST_HPP
