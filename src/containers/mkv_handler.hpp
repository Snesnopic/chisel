//
// created by giuseppe francione on 25/09/25
//

#ifndef CHISEL_MKV_HANDLER_HPP
#define CHISEL_MKV_HANDLER_HPP

#include <string>
#include <vector>
#include <map>
#include "container.hpp"
#include "../cli/cli_parser.hpp"

// type of stream inside a matroska container
enum class TrackType {
    Audio,
    Video,
    Subtitle,
    Attachment,
    Unknown
};

// information about an extracted track or attachment
struct TrackInfo {
    std::string path;                          // temporary file path
    TrackType type = TrackType::Unknown;       // stream type
    int stream_index = -1;                     // original stream index
    std::string codec_name;                    // codec name, e.g. "flac", "alac", "subrip"
    std::map<std::string, std::string> metadata; // per-stream metadata
    std::string mime_type;                     // useful for attachments (e.g. image/jpeg, font/ttf)
};

// chapter information
struct ChapterInfo {
    int id = -1;                               // chapter id
    int64_t start_time = 0;                    // start timestamp
    int64_t end_time = 0;                      // end timestamp
    std::map<std::string, std::string> metadata; // chapter metadata (titles, languages)
};

// extended container job for matroska
struct MkvJob : ContainerJob {
    std::vector<TrackInfo> tracks;             // extracted tracks and attachments
    std::map<std::string, std::string> global_metadata; // global metadata (title, artist, etc.)
    std::vector<ChapterInfo> chapters;         // chapter list
    std::map<std::string, std::string> tags;   // global tags
};

class MkvHandler : public IContainer {
public:
    ContainerJob prepare(const std::filesystem::path& path) override;

    bool finalize(const ContainerJob &job, Settings& settings) override;

private:
    // helper to extract tracks, attachments, metadata and chapters using ffmpeg
    static bool extract_with_ffmpeg(const std::filesystem::path& mkv_path, MkvJob& job);

    // helper to rebuild mkv with recompressed streams and preserved metadata
    static bool create_with_ffmpeg(const MkvJob& job, const std::filesystem::path& out_path, Settings& settings);
};

#endif // CHISEL_MKV_HANDLER_HPP