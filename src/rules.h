// Small rules that don't touch Windows, kept apart so tools/tests can check them anywhere.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>

// Characters Windows doesn't allow in a file name (and control characters).
inline bool NameCharOk(wchar_t c) { return c >= 32 && c != 127 && !wcschr(L"\\/:*?\"<>|", c); }

// "IMG_1.jpg" -> "IMG_1" and ".jpg". The extension starts at the last dot (not a leading one).
inline void SplitName(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        stem = name;
        ext.clear();
    } else {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
}

inline std::wstring TrimSpaces(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L' ');
    if (a == std::wstring::npos) return L"";
    return s.substr(a, s.find_last_not_of(L' ') - a + 1);
}

// CON, NUL, COM1 ... are devices to Windows, with or without an extension.
inline bool ReservedName(const std::wstring& stem) {
    std::wstring base = TrimSpaces(stem.substr(0, stem.find(L'.')));
    for (auto& c : base) c = (wchar_t)towupper(c);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return true;
    return base.size() == 4 && (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
           base[3] >= L'1' && base[3] <= L'9';
}

// Why `stem` (already trimmed) can't be used as a file name, or null if it can.
inline const wchar_t* NameProblem(const std::wstring& stem) {
    if (stem.empty()) return L"A file name can’t be empty";
    for (wchar_t c : stem)
        if (!NameCharOk(c)) return L"A file name can’t contain  \\ / : * ? \" < > |";
    if (ReservedName(stem)) return L"Windows reserves that name";
    return nullptr;
}

// Premultiplied colour channel back to a straight one.
inline uint8_t Unpremultiply(unsigned c, unsigned a) {
    return (uint8_t)(a ? std::min(255u, (c * 255 + a / 2) / a) : 0u);
}
