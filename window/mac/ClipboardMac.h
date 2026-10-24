#pragma once
// A pasteboard's text, written and read, for the child window (window/mac/WindowMac.mm) and the app's platform
// (app/mac/AppMac.mm), each an Objective-C++ source with AppKit imported first. The general pasteboard is the
// clipboard; a suite holds these on a pasteboard of its own (NSPasteboard pasteboardWithUniqueName), so a run on a
// person's own Mac never touches what they copied. Text that is not UTF-8 is repaired first, each ill-formed part one
// U+FFFD, as the canvas repairs what it draws, so a copy never fails on the bytes it was handed.
#include <string>

#include "Text.h"

namespace reckless::window::mac
{
/** `utf8` on `pasteboard` as its text, everything it held before cleared.
    @param pasteboard  the pasteboard: the general one for a copy
    @param utf8        the text, UTF-8
    @return            true when the pasteboard took it */
inline bool WritePasteboardText(NSPasteboard *pasteboard, const std::string &utf8)
{
    const std::string repaired = gfx::IsValidUtf8(utf8) ? utf8 : gfx::RepairedUtf8(utf8);
    NSString         *text     = [[NSString alloc] initWithBytes:repaired.data()
                                                          length:repaired.size()
                                                        encoding:NSUTF8StringEncoding];
    if (text == nil)
    {
        return false;
    }
    [pasteboard clearContents];
    return [pasteboard setString:text forType:NSPasteboardTypeString];
}

/** `pasteboard`'s text, for a paste.
    @param pasteboard  the pasteboard: the general one for a paste
    @return            its string in UTF-8, or empty when it holds none */
inline std::string ReadPasteboardText(NSPasteboard *pasteboard)
{
    NSString *text = [pasteboard stringForType:NSPasteboardTypeString];
    if (text == nil)
    {
        return {};
    }
    const char *bytes = [text UTF8String];
    return bytes != nullptr ? std::string(bytes, [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding])
                            : std::string();
}
} // namespace reckless::window::mac
