# chisel

**chisel** is an experimental project aiming to recreate the functionality of [FileOptimizer](https://nikkhokkho.sourceforge.io/static.php?page=FileOptimizer) and its encoders in a single, cross‑platform monolithic binary.  
It focuses on lossless recompression of various file formats by integrating multiple specialized encoders.

---

## Requirements

The project builds all its dependencies automatically via Git submodules. You only need a C++23 build toolchain and basic build tools.

-   **All Platforms:**
  -   `git` (with LFS support: run `git lfs install` once)
  -   `cmake` (≥ 3.20)
  -   `ninja` (recommended)
-   **Linux:**
  -   A modern C++23 compiler (GCC ≥ 11 or Clang ≥ 14)
  -   `build-essential`, `pkg-config`
  -   `autoconf`, `automake`, `libtool`, `m4`, `nasm`, `yasm` (required by some submodules)
-   **macOS:**
  -   Xcode Command Line Tools (Clang with C++23 support)
  -   `pkg-config`
  -   `autoconf`, `automake`, `libtool`, `nasm`, `yasm` (required by some submodules)
-   **Windows:**
  -   Visual Studio 2022 (with MSVC C++23 toolchain)

---

## Installing dependencies

### Linux (Debian/Ubuntu)
This command installs only the build tools. All libraries are submodules.
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build help2man pkg-config git \
autoconf automake libtool m4 nasm yasm ccache
```

### macOS (Homebrew)
```bash
brew update
brew install cmake ninja pkg-config git autoconf help2man automake libtool nasm yasm
```

### Windows
Ensure you have installed Visual Studio 2022 (with the "Desktop development with C++" workload) and Git. No other package managers are required.

---

## Building chisel

### Clone the repository and initialize all submodules:
```bash
  git clone https://github.com/Snesnopic/chisel.git
  cd chisel
  git lfs install
  git lfs pull
  git submodule update --init --recursive
```
### Configure and build with CMake (Linux / macOS):
```bash
mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Configure and build with CMake (Windows):
```powershell
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Installing (Optional)

After building the project, you can install the `chisel` executable and its documentation (manpage) onto your system by running:

```bash
sudo cmake --install . --prefix /usr/local
```

## Usage

`./chisel <file-or-directory>... [options]`

**Arguments:**
-   `inputs...`
    One or more files or directories to process.
    Use `-` to read from `stdin` (standard input).

**Options:**
-   `-h, --help`
    Show the help message and exit.

-   `--version`
    Display program version information and exit.

-   `-o, --output <PATH>`
    Write optimized files to PATH instead of modifying them in-place.
    If the input is `stdin` (-), PATH must be a file.
    Otherwise, PATH must be a directory.

-   `--report <FILE>`
    Export a final CSV report to the specified file.

-   `-r, --recursive`
    Recursively scan input folders.

-   `-q, --quiet`
    Suppress non-error console output (progress bar, results).

-   `--dry-run`
    Use chisel without replacing original files.

-   `--no-meta`
    Don't preserve files metadata. (Metadata is preserved by default).

-   `--verify-checksums`
    Verify raw checksums before replacing files.

-   `--threads <N>`
    Number of worker threads to use (default: half of available cores).

-   `--log-level <LEVEL>`
    Set logging verbosity (ERROR, WARNING, INFO, DEBUG, NONE). Default is INFO.

-   `--include <PATTERN>`
    Process only files matching regex PATTERN. (Can be used multiple times).

-   `--exclude <PATTERN>`
    Do not process files matching regex PATTERN. (Can be used multiple times).

-   `--mode <MODE>`
    Select how multiple encoders are applied to a file (`pipe` or `parallel`).
    `pipe` (default): Encoders are chained; output of one is input to the next.
    `parallel`: All encoders run on the original file; the smallest result is chosen.

-   `--regenerate-magic`
    Re-install the libmagic file-detection database. (Linux and macOS)

-   `--recompress-unencodable <FORMAT>`
    Recompress archives that can be opened but not re-written (like RAR)
    into a different format (zip, 7z, tar, gz, bz2, xz, wim).
    If not specified, such archives are left untouched.

**Examples:**
-   `./chisel file.jpg dir/ --recursive --threads 4`
-   `./chisel archive.zip`
-   `./chisel archive.rar --recompress-unencodable 7z`
-   `./chisel dir/ --report report.csv`
-   `cat file.png | ./chisel - -o out.png`
-   `cat file.png | ./chisel - > out.png`

---

## Supported formats

| Category   | Format                           | MIME                                                                                                                                                                                                                                                                       | Extensions                   | Library/Libraries            |
|------------|----------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------|------------------------------|
| Images     | JPEG                             | image/jpeg, image/jpg                                                                                                                                                                                                                                                      | .jpg, .jpeg                  | mozjpeg                      |
| Images     | GIF                              | image/gif                                                                                                                                                                                                                                                                  | .gif                         | gifsicle, flexigif           |
| Images     | JPEG XL                          | image/jxl                                                                                                                                                                                                                                                                  | .jxl                         | libjxl                       |
| Images     | WebP                             | image/webp, image/x-webp                                                                                                                                                                                                                                                   | .webp                        | libwebp                      |
| Images     | PNG                              | image/png                                                                                                                                                                                                                                                                  | .png                         | zlib/Deflate, zopflipng      |
| Images     | TIFF                             | image/tiff, image/tiff-fx                                                                                                                                                                                                                                                  | .tif, .tiff                  | libtiff                      |
| Images     | TrueVision TGA                   | image/x-tga, image/tga                                                                                                                                                                                                                                                     | .tga                         | stb                          |
| Images     | Windows Bitmap                   | image/bmp, image/x-ms-bmp                                                                                                                                                                                                                                                  | .bmp, .dib                   | bmplib                       |
| Images     | Portable Anymap                  | image/x-portable-anymap, image/x-portable-pixmap                                                                                                                                                                                                                           | .pnm, .ppm, .pgm             | stb (read), internal (write) |
| Images     | OpenRaster                       | image/openraster                                                                                                                                                                                                                                                           | .ora                         | libarchive (ZIP-based)       |
| Documents  | PDF                              | application/pdf                                                                                                                                                                                                                                                            | .pdf                         | qpdf                         |
| Documents  | Microsoft Office OOXML           | docx: application/vnd.openxmlformats-officedocument.wordprocessingml.document<br>xlsx: application/vnd.openxmlformats-officedocument.spreadsheetml.sheet<br>pptx: application/vnd.openxmlformats-officedocument.presentationml.presentation, application/vnd.ms-powerpoint | .docx, .xlsx, .pptx          | —                            |
| Documents  | OpenDocument                     | odt: application/vnd.oasis.opendocument.text<br>ods: application/vnd.oasis.opendocument.spreadsheet<br>odp: application/vnd.oasis.opendocument.presentation<br>odg: application/vnd.oasis.opendocument.graphics<br>odf: application/vnd.oasis.opendocument.formula         | .odt, .ods, .odp, .odg, .odf | —                            |
| Documents  | EPUB                             | application/epub+zip                                                                                                                                                                                                                                                       | .epub                        | libarchive (ZIP-based)       |
| Documents  | Comic Book                       | CBZ: application/vnd.comicbook+zip<br>CBT: application/vnd.comicbook+tar                                                                                                                                                                                                   | .cbz, .cbt                   | libarchive                   |
| Documents  | XPS                              | application/vnd.ms-xpsdocument, application/oxps                                                                                                                                                                                                                           | .xps, .oxps                  | libarchive (ZIP-based)       |
| Documents  | DWFX                             | model/vnd.dwfx+xps                                                                                                                                                                                                                                                         | .dwfx                        | libarchive (ZIP-based)       |
| Audio      | FLAC                             | audio/flac, audio/x-flac                                                                                                                                                                                                                                                   | .flac                        | libFLAC, TagLib              |
| Audio      | Ogg (FLAC stream)                | audio/ogg, audio/oga                                                                                                                                                                                                                                                       | .ogg, .oga                   | libFLAC, libogg              |
| Audio      | Ogg Vorbis/Opus (Cover Art only) | audio/ogg, audio/vorbis, audio/opus                                                                                                                                                                                                                                        | .ogg, .opus                  | TagLib (covers)              |
| Audio      | MP3 (Cover Art only)             | audio/mpeg                                                                                                                                                                                                                                                                 | .mp3                         | TagLib (covers)              |
| Audio      | M4A/MP4 (Cover Art only)         | audio/mp4, audio/x-m4a, video/mp4                                                                                                                                                                                                                                          | .m4a, .mp4, .m4b             | TagLib (covers)              |
| Audio      | WAV (Cover Art only)             | audio/wav, audio/x-wav                                                                                                                                                                                                                                                     | .wav                         | TagLib (covers)              |
| Audio      | AIFF (Cover Art only)            | audio/x-aiff, audio/aiff                                                                                                                                                                                                                                                   | .aif, .aiff, .aifc           | TagLib (covers)              |
| Audio      | Monkey's Audio                   | audio/ape, audio/x-ape                                                                                                                                                                                                                                                     | .ape                         | MACLib, TagLib               |
| Audio      | WavPack                          | audio/x-wavpack, audio/x-wavpack-correction                                                                                                                                                                                                                                | .wv, .wvp, .wvc              | wavpack                      |
| Databases  | SQLite                           | application/vnd.sqlite3, application/x-sqlite3                                                                                                                                                                                                                             | .sqlite, .db                 | sqlite3                      |
| Archives   | Zip                              | application/zip, application/x-zip-compressed                                                                                                                                                                                                                              | .zip                         | libarchive                   |
| Archives   | 7z                               | application/x-7z-compressed                                                                                                                                                                                                                                                | .7z                          | libarchive                   |
| Archives   | Tar                              | application/x-tar                                                                                                                                                                                                                                                          | .tar                         | libarchive                   |
| Archives   | GZip                             | application/gzip                                                                                                                                                                                                                                                           | .gz                          | libarchive                   |
| Archives   | BZip2                            | application/x-bzip2                                                                                                                                                                                                                                                        | .bz2                         | libarchive                   |
| Archives   | Xz                               | application/x-xz                                                                                                                                                                                                                                                           | .xz                          | libarchive                   |
| Archives   | Rar                              | application/vnd.rar, application/x-rar-compressed                                                                                                                                                                                                                          | .rar                         | libarchive (read-only)       |
| Archives   | ISO                              | application/x-iso9660-image                                                                                                                                                                                                                                                | .iso                         | libarchive                   |
| Archives   | CPIO                             | application/x-cpio                                                                                                                                                                                                                                                         | .cpio                        | libarchive                   |
| Archives   | LZMA                             | application/x-lzma                                                                                                                                                                                                                                                         | .lzma                        | libarchive                   |
| Archives   | AR (Static Lib)                  | application/x-archive                                                                                                                                                                                                                                                      | .a, .ar, .lib                | libarchive                   |
| Archives   | Zstandard                        | application/zstd, application/x-zstd                                                                                                                                                                                                                                       | .zst, .tzst, .tar.zst        | libarchive                   |
| Archives   | CAB                              | application/vnd.ms-cab-compressed                                                                                                                                                                                                                                          | .cab                         | libarchive                   |
| Archives   | WIM                              | application/x-ms-wim                                                                                                                                                                                                                                                       | .wim                         | libarchive                   |
| Archives   | JAR                              | application/java-archive                                                                                                                                                                                                                                                   | .jar                         | libarchive (ZIP-based)       |
| Archives   | XPI                              | application/x-xpinstall                                                                                                                                                                                                                                                    | .xpi                         | libarchive (ZIP-based)       |
| Archives   | APK                              | application/vnd.android.package-archive                                                                                                                                                                                                                                    | .apk                         | libarchive (ZIP-based)       |
| Scientific | MSEED                            | application/vnd.fdsn.mseed                                                                                                                                                                                                                                                 | .mseed                       | libmseed                     |