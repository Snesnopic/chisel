//
// Created by Giuseppe Francione on 19/10/25.
//

#include "processor_registry.hpp"
#include "aiff_processor.hpp"
#include "ape_processor.hpp"
#include "archive_processor.hpp"
#include "bmp_processor.hpp"
#include "flac_processor.hpp"
#include "flexigif_processor.hpp"
#include "gif_processor.hpp"
#include "jpeg_processor.hpp"
#include "jxl_processor.hpp"
#include "mkv_processor.hpp"
#include "mp4_processor.hpp"
#include "mpeg_processor.hpp"
#include "mseed_processor.hpp"
#include "odf_processor.hpp"
#include "ogg_processor.hpp"
#include "ooxml_processor.hpp"
#include "pdf_processor.hpp"
#include "png_processor.hpp"
#include "pnm_processor.hpp"
#include "sqlite_processor.hpp"
#include "tiff_processor.hpp"
#include "tga_processor.hpp"
#include "xml_processor.hpp"
#include "wav_processor.hpp"
#include "wavpack_processor.hpp"
#include "webp_processor.hpp"
#include "zopflipng_processor.hpp"
#include "woff2_processor.hpp"
#include "brotli_processor.hpp"
#include "asf_processor.hpp"
#include "dsdiff_processor.hpp"
#include "dsf_processor.hpp"
#include "mpc_processor.hpp"
#include "tta_processor.hpp"
#include "cfbf_processor.hpp"
#include "lzma_processor.hpp"
#include "zstd_processor.hpp"
#include "bzip2_processor.hpp"
#include "ico_processor.hpp"
#include "swf_processor.hpp"
#include "gft_processor.hpp"
#include "rdb_processor.hpp"
#include "icns_processor.hpp"
#include "woff_processor.hpp"
#include "kanzi_processor.hpp"
#include "vcf_processor.hpp"
#include "pe_processor.hpp"
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
    processors_.push_back(std::make_unique<GifProcessor>());
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
    processors_.push_back(std::make_unique<PnmProcessor>());
    processors_.push_back(std::make_unique<XmlProcessor>());
    processors_.push_back(std::make_unique<Woff2Processor>());
    processors_.push_back(std::make_unique<BrotliProcessor>());
    processors_.push_back(std::make_unique<AsfProcessor>());
    processors_.push_back(std::make_unique<DsdiffProcessor>());
    processors_.push_back(std::make_unique<DsfProcessor>());
    processors_.push_back(std::make_unique<MpcProcessor>());
    processors_.push_back(std::make_unique<TtaProcessor>());
    processors_.push_back(std::make_unique<CfbfProcessor>());
    processors_.push_back(std::make_unique<LzmaProcessor>());
    processors_.push_back(std::make_unique<ZstdProcessor>());
    processors_.push_back(std::make_unique<Bzip2Processor>());
    processors_.push_back(std::make_unique<IcoProcessor>());
    processors_.push_back(std::make_unique<SwfProcessor>());
    processors_.push_back(std::make_unique<GftProcessor>());
    processors_.push_back(std::make_unique<RdbProcessor>());
    processors_.push_back(std::make_unique<IcnsProcessor>());
    processors_.push_back(std::make_unique<WoffProcessor>());
    processors_.push_back(std::make_unique<KanziProcessor>());
    processors_.push_back(std::make_unique<VcfProcessor>());
    processors_.push_back(std::make_unique<PeProcessor>());
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

bool ProcessorRegistry::supports_mime(const std::string& mime) const {
    const std::string_view mime_view = mime;

    return std::ranges::any_of(processors_, [mime_view](const auto& proc) {
        const auto mimes = proc->get_supported_mime_types();
        return std::ranges::find(mimes, mime_view) != mimes.end();
    });
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