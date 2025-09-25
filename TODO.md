# TODO List – Monolith Project

## Testing

- [ ] Unit test for each encoder (`FlacEncoder` `PngEncoder`, `JpegEncoder`).
- [ ] Create a dataset of test files (small, medium, large, with and without metadata).
- [ ] Test edge cases: corrupted files, unsupported formats, empty files.
- [ ] Regression tests to ensure recompression is lossless.

## Refactoring / Architecture

- [ ] Abstraction of common code among encoders (temporary file handling, size comparison, replacement).
- [ ] Make encoder classes abstract for MIME and allow multiple implementations for the same format.
- [ ] Support multiple encoder execution per MIME in pipeline mode (output of one as input of the next) in addition to the already implemented parallel mode.

## PNG

- [ ] Improve `PngEncoder` using an optimized deflate (e.g. `zopfli` or `libdeflate`).
- [ ] Create an alternative PNG encoder (`PngLibdeflateEncoder`) and automatically compare results with the current one.
- [ ] Support selective removal of non-essential chunks (EXIF, iTXt, etc.).
- [ ] Benchmark between current implementation and the optimized one.

## FLAC

- [ ] Verify maximum compression parameters compatible with `streamable_subset=false`.
- [ ] Better metadata handling: always include valid STREAMINFO when `preserve_metadata_` is true.
- [ ] Optional support for tag editing.

## Archives

- [ ] Handle symlinks and special files in writing to preserve the original structure.
- [ ] Implement automatic CP437 fallback for ZIP with non-UTF-8 names if libarchive reports charset issues.
- [ ] Limit extraction depth for nested archives to avoid infinite loops.

## New MIME types / Codecs

- [ ] MP3 – suggested library: [LAME](https://lame.sourceforge.io/) or [mp3packer](https://github.com/da-x/mp3packer) for lossless frame-level recompression.
- [ ] AAC / M4A – library: [FDK-AAC](https://github.com/mstorsjo/fdk-aac) or `qaac` (wrapper only, lossless if it recompresses identical ADTS).
- [ ] Opus – library: [libopus](https://opus-codec.org/) (lossless only if frames are copied).
- [ ] WAV – recompression with FLAC or WavPack lossless.
- [ ] WebP lossless – library: [libwebp](https://developers.google.com/speed/webp).
- [ ] AVIF lossless – library: [libavif](https://github.com/AOMediaCodec/libavif).

## Other improvements

- [ ] File hash cache to skip files already processed in previous runs.
- [ ] More detailed progress bar (estimated time, average speed).
- [ ] Support for pipe/STDIN-STDOUT for use in shell pipelines.