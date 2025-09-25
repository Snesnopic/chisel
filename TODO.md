# TODO List – Monolith Project

## Testing

- [ ] Unit tests for each encoder (`FlacEncoder`, `PngEncoder`, `JpegEncoder`, `AlacEncoder`, etc.).
- [ ] Create a reproducible dataset of test files (small, medium, large, with and without metadata).
- [ ] Test edge cases: corrupted files, unsupported formats, empty files, mismatched extensions.
- [ ] Regression tests to ensure recompression is bit‑exact and lossless.
- [ ] Validate metadata preservation across formats (cover art, tags, chapters).

## Refactoring / Architecture

- [ ] Abstract common logic across encoders (temporary file handling, size comparison, replacement).
- [ ] Make encoder classes MIME-aware and allow multiple implementations per MIME.
- [ ] Support pipeline mode: chaining multiple encoders per MIME (e.g. `PngEncoder → PngLibdeflateEncoder`) in addition to parallel mode.
- [ ] Introduce a central `ReencoderDispatcher` to handle format/container/codec decisions.
- [ ] Normalize MIME detection and extension mapping.

## PNG

- [ ] Improve `PngEncoder` using optimized deflate (e.g. `zopfli`, `libdeflate`).
- [ ] Add `PngLibdeflateEncoder` and compare output size vs. current encoder.
- [ ] Support selective removal of non-essential chunks (EXIF, iTXt, sPLT, etc.).
- [ ] Benchmark compression ratio and speed across implementations.

## FLAC

- [ ] Verify maximum compression parameters compatible with `streamable_subset=false`.
- [ ] Improve metadata handling: ensure valid STREAMINFO and preserve PICTURE blocks.
- [ ] Optional support for tag editing and Vorbis comment manipulation.
- [ ] Add streaming-mode encoder to avoid full PCM buffering for large files.

## ALAC / MP4 / M4A

- [ ] Improve cover art recompression (JPEG/PNG) and reinsertion via FFmpeg.
- [ ] Preserve atom metadata (`©nam`, `©ART`, `©alb`, etc.) during re‑encoding.
- [ ] Support chapter copying and gapless playback flags.
- [ ] Validate compatibility with iTunes-style tagging.

## Archives

- [ ] Handle symlinks, device files, and permissions when writing archives.
- [ ] Implement CP437 fallback for ZIP with non-UTF-8 names if libarchive reports charset issues.
- [ ] Limit extraction depth for nested archives to avoid infinite loops.
- [ ] Add recompression fallback for unencodable formats (`--recompress-unencodable`).

## MKV / Matroska

- [ ] Replace libebml/libmatroska usage with FFmpeg for easier track extraction and repacking.
- [ ] Support FLAC/ALAC recompression inside MKV containers.
- [ ] Preserve chapters, tags, and attachments (e.g. fonts, cover art).
- [ ] Optional mkclean pass after remux for EBML optimization.

## New MIME types / Codecs

- [ ] MP3 – library: [LAME](https://lame.sourceforge.io/) or [mp3packer](https://github.com/da-x/mp3packer) for frame-level recompression.
- [ ] AAC / M4A – library: [FDK-AAC](https://github.com/mstorsjo/fdk-aac) or `qaac` (wrapper only).
- [ ] Opus – library: [libopus](https://opus-codec.org/) (lossless only if frames are copied).
- [ ] WAV – recompression with FLAC or WavPack.
- [ ] WebP lossless – library: [libwebp](https://developers.google.com/speed/webp).
- [ ] AVIF lossless – library: [libavif](https://github.com/AOMediaCodec/libavif).

## Other improvements

- [ ] File hash cache to skip already processed files across runs.
- [ ] More detailed progress bar (estimated time, average speed, per‑file status).
- [ ] Support for pipe/STDIN-STDOUT for shell pipelines and integration with other tools.
- [ ] CSV report enhancements: include original size, recompressed size, codec used, metadata preserved.
- [ ] Add dry-run diff mode to preview changes without writing output.