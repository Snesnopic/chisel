#include "platform.hpp"
#include "i18n.hpp"
#include "ui.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace fs = std::filesystem;

namespace chisel_win {

    namespace {
        constexpr const wchar_t* kKey = L"Software\\Snesnopic\\Chisel";

        template <typename T>
        struct ComPtr {
            T* p = nullptr;
            ~ComPtr() { if (p) p->Release(); }
            T** operator&() { return &p; }
            T* operator->() const { return p; }
            explicit operator bool() const { return p != nullptr; }
        };

        std::optional<fs::path> item_path(IShellItem* item) {
            PWSTR raw = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return std::nullopt;
            fs::path path(raw);
            CoTaskMemFree(raw);
            return path;
        }

        std::vector<fs::path> run_open_dialog(HWND owner, const FILEOPENDIALOGOPTIONS extra, const wchar_t* title) {
            std::vector<fs::path> result;
            ComPtr<IFileOpenDialog> dialog;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | extra);
            dialog->SetTitle(title);
            if (FAILED(dialog->Show(owner))) return result;
            ComPtr<IShellItemArray> items;
            if (FAILED(dialog->GetResults(&items))) return result;
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    if (auto path = item_path(item.p)) result.push_back(std::move(*path));
                }
            }
            return result;
        }
    } // namespace

    RegistryKeyValueStore::RegistryKeyValueStore() {
        RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key_, nullptr);
    }

    RegistryKeyValueStore::~RegistryKeyValueStore() {
        if (key_) RegCloseKey(key_);
    }

    std::optional<std::string> RegistryKeyValueStore::get(const std::string_view key) const {
        if (!key_) return std::nullopt;
        const std::wstring name = widen(key);
        DWORD bytes = 0;
        if (RegGetValueW(key_, nullptr, name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return std::nullopt;
        std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(key_, nullptr, name.c_str(), RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return std::nullopt;
        value.resize(wcsnlen(value.c_str(), value.size()));
        return narrow(value);
    }

    void RegistryKeyValueStore::set(const std::string_view key, const std::string_view value) {
        if (!key_) return;
        const std::wstring name = widen(key);
        const std::wstring data = widen(value);
        RegSetValueExW(key_, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                       static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    }

    bool RegistryKeyValueStore::flush() {
        return key_ && RegFlushKey(key_) == ERROR_SUCCESS;
    }

    fs::path app_data_folder() {
        PWSTR raw = nullptr;
        fs::path folder;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
            folder = fs::path(raw) / L"Chisel";
        } else {
            std::error_code ec;
            folder = fs::temp_directory_path(ec) / L"Chisel";
        }
        if (raw) CoTaskMemFree(raw);
        std::error_code ec;
        fs::create_directories(folder, ec);
        return folder;
    }

    std::vector<fs::path> pick_files(HWND owner) {
        return run_open_dialog(owner, FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST, tr(L"Add files"));
    }

    std::vector<fs::path> pick_folders(HWND owner, const bool multiple) {
        return run_open_dialog(owner, FOS_PICKFOLDERS | (multiple ? FOS_ALLOWMULTISELECT : 0),
                               multiple ? tr(L"Add folder") : tr(L"Choose the output folder"));
    }

    std::optional<fs::path> pick_save_file(HWND owner, const wchar_t* default_name, const wchar_t* filter_name,
                                           const wchar_t* filter_spec) {
        ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
        const COMDLG_FILTERSPEC spec{filter_name, filter_spec};
        dialog->SetFileTypes(1, &spec);
        dialog->SetFileName(default_name);
        dialog->SetDefaultExtension(L"csv");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
        if (FAILED(dialog->Show(owner))) return std::nullopt;
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return std::nullopt;
        return item_path(item.p);
    }

    void show_in_folder(const fs::path& file) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            return;
        }
        ShellExecuteW(nullptr, L"open", file.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

} // namespace chisel_win
