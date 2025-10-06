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

- [ ] Integrate `gifsicle` for palette/frame optimization.

## Archives

- [ ] Add support for 7Z recompression using 7zip SDK.
- [ ] Add support for TAR.GZ recompression using zlib/libdeflate.

## MKV / Matroska

- [ ] Preserve chapters, tags, and attachments (e.g. fonts, cover art).
- [ ] Optional `mkclean` pass after remux for EBML optimization.
- [ ] Finish Matroska container support (currently unfinished).

## New MIME types / Codecs

- [ ] MP3 – integrate `mp3packer` for frame-level repacking.
- [ ] Monkey's Audio (APE) – integrate `MACLib` for recompression.
- [ ] Investigate additional lossless formats (e.g. ALAC, TAK, OptimFROG).
- [ ] Investigate scientific formats (NetCDF, HDF5) for future integration.

## Build / CI

- [ ] Add GitHub Actions workflow for Linux and Windows builds.
- [ ] Fix compilation issues on Linux and Windows (ensure reproducible builds).
- [ ] Ensure all third-party libraries are linked statically.
- [ ] Add reproducibility checks (deterministic builds, no embedded timestamps).

## Other improvements

- [ ] File hash cache to skip already processed files across runs.
- [ ] Investigate further metadata preservation strategies across all formats.
- [ ] Improve logging granularity and structured output for CI integration.