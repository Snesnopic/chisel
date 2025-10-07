# monolith

**monolith** is an experimental project aiming to recreate the functionality of [FileOptimizer](https://nikkhokkho.sourceforge.io/static.php?page=FileOptimizer) and its encoders in a single, cross‑platform monolithic binary.  
It focuses on lossless recompression of various file formats by integrating multiple specialized encoders, with a strict emphasis on reproducibility, static linking, and Unix‑orthodox CLI behavior.

---

## Requirements

To build **monolith** you need only a few system tools. All other libraries are automatically fetched and built by CMake or installed through your system package manager.

### Linux
- CMake ≥ 3.20
- A modern C++23 compiler (GCC ≥ 11 or Clang ≥ 14)
- Git
- Autotools (for building libmagic)
- Standard build tools (make, pkg-config, etc.)

### macOS
- CMake ≥ 3.20
- Xcode Command Line Tools (Clang with C++23 support)
- Git
- Autotools (for building libmagic)
- [Homebrew](https://brew.sh/) is recommended for installing missing build tools

### Windows
- CMake ≥ 3.20
- Visual Studio 2022 (MSVC with C++23 support)
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) for dependency integration

---

## Installing dependencies

### Linux (Debian/Ubuntu)
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config git curl wget \
autoconf automake libtool m4 nasm yasm \
zlib1g-dev libpng-dev libjpeg-dev libwebp-dev libtiff-dev \
libogg-dev liblzma-dev libbz2-dev liblz4-dev libxml2-dev libexpat1-dev \
python3 python3-pip ccache libqpdf-dev
```

### macOS (Homebrew)
```bash
brew update
brew install cmake ninja pkg-config autoconf automake libtool git wget nasm yasm qpdf
```

### Windows
Install [vcpkg](https://github.com/microsoft/vcpkg) and ensure it is available in your environment.  
Dependencies will be resolved automatically through vcpkg when configuring with CMake.

---

## Building monolith

### Linux / macOS
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
-DBUILD_TESTING=OFF -DQPDF_BUILD_TESTS=OFF -DQPDF_BUILD_EXAMPLES=OFF \
-DQPDF_BUILD_DOC=OFF -DQPDF_BUILD_FUZZ=OFF -DQPDF_INSTALL=OFF \
-DSKIP_INSTALL_ALL=ON -DINSTALL_MANPAGES=OFF -DREQUIRE_CRYPTO_OPENSSL=OFF
cmake --build . --config Release
```

### Windows
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
-DCMAKE_BUILD_TYPE=Release \
-DBUILD_TESTING=OFF -DQPDF_BUILD_TESTS=OFF -DQPDF_BUILD_EXAMPLES=OFF \
-DQPDF_BUILD_DOC=OFF -DQPDF_BUILD_FUZZ=OFF -DQPDF_INSTALL=OFF \
-DSKIP_INSTALL_ALL=ON -DINSTALL_MANPAGES=OFF -DREQUIRE_CRYPTO_OPENSSL=OFF
cmake --build build --config Release
```
---

## Usage

`./monolith <file-or-directory>... [options]`

**Options:**
- `--dry-run`                  Use monolith without replacing original files.
- `--no-meta`                  Do not preserve metadata in recompressed files.
- `--recursive`                Process directories recursively.
- `--threads N`                Number of worker threads to use (default: half of available cores).
- `--log-level LEVEL`          Set logging verbosity (ERROR, WARNING, INFO, DEBUG, NONE).
- `-o, --output-csv FILE`      CSV report export filename. Must be a file path (stdout not supported).
- `--recompress-unencodable FORMAT`  
  Allows to recompress archives that can be opened but not recompressed  
  into a different format (zip, 7z, tar, gz, bz2, xz, wim).  
  If not specified, such archives are left untouched.

**Pipe mode:**
- If `-` is given as the only input, monolith reads a single file from stdin and writes the optimized result to stdout.
- In this mode:
  - Only one input (`-`) is allowed, not mixed with other files.
  - No progress bar or CSV/console report is generated.
  - Only the optimized file (or the original if no improvement) is written to stdout.
  - Errors and warnings are printed to stderr.

**Examples:**
- `./monolith file.jpg dir/ --recursive --threads 4`
- `./monolith archive.zip`
- `./monolith archive.rar --recompress-unencodable 7z`
- `./monolith dir/ -o report.csv`
- `cat file.png | ./monolith - > out.png`

---

## Supported formats

Currently implemented:

- **JPEG**
  - MIME: image/jpeg
  - Lossless recompression via mozjpeg

- **GIF**
  - MIME: image/gif
  - Lossless recompression via gifsicle

- **JPEG XL**
  - MIME: image/jxl
  - Lossless recompression via libjxl

- **WebP**
  - MIME: image/webp
  - Lossless recompression via libwebp

- **PNG**
  - MIME: image/png
  - Lossless recompression via zlib/Deflate and zopflipng

- **TIFF**
  - MIME: image/tiff
  - Lossless recompression via libtiff

- **PDF**
  - MIME: application/pdf
  - Structural optimization via qpdf

- **FLAC**
  - MIME: audio/flac
  - Lossless audio recompression

- **WavPack**
  - MIME: audio/x-wavpack
  - Lossless audio recompression via wavpack

- **SQLite**
  - MIME: application/vnd.sqlite3
  - Vacuum and page reordering for compactness

- **Archive formats** (via libarchive):
  - application/zip (.zip)
  - application/x-7z-compressed (.7z)
  - application/x-tar (.tar)
  - application/gzip (.gz)
  - application/x-bzip2 (.bz2)
  - application/x-xz (.xz)
  - application/x-rar (.rar, read-only)
  - application/x-iso9660-image (.iso)
  - application/x-cpio (.cpio)
  - application/x-lzma (.lzma)
  - application/vnd.ms-cab-compressed (.cab)
  - application/x-ms-wim (.wim)

- **Matroska containers (unfinished)**
  - MIME: video/x-matroska, audio/x-matroska
  - Structural optimization via libebml/libmatroska

- **Microsoft Office OpenXML containers**
  - application/vnd.openxmlformats-officedocument.wordprocessingml.document (.docx)
  - application/vnd.openxmlformats-officedocument.spreadsheetml.sheet (.xlsx)
  - application/vnd.openxmlformats-officedocument.presentationml.presentation (.pptx)

- **MSEED files**
  - application/vnd.fdsn.mseed (.mseed)

---

## Planned

- Additional image formats (HEIC/HEIF, AVIF)
- Additional audio formats (AAC, Ogg Vorbis, MP3)
- Finish Matroska container support
- Extended archive support and metadata preservation
- Advanced recompression for scientific data (NetCDF, HDF5)