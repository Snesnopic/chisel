//
// Created by Giuseppe Francione on 19/10/25.
//

#include "processor_registry.hpp"

// include headers for all concrete processor implementations
#include "../processors/ape_processor.hpp"        // assuming ape_processor.hpp exists
#include "../processors/archive_processor.hpp"    // assuming archive_processor.hpp exists
#include "../processors/flac_processor.hpp"
#include "../processors/flexigif_processor.hpp" // assuming flexigif_processor.hpp exists
#include "../processors/gif_processor.hpp"        // assuming gif_processor.hpp exists
#include "../processors/jpeg_processor.hpp"       // assuming jpeg_processor.hpp exists
#include "../processors/jxl_processor.hpp"        // assuming jxl_processor.hpp exists
#include "../processors/mkv_processor.hpp"        // assuming mkv_processor.hpp exists
#include "../processors/mseed_processor.hpp"      // assuming mseed_processor.hpp exists
#include "../processors/odf_processor.hpp"        // assuming odf_processor.hpp exists
#include "../processors/ooxml_processor.hpp"      // assuming ooxml_processor.hpp exists
#include "../processors/pdf_processor.hpp"        // assuming pdf_processor.hpp exists
#include "../processors/png_processor.hpp"        // assuming png_processor.hpp exists
#include "../processors/sqlite_processor.hpp"     // assuming sqlite_processor.hpp exists
#include "../processors/tiff_processor.hpp"       // assuming tiff_processor.hpp exists
#include "../processors/wavpack_processor.hpp"    // assuming wavpack_processor.hpp exists
#include "../processors/webp_processor.hpp"       // assuming webp_processor.hpp exists
#include "../processors/zopflipng_processor.hpp"  // assuming zopflipng_processor.hpp exists

#include <algorithm> // for std::find_if

namespace chisel {

ProcessorRegistry::ProcessorRegistry() {
    // register all concrete processor instances (or factories) here.
    // the order might matter if multiple processors claim the same mime/extension,
    // as find_by_* currently returns the first match.
    // using make_unique ensures proper memory management.

    processors_.push_back(std::make_unique<FlacProcessor>());
    processors_.push_back(std::make_unique<WavpackProcessor>()); // assuming WavPackProcessor exists
    processors_.push_back(std::make_unique<ApeProcessor>());     // assuming ApeProcessor exists
    processors_.push_back(std::make_unique<JpegProcessor>());    // assuming JpegProcessor exists
    processors_.push_back(std::make_unique<PngProcessor>());     // assuming PngProcessor exists
    processors_.push_back(std::make_unique<ZopflipngProcessor>());// assuming ZopfliPngProcessor exists
    processors_.push_back(std::make_unique<WebpProcessor>());    // assuming WebpProcessor exists
    processors_.push_back(std::make_unique<GifProcessor>());     // assuming GifProcessor exists
    processors_.push_back(std::make_unique<FlexigifProcessor>());// assuming FlexiGifProcessor exists
    processors_.push_back(std::make_unique<TiffProcessor>());    // assuming TiffProcessor exists
    processors_.push_back(std::make_unique<JxlProcessor>());     // assuming JxlProcessor exists
    processors_.push_back(std::make_unique<PdfProcessor>());     // assuming PdfProcessor exists
    processors_.push_back(std::make_unique<ArchiveProcessor>()); // assuming ArchiveProcessor exists (for zip, tar, etc.)
    processors_.push_back(std::make_unique<OOXMLProcessor>());   // assuming OoxmlProcessor exists (for docx, xlsx, pptx)
    processors_.push_back(std::make_unique<OdfProcessor>());     // assuming OdfProcessor exists (for odt, ods, odp)
    processors_.push_back(std::make_unique<SqliteProcessor>());  // assuming SqliteProcessor exists
    processors_.push_back(std::make_unique<MseedProcessor>());   // assuming MseedProcessor exists
    processors_.push_back(std::make_unique<MkvProcessor>());     // assuming MkvProcessor exists

    // add new processors here as they are implemented
}

// finds the first registered processor that supports the given mime type.
[[nodiscard]] std::optional<IProcessor*> ProcessorRegistry::find_by_mime(const std::string& mime) const {
    // iterate through all registered processors
    for (const auto& proc_ptr : processors_) {
        // check the list of mime types supported by the current processor
        for (const auto supported_mime : proc_ptr->get_supported_mime_types()) {
            // return a non-owning pointer if a match is found
            if (supported_mime == mime) {
                return proc_ptr.get();
            }
        }
    }
    // return nullopt if no processor supports this mime type
    return std::nullopt;
}

// finds the first registered processor that supports the given file extension.
// performs a case-insensitive comparison.
[[nodiscard]] std::optional<IProcessor*> ProcessorRegistry::find_by_extension(const std::string& ext) const {
    if (ext.empty() || ext[0] != '.') {
        // extensions are expected to start with a dot
        return std::nullopt;
    }

    // simple case-insensitive comparison helper
    auto iequals = [](std::string_view s1, std::string_view s2) {
        return std::equal(s1.begin(), s1.end(), s2.begin(), s2.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    };

    // iterate through all registered processors
    for (const auto& proc_ptr : processors_) {
        // check the list of extensions supported by the current processor
        for (const auto supported_ext : proc_ptr->get_supported_extensions()) {
            // perform case-insensitive comparison
            if (iequals(supported_ext, ext)) {
                // return a non-owning pointer if a match is found
                return proc_ptr.get();
            }
        }
    }
    // return nullopt if no processor supports this extension
    return std::nullopt;
}

} // namespace chisel