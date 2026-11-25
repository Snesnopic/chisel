#include "i18n.hpp"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace chisel_win {

    namespace {
        struct Hash {
            using is_transparent = void;
            std::size_t operator()(const std::wstring_view text) const noexcept {
                return std::hash<std::wstring_view>{}(text);
            }
        };
        using Catalog = std::unordered_map<std::wstring, std::wstring, Hash, std::equal_to<>>;

        std::wstring g_language = L"en";
        Catalog g_catalog;
        // plurals by "one<TAB>other": the language's forms, singular first
        std::unordered_map<std::wstring, std::vector<std::wstring>, Hash, std::equal_to<>> g_plurals;
        bool g_pseudo = false;
        std::mutex g_pseudo_mutex;
        Catalog g_pseudo_cache;

        std::wstring lower_primary(const std::wstring_view tag) {
            std::wstring out;
            for (const wchar_t c : tag) {
                if (c == L'-' || c == L'_') break;
                out.push_back(static_cast<wchar_t>(std::towlower(c)));
            }
            return out;
        }

        std::wstring resource_name(const std::wstring_view code) {
            std::wstring name = L"CHISEL_LANG_";
            for (const wchar_t c : code) name.push_back(static_cast<wchar_t>(std::towupper(c)));
            return name;
        }

        bool has_catalog(const std::wstring_view code) {
            return FindResourceW(GetModuleHandleW(nullptr), resource_name(code).c_str(), RT_RCDATA) != nullptr;
        }

        std::wstring unescape(const std::wstring_view text) {
            std::wstring out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == L'\\' && i + 1 < text.size()) {
                    const wchar_t next = text[++i];
                    if (next == L't') out.push_back(L'\t');
                    else if (next == L'n') out.push_back(L'\n');
                    else out.push_back(next);
                } else {
                    out.push_back(text[i]);
                }
            }
            return out;
        }

        // utf-8 lines "english<TAB>translation" or, for plurals, "one<TAB>other<TAB>singular<TAB>plural"; "#" starts a comment
        void load_catalog(const std::wstring_view code) {
            const HMODULE module = GetModuleHandleW(nullptr);
            const HRSRC found = FindResourceW(module, resource_name(code).c_str(), RT_RCDATA);
            if (!found) return;
            const HGLOBAL loaded = LoadResource(module, found);
            const auto* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
            const DWORD size = SizeofResource(module, found);
            if (!bytes || size == 0) return;
            std::string_view utf8(bytes, size);
            if (utf8.starts_with("\xEF\xBB\xBF")) utf8.remove_prefix(3);
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring text(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), text.data(), n);

            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t end = text.find(L'\n', start);
                if (end == std::wstring::npos) end = text.size();
                std::wstring_view line(text.data() + start, end - start);
                start = end + 1;
                if (line.ends_with(L'\r')) line.remove_suffix(1);
                if (line.empty() || line.front() == L'#') continue;
                std::vector<std::wstring> columns;
                for (std::size_t from = 0;;) {
                    const std::size_t tab = line.find(L'\t', from);
                    columns.push_back(unescape(line.substr(from, tab == std::wstring_view::npos ? std::wstring_view::npos : tab - from)));
                    if (tab == std::wstring_view::npos) break;
                    from = tab + 1;
                }
                if (columns.size() == 2) {
                    g_catalog.insert_or_assign(std::move(columns[0]), std::move(columns[1]));
                } else if (columns.size() == 4) {
                    g_plurals.insert_or_assign(columns[0] + L'\t' + columns[1], std::vector<std::wstring>{columns[2], columns[3]});
                }
            }
        }

        // accented and about 40% longer, to find untranslated text and cramped layouts
        std::wstring pseudo(const std::wstring_view english) {
            static constexpr std::wstring_view plain = L"aeiouAEIOUcnyszCNYSZ";
            static constexpr std::wstring_view accented = L"áéíóúÁÉÍÓÚçñýšžÇÑÝŠŽ";
            const std::size_t tab = english.find(L'\t');
            const std::wstring_view body = english.substr(0, tab);
            std::wstring out = L"[";
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (body[i] == L'{') {
                    const std::size_t close = body.find(L'}', i);
                    if (close != std::wstring_view::npos) {
                        out.append(body.substr(i, close - i + 1));
                        i = close;
                        continue;
                    }
                }
                const std::size_t at = plain.find(body[i]);
                out.push_back(at == std::wstring_view::npos ? body[i] : accented[at]);
            }
            out.append(std::max<std::size_t>(1, body.size() * 2 / 5), L'~');
            out.push_back(L']');
            if (tab != std::wstring_view::npos) out.append(english.substr(tab));
            return out;
        }

        std::wstring substitute(const std::wstring_view text, const std::wstring_view* args, const std::size_t count) {
            std::wstring out;
            out.reserve(text.size() + 32);
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == L'{' && i + 2 < text.size() && text[i + 1] >= L'0' && text[i + 1] <= L'9' && text[i + 2] == L'}') {
                    const auto index = static_cast<std::size_t>(text[i + 1] - L'0');
                    if (index < count) out.append(args[index]);
                    i += 2;
                } else {
                    out.push_back(text[i]);
                }
            }
            return out;
        }

        std::vector<std::wstring> windows_languages() {
            std::vector<std::wstring> out;
            ULONG count = 0, size = 0;
            if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) return out;
            std::wstring buffer(size, L'\0');
            if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) return out;
            for (const wchar_t* tag = buffer.c_str(); *tag; tag += std::wcslen(tag) + 1) out.emplace_back(tag);
            return out;
        }
    } // namespace

    void init_language(const std::wstring_view requested) {
        g_catalog.clear();
        g_plurals.clear();
        g_pseudo = false;
        g_language = L"en";
        if (const std::wstring code = lower_primary(requested); code == L"pseudo") {
            g_pseudo = true;
            g_language = code;
            return;
        } else if (code == L"en") {
            return;
        } else if (!code.empty() && has_catalog(code)) {
            g_language = code;
            load_catalog(code);
            return;
        }
        // the first of the user's languages chisel has, english included
        for (const std::wstring& tag : windows_languages()) {
            const std::wstring code = lower_primary(tag);
            if (code == L"en") return;
            if (!code.empty() && has_catalog(code)) {
                g_language = code;
                load_catalog(code);
                return;
            }
        }
    }

    std::wstring_view language() {
        return g_language;
    }

    const wchar_t* tr(const wchar_t* english) {
        if (g_pseudo) {
            std::lock_guard lock(g_pseudo_mutex);
            auto it = g_pseudo_cache.find(std::wstring_view(english));
            if (it == g_pseudo_cache.end()) it = g_pseudo_cache.emplace(english, pseudo(english)).first;
            return it->second.c_str();
        }
        if (g_catalog.empty()) return english;
        const auto it = g_catalog.find(std::wstring_view(english));
        return it == g_catalog.end() ? english : it->second.c_str();
    }

    std::wstring trf(const wchar_t* english, const std::initializer_list<std::wstring_view> args) {
        return substitute(tr(english), args.begin(), args.size());
    }

    std::wstring trn(const wchar_t* one, const wchar_t* other, const unsigned long long n,
                     const std::initializer_list<std::wstring_view> args) {
        const std::wstring count = std::to_wstring(n);
        std::vector<std::wstring_view> all;
        all.reserve(args.size() + 1);
        all.push_back(count);
        all.insert(all.end(), args.begin(), args.end());
        // english and italian: 1 is singular, 0 and the rest plural
        const std::size_t form = n == 1 ? 0 : 1;
        if (!g_plurals.empty()) {
            const auto it = g_plurals.find(std::wstring(one) + L'\t' + other);
            if (it != g_plurals.end() && form < it->second.size()) return substitute(it->second[form], all.data(), all.size());
        }
        return substitute(g_pseudo ? tr(form == 0 ? one : other) : (form == 0 ? one : other), all.data(), all.size());
    }

    wchar_t decimal_separator() {
        static const wchar_t separator = [] {
            wchar_t text[8] = {};
            if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, text, 8) > 1) return text[0];
            return L'.';
        }();
        return separator;
    }

    const wchar_t* date_locale() {
        static const std::wstring name = [] {
            wchar_t user[LOCALE_NAME_MAX_LENGTH] = {};
            const bool known = GetUserDefaultLocaleName(user, LOCALE_NAME_MAX_LENGTH) > 0;
            if (g_pseudo || (known && lower_primary(user) == g_language)) return std::wstring(known ? user : L"");
            if (g_language == L"it") return std::wstring(L"it-IT");
            return std::wstring(L"en-US");
        }();
        return name.empty() ? LOCALE_NAME_USER_DEFAULT : name.c_str();
    }

} // namespace chisel_win
