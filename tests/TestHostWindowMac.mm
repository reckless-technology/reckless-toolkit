// Test-only: TestHostWindow (TestHostWindow.h) on AppKit - a borderless NSWindow whose content view an editor attaches
// its child view to, ordered in and out as a host shows and hides its plugin window. Built with ARC; the window is held
// through a retained bridge so the header stays plain C++.
#import <AppKit/AppKit.h>

#include <cmath>
#include <cstdint>

#include "TestHostWindow.h"

namespace reckless::test
{
TestHostWindow::TestHostWindow(int width, int height)
{
    [NSApplication sharedApplication];
    NSWindow *window          = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                                            styleMask:NSWindowStyleMaskBorderless
                                                              backing:NSBackingStoreBuffered
                                                                defer:NO];
    window.releasedWhenClosed = NO; // ARC owns it: closing must not release it a second time
    window_                   = (__bridge_retained void *)window;
}

TestHostWindow::~TestHostWindow()
{
    if (window_ != nullptr)
    {
        NSWindow *window = (__bridge_transfer NSWindow *)window_;
        [window orderOut:nil];
        [window close];
        window_ = nullptr;
    }
}

bool TestHostWindow::Valid() const noexcept
{
    return window_ != nullptr;
}

void *TestHostWindow::Parent() const noexcept
{
    return window_ != nullptr ? (__bridge void *)((__bridge NSWindow *)window_).contentView : nullptr;
}

void TestHostWindow::Show(bool on_screen)
{
    if (window_ == nullptr)
    {
        return;
    }
    NSWindow *window = (__bridge NSWindow *)window_;
    if (on_screen)
    {
        [window orderFront:nil];
    }
    else
    {
        [window orderOut:nil];
    }
}

bool TestHostWindow::PopupShowing() const
{
    for (NSWindow *window in [NSApp windows])
    {
        if ([window isKindOfClass:[NSPanel class]] && ![window isKindOfClass:[NSSavePanel class]] && window.visible &&
            window.level == NSPopUpMenuWindowLevel)
        {
            return true;
        }
    }
    return false;
}

bool TestHostWindow::ScrollWheel(float dy, bool inverted)
{
    if (window_ == nullptr)
    {
        return false;
    }
    NSWindow *window   = (__bridge NSWindow *)window_;
    NSView   *attached = window.contentView.subviews.firstObject;
    if (attached == nil)
    {
        return false;
    }
    // A pixel scroll, as a trackpad's: an event with no window places itself in screen coordinates, which a view reads
    // as its window's, so the window's own middle is given as that point (the Quartz event's location is flipped
    // against the main display's height). Field 137 of a scroll wheel event is where the system marks the device's
    // direction inverted, which NSEvent's isDirectionInvertedFromDevice reads back.
    const NSRect  content = [window contentRectForFrameRect:window.frame];
    const NSPoint middle  = NSMakePoint(content.size.width / 2.0, content.size.height / 2.0);
    const CGFloat primary = NSScreen.screens.firstObject.frame.size.height;
    CGEventRef    scroll =
        CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 1, static_cast<int32_t>(std::lround(dy)));
    if (scroll == nullptr)
    {
        return false;
    }
    CGEventSetLocation(scroll, CGPointMake(middle.x, primary - middle.y));
    CGEventSetIntegerValueField(scroll, static_cast<CGEventField>(137), inverted ? 1 : 0);
    NSEvent *event = [NSEvent eventWithCGEvent:scroll];
    CFRelease(scroll);
    if (event == nil || [event isDirectionInvertedFromDevice] != inverted)
    {
        return false; // the system no longer marks it there: nothing a view hears could say what it reads
    }
    [attached scrollWheel:event];
    return true;
}
} // namespace reckless::test
