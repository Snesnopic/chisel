//
// Created by Giuseppe Francione on 21/10/26.
//

/**
 * @file mkv_tracks.hpp
 * @brief Reads what a Matroska file's tracks hold, frame by frame, without libmatroska.
 *
 * Kept independent of the libraries that rewrite Matroska files, so that it can tell whether a rewrite
 * kept every frame of every track.
 */
#ifndef CHISEL_MKV_TRACKS_HPP
#define CHISEL_MKV_TRACKS_HPP

#include <filesystem>
#include <string>

namespace chisel {

struct MkvTrackComparison {
    bool source_read = false;  ///< false when the source itself couldn't be read, so nothing was compared
    std::string difference;    ///< empty when every track holds the same frames
};

/**
 * @brief Compares the tracks of two Matroska files: codec, codec private data, every frame in order,
 * block additions and discard padding.
 */
MkvTrackComparison compare_mkv_tracks(const std::filesystem::path& source, const std::filesystem::path& result);

} // namespace chisel

#endif // CHISEL_MKV_TRACKS_HPP
