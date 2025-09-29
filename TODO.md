# TODO List – Monolith Project

## Testing

- [ ] Unit tests for each encoder (`FlacEncoder`, `PngEncoder`, `JpegEncoder`, `WavpackEncoder`, `WebpEncoder`).
- [ ] Create a reproducible dataset of test files (small, medium, large, with and without metadata).
- [ ] Test edge cases: corrupted files, unsupported formats, empty files, mismatched extensions.
- [ ] Regression tests to ensure recompression is bit‑exact and lossless.
- [ ] Validate metadata preservation across formats (cover art, tags, chapters).

## Refactoring / Architecture

- [ ] Abstract common logic across encoders.
- [ ] Make encoder classes more MIME-aware.
- [ ] Support pipeline mode: chaining multiple encoders per MIME (e.g. `PngEncoder → PngLibdeflateEncoder`) in addition to parallel mode.
- [ ] Introduce a central `ReencoderDispatcher` to handle format/container/codec decisions.
- [ ] Normalize MIME detection and extension mapping.

## PNG

- [ ] Improve `PngEncoder` using optimized deflate (e.g. `zopfli`, `libdeflate`).
- [ ] Add `PngLibdeflateEncoder` and compare output size vs. current encoder.
- [ ] Support selective removal of non-essential chunks (EXIF, iTXt, sPLT, etc.).
- [ ] Benchmark compression ratio and speed across implementations.
- [ ] Integrate multiple PNG optimizers (`optipng`, `oxipng`, `advpng`, `pngout`, `pngrewrite`, `pngwolf`, `TruePNG`) and select the best result.

## FLAC

- [ ] Verify maximum compression parameters compatible with `streamable_subset=false`.
- [ ] Improve metadata handling: ensure valid STREAMINFO and preserve PICTURE blocks.
- [ ] Optional support for tag editing and Vorbis comment manipulation.
- [ ] Add streaming-mode encoder to avoid full PCM buffering for large files.
- [ ] Implement brute-force recompression across presets 0–8 and select the smallest output.

## WavPack

- [ ] Rename `WavEncoder` to `WavpackEncoder` for clarity.
- [ ] Ensure maximum compression flags are always set (`CONFIG_HIGH_FLAG | CONFIG_VERY_HIGH_FLAG`).
- [ ] Validate complete tag copying (ReplayGain, cuesheet, etc.).
- [ ] Add tests with both `.wv` and `.wvc` inputs.
- [ ] Implement brute-force recompression across compression modes and select the smallest output.

## JPEG

- [ ] Integrate `mozjpeg` for lossless optimization.
- [ ] Support optional stripping of EXIF/ICC metadata.
- [ ] Benchmark compression ratio vs. `jpegtran`.
- [ ] Integrate multiple JPEG optimizers (`jpegtran`, `jpegoptim`, `jpeg-recompress`, `guetzli`, `cjpegli`) and select the best result.

## WebP

- [ ] Improve `WebpEncoder` with advanced lossless options (`-m 6`, `-q 100`).
- [ ] Support removal of non-essential chunks (XMP, ICC).
- [ ] Benchmark compression ratio and speed.
- [ ] Integrate `libwebp` and brute-force recompression with different parameters.

## GIF

- [ ] Integrate `gifsicle` for palette/frame optimization.
- [ ] Benchmark compression ratio and speed.

## Archives

- [ ] Handle symlinks, device files, and permissions when writing archives.
- [ ] Implement CP437 fallback for ZIP with non-UTF-8 names if libarchive reports charset issues.
- [ ] Limit extraction depth for nested archives to avoid infinite loops.
- [ ] Add recompression fallback for unencodable formats (`--recompress-unencodable`).
- [ ] Integrate multiple ZIP optimizers (`advzip`, `DeflOpt`, `ECT`, `zRecompress`) and select the best result.
- [ ] Add support for 7Z recompression using 7zip SDK.
- [ ] Add support for TAR.GZ recompression using zlib/libdeflate.

## MKV / Matroska

- [ ] Preserve chapters, tags, and attachments (e.g. fonts, cover art).
- [ ] Optional `mkclean` pass after remux for EBML optimization.

## PDF

- [ ] Integrate `qpdf` for linearization and stream recompression.
- [ ] Integrate `mutool` and `cpdf` as alternative optimizers and select the best result.

## Office / OpenDocument

- [ ] DOCX/XLSX/PPTX – re-zip with `libzip` or `7zip`.
- [ ] ODT/ODS/ODP – re-zip with `libzip` or `7zip`.
- [ ] EPUB/CBZ/CBT – re-zip with `libzip` or `7zip`.

## New MIME types / Codecs

- [ ] MP3 – integrate `mp3packer` for frame-level repacking.
- [ ] Opus – integrate `libopus` for repacketization.
- [ ] Vorbis (Ogg) – integrate `libogg` / `oggz-tools` for repacketization.
- [ ] AVIF – integrate `libavif` for lossless recompression.
- [ ] HEIC/HEIF – integrate `libheif` for lossless recompression.
- [ ] Monkey'wavpacks Audio (APE) – integrate `MACLib` for recompression.

## Database / Binaries

- [ ] SQLite – always run VACUUM to reduce file size.
- [ ] EXE/DLL/OCX/SYS – integrate `UPX` for executable compression.
- [ ] OBJ/A/LIB – integrate `strip` from binutils to remove symbols.

## Other improvements

- [ ] File hash cache to skip already processed files across runs.
- [ ] More detailed progress bar (estimated time, average speed, per-file status).
- [ ] Support for pipe/STDIN-STDOUT for shell pipelines and integration with other tools.
- [ ] CSV report enhancements: include original size, recompressed size, codec used, metadata preserved.