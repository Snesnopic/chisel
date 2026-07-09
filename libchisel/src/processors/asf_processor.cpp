//
// Created by Giuseppe Francione on 24/03/26.
//

#include "../../include/asf_processor.hpp"
#include "../../include/audio_metadata_util.hpp"

namespace chisel {
namespace fs = std::filesystem;

void AsfProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
}

std::optional<ExtractedContent> AsfProcessor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "asf-processor", get_name());
}

std::filesystem::path AsfProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

} // namespace chisel
