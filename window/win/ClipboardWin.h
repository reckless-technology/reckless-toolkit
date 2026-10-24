#pragma once
// The Windows clipboard's text, written and read, for the child window (window/win/WindowWin.cpp) and the app's
// platform (app/win/AppWin.cpp), each including it after <windows.h>. The clipboard is opened against a window of the
// caller's - EmptyClipboard makes that window the owner, without which SetClipboardData fails - and closed again at
// once; another program holding it open is waited for a few milliseconds before the copy or the paste gives up. Text
// is CF_UNICODETEXT, UTF-16 with its terminator, from which Windows makes the other text formats a paste may ask for.
#include <string>

namespace reckless::window::win
{
/** The clipboard opened against `owner`, tried for a few milliseconds while another program holds it.
    @param owner  the window the clipboard is opened against; the owner once it is emptied
    @return       true when it is open, for the caller to close */
inline bool OpenClipboardFor(HWND owner)
{
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (OpenClipboard(owner))
        {
            return true;
        }
        Sleep(2);
    }
    return false;
}

/** `utf8` on the clipboard as its text, everything it held before replaced.
    @param owner  a window of the caller's, which becomes the clipboard's owner; not null
    @param utf8   the text, UTF-8
    @return       true when the clipboard took it */
inline bool WriteClipboardText(HWND owner, const std::string &utf8)
{
    const int length =
        utf8.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!utf8.empty() && length <= 0)
    {
        return false;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(length) + 1) * sizeof(wchar_t));
    if (memory == nullptr)
    {
        return false;
    }
    auto *wide = static_cast<wchar_t *>(GlobalLock(memory));
    if (wide == nullptr)
    {
        GlobalFree(memory);
        return false;
    }
    if (length > 0)
    {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide, length);
    }
    wide[length] = L'\0';
    GlobalUnlock(memory);
    if (owner == nullptr || !OpenClipboardFor(owner))
    {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool taken = SetClipboardData(CF_UNICODETEXT, memory) != nullptr; // the system's now, when taken
    CloseClipboard();
    if (!taken)
    {
        GlobalFree(memory);
    }
    return taken;
}

/** The clipboard's text, for a paste.
    @param owner  a window of the caller's the clipboard is opened against
    @return       the text in UTF-8, or empty when it holds none or stays held by another program */
inline std::string ReadClipboardText(HWND owner)
{
    std::string text;
    if (!OpenClipboardFor(owner))
    {
        return text;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr)
    {
        if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(data)); wide != nullptr)
        {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1)
            {
                text.resize(static_cast<std::size_t>(bytes - 1));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}
} // namespace reckless::window::win
