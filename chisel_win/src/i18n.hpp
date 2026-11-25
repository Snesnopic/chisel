#ifndef CHISEL_WIN_I18N_HPP
#define CHISEL_WIN_I18N_HPP

#include <initializer_list>
#include <string>
#include <string_view>

namespace chisel_win {

    // "en", a language with a catalog in the resources, or "pseudo"; empty follows Windows' display languages
    void init_language(std::wstring_view requested);
    std::wstring_view language();

    // the english text is the key; the pointer stays valid until the process ends
    const wchar_t* tr(const wchar_t* english);
    // {0}, {1}... take the arguments in order
    std::wstring trf(const wchar_t* english, std::initializer_list<std::wstring_view> args);
    // one or other by n, {0} is n and the arguments follow from {1}
    std::wstring trn(const wchar_t* one, const wchar_t* other, unsigned long long n,
                     std::initializer_list<std::wstring_view> args = {});

    // the user's decimal separator, from the regional format
    wchar_t decimal_separator();
    // for month names inside translated text: the user's locale when it speaks the ui language
    const wchar_t* date_locale();

} // namespace chisel_win

#endif // CHISEL_WIN_I18N_HPP
