// A pasteboard's text written and read back (window/mac/ClipboardMac.h), on a pasteboard of the suite's own: the Mac
// runner is a person's session, and the general pasteboard is theirs. The child window and the platform write the
// general pasteboard through these same two functions.
#import <AppKit/AppKit.h>

#include <gtest/gtest.h>

#include <string>

#include "../window/mac/ClipboardMac.h"

TEST(ClipboardMac, APasteboardsTextIsWhatWasWrittenUtf8Whole)
{
    NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
    ASSERT_NE(pasteboard, nil);
    const std::string text = "1. e4 e5 2. Nf3 \xe2\x99\x9e\nCaf\xc3\xa9 \xf0\x9f\x98\x80 end";
    ASSERT_TRUE(reckless::window::mac::WritePasteboardText(pasteboard, text));
    EXPECT_EQ(reckless::window::mac::ReadPasteboardText(pasteboard), text);
    ASSERT_TRUE(reckless::window::mac::WritePasteboardText(pasteboard, "\xff not UTF-8"));
    EXPECT_EQ(reckless::window::mac::ReadPasteboardText(pasteboard), "\xef\xbf\xbd not UTF-8")
        << "repaired, so the copy is made: the stray byte one U+FFFD";
    [pasteboard clearContents];
    EXPECT_EQ(reckless::window::mac::ReadPasteboardText(pasteboard), "") << "a pasteboard with no text gives none";
    [pasteboard releaseGlobally];
}
