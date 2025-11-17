//
// Created by Giuseppe Francione on 17/11/25.
//

#ifndef CHISEL_AUDIO_METADATA_UTIL_HPP
#define CHISEL_AUDIO_METADATA_UTIL_HPP

#include <filesystem>
#include <string>
#include <vector>
#include <any>

namespace chisel {

/**
 * @brief Neutral, format-agnostic cover art metadata.
 *
 * This struct is the single source of truth across the pipeline:
 * - Extractors populate it from container-specific tags/blocks.
 * - Image optimizers modify temp_file_path contents.
 * - Rebuilders read from it and reconstruct container-specific cover entries.
 *
 * Notes:
 * - width/height/depth/colors are calculated at reinsertion time from the final image
 * when required by formats like FLAC/OGG (METADATA_BLOCK_PICTURE).
 * - picture_type is a semantic role (front/back/leaflet...), normalized to int.
 * Map container-specific enums (FLAC::Picture::Type, ID3v2 APIC type, etc.) to this field.
 * - format_specific can carry container-specific details when needed
 * (e.g., MP4::CoverArt::Format, APE custom tag keys).
 */
struct AudioCoverInfo {
    // path to the extracted/optimized image
    std::filesystem::path temp_file_path;

    // image identity
    std::string mime_type;     // e.g., "image/jpeg", "image/png"
    std::string description;   // e.g., "Front cover"

    // semantic role (normalized)
    int picture_type = 0;      // map container-specific types to an agreed integer domain

    // image technical metadata (computed at reinsertion when required)
    int width = 0;             // pixels
    int height = 0;            // pixels
    int depth = 0;             // bits per pixel/channel configuration
    int colors = 0;            // optional; rarely used

    // container-specific extras (optional, type-erased)
    std::any format_specific;  // e.g., MP4::CoverArt::Format, custom APE tag info
};

/**
 * @brief Aggregated state for all covers from a single audio file.
 *
 * Preserve order to guarantee identical reconstruction when required.
 */
struct AudioExtractionState {
    std::vector<AudioCoverInfo> extracted_covers;
};

/**
 * @brief Minimal utility for extracting and reinserting cover art from audio files.
 *
 * Responsibilities:
 * - Extract: Read all embedded images from an audio container at input_path,
 * write them to temp_dir, and return normalized metadata in AudioExtractionState.
 * - Rebuild: Remove existing pictures and reinsert covers from state,
 * computing technical fields (width/height/depth/colors) on the fly when required.
 *
 * Supported containers are decided by implementation (TagLib-based detection + format-specific code).
 * Extend without changing the neutral metadata schema.
 */
class AudioMetadataUtil {
public:
    /**
     * @brief Extract all cover art from an audio file into a temporary directory.
     * @param input_path Path to the audio container file.
     * @param temp_dir Directory where extracted images will be written.
     * @return AudioExtractionState holding normalized metadata for all covers.
     */
    static AudioExtractionState extractCovers(const std::filesystem::path& input_path,
                                              const std::filesystem::path& temp_dir);

    /**
     * @brief Reinsert (replace) cover art in an audio file from a previously extracted state.
     *
     * Implementations should:
     * - Remove existing covers.
     * - Read final bytes from temp_file_path for each cover.
     * - Compute width/height/depth/colors from the final image when required (e.g., FLAC/OGG).
     * - Map picture_type and description back to container-specific fields.
     * - Use format_specific where necessary (e.g., MP4 JPEG/PNG selection).
     *
     * @param input_path Path to the audio container file to modify in place.
     * @param state Normalized metadata and paths to final images to embed.
     * @return true on success; false on failure.
     */
    static bool rebuildCovers(const std::filesystem::path& input_path,
                              const AudioExtractionState& state);
};

} // namespace chisel

#endif // CHISEL_AUDIO_METADATA_UTIL_HPP
