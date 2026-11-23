#ifndef CHISEL_APP_SETTINGS_HPP
#define CHISEL_APP_SETTINGS_HPP

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace chisel_app {

    // where a smaller file goes, relative to its original
    enum class OutputMode {
        Overwrite,    // chisel replaces the original in place
        SideBySide,   // the result is saved as name.compressed.ext next to the original
        KeepOriginal, // the original is renamed name.original.ext and the result takes its name
        OutputFolder  // only the smaller files go to the output folder, keeping their paths below the folders they were added from
    };

    std::string_view to_string(OutputMode mode);
    std::optional<OutputMode> output_mode_from_string(std::string_view text);

    // half the logical cores, at least one
    unsigned default_threads();

    struct Settings {
        unsigned iterations = 15;
        unsigned iterations_large = 5;
        unsigned threads = default_threads();
        bool preserve_metadata = true;
        bool verify_checksums = false;
        bool break_signatures = false;
        bool hide_unsupported = true;
        bool recursive_folder_import = true;
        OutputMode output_mode = OutputMode::KeepOriginal;
        std::filesystem::path output_folder;
        bool copy_unchanged = false;   // OutputFolder: also copy the files that didn't get smaller
        bool progressive_jpeg = true;  // a JPEG may switch between baseline and progressive, whichever is smaller
        bool exhaustive_flac = false;  // FLAC: try every encoding exactly, far slower

        bool operator==(const Settings&) const = default;
    };

    // string values by key; platform backends (registry, gsettings, a file) implement it
    class KeyValueStore {
    public:
        virtual ~KeyValueStore() = default;
        [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) const = 0;
        virtual void set(std::string_view key, std::string_view value) = 0;
        virtual bool flush() { return true; }
    };

    // key=value lines in a utf-8 file, written whole through a temp file and a rename
    class FileKeyValueStore final : public KeyValueStore {
    public:
        explicit FileKeyValueStore(std::filesystem::path file);
        [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
        void set(std::string_view key, std::string_view value) override;
        bool flush() override;

    private:
        std::filesystem::path file_;
        std::map<std::string, std::string, std::less<>> values_;
    };

    // missing or invalid values fall back to the defaults
    Settings load_settings(const KeyValueStore& store);
    void save_settings(const Settings& settings, KeyValueStore& store);

} // namespace chisel_app

#endif // CHISEL_APP_SETTINGS_HPP
