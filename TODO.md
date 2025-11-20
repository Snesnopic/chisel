# TODO

## Testing

- [ ] Test edge cases: corrupted files, unsupported formats, empty files, mismatched extensions.
- [ ] Validate metadata preservation across formats (cover art, tags, chapters).
- [ ] Improve resiliency of mime detection, maybe enforce magic db regeneration on first use.
- [ ] Validate PNM binary conversion (ensure ASCII P1-P3 inputs are correctly converted to Binary P4-P6).

## Refactoring / Architecture

- [ ] Normalize MIME detection and extension mapping.
- [ ] Implement a UriProcessor to detect and process embedded data URIs (e.g. data:image/*;base64) in HTML, CSS, JS, XML, SVG. Extract, decode, optimize via existing processors, and reinsert re-encoded content.
- [ ] Review and clean up unused or redundant CMake variables.
- [ ] Complete refactoring of third-party library integrations (libFLAC, libwavpack) to use `FILE*` or callback-based APIs instead of filenames, ensuring full Unicode support on Windows.

## WavPack

- [ ] Validate complete tag copying (ReplayGain, cuesheet, etc.).
- [ ] Add tests with both `.wv` and `.wvc` inputs.
- [ ] Implement brute-force recompression across compression modes and select the smallest output.

## JPEG

- [ ] Support optional stripping of EXIF/ICC metadata.
- [ ] Benchmark compression ratio vs. `jpegtran`.
- [ ] Integrate multiple JPEG optimizers (`jpegtran`, `jpegoptim`, `jpeg-recompress`, `guetzli`, `cjpegli`) and select the best result.

## WebP

- [ ] Improve `WebpEncoder` with advanced lossless options (`-m 6`, `-q 100`).
- [ ] Support removal of non-essential chunks (XMP, ICC).

## GIF

- [ ] Plan a fork of `gifsicle` to refactor away global variables and enable true multithreaded GifProcessor execution.

## PDF

- [ ] Investigate **pdfsizeopt** techniques (image recompression, font unification, metadata stripping)  
  ↳ <https://github.com/pts/pdfsizeopt>

## Archives

- [ ] Remove bzip2 from testing and libarchive supported archives.
- [ ] Add support for 7Z recompression using 7zip SDK.
- [ ] Investigate integration of BestCFBF (<https://papas-best.com/downloads/bestcfbf/stable/bestcfbf.cpp>) for optimizing MSI, DOC, PPT, XLS:
  - On Windows: adapt and integrate directly with COM Structured Storage APIs.
  - On Linux/macOS: research alternative libraries (e.g. libgsf, olefile) to replicate functionality.
- [ ] Explore Leanify-style handling of container formats that are essentially ZIP/LZMA/Deflate (APK, XPS, XPInstall, EPUB, DOCX, ODT, etc.) and integrate similar recursive optimization.
- [ ] Investigate **advmng** for MNG recompression (delta compression, ancillary chunk removal)  
  ↳ <https://www.advancemame.it/doc-advmng>
- [ ] Rewrite hardlink handling in archive_processor with a cross-platform approach, since current implementation is not available on Windows.

## MKV / Matroska

- [ ] Preserve chapters, tags, and attachments (e.g. fonts, cover art).
- [ ] Finish Matroska container support (currently unfinished).

## New MIME types / Codecs

- [ ] MP3 – integrate `mp3packer` for frame-level repacking.  
  ↳ <https://github.com/da-x/mp3packer>
- [ ] ALAC – investigate integration via libavcodec or standalone decoder.
- [ ] TAK – closed source, not feasible (note).
- [ ] LA (Lossless Audio) – abandoned, not feasible (note).
- [ ] TTA (The True Audio) – integrate open source library.  
  ↳ <https://github.com/stseelig/libttaR>
- [ ] MPEG‑4 ALS – investigate reference implementation.  
  ↳ <https://www.iso.org/standard/43345.html>
- [ ] Ogg Vorbis – investigate recompression techniques (codebook optimization) like `OptiVorbis` (Rust).  
  ↳ <https://github.com/fhanau/optivorbis>
- [ ] Lepton (Rust JPEG recompressor) – consider FFI integration.  
  ↳ <https://github.com/dropbox/lepton> (original C++), <https://github.com/microsoft/lepton_jpeg_rust>
- [ ] WOFF/WOFF2 – recompression via zlib/Brotli.  
  ↳ <https://www.w3.org/TR/WOFF2/>
- [ ] SWF – recompress embedded zlib/LZMA streams (legacy, low priority).  
  ↳ <https://en.wikipedia.org/wiki/SWF>
- [ ] STL – ASCII recompression, binary deduplication of triangles.  
  ↳ <https://en.wikipedia.org/wiki/STL_(file_format)>
- [ ] PCX – palette optimization and RLE recompression.  
  ↳ <https://en.wikipedia.org/wiki/PCX>
- [ ] ICO – optimize embedded PNG/BMP.  
  ↳ <https://en.wikipedia.org/wiki/ICO_(file_format)>
- [ ] SVG/XML/HTML/CSS/JS – minification and metadata stripping.  
  ↳ <https://www.w3.org/Graphics/SVG/>
- [ ] FB2 – FictionBook container optimization.  
  ↳ <https://en.wikipedia.org/wiki/FictionBook>
- [ ] MPEG1/2 – investigate Ocarina recompression approach.  
  ↳ <https://encode.su/threads/1111-Ocarina-s-MPEG1-and-MPEG2-video-compressor>
- [ ] H.264 – investigate lossless recompression (Pied Piper / losslessh264).  
  ↳ <https://encode.su/threads/2285-H264-Lossless-recompression-Pied-Piper-(losslessh264)>
- [ ] Sound Slimmer – investigate MP3/AAC archival recompression concepts.  
  ↳ <https://audiophilesoft.com/load/junk/sound_slimmer_v1_04_001/9-1-0-61>
- [ ] Executables (PE/EXE/DLL) – optional Leanify-style recompression.  
  ↳ <https://en.wikipedia.org/wiki/Portable_Executable>
- [ ] Lua bytecode – optional Leanify-style recompression.  
  ↳ <https://www.lua.org/manual/5.4/manual.html#6.4>
- [ ] RDB, GFT – niche formats, investigate feasibility.
- [ ] WebAssembly – integrate wasm-opt (Binaryen).  
  ↳ <https://github.com/WebAssembly/binaryen>
- [ ] HTML/XML – integrate tidy-html5 for cleanup/minification.  
  ↳ <https://github.com/htacg/tidy-html5>
- [ ] HDR (Radiance RGBE) – add support for HDR file compression using stb_image/stb_image_write.  
  ↳ <https://github.com/nothings/stb>
- [ ] OpenEXR – integrate openexr/imath for PIZ/ZIP lossless recompression.
- [ ] FITS – integrate cfitsio for scientific data compression.

  | Processor          | Lossless | Metadata | Container | Notes                                                                                                                                                                                                   |
  |--------------------|:--------:|:--------:|:---------:|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
  | FlacProcessor      |    ✅     |    ✅     |     ✅     | Works. Recompresses audio & optimizes cover art.                                                                                                                                                        |
  | WavPackProcessor   |    ✅     |    🟡    |     ❌     | Needs verification on complete tag copying (ReplayGain, etc.). <br>Test `.wvc` files. <br>Consider brute-force modes.                                                                                   |
  | ApeProcessor       |    🟡    |    ✅     |     ✅     | Recompresses audio (MACLib) & optimizes cover art (TagLib).                                                                                                                                             |
  | OggProcessor       |    ✅     |    ✅     |     ✅     | Recompresses Ogg FLAC streams using `libFLAC`. <br>Container-only mode for Vorbis/Opus: extracts/optimizes cover art.                                                                                   |
  | MpegProcessor      |    ❌     |    ✅     |     ✅     | Container-only mode: extracts/optimizes ID3v2 cover art. Audio recompression pending.                                                                                                                   |
  | Mp4Processor       |    ❌     |    ✅     |     ✅     | Container-only mode: extracts/optimizes 'covr' atom (JPEG/PNG).                                                                                                                                         |
  | WavProcessor       |    ❌     |    ✅     |     ✅     | Container-only mode: extracts/optimizes ID3v2 cover art inside RIFF.                                                                                                                                    |
  | AiffProcessor      |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes ID3v2 cover art inside AIFF.                                                                                                                                         |
  | JpegProcessor      |    🟡    |    🟡    |   N.A.    | Copies APP/COM markers. <br>Add optional metadata stripping. <br>Integrate other optimizers. <br>raw_equal implemented (pixel compare).                                                                 |
  | PngProcessor       |    🟡    |    🟡    |   N.A.    | Works. Needs formal verification for lossless & metadata (iCCP, sRGB, text chunks...).                                                                                                                  |
  | ZopfliPngProcessor |    🟡    |    🟡    |   N.A.    | raw_equal implemented (pixel compare). <br>Copies standard chunks via `zopflipng_lib`. <br>Needs ability to parameterize iterations.                                                                    |
  | WebpProcessor      |    🟡    |    🟡    |   N.A.    | Copies EXIF/XMP/ICCP chunks. <br>Improve lossless options (`-m 6`, `-q 100`). <br>Add optional chunk removal. <br>raw_equal implemented (pixel compare).                                                |
  | GifProcessor       |    ❌     |    ❌     |   N.A.    | (gifsicle) **Currently disabled**. <br>Needs fork of `gifsicle` to fix Windows build and make thread-safe.                                                                                              |
  | FlexiGifProcessor  |    🟡    |    ❌     |   N.A.    | (flexigif) Needs verification. <br>Needs ability to parameterize iterations/settings (like Zopfli).                                                                                                     |
  | TiffProcessor      |    🟡    |    🟡    |   N.A.    | Copies standard metadata tags (XMP, EXIF, ICC). <br>Uses Deflate compression. <br>Needs verification.                                                                                                   |
  | JxlProcessor       |    🟡    |    🟡    |   N.A.    | Re-encode loop implemented. <br>Metadata preservation (JXL box) implemented, but needs verification. <br>raw_equal implemented (pixel compare).                                                         |
  | TgaProcessor       |    ✅     |    ❌     |   N.A.    | Uses stb_image to re-apply RLE. <br>`raw_equal` implemented (pixel compare). <br>Metadata not preserved.                                                                                                |
  | BmpProcessor       |    ✅     |    ✅     |   N.A.    | Uses `bmplib`. Supports RLE4, RLE8, RLE24 (OS/2), and Huffman 1D compression. Preserves DPI and ICC profiles.                                                                                           |
  | PnmProcessor       |    ✅     |   N.A.   |   N.A.    | Uses `stb_image` to read and internal writer. Optimizes by converting ASCII formats (P1-P3) to Binary (P4-P6). Needs verification.                                                                      |
  | SqliteProcessor    |    ✅     |   N.A.   |   N.A.    | `VACUUM` + `ANALYZE` are standard, safe operations. <br>Considered verified.                                                                                                                            |
  | MseedProcessor     |    ✅     |    ✅     |   N.A.    | Metadata is part of header structure. <br>Considered complete. <br>May be extended for JSON header metadata.                                                                                            |
  | MkvProcessor       |    🟡    |    🟡    |     ❌     | Uses `mkclean`. <br>Container extraction/finalization is TODO. <br>Verify chapter/tag/attachment preservation.                                                                                          |
  | ArchiveProcessor   |    ❌     |   N.A.   |    🟡     | Core extractor/rebuilder using `libarchive`. <br>Needs extensive testing for archive types (ZIP, TAR, RAR...). <br>Rewrite hardlink handling. <br>Add 7z SDK support.                                   |
  | PdfProcessor       |    🟡    |   N.A.   |    🟡     | Extracts streams, recompresses Flate streams with Zopfli using `qpdf`. <br>Complex format, needs verification. <br>Investigate `pdfsizeopt` techniques. <br>raw_equal implemented (raw stream compare). |
  | OOXMLProcessor     |    ❌     |   N.A.   |    🟡     | Extracts ZIP, recompresses embedded PNG/JPG with Zopfli. <br>Needs verification. <br>Explore Leanify-style recursive optimization.                                                                      |
  | OdfProcessor       |    ❌     |   N.A.   |    🟡     | Extracts ZIP, recompresses embedded XML with Zopfli. <br>Stores `mimetype` uncompressed. <br>Needs verification. <br>Explore Leanify-style recursive optimization.                                      |

*(Legend: ✅ = Verified, 🟡 = Partially implemented/Needs verification, ❌ = Not implemented/Missing, N.A. = Not Applicable)*