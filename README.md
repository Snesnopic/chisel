# monolith

**monolith** is an experimental project aiming to recreate the functionality of [FileOptimizer](https://nikkhokkho.sourceforge.io/static.php?page=FileOptimizer) and its encoders in a single, cross‑platform monolithic binary.  
It focuses on lossless recompression of various file formats by integrating multiple specialized encoders.

---

## Requirements

- C++23 or newer
- [CMake](https://cmake.org/) >= 4.0

---

## Installing dependencies

### Linux (Debian/Ubuntu)
TBD

### macOS (Homebrew)
TBD

### Windows (vcpkg)
Install [vcpkg](https://github.com/microsoft/vcpkg), then:
TBD

---

## Building monolith

### Linux / macOS
```bash
mkdir build && cd build
cmake ..
make
```

### Windows
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake ..
cmake --build build
```

---

## Usage

```bash
./monolith [--no-meta] [--recursive] [--threads N] [--log-level LEVEL] <file-or-directory>...
```

**Options:**
- `--no-meta`Do not preserve metadata in recompressed files.
- `--recursive`Process directories recursively.
- `--threads N`Number of worker threads to use (default: half of available cores).
- `--log-level LEVEL`Set logging verbosity ('DEBUG', 'INFO', 'WARNING', 'ERROR', 'NONE').

---

## Supported formats

Currently implemented:
- **JPEG** (lossless recompression via mozjpeg)
- **PNG** (lossless recompression via zlib/Deflate)
- **FLAC** (lossless audio recompression)
- **Archive formats** (ZIP, TAR, 7z, etc. via libarchive)

Planned:
- Additional image and audio formats.

---