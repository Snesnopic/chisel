# TODO

## Testing

- [ ] Test edge cases: corrupted files, unsupported formats, empty files, mismatched extensions.

## JPEG

- [ ] Support optional stripping of EXIF/ICC metadata.
- [ ] Benchmark compression ratio vs. `jpegtran`.
- [ ] Integrate multiple JPEG optimizers (`jpegtran`, `jpegoptim`, `jpeg-recompress`, `guetzli`, `cjpegli`) and select the best result.

## WebP

- [ ] Improve `WebpEncoder` with advanced lossless options (`-m 6`, `-q 100`).
- [ ] Support removal of non-essential chunks (XMP, ICC).

## PDF

- [ ] Investigate **pdfsizeopt** techniques (image recompression, font unification, metadata stripping)  
  ↳ <https://github.com/pts/pdfsizeopt>

## Archives

- [ ] Add support for 7Z recompression using 7zip SDK.
- [ ] Rewrite hardlink handling in archive_processor with a cross-platform approach, since current implementation is not available on Windows.

## MKV / Matroska

- [ ] Finish Matroska container support (currently not all files supported).

## New MIME types / Codecs

- [ ] ALAC – investigate integration via libavcodec or standalone decoder.
- [ ] TAK – closed source, not feasible (note).
- [ ] LA (Lossless Audio) – abandoned, not feasible (note).
- [ ] MPEG‑4 ALS – investigate reference implementation.  
  ↳ <https://www.iso.org/standard/43345.html>
- [ ] Lepton (Rust JPEG recompressor) – consider FFI integration.  
  ↳ <https://github.com/dropbox/lepton> (original C++), <https://github.com/microsoft/lepton_jpeg_rust>
- [ ] STL – ASCII recompression, binary deduplication of triangles.  
  ↳ <https://en.wikipedia.org/wiki/STL_(file_format)>
- [ ] FB2 – FictionBook container optimization.  
  ↳ <https://en.wikipedia.org/wiki/FictionBook>
- [ ] MPEG1/2 – investigate Ocarina recompression approach.  
  ↳ <https://encode.su/threads/1111-Ocarina-s-MPEG1-and-MPEG2-video-compressor>
- [ ] H.264 – investigate lossless recompression (Pied Piper / losslessh264).  
  ↳ <https://encode.su/threads/2285-H264-Lossless-recompression-Pied-Piper-(losslessh264)>
- [ ] Lua bytecode – optional Leanify-style recompression.  
  ↳ <https://www.lua.org/manual/5.4/manual.html#6.4>
- [ ] WebAssembly – integrate wasm-opt (Binaryen).  
  ↳ <https://github.com/WebAssembly/binaryen>
- [ ] HALAC (High Availability Lossless Audio Codec) - add support if and when the source code gets updated
  ↳ <https://github.com/Hakan-Abbas/HALAC-High-Availability-Lossless-Audio-Compression>
- [ ] OpenEXR – integrate openexr/imath for PIZ/ZIP lossless recompression.
- [ ] FITS – integrate cfitsio for scientific data compression.
- [ ] SWF (ZWS/LZMA variant) – add LZMA-compressed SWF support (currently only CWS/FWS handled).

## Processor Status

  | Processor          | Lossless | Metadata | Container | Notes                                                                                                                                                                                                                      |
  |--------------------|:--------:|:--------:|:---------:|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
  | FlacProcessor      |    ✅     |    ✅     |     ✅     | Works. Recompresses audio & optimizes cover art.                                                                                                                                                                           |
  | WavPackProcessor   |    ✅     |    ✅     |     ✅     | Works. Consider additional compression methods.                                                                                                                                                                            |
  | ApeProcessor       |    ✅     |    ✅     |     ✅     | Recompresses audio (MACLib) & optimizes cover art (TagLib).                                                                                                                                                                |
  | OggProcessor       |    ✅     |    ✅     |     ✅     | Recompresses Ogg FLAC (`libFLAC`) and Ogg Vorbis (`OptiVorbis`). Direct copy for Opus. Extracts/optimizes cover art securely avoiding memory leaks.                                                                        |
  | MpegProcessor      |    ✅     |    ✅     |     ✅     | Recompresses MP3 audio using `mp3packer` (except on Windows). Extracts/optimizes ID3v2 cover art.                                                                                                                          |
  | Mp4Processor       |    ❌     |    ✅     |     ✅     | Container-only mode: extracts/optimizes 'covr' atom (JPEG/PNG).                                                                                                                                                            |
  | WavProcessor       |    ❌     |    ✅     |     ✅     | Container-only mode: extracts/optimizes ID3v2 cover art inside RIFF.                                                                                                                                                       |
  | AiffProcessor      |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes ID3v2 cover art inside AIFF.                                                                                                                                                            |
  | AsfProcessor       |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes cover art (TagLib). Audio recompression is a placeholder (passthrough). Covers ASF/WMA/WMV.                                                                                             |
  | DsdiffProcessor    |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes cover art (TagLib). Audio recompression is a placeholder (passthrough). Covers DSD (.dff).                                                                                              |
  | DsfProcessor       |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes cover art (TagLib). Audio recompression is a placeholder (passthrough). Covers Sony DSF.                                                                                                |
  | MpcProcessor       |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes cover art (TagLib). Audio recompression is a placeholder (passthrough). Covers Musepack (.mpc/.mpp).                                                                                    |
  | TtaProcessor       |    ❌     |    ✅     |     ✅     | Container-only: extracts/optimizes cover art (TagLib). Audio recompression is a placeholder (passthrough). Covers True Audio (.tta).                                                                                       |
  | JpegProcessor      |    ✅     |    🟡    |   N.A.    | Copies APP/COM markers. <br>Add optional metadata stripping. <br>Integrate other optimizers. <br>`raw_equal` implemented (pixel compare).                                                                                  |
  | PngProcessor       |    ✅     |    🟡    |   N.A.    | Works. Needs formal verification for lossless & metadata (iCCP, sRGB, text chunks...).                                                                                                                                     |
  | ZopfliPngProcessor |    ✅     |    🟡    |   N.A.    | `raw_equal` implemented (pixel compare). <br>Copies standard chunks via `zopflipng_lib`. <br>Needs ability to parameterize iterations.                                                                                     |
  | WebpProcessor      |    ✅     |    🟡    |   N.A.    | Copies EXIF/XMP/ICCP chunks. <br>Improve lossless options (`-m 6`, `-q 100`). <br>Add optional chunk removal. <br>`raw_equal` implemented (pixel compare).                                                                 |
  | GifProcessor       |    ✅     |    ✅     |   N.A.    | Works. Could use a better fork.                                                                                                                                                                                            |
  | FlexiGifProcessor  |    ✅     |    ❌     |   N.A.    | Needs ability to parameterize iterations/settings (like Zopfli).                                                                                                                                                           |
  | TiffProcessor      |    ✅     |    🟡    |   N.A.    | Copies standard metadata tags (XMP, EXIF, ICC). <br>Uses Deflate compression. <br>Needs verification.                                                                                                                      |
  | JxlProcessor       |    ✅     |    🟡    |   N.A.    | Re-encode loop implemented. <br>Metadata preservation (JXL box) implemented, but needs verification. <br>`raw_equal` implemented (pixel compare).                                                                          |
  | TgaProcessor       |    ✅     |    ❌     |   N.A.    | Uses stb_image to re-apply RLE. <br>`raw_equal` implemented (pixel compare). <br>Metadata not preserved.                                                                                                                   |
  | BmpProcessor       |    ✅     |    ✅     |   N.A.    | Uses `bmplib`. Supports RLE4, RLE8, RLE24 (OS/2), and Huffman 1D compression. Preserves DPI and ICC profiles.                                                                                                              |
  | PnmProcessor       |    ✅     |   N.A.   |   N.A.    | Uses `stb_image` to read and internal writer. Optimizes by converting ASCII formats (P1-P3) to Binary (P4-P6). Needs verification.                                                                                         |
  | PcxProcessor       |    ✅     |    🟡    |   N.A.    | Decode + canonical RLE re-encode. Handles DCX multi-page containers. `raw_equal` (pixel compare). Metadata: resolution normalizable, filler zeroed if `!preserve_metadata`. Needs verification.                            |
  | Jp2Processor       |    ✅     |    🟡    |   N.A.    | Decode + lossless re-encode via OpenJPEG (5/3 wavelet). Supports `.jp2`, `.j2k`, `.j2c`. `raw_equal` (pixel compare). Metadata preservation needs verification.                                                            |
  | MngProcessor       |    ✅     |    🟡    |   N.A.    | MNG: Zopfli re-compression of IDAT chunks. JNG: mozjpeg re-compression of JDAT + Zopfli for alpha IDAT. `raw_equal` (pixel/payload compare). Chunk metadata preserved. Needs verification.                                 |
  | IcnsProcessor      |    ✅     |   N.A.   |     ✅     | Extracts all icon blocks, re-optimizes PNG/JPEG 2000 payloads, rebuilds ICNS container. `raw_equal` (byte compare). Needs verification.                                                                                    |
  | IcoProcessor       |    ✅     |   N.A.   |     ✅     | Extracts PNG and BMP payloads from ICO/CUR, re-optimizes, rebuilds container with updated offsets. `raw_equal` (byte compare). Needs verification.                                                                         |
  | GftProcessor       |    ✅     |   N.A.   |     ✅     | Custom game format: preserves header verbatim, re-optimizes embedded image payload (PNG/JPEG/GIF). `raw_equal` (byte compare). Needs verification.                                                                         |
  | SqliteProcessor    |    ✅     |   N.A.   |   N.A.    | `VACUUM` + `ANALYZE` are standard, safe operations. <br>Considered verified.                                                                                                                                               |
  | MseedProcessor     |    ✅     |    ✅     |   N.A.    | Metadata is part of header structure. <br>Considered complete. <br>May be extended for JSON header metadata.                                                                                                               |
  | JsonProcessor      |    ✅     |   N.A.   |   N.A.    | Minification via `yyjson`. `raw_equal` (semantic compare: re-serialize both and compare minified output). Considered verified.                                                                                             |
  | XmlProcessor       |    ✅     |   N.A.   |     ✅     | Minification via `pugixml` (`format_raw`). Extracts base64 data URIs from attributes, re-optimizes, re-embeds. `raw_equal` (semantic compare). Covers XML/SVG/HTML (XML-compliant). Needs verification.                    |
  | VcfProcessor       |    ✅     |   N.A.   |     ✅     | Extracts base64-encoded PHOTO fields from vCard, re-optimizes, re-embeds with correct line wrapping (74 chars). `raw_equal` (byte compare). Needs verification.                                                            |
  | WoffProcessor      |    ✅     |    🟡    |   N.A.    | Decompresses all font tables, re-compresses with zlib best compression, stores uncompressed if smaller. `raw_equal` (per-table decompressed compare). Metadata block stripped if `!preserve_metadata`. Needs verification. |
  | Woff2Processor     |    ✅     |    🟡    |   N.A.    | Decodes to TTF, re-encodes with brotli quality 11. `raw_equal` (TTF payload compare). Metadata handling needs verification.                                                                                                |
  | BrotliProcessor    |    ✅     |   N.A.   |     ✅     | Decompresses, re-compresses at quality 11, window size 24. Inner content re-optimized by pipeline. `raw_equal` (decompressed payload compare). Considered verified.                                                        |
  | Bzip2Processor     |    ✅     |   N.A.   |     ✅     | Decompresses, re-compresses at block size 9 (max). Inner content re-optimized by pipeline. `raw_equal` (decompressed payload compare). Considered verified.                                                                |
  | LzmaProcessor      |    ✅     |   N.A.   |     ✅     | Decompresses (auto-detects `.xz` / `.lzma`), re-compresses at preset 9+extreme. Handles both XZ and legacy LZMA containers. `raw_equal` (decompressed payload compare). Considered verified.                               |
  | ZstdProcessor      |    ✅     |   N.A.   |     ✅     | Decompresses, re-compresses at level 22 (max). Inner content re-optimized by pipeline. `raw_equal` (decompressed payload compare). Considered verified.                                                                    |
  | KanziProcessor     |    ✅     |   N.A.   |     ✅     | Decompresses via Kanzi BlockDecompressor, re-compresses at level 9 (EXE+RLT+TEXT+UTF+DNA&TPAQX) with 64-bit checksum. `raw_equal` handled by pipeline. Considered verified.                                                |
  | SwfProcessor       |    ✅     |   N.A.   |   N.A.    | Decompresses FWS (uncompressed) and CWS (zlib) variants, re-compresses to CWS with zlib level 9. ZWS (LZMA) is currently skipped. `raw_equal` (uncompressed payload compare). Needs verification.                          |
  | PeProcessor        |    ✅     |   N.A.   |     ✅     | Traverses PE resource section, extracts embedded images (PNG/JPEG/GIF/BMP), re-optimizes in-place. Recalculates PE checksum, invalidates Authenticode signature. `raw_equal` (byte compare). Needs verification.           |
  | RdbProcessor       |    ✅     |   N.A.   |     ✅     | Custom game DB archive: extracts indexed payloads, re-optimizes, rebuilds with updated offsets. `raw_equal` (byte compare). Needs verification.                                                                            |
  | CfbfProcessor      |    ✅     |   N.A.   |   N.A.    | Defragments/compacts OLE2 Compound File (DOC, XLS, PPT, etc.) via Windows Structured Storage API (`STGC_CONSOLIDATE`). Windows-only; passthrough on other platforms. Considered verified on Windows.                       |
  | MkvProcessor       |    🟡    |    🟡    |     ❌     | Uses `mkclean`. <br>Container extraction/finalization is TODO. <br>Verify chapter/tag/attachment preservation.                                                                                                             |
  | ArchiveProcessor   |    ✅     |   N.A.   |    🟡     | Core extractor/rebuilder using `libarchive`. <br>Needs extensive testing for archive types (ZIP, TAR...). <br>Rewrite hardlink handling. <br>Add 7z SDK support.                                                           |
  | PdfProcessor       |    ✅     |   N.A.   |    🟡     | Extracts streams, recompresses Flate streams with Zopfli using `qpdf`. <br>Complex format, needs verification. <br>Investigate `pdfsizeopt` techniques. <br>`raw_equal` implemented (raw stream compare).                  |
  | OOXMLProcessor     |    ✅     |   N.A.   |    🟡     | Extracts ZIP, recompresses embedded PNG/JPG with Zopfli. <br>Needs verification. <br>Explore Leanify-style recursive optimization.                                                                                         |
  | OdfProcessor       |    ✅     |   N.A.   |    🟡     | Extracts ZIP, recompresses embedded XML with Zopfli. <br>Stores `mimetype` uncompressed. <br>Needs verification. <br>Explore Leanify-style recursive optimization.                                                         |
  | MimeProcessor      |    ✅     |   N.A.   |     ✅     | Parses MIME multipart messages, extracts base64-encoded attachments, re-optimizes binary payloads, re-encodes to base64 (76-char lines). `raw_equal` (chunk-by-chunk semantic compare). Needs verification.                |

*(Legend: ✅ = Verified/Implemented, 🟡 = Partially implemented/Needs verification, ❌ = Not implemented/Missing, N.A. = Not Applicable)*