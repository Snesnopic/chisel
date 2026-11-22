#include "chisel_app/settings.hpp"
#include "chisel_app/paths.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace chisel_app {

    namespace {
        // same keys as ChiselSwift's UserDefaults
        constexpr std::string_view kIterations = "iterations";
        constexpr std::string_view kIterationsLarge = "iterationsLarge";
        constexpr std::string_view kThreads = "threads";
        constexpr std::string_view kPreserveMetadata = "preserveMetadata";
        constexpr std::string_view kVerifyChecksums = "verifyChecksums";
        constexpr std::string_view kBreakSignatures = "breakSignatures";
        constexpr std::string_view kHideUnsupported = "hideUnsupported";
        constexpr std::string_view kRecursiveFolderImport = "recursiveFolderImport";
        constexpr std::string_view kOutputMode = "outputMode";
        constexpr std::string_view kOutputFolder = "outputFolder";

        std::optional<unsigned> parse_unsigned(const std::optional<std::string>& text) {
            if (!text) return std::nullopt;
            unsigned value = 0;
            const auto* end = text->data() + text->size();
            const auto [ptr, ec] = std::from_chars(text->data(), end, value);
            if (ec != std::errc{} || ptr != end) return std::nullopt;
            return value;
        }

        std::optional<bool> parse_bool(const std::optional<std::string>& text) {
            if (!text) return std::nullopt;
            if (*text == "true" || *text == "1") return true;
            if (*text == "false" || *text == "0") return false;
            return std::nullopt;
        }

        std::string escape(const std::string_view text) {
            std::string out;
            for (const char c : text) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    default: out += c;
                }
            }
            return out;
        }

        std::string unescape(const std::string_view text) {
            std::string out;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '\\' || i + 1 == text.size()) {
                    out += text[i];
                    continue;
                }
                const char next = text[++i];
                out += next == 'n' ? '\n' : next == 'r' ? '\r' : next;
            }
            return out;
        }
    } // namespace

    std::string_view to_string(const OutputMode mode) {
        switch (mode) {
            case OutputMode::Overwrite: return "overwrite";
            case OutputMode::SideBySide: return "sideBySide";
            case OutputMode::KeepOriginal: return "keepOriginal";
            case OutputMode::OutputFolder: return "outputFolder";
        }
        return "keepOriginal";
    }

    std::optional<OutputMode> output_mode_from_string(const std::string_view text) {
        for (const auto mode : {OutputMode::Overwrite, OutputMode::SideBySide, OutputMode::KeepOriginal, OutputMode::OutputFolder}) {
            if (to_string(mode) == text) return mode;
        }
        return std::nullopt;
    }

    unsigned default_threads() {
        return std::max(1u, std::thread::hardware_concurrency() / 2);
    }

    FileKeyValueStore::FileKeyValueStore(fs::path file) : file_(std::move(file)) {
        std::ifstream in(file_, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            values_[line.substr(0, eq)] = unescape(std::string_view(line).substr(eq + 1));
        }
    }

    std::optional<std::string> FileKeyValueStore::get(const std::string_view key) const {
        const auto it = values_.find(key);
        if (it == values_.end()) return std::nullopt;
        return it->second;
    }

    void FileKeyValueStore::set(const std::string_view key, const std::string_view value) {
        values_.insert_or_assign(std::string(key), std::string(value));
    }

    bool FileKeyValueStore::flush() {
        std::error_code ec;
        if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
        auto temp = file_;
        temp += ".tmp" + std::to_string(std::random_device{}());
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            for (const auto& [key, value] : values_) out << key << '=' << escape(value) << '\n';
            out.close();
            if (out.fail()) {
                fs::remove(temp, ec);
                return false;
            }
        }
        fs::rename(temp, file_, ec);
        if (ec) fs::remove(temp, ec);
        return !ec;
    }

    Settings load_settings(const KeyValueStore& store) {
        Settings s;
        if (const auto v = parse_unsigned(store.get(kIterations)); v && *v > 0) s.iterations = *v;
        if (const auto v = parse_unsigned(store.get(kIterationsLarge)); v && *v > 0) s.iterations_large = *v;
        if (const auto v = parse_unsigned(store.get(kThreads)); v && *v > 0) s.threads = *v;
        if (const auto v = parse_bool(store.get(kPreserveMetadata))) s.preserve_metadata = *v;
        if (const auto v = parse_bool(store.get(kVerifyChecksums))) s.verify_checksums = *v;
        if (const auto v = parse_bool(store.get(kBreakSignatures))) s.break_signatures = *v;
        if (const auto v = parse_bool(store.get(kHideUnsupported))) s.hide_unsupported = *v;
        if (const auto v = parse_bool(store.get(kRecursiveFolderImport))) s.recursive_folder_import = *v;
        if (const auto v = store.get(kOutputMode)) {
            if (const auto mode = output_mode_from_string(*v)) s.output_mode = *mode;
        }
        if (const auto v = store.get(kOutputFolder)) s.output_folder = from_utf8(*v);
        return s;
    }

    void save_settings(const Settings& s, KeyValueStore& store) {
        const auto flag = [](const bool b) { return b ? "true" : "false"; };
        store.set(kIterations, std::to_string(s.iterations));
        store.set(kIterationsLarge, std::to_string(s.iterations_large));
        store.set(kThreads, std::to_string(s.threads));
        store.set(kPreserveMetadata, flag(s.preserve_metadata));
        store.set(kVerifyChecksums, flag(s.verify_checksums));
        store.set(kBreakSignatures, flag(s.break_signatures));
        store.set(kHideUnsupported, flag(s.hide_unsupported));
        store.set(kRecursiveFolderImport, flag(s.recursive_folder_import));
        store.set(kOutputMode, to_string(s.output_mode));
        store.set(kOutputFolder, to_utf8(s.output_folder));
    }

} // namespace chisel_app
