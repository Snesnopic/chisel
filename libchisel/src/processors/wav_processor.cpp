//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/wav_processor.hpp"
#include "../../include/audio_metadata_util.hpp"

namespace chisel {
namespace fs = std::filesystem;

void WavProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
}

std::optional<ExtractedContent> WavProcessor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "wav-processor", get_name());
}

std::filesystem::path WavProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

} // namespace chisel
