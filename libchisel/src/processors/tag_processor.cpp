//
// Created by Giuseppe Francione on 11/07/26.
//

#include "../../include/tag_processor.hpp"
#include "../../include/c2pa_manifest.hpp"
#include "../../include/audio_metadata_util.hpp"

namespace chisel {
namespace fs = std::filesystem;

void TagProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
}

std::optional<ExtractedContent> TagProcessor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "tag-processor", get_name());
}

std::filesystem::path TagProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

bool TagProcessor::is_signed(const std::filesystem::path& file_path) const {
    return c2pa::riff_has_manifest(file_path);
}

} // namespace chisel
