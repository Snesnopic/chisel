//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/processor_registry.hpp"
#include "../../include/aiff_processor.hpp"
#include "../../include/ape_processor.hpp"
#include "../../include/archive_processor.hpp"
#include "../../include/bmp_processor.hpp"
#include "../../include/flac_processor.hpp"
#include "../../include/flexigif_processor.hpp"
#include "../../include/gif_processor.hpp"
#include "../../include/jpeg_processor.hpp"
#include "../../include/jxl_processor.hpp"
#include "../../include/mkv_processor.hpp"
#include "../../include/mp4_processor.hpp"
#include "../../include/mpeg_processor.hpp"
#include "../../include/mseed_processor.hpp"
#include "../../include/odf_processor.hpp"
#include "../../include/ogg_processor.hpp"
#include "../../include/ooxml_processor.hpp"
#include "../../include/pdf_processor.hpp"
#include "../../include/png_processor.hpp"
#include "../../include/sqlite_processor.hpp"
#include "../../include/tiff_processor.hpp"
#include "../../include/tga_processor.hpp"
#include "../../include/wav_processor.hpp"
#include "../../include/wavpack_processor.hpp"
#include "../../include/webp_processor.hpp"
#include "../../include/zopflipng_processor.hpp"
#include <algorithm>
#include <cctype>

namespace chisel {

ProcessorRegistry::ProcessorRegistry() {
    processors_.push_back(std::make_unique<FlacProcessor>());
    processors_.push_back(std::make_unique<WavPackProcessor>());
    processors_.push_back(std::make_unique<ApeProcessor>());
    processors_.push_back(std::make_unique<JpegProcessor>());
    processors_.push_back(std::make_unique<PngProcessor>());
    processors_.push_back(std::make_unique<ZopfliPngProcessor>());
    processors_.push_back(std::make_unique<WebpProcessor>());
    // processors_.push_back(std::make_unique<GifProcessor>());
    processors_.push_back(std::make_unique<TgaProcessor>());
    processors_.push_back(std::make_unique<FlexiGifProcessor>());
    processors_.push_back(std::make_unique<TiffProcessor>());
    processors_.push_back(std::make_unique<JxlProcessor>());
    processors_.push_back(std::make_unique<PdfProcessor>());
    processors_.push_back(std::make_unique<ArchiveProcessor>());
    processors_.push_back(std::make_unique<OOXMLProcessor>());
    processors_.push_back(std::make_unique<OdfProcessor>());
    processors_.push_back(std::make_unique<SqliteProcessor>());
    processors_.push_back(std::make_unique<MseedProcessor>());
    processors_.push_back(std::make_unique<MkvProcessor>());
    processors_.push_back(std::make_unique<MpegProcessor>());
    processors_.push_back(std::make_unique<WavProcessor>());
    processors_.push_back(std::make_unique<Mp4Processor>());
    processors_.push_back(std::make_unique<OggProcessor>());
    processors_.push_back(std::make_unique<AiffProcessor>());
    processors_.push_back(std::make_unique<BmpProcessor>());
}

std::vector<IProcessor*> ProcessorRegistry::find_by_mime(const std::string& mime) const {
    std::vector<IProcessor*> result;
    for (const auto& proc_ptr : processors_) {
        for (const auto supported_mime : proc_ptr->get_supported_mime_types()) {
            if (supported_mime == mime) {
                result.push_back(proc_ptr.get());
            }
        }
    }
    return result;
}

std::vector<IProcessor*> ProcessorRegistry::find_by_extension(const std::string& ext) const {
    std::vector<IProcessor*> result;
    if (ext.empty() || ext[0] != '.') return result;

    auto iequals = [](const std::string_view s1, const std::string_view s2) {
        return std::equal(s1.begin(), s1.end(), s2.begin(), s2.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    };

    for (const auto& proc_ptr : processors_) {
        for (const auto supported_ext : proc_ptr->get_supported_extensions()) {
            if (iequals(supported_ext, ext)) {
                result.push_back(proc_ptr.get());
            }
        }
    }
    return result;
}

} // namespace chisel