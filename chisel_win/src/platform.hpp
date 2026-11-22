#ifndef CHISEL_WIN_PLATFORM_HPP
#define CHISEL_WIN_PLATFORM_HPP

#include "chisel_app/settings.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chisel_win {

    // settings as REG_SZ values under HKEY_CURRENT_USER\Software\Snesnopic\Chisel
    class RegistryKeyValueStore final : public chisel_app::KeyValueStore {
    public:
        RegistryKeyValueStore();
        ~RegistryKeyValueStore() override;
        RegistryKeyValueStore(const RegistryKeyValueStore&) = delete;
        RegistryKeyValueStore& operator=(const RegistryKeyValueStore&) = delete;
        [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
        void set(std::string_view key, std::string_view value) override;
        bool flush() override;

    private:
        HKEY key_ = nullptr;
    };

    // %LOCALAPPDATA%\Chisel, created on first use
    std::filesystem::path app_data_folder();

    std::vector<std::filesystem::path> pick_files(HWND owner);
    std::vector<std::filesystem::path> pick_folders(HWND owner, bool multiple);
    std::optional<std::filesystem::path> pick_save_file(HWND owner, const wchar_t* default_name, const wchar_t* filter_name,
                                                        const wchar_t* filter_spec);
    void show_in_folder(const std::filesystem::path& file);

} // namespace chisel_win

#endif // CHISEL_WIN_PLATFORM_HPP
