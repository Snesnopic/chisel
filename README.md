# monolith

**monolith** is an experimental project aiming to recreate the functionality of [FileOptimizer](https://nikkhokkho.sourceforge.io/static.php?page=FileOptimizer) and its encoders in a single, cross‑platform monolithic binary.  
It focuses on lossless recompression of various file formats by integrating multiple specialized encoders.

---

## Requirements

To build monolith you need only a few system tools, since all other libraries are automatically fetched and built by CMake:

### Linux
- CMake ≥ 3.20
- A modern C++23 compiler (GCC ≥ 11 or Clang ≥ 14)
- Git
- Autotools (for building libmagic)

### macOS
- CMake ≥ 3.20
- Xcode Command Line Tools (Clang with C++23 support)
- Git
- Autotools (for building libmagic)
- Homebrew is recommended for installing missing build tools

### Windows
- CMake ≥ 3.20
- Visual Studio 2022 (MSVC with C++23 support)
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) for dependency integration

---

## Installing dependencies

### Linux (Debian/Ubuntu)
```bash
sudo apt update
sudo apt install build-essential cmake git autoconf automake libtool
```

### macOS (Homebrew)
```bash
brew install cmake git autoconf automake libtool
```

### Windows
Install [vcpkg](https://github.com/microsoft/vcpkg) and ensure it is available in your environment.

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
./monolith <file-or-directory>... [options]
```

**Options:**
- `--dry-run`                  Use monolith without replacing original files.
- `--no-meta`                  Do not preserve metadata in recompressed files.
- `--recursive`                Process directories recursively.
- `--threads N`                Number of worker threads to use (default: half of available cores).
- `--log-level LEVEL`          Set logging verbosity (ERROR, WARNING, INFO, DEBUG, NONE).
- `-o, --output-csv FILE`      CSV report export filename. If not specified, report is printed on stdout.
- `--recompress-unencodable FORMAT`  
  Allows to recompress archives that can be opened but not recompressed  
  into a different format (zip, 7z, tar, gz, bz2, xz, wim).  
  If not specified, such archives are left untouched.

**Examples:**
- `./monolith file.jpg dir/ --recursive --threads 4`
- `./monolith archive.zip`
- `./monolith archive.rar --recompress-unencodable 7z`
- `./monolith dir/ -o report.csv`

---

## Supported formats

Currently implemented:
- **JPEG** (lossless recompression via mozjpeg)
- **WAVPACK** (lossless recompression via wavpack)
- **JPEG XL** (lossless recompression via libjxl)
- **PNG** (lossless recompression via zlib/Deflate)
- **FLAC** (lossless audio recompression)
- **PDF** (via qpdf)
- **Archive formats** (ZIP, TAR, 7z, etc. via libarchive)

Planned:
- Additional image and audio formats
- MKV/Matroska support via FFmpeg