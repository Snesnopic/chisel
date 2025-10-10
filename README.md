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
- `--mode MODE`                Select how multiple encoders are applied to a file.

  PIPE (default): encoders are chained, the output of one becomes the input of the next.

  PARALLEL: all encoders run independently on the original file, and the smallest result is chosen.
- `-o, --output-csv FILE`      CSV report export filename. Must be a file path (stdout not supported).
- `--regenerate-magic`         Re-install libmagic file-detection database.
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

### Images
- **JPEG**
  - **MIME:** `image/jpeg`, `image/jpg`
  - **Extensions:** .jpg, .jpeg
  - **Library:** mozjpeg
- **GIF**
  - **MIME:** `image/gif`
  - **Extensions:** .gif
  - **Libraries:** gifsicle, flexigif
- **JPEG XL**
  - **MIME:** `image/jxl`
  - **Extensions:** .jxl
  - **Library:** libjxl
- **WebP**
  - **MIME:** `image/webp`, `image/x-webp`
  - **Extensions:** .webp
  - **Library:** libwebp
- **PNG**
  - **MIME:** `image/png`
  - **Extensions:** .png
  - **Libraries:** zlib/Deflate, zopflipng
- **TIFF**
  - **MIME:** `image/tiff`, `image/tiff-fx`
  - **Extensions:** .tif, .tiff
  - **Library:** libtiff
- **OpenRaster**
  - **MIME:** `image/openraster`
  - **Extensions:** .ora
  - **Library:** libarchive (ZIP-based)

---

### Documents
- **PDF**
  - **MIME:** `application/pdf`
  - **Extensions:** .pdf
  - **Library:** qpdf
- **Microsoft Office OpenXML**
  - **DOCX MIME:** `application/vnd.openxmlformats-officedocument.wordprocessingml.document`
  - **XLSX MIME:** `application/vnd.openxmlformats-officedocument.spreadsheetml.sheet`
  - **PPTX MIME:** `application/vnd.openxmlformats-officedocument.presentationml.presentation`, `application/vnd.ms-powerpoint`
  - **Extensions:** .docx, .xlsx, .pptx
- **OpenDocument**
  - **ODT MIME:** `application/vnd.oasis.opendocument.text`
  - **ODS MIME:** `application/vnd.oasis.opendocument.spreadsheet`
  - **ODP MIME:** `application/vnd.oasis.opendocument.presentation`
  - **ODG MIME:**  `application/vnd.oasis.opendocument.graphics`
  - **ODF MIME:**  `application/vnd.oasis.opendocument.formula`,
  - **Extensions:** .odt, .ods, .odp, .odg, .odf
- **EPUB**
  - **MIME:** `application/epub+zip`
  - **Extensions:** .epub
  - **Library:** libarchive (ZIP-based)
- **Comic Book**
  - **CBZ MIME:** `application/vnd.comicbook+zip`
  - **CBT MIME:** `application/vnd.comicbook+tar`
  - **Extensions:** .cbz, .cbt
  - **Library:** libarchive
- **XPS**
  - **MIME:** `application/vnd.ms-xpsdocument`, `application/oxps`
  - **Extensions:** .xps, .oxps
  - **Library:** libarchive (ZIP-based)
- **DWFX**
  - **MIME:** `model/vnd.dwfx+xps`
  - **Extensions:** .dwfx
  - **Library:** libarchive (ZIP-based)

---

### Audio
- **FLAC**
  - **MIME:** `audio/flac`, `audio/x-flac`
  - **Extensions:** .flac
  - **Library:** libFLAC
- **Monkey's Audio**
  - **MIME:** `audio/ape`, `audio/x-ape`
  - **Extensions:** .ape
  - **Library:** MACLib
- **WavPack**
  - **MIME:** `audio/x-wavpack`, `audio/x-wavpack-correction`
  - **Extensions:** .wv, .wvp, .wvc
  - **Library:** wavpack

---

### Databases
- **SQLite**
  - **MIME:** `application/vnd.sqlite3`, `application/x-sqlite3`
  - **Extensions:** .sqlite, .db
  - **Library:** sqlite3

---

### Archive and container formats
- **Zip**
  - **MIME:** `application/zip`, `application/x-zip-compressed`
  - **Extensions:** .zip
  - **Library:** libarchive
- **7z**
  - **MIME:** `application/x-7z-compressed`
  - **Extensions:** .7z
  - **Library:** libarchive
- **Tar**
  - **MIME:** `application/x-tar`
  - **Extensions:** .tar
  - **Library:** libarchive
- **GZip**
  - **MIME:** `application/gzip`
  - **Extensions:** .gz
  - **Library:** libarchive
- **BZip2**
  - **MIME:** `application/x-bzip2`
  - **Extensions:** .bz2
  - **Library:** libarchive
- **Xz**
  - **MIME:** `application/x-xz`
  - **Extensions:** .xz
  - **Library:** libarchive
- **Rar**
  - **MIME:** `application/vnd.rar`, `application/x-rar-compressed`
  - **Extensions:** .rar
  - **Library:** libarchive (read-only)
- **ISO**
  - **MIME:** `application/x-iso9660-image`
  - **Extensions:** .iso
  - **Library:** libarchive
- **CPIO**
  - **MIME:** `application/x-cpio`
  - **Extensions:** .cpio
  - **Library:** libarchive
- **LZMA**
  - **MIME:** `application/x-lzma`
  - **Extensions:** .lzma
  - **Library:** libarchive
- **CAB**
  - **MIME:** `application/vnd.ms-cab-compressed`
  - **Extensions:** .cab
  - **Library:** libarchive
- **WIM**
  - **MIME:** `application/x-ms-wim`
  - **Extensions:** .wim
  - **Library:** libarchive
- **JAR**
  - **MIME:** `application/java-archive`
  - **Extensions:** .jar
  - **Library:** libarchive (ZIP-based)
- **XPI**
  - **MIME:** `application/x-xpinstall`
  - **Extensions:** .xpi
  - **Library:** libarchive (ZIP-based)
- **APK**
  - **MIME:** `application/vnd.android.package-archive`
  - **Extensions:** .apk
  - **Library:** libarchive (ZIP-based)

---

### Scientific and seismic
- **MSEED**
  - **MIME:** `application/vnd.fdsn.mseed`
  - **Extensions:** .mseed
  - **Library:** libmseed