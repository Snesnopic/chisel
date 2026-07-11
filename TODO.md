# TODO

## JPEG

- [ ] Support optional stripping of EXIF/ICC metadata.
- [ ] Benchmark compression ratio vs. `jpegtran`.
- [ ] Integrate multiple JPEG optimizers (`jpegtran`, `jpegoptim`, `jpeg-recompress`, `guetzli`, `cjpegli`) and select the best result.

## WebP

- [ ] Support removal of non-essential chunks (XMP, ICC).

## PDF

- [ ] Investigate **pdfsizeopt** techniques (image recompression, font unification, metadata stripping)  
  ↳ <https://github.com/pts/pdfsizeopt>

## Ogg / Vorbis

- [ ] Vendor a lightweight Vorbis decoder (e.g. `stb_vorbis`) to give `OggProcessor::raw_equal()` a real PCM-based
  verification for Vorbis streams, same approach as the minimp3 integration for `MpegProcessor`. Currently
  `raw_equal()` just trusts OptiVorbis's documented losslessness guarantee without independently verifying it.

## JSON

- [ ] Vendor `yyjson` as its own proper submodule/dependency instead of reusing the copy bundled inside
  `third_party/mseedout/third_party/libmseed` via a shared include path -- `JsonProcessor` currently only
  builds because that path happens to be on `libchisel`'s include directories for `MseedProcessor`'s sake.

## Archives

- [ ] Add support for 7Z recompression using 7zip SDK.

## Zstd

- [ ] `ZstdProcessor::get_supported_extensions()` only declares `.zst`, but the README's "Supported formats"
  table also lists `.tzst`/`.tar.zst` (a tar archive compressed as a whole with zstd, not a raw zstd stream).
  Registering those extensions directly on `ZstdProcessor` would be wrong as-is: naively decompressing one
  produces a raw uncompressed `.tar`, which nothing then re-optimizes or re-wraps back into `.tar.zst` --
  same class of problem `ArchiveProcessor` already solves for `.tgz` (gzip-wrapped tar). Needs either
  dedicated `.tzst`/`.tar.zst` handling in `ArchiveProcessor` (mirroring the `.tgz` path) or a decision to
  drop those two extensions from the README instead.

## MKV / Matroska

- [ ] Investigate extracting and re-optimizing embedded audio tracks (not just attachments/cover art, already handled) and reinserting them correctly.

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
