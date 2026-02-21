//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/mpeg_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include <mutex>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include "../../../third_party/vbrfix/include/vbrfix/vbrfix.hpp"

// main's print mutex, since we redirect stdout and stderr to null while compressing with mp3
// TODO: edit cloned mp3packer's branch to remove all prints

#ifdef HAVE_MP3PACKER
std::mutex g_console_mtx;
static std::mutex g_mp3packer_mutex;
extern "C" {
#include <caml/mlvalues.h>
#include <caml/callback.h>
#include <caml/alloc.h>
#include <caml/memory.h>
#include <caml/threads.h>
}

#ifdef _WIN32
    #include <io.h>
    #define DUP _dup
    #define DUP2 _dup2
    #define FILENO _fileno
    #define CLOSE _close
    #define NULL_DEVICE "NUL"
    #ifndef STDOUT_FILENO
        #define STDOUT_FILENO 1
    #endif
    #ifndef STDERR_FILENO
        #define STDERR_FILENO 2
    #endif
#else
    #include <unistd.h>
    #define DUP dup
    #define DUP2 dup2
    #define FILENO fileno
    #define CLOSE close
    #define NULL_DEVICE "/dev/null"
#endif


namespace {
    class ScopedOutputSilencer {
        int original_stdout;
        int original_stderr;
        int null_fd;
        bool active;

    public:
        explicit ScopedOutputSilencer(bool silence = true) : active(silence) {
            if (!active) return;

            fflush(stdout);
            fflush(stderr);

            original_stdout = DUP(STDOUT_FILENO);
            original_stderr = DUP(STDERR_FILENO);

            null_fd = open(NULL_DEVICE, O_WRONLY);

            if (null_fd >= 0) {
                DUP2(null_fd, STDOUT_FILENO);
                DUP2(null_fd, STDERR_FILENO);
            }
        }

        ~ScopedOutputSilencer() {
            if (!active) return;

            fflush(stdout);
            fflush(stderr);

            DUP2(original_stdout, STDOUT_FILENO);
            DUP2(original_stderr, STDERR_FILENO);

            CLOSE(original_stdout);
            CLOSE(original_stderr);
            if (null_fd >= 0) CLOSE(null_fd);
        }
    };

    struct scoped_ocaml_lock {
        scoped_ocaml_lock() {
            caml_acquire_runtime_system();
        }
        ~scoped_ocaml_lock() {
            caml_release_runtime_system();
        }
    };

    int run_ocaml_mp3packer(const std::string& input, const std::string& output) {
        // register thread once per lifecycle
        thread_local bool is_registered = false;
        if (!is_registered) {
            if (caml_c_thread_register() == 0) {
                return -999;
            }
            is_registered = true;
        }

        scoped_ocaml_lock lock;

        CAMLparam0();
        CAMLlocal3(v_input, v_output, v_res);

        int result = -1;
        const value* func = caml_named_value("caml_pack_mp3");

        if (func != nullptr) {
            v_input = caml_copy_string(input.c_str());
            v_output = caml_copy_string(output.c_str());
            v_res = caml_callback2(*func, v_input, v_output);
            result = Int_val(v_res);
        }

        CAMLreturnT(int, result);
    }
}

#endif // HAVE_MP3PACKER

#include "file_type.hpp"

namespace chisel {
namespace fs = std::filesystem;

void MpegProcessor::recompress(const fs::path& input,
                              const fs::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

#ifdef HAVE_MP3PACKER
    std::scoped_lock lock(g_mp3packer_mutex, g_console_mtx);

    Logger::log(LogLevel::Info, "Starting compression via ocaml: " + input.string(), get_name());

    if (fs::exists(output)) {
        fs::remove(output);
    }

    int result_code = 1;

    try {
        ScopedOutputSilencer hush(true);
        result_code = run_ocaml_mp3packer(input.string(), output.string());

    } catch (const std::exception& e) {
        throw std::runtime_error("Exception during OCaml execution wrapper: " + std::string(e.what()));
    }

    if (result_code == -999) {
        throw std::runtime_error("Failed to register worker thread with OCaml runtime.");
    }
    if (result_code == -1) {
        throw std::runtime_error("OCaml function 'caml_pack_mp3' not found. Runtime not initialized?");
    }
    if (result_code != 0) {
        throw std::runtime_error("MP3Packer failed with exit code: " + std::to_string(result_code));
    }

    Logger::log(LogLevel::Debug, "Compression successful.", get_name());
    try {
        ScopedOutputSilencer hush_vbr(true);

        vbrfix::FixParams params;
        params.always_skip = false;

        const std::vector<uint8_t> fixed_data = vbrfix::fix_mp3(output, params);

        std::ofstream ofs(output, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            throw std::runtime_error("Failed to open output file for writing VBR fix data.");
        }

        ofs.write(reinterpret_cast<const char*>(fixed_data.data()), static_cast<std::streamsize>(fixed_data.size()));
        ofs.close();

    } catch (const std::exception& e) {
        throw std::runtime_error("Exception during VBR fix processing: " + std::string(e.what()));
    }

    Logger::log(LogLevel::Debug, "Compression and vbr fix successful.", get_name());
#else

    Logger::log(LogLevel::Warning, "Mp3packer disabled inside build", get_name());

    std::error_code ec;
    fs::copy_file(input, output, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        throw std::runtime_error("Copy failed: " + ec.message());
    }

#endif // HAVE_MP3PACKER
    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> MpegProcessor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "mp3-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Info, "No embedded cover art found.", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Unknown;

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), get_name());
    return content;
}

std::filesystem::path MpegProcessor::finalize_extraction(const ExtractedContent &content) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), get_name());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (state_ptr == nullptr) {
        Logger::log(LogLevel::Error, "Failed to retrieve extraction state.", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".mp3");

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy audio file: " + std::string(e.what()), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "RebuildCovers failed", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        fs::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + final_temp_path.string(), get_name());
    return final_temp_path;
}

} // namespace chisel