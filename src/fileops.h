// Things Glance does to files on your behalf: recycle, show in Explorer, copy.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// True if the file's drive has a Recycle Bin (network shares and some USB
// drives don't).
bool HasRecycleBin(const std::wstring& path);

// Moves a file to the Recycle Bin, or deletes it permanently when the drive
// has no Recycle Bin or it's switched off (as Picasa did).
bool DeleteForUser(const std::wstring& path);

// Opens an Explorer window on the file's folder with the file selected.
void ShowInExplorer(const std::wstring& path);

// What Ctrl+C puts on the clipboard, prepared off the UI thread.
struct ClipData {
    std::wstring path;
    std::vector<uint8_t> dib;  // CF_DIB: BITMAPINFOHEADER + bottom-up 32bpp pixels
    std::vector<uint8_t> png;  // "PNG" format, only for images with transparency
    bool ok = false;
};

// Decodes `path` at full size, turned the way it is shown (EXIF orientation
// plus `quarterTurns` clockwise). Call from a worker thread.
void PrepareClipboard(const std::wstring& path, int quarterTurns, ClipData& out);

// Puts the file, the bitmap and (if present) the PNG on the clipboard.
bool SetClipboard(HWND owner, const ClipData& data);

// Puts plain text on the clipboard.
bool SetClipboardText(HWND owner, const std::wstring& text);

// The text on the clipboard, or empty.
std::wstring GetClipboardText(HWND owner);

// Renames a file in place. Returns 0, or the Win32 error (ERROR_ALREADY_EXISTS when the
// name is taken: nothing is ever overwritten). Waits briefly if the file is in use.
// Call from a worker thread.
DWORD RenameForUser(const std::wstring& from, const std::wstring& to);
