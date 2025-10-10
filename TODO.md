# TODO List – Monolith Project

## Testing

- [ ] Unit tests for each encoder.
- [ ] Create a reproducible dataset of test files (small, medium, large, with and without metadata).
- [ ] Test edge cases: corrupted files, unsupported formats, empty files, mismatched extensions.
- [ ] Regression tests to ensure recompression is bit‑exact and lossless.
- [ ] Validate metadata preservation across formats (cover art, tags, chapters).

## Refactoring / Architecture

- [ ] Abstract common logic across encoders.
- [ ] Make encoder classes more MIME-aware.
- [ ] Support pipeline mode: chaining multiple encoders per MIME in addition to parallel mode.
- [ ] Normalize MIME detection and extension mapping.
- [ ] Begin support for using monolith as a library (public API, minimal dependencies).
- [ ] Implement a UriHandler to detect and process embedded data URIs (e.g. data:image/*;base64) in HTML, CSS, JS, XML, SVG. Extract, decode, optimize via existing encoders, and reinsert re-encoded content.

## FLAC

- [ ] Verify maximum compression parameters compatible with `streamable_subset=false`.
- [ ] Improve metadata handling: ensure valid STREAMINFO and preserve PICTURE blocks.
- [ ] Optional support for tag editing and Vorbis comment manipulation.
- [ ] Add streaming-mode encoder to avoid full PCM buffering for large files.
- [ ] Implement brute-force recompression across presets 0–8 and select the smallest output.

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

- [ ] Improve `gifsicle` inclusion (remove duplicate symbols).

## BMP

- [ ] Manually write an encoder with RLE encoding and palette reduction.

## Archives

- [ ] Add support for 7Z recompression using 7zip SDK.
- [ ] Investigate integration of BestCFBF (https://papas-best.com/downloads/bestcfbf/stable/bestcfbf.cpp) for optimizing MSI, DOC, PPT, XLS:
    - On Windows: adapt and integrate directly with COM Structured Storage APIs.
    - On Linux/macOS: research alternative libraries (e.g. libgsf, olefile) to replicate functionality.
- [ ] Explore Leanify-style handling of container formats that are essentially ZIP/LZMA/Deflate (APK, XPS, XPInstall, EPUB, DOCX, ODT, etc.) and integrate similar recursive optimization.

## MKV / Matroska

- [ ] Preserve chapters, tags, and attachments (e.g. fonts, cover art).
- [ ] Optional `mkclean` pass after remux for EBML optimization.
- [ ] Finish Matroska container support (currently unfinished).

## New MIME types / Codecs

- [ ] MP3 – integrate `mp3packer` for frame-level repacking.
- [ ] Investigate additional lossless formats (e.g. ALAC, TAK).
- [ ] Investigate scientific formats (NetCDF, HDF5) for future integration.

## Build / CI

- [ ] Fix compilation issues on Windows (ensure reproducible builds).
- [ ] Ensure all third-party libraries are linked statically.
- [ ] Add reproducibility checks (deterministic builds, no embedded timestamps).

## Other improvements

- [ ] File hash cache to skip already processed files across runs.
- [ ] Investigate further metadata preservation strategies across all formats.
- [ ] Improve logging granularity and structured output for CI integration.
- [ ] Investigate which apt/brew packages are actually needed for compiling.
- [ ] Future: implement a general XML minifier (with optional extensions for subtitle formats such as SRT, VTT, ASS).