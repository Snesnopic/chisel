//
// Created by Giuseppe Francione on 19/11/25.
//

#include "../../include/aiff_processor.hpp"
#include "../../include/audio_metadata_util.hpp"

namespace chisel {
namespace fs = std::filesystem;

void AiffProcessor::recompress(const fs::path& input,
                               const fs::path& output, const ProcessingOptions &options) {
    AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
}

std::optional<ExtractedContent> AiffProcessor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "aiff-processor", get_name());
}

std::filesystem::path AiffProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

} // namespace chisel
