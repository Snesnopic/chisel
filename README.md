# chisel

**chisel** is an experimental project aiming to recreate the functionality of [FileOptimizer](https://nikkhokkho.sourceforge.io/static.php?page=FileOptimizer) and its encoders in a single, cross‑platform monolithic binary.  
It focuses on lossless recompression of various file formats by integrating multiple specialized encoders.

---
## Installation

### Quick Install (macOS / Linux)
The easiest way to install `chisel` is via [Homebrew](https://brew.sh). This method automatically handles updates and dependencies.

```bash
brew update
brew tap snesnopic/chsl
brew install chsl
```
### Building from Source
If you prefer to compile chisel manually, or if you are using Windows, please follow the build instructions below.

## Requirements

The project builds all its dependencies automatically via Git submodules.

-   **All Platforms:**
  -   `git` (with LFS support: run `git lfs install` once)
  -   `cmake` (≥ 3.20)
  -   `ninja` (recommended)
  -   Rust toolchain (required for OptiVorbis integration; install via [rustup.rs](https://rustup.rs))
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
autoconf automake libtool m4 nasm yasm ccache opam
curl https://sh.rustup.rs -sSf | sh

# Initialize OCaml toolchain for MP3 support
opam init -y --disable-sandboxing
eval $(opam env)
opam install dune dune-configurator -y
```

### macOS (Homebrew)
```bash
brew update
brew install cmake ninja pkg-config git autoconf help2man automake libtool nasm yasm opam
curl https://sh.rustup.rs -sSf | sh

# Initialize OCaml toolchain for MP3 support
opam init -y
eval $(opam env)
opam install dune dune-configurator -y
```

### Windows
Ensure you have installed Visual Studio 2022 (with the "Desktop development with C++" workload) and Git.
```powershell
# Download Visual Studio 2022 Community bootstrapper
Invoke-WebRequest "https://aka.ms/vs/17/release/vs_community.exe" -OutFile vs.exe

# Install "Desktop development with C++" workload
.\vs.exe --quiet --wait --norestart --nocache `
  --add Microsoft.VisualStudio.Workload.NativeDesktop `
  --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  --add Microsoft.VisualStudio.Component.Windows10SDK.22621

# Install Rust toolchain
Invoke-WebRequest https://win.rustup.rs/x86_64 -OutFile rustup-init.exe
.\rustup-init.exe -y
```
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

### Configure and build with CMake:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```
> If you have Ninja, you can add `-G "Ninja"` to the first command.

### Opting out of specific encoders
If you do not want to install the OCaml toolchain, you can disable MP3 optimization (mp3packer) by passing `-DENABLE_MP3PACKER=OFF` to CMake during configuration.

You can also opt out of the OptiVorbis integration (which requires Rust) with `-DENABLE_OPTIVORBIS=OFF`.
You can do the same for MKV optimizations with `-DENABLE_MATROSKA=OFF`.

## Usage

`./chsl <file-or-directory>... [options]`

**Arguments:**
-   `inputs...`
    One or more files or directories to process.
    Use `-` to read from `stdin`.

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
    Set logging verbosity (ERROR, WARNING, INFO, DEBUG, NONE). Default is ERROR.

-   `--log-file <FILE>`
    Write logs to the specified file (default: no file logging).

-   `--include <PATTERN>`
    Process only files matching regex PATTERN. (Can be used multiple times).

-   `--exclude <PATTERN>`
    Do not process files matching regex PATTERN. (Can be used multiple times).

-   `--iterations <N>`
    Number of iterations for Zopfli based compression (default: 15).

-   `--iterations-large <N>`
    Number of iterations for Zopfli on large images (default: 5).

-   `--max-tokens <N>`
    Number of tokens for FlexiGif compression (default: 10000).

-   `--mode <MODE>`
    Select how multiple encoders are applied to a file (`pipe` or `parallel`).
    `pipe` (default): Encoders are chained; output of one is input to the next.
    `parallel`: All encoders run on the original file; the smallest result is chosen.


**Examples:**
-   `./chsl file.jpg dir/ --recursive --threads 4`
-   `./chsl archive.zip`
-   `./chsl dir/ --report report.csv`
-   `cat file.png | ./chsl - -o out.png`
-   `cat file.png | ./chsl - > out.png`

---

## How it works

`chisel` scans the input file(s) to understand their actual format. On Windows, detection is currently based on file extensions, while on Linux and macOS it relies on `libmagic` for accurate MIME type detection. If a relevant `Processor` is found for the input, the file goes through a pipeline with 3 phases:

**Phase 1: Extraction & discovery**
The system identifies files whose compatible `Processor`s are flagged as containers. This includes traditional archives (like ZIP or Tar), PDF documents, and even audio files (like MP3 or FLAC) that contain embedded cover art within their ID3/APE tags. These internal files are extracted to a temporary location and exposed to the pipeline recursively. This means `chisel` is perfectly capable of compressing an image inside a ZIP archive, inside another ZIP archive.

**Phase 2: Recompression**
All discovered and extracted files are delegated to a thread pool. The worker thread will invoke the `recompress` function of the file's designated `Processor` (if available;not all formats are compressible, just like not all formats are containers).

If multiple processors are registered for the same file type, two modes of operation can occur, depending on the `--mode` flag:

* **PIPE (Default):** Processors are chained sequentially. The optimized output of the first processor becomes the input for the next one, in the exact order they are registered in `processor_registry.cpp`.
* **PARALLEL:** Every processor runs its `recompress` function simultaneously on a fresh copy of the original file, and the smallest resulting file is chosen. *(Note: This behavior is likely to be deprecated soon, as PIPE mode typically yields better cumulative results, and scenarios with multiple encoders for the exact same format are rare).*

If, at the end of this phase, the recompressed file is not strictly smaller than its original counterpart, the new file is discarded and the original is preserved.

**Phase 3: Finalization**
All files that were originally classified as containers, and whose contents were extracted in Phase 1, are now rebuilt. The `Processor` will repack the container using the newly optimized internal files, preserving the original structure.

---

## Adding a new Processor

Extending `chisel` with a new encoder or format requires just a few operations:

1. **Define the Processor:**
   Create a new header in `libchisel/include/processors/`, inheriting from `IProcessor`. You must meaningfully implement the required metadata methods:
* `get_name()`
* `get_supported_mime_types()`
* `get_supported_extensions()`
* `can_recompress()`
* `can_extract_contents()`


2. **Implement the core logic:**
   Write the implementation in `libchisel/src/processors/`.
* Implement `recompress()`, making sure to respect the `options.preserve_metadata` flag if applicable for your format.
* If your processor is a container, you must override `prepare_extraction()` and `finalize_extraction()`, ensuring the exact structure of the container is restored during finalization.
* *Note:* Implementing the `raw_equal()` method (used to verify that the meaningful content is bit-identical before and after compression) isn't strictly required to run the tool, but all tests run on the GitHub CI workers will execute with the `--verify-checksums` flag enabled, so it is highly recommended.


3. **Register the Processor:**
   The final step is to instantiate and register your new class inside the constructor of `ProcessorRegistry` in `libchisel/src/processor_registry.cpp`.

---

## Supported formats

| Category      | Format                     | MIME                                                                                                                                                                                                                                                                       | Extensions                                        | Libraries                          |
|---------------|----------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------|------------------------------------|
| Images        | JPEG                       | image/jpeg, image/jpg                                                                                                                                                                                                                                                      | .jpg, .jpeg                                       | mozjpeg                            |
| Images        | GIF                        | image/gif                                                                                                                                                                                                                                                                  | .gif                                              | gifsicle, flexigif                 |
| Images        | JPEG XL                    | image/jxl                                                                                                                                                                                                                                                                  | .jxl                                              | libjxl                             |
| Images        | WebP                       | image/webp, image/x-webp                                                                                                                                                                                                                                                   | .webp                                             | libwebp                            |
| Images        | PNG                        | image/png                                                                                                                                                                                                                                                                  | .png                                              | zlib/Deflate, zopflipng            |
| Images        | TIFF                       | image/tiff, image/tiff-fx                                                                                                                                                                                                                                                  | .tif, .tiff                                       | libtiff                            |
| Images        | TrueVision TGA             | image/x-tga, image/tga                                                                                                                                                                                                                                                     | .tga                                              | stb                                |
| Images        | Windows Bitmap             | image/bmp, image/x-ms-bmp                                                                                                                                                                                                                                                  | .bmp, .dib                                        | bmplib                             |
| Images        | Portable Anymap            | image/x-portable-anymap, image/x-portable-pixmap                                                                                                                                                                                                                           | .pnm, .ppm, .pgm                                  | stb (read), internal (write)       |
| Images        | OpenRaster                 | image/openraster                                                                                                                                                                                                                                                           | .ora                                              | libarchive (ZIP-based)             |
| Images        | SVG                        | image/svg+xml                                                                                                                                                                                                                                                              | .svg                                              | pugixml                            |
| Documents     | XML Data                   | application/xml, text/xml, application/xhtml+xml, application/vnd.google-earth.kml+xml, application/gpx+xml, model/vnd.collada+xml, application/rss+xml, application/atom+xml, application/rdf+xml                                                                         | .xml, .xhtml, .kml, .gpx, .dae, .rss, .atom, .xmp | pugixml                            |
| Documents     | PDF                        | application/pdf                                                                                                                                                                                                                                                            | .pdf                                              | qpdf                               |
| Documents     | Microsoft Office OOXML     | docx: application/vnd.openxmlformats-officedocument.wordprocessingml.document<br>xlsx: application/vnd.openxmlformats-officedocument.spreadsheetml.sheet<br>pptx: application/vnd.openxmlformats-officedocument.presentationml.presentation, application/vnd.ms-powerpoint | .docx, .xlsx, .pptx                               | -                                  |
| Documents     | CFBF (Legacy Office / MSI) | application/x-ole-storage, application/msword, application/vnd.ms-excel, application/vnd.ms-powerpoint, application/x-msi                                                                                                                                                  | .doc, .xls, .ppt, .msi                            | -                                  |
| Documents     | OpenDocument               | odt: application/vnd.oasis.opendocument.text<br>ods: application/vnd.oasis.opendocument.spreadsheet<br>odp: application/vnd.oasis.opendocument.presentation<br>odg: application/vnd.oasis.opendocument.graphics<br>odf: application/vnd.oasis.opendocument.formula         | .odt, .ods, .odp, .odg, .odf                      | -                                  |
| Documents     | EPUB                       | application/epub+zip                                                                                                                                                                                                                                                       | .epub                                             | libarchive (ZIP-based)             |
| Documents     | Comic Book                 | CBZ: application/vnd.comicbook+zip<br>CBT: application/vnd.comicbook+tar                                                                                                                                                                                                   | .cbz, .cbt                                        | libarchive                         |
| Documents     | XPS                        | application/vnd.ms-xpsdocument, application/oxps                                                                                                                                                                                                                           | .xps, .oxps                                       | libarchive (ZIP-based)             |
| Documents     | DWFX                       | model/vnd.dwfx+xps                                                                                                                                                                                                                                                         | .dwfx                                             | libarchive (ZIP-based)             |
| Documents     | 3MF (3D)                   | application/vnd.ms-package                                                                                                                                                                                                                                                 | .3mf                                              | libarchive (ZIP-based)             |
| Documents     | KMZ (Google Earth)         | application/vnd.google-earth.kmz                                                                                                                                                                                                                                           | .kmz                                              | libarchive (ZIP-based)             |
| Audio         | FLAC                       | audio/flac, audio/x-flac                                                                                                                                                                                                                                                   | .flac                                             | libFLAC, TagLib                    |
| Audio         | Ogg (FLAC stream)          | audio/ogg, audio/oga                                                                                                                                                                                                                                                       | .ogg, .oga                                        | libFLAC, libogg                    |
| Audio         | Ogg Vorbis/Opus            | audio/ogg, audio/vorbis, audio/opus                                                                                                                                                                                                                                        | .ogg, .opus                                       | OptiVorbis, TagLib (covers)        |
| Audio         | MP3                        | audio/mpeg                                                                                                                                                                                                                                                                 | .mp3                                              | mp3packer, vbrfix, TagLib (covers) |
| Audio         | M4A/MP4 (Cover Art only)   | audio/mp4, audio/x-m4a, video/mp4                                                                                                                                                                                                                                          | .m4a, .mp4, .m4b                                  | TagLib (covers)                    |
| Audio         | WAV (Cover Art only)       | audio/wav, audio/x-wav                                                                                                                                                                                                                                                     | .wav                                              | TagLib (covers)                    |
| Audio         | AIFF (Cover Art only)      | audio/x-aiff, audio/aiff                                                                                                                                                                                                                                                   | .aif, .aiff, .aifc                                | TagLib (covers)                    |
| Audio         | Monkey's Audio             | audio/ape, audio/x-ape                                                                                                                                                                                                                                                     | .ape                                              | MACLib, TagLib                     |
| Audio         | WavPack                    | audio/x-wavpack, audio/x-wavpack-correction                                                                                                                                                                                                                                | .wv, .wvp, .wvc                                   | wavpack                            |
| Audio         | DSF (DSD Stream File)      | audio/dsf, audio/x-dsf                                                                                                                                                                                                                                                     | .dsf                                              | TagLib (covers)                    |
| Audio         | DSDIFF                     | audio/dff, audio/x-dff                                                                                                                                                                                                                                                     | .dff                                              | TagLib (covers)                    |
| Audio         | Musepack                   | audio/musepack, audio/x-musepack                                                                                                                                                                                                                                           | .mpc, .mp+, .mpp                                  | TagLib (covers)                    |
| Audio         | TrueAudio                  | audio/tta, audio/x-tta                                                                                                                                                                                                                                                     | .tta                                              | TagLib (covers)                    |
| Video / Audio | Matroska / WebM            | video/x-matroska, audio/x-matroska, video/webm, audio/webm                                                                                                                                                                                                                 | .mkv, .mka, .webm                                 | mkclean, TagLib (attachments)      |
| Video / Audio | ASF / WMA / WMV            | audio/x-ms-wma, video/x-ms-wmv, video/x-ms-asf                                                                                                                                                                                                                             | .wma, .wmv, .asf                                  | TagLib (covers)                    |
| Archives      | Brotli                     | application/x-brotli, application/brotli                                                                                                                                                                                                                                   | .br                                               | brotli                             |
| Archives      | Zip                        | application/zip, application/x-zip-compressed                                                                                                                                                                                                                              | .zip                                              | libarchive                         |
| Archives      | 7z                         | application/x-7z-compressed                                                                                                                                                                                                                                                | .7z                                               | libarchive                         |
| Archives      | Tar                        | application/x-tar                                                                                                                                                                                                                                                          | .tar                                              | libarchive                         |
| Archives      | GZip                       | application/gzip                                                                                                                                                                                                                                                           | .gz                                               | libarchive                         |
| Archives      | BZip2                      | application/x-bzip2                                                                                                                                                                                                                                                        | .bz2                                              | libarchive                         |
| Archives      | Xz                         | application/x-xz                                                                                                                                                                                                                                                           | .xz                                               | libarchive                         |
| Archives      | ISO                        | application/x-iso9660-image                                                                                                                                                                                                                                                | .iso                                              | libarchive                         |
| Archives      | CPIO                       | application/x-cpio                                                                                                                                                                                                                                                         | .cpio                                             | libarchive                         |
| Archives      | LZMA                       | application/x-lzma                                                                                                                                                                                                                                                         | .lzma                                             | libarchive                         |
| Archives      | AR (Static Lib)            | application/x-archive                                                                                                                                                                                                                                                      | .a, .ar, .lib                                     | libarchive                         |
| Archives      | Zstandard                  | application/zstd, application/x-zstd                                                                                                                                                                                                                                       | .zst, .tzst, .tar.zst                             | libarchive                         |
| Archives      | CAB                        | application/vnd.ms-cab-compressed                                                                                                                                                                                                                                          | .cab                                              | libarchive                         |
| Archives      | WIM                        | application/x-ms-wim                                                                                                                                                                                                                                                       | .wim                                              | libarchive                         |
| Archives      | JAR                        | application/java-archive                                                                                                                                                                                                                                                   | .jar                                              | libarchive (ZIP-based)             |
| Archives      | XPI                        | application/x-xpinstall                                                                                                                                                                                                                                                    | .xpi                                              | libarchive (ZIP-based)             |
| Archives      | APK                        | application/vnd.android.package-archive                                                                                                                                                                                                                                    | .apk                                              | libarchive (ZIP-based)             |
| Archives      | VSIX / NuGet               | application/zip                                                                                                                                                                                                                                                            | .vsix, .nupkg                                     | libarchive (ZIP-based)             |
| Archives      | Java EE                    | application/java-archive                                                                                                                                                                                                                                                   | .war, .ear                                        | libarchive (ZIP-based)             |
| Archives      | Android Bundle             | application/vnd.android.package-archive                                                                                                                                                                                                                                    | .aab                                              | libarchive (ZIP-based)             |
| Fonts         | WOFF2                      | fonts/woff2                                                                                                                                                                                                                                                                | .woff2                                            | woff2                              |
| Scientific    | MSEED                      | application/vnd.fdsn.mseed                                                                                                                                                                                                                                                 | .mseed                                            | libmseed                           |
| Databases     | SQLite                     | application/vnd.sqlite3, application/x-sqlite3                                                                                                                                                                                                                             | .sqlite, .db                                      | sqlite3                            |
