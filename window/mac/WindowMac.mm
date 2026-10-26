// The child window on macOS: an NSView drawing a view tree through Skia into a raster at the backing scale, put on
// screen through a CGImage, with the mouse, the keys and dropped files handed to a MouseRouter and a timer for the
// frame callback - or, where a product has asked for it (ChildWindow::SetFramePacing), the display's own link, whose
// refreshes the window counts. A popup is the same surface in a borderless, non-activating panel over the host's
// window, watched by a local event monitor for presses outside it. Screen points are Cocoa's (y up from the main
// screen's bottom), opaque to the views, which only hand them back to Popup::Contains.
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "ClipboardMac.h"
#include "SkiaCanvas.h"
#include "Window.h"

// The view's class is registered at run time (PanelViewClass, below) under the module's own name for it -
// reckless_module_window gives each module its own, RecklessPanelView_<target> - and the address of this loaded image.
// The Objective-C runtime keeps one class per name in a process: of two images defining a class under one name, each
// still makes its views from its own, but only one is found by the name - which one is undefined, the runtime warns -
// so whatever looks the class up by its name, as key-value observing does when it makes a subclass of a view's class,
// may take one image's class for the other's views, and lose it when that image is unloaded. Two images of one module
// in a process are a plug-in's VST3 and its AU in one host, or two versions of a plug-in, the one a host scans in
// beside the one an open session holds: a name compiled in is shared by every build of the module, and one made as the
// image makes its first view is the image's own.
#ifndef RECKLESS_PANEL_VIEW
#define RECKLESS_PANEL_VIEW RecklessPanelView
#endif
#define RECKLESS_PANEL_VIEW_TEXT2(name) #name
#define RECKLESS_PANEL_VIEW_TEXT(name)  RECKLESS_PANEL_VIEW_TEXT2(name)

namespace reckless::window
{
class MacSurface;
class MacPopup;
} // namespace reckless::window

namespace reckless::window
{
namespace
{
/** The system cursor a view's cursor kind stands for.
    @param cursor  the cursor a view asked for
    @return        the matching system cursor; the arrow for kArrow */
NSCursor *CursorFor(view::Cursor cursor)
{
    switch (cursor)
    {
    case view::Cursor::kPointingHand:
        return [NSCursor pointingHandCursor];
    case view::Cursor::kIBeam:
        return [NSCursor IBeamCursor];
    case view::Cursor::kGrab:
        return [NSCursor openHandCursor];
    case view::Cursor::kGrabbing:
        return [NSCursor closedHandCursor];
    case view::Cursor::kUpDown:
        return [NSCursor resizeUpDownCursor];
    case view::Cursor::kLeftRight:
        return [NSCursor resizeLeftRightCursor];
    case view::Cursor::kCrosshair:
        return [NSCursor crosshairCursor];
    case view::Cursor::kArrow:
        break;
    }
    return [NSCursor arrowCursor];
}

/** The panel's view class, registered once per loaded image (the methods are below).
    @return the class, made on the first call */
Class PanelViewClass();

/** The surface a view of the class draws, in its one instance variable; null once the surface let it go.
    @param view  a view of PanelViewClass()
    @return      the surface, or nullptr */
MacSurface *OwnerOf(id view);

/** Set the surface a view of the class draws.
    @param view   a view of PanelViewClass()
    @param owner  the surface; nullptr when the surface lets the view go */
void SetOwner(NSView *view, MacSurface *owner);

/** The selector of the view method a display link made from that view calls (ViewDisplayLinkTicked), registered by
    name because NSView declares nothing of it.
    @return  the selector */
SEL DisplayLinkTickedSelector()
{
    return sel_registerName("recklessDisplayLinkTicked:");
}

/** Text in Cocoa's form.
    @param utf8  the text, in UTF-8
    @return      the autoreleased NSString */
NSString *StringOf(const std::string &utf8)
{
    return [NSString stringWithUTF8String:utf8.c_str()];
}

// --------------------------------------------------------------------------------------------------------------
// The display's own clock, where a product has asked to be paced on it (ChildWindow::SetFramePacing).
// --------------------------------------------------------------------------------------------------------------

/** Mach's absolute time in seconds, which is the clock CACurrentMediaTime reads and the one a display link's own
    timestamps are on - so a CoreVideo link's host time and a Core Animation link's timestamp are the same clock.
    @param host_time  mach's absolute time, as a CVTimeStamp carries it
    @return           the same instant in seconds */
double SecondsOfHostTime(std::uint64_t host_time) noexcept
{
    static const double tick_seconds = [] {
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);
        return timebase.denom > 0 ? static_cast<double>(timebase.numer) / timebase.denom / 1.0e9 : 0.0;
    }();
    return static_cast<double>(host_time) * tick_seconds;
}

/** The screen `view` is on: the main screen where its window is on none yet, since a window being made is on no
    screen and its display's rate is still worth reading.
    @param view  the view; may be on no window or no screen
    @return      the screen, or nil where there is not even a main screen */
NSScreen *ScreenOf(NSView *view)
{
    NSScreen *screen = [[view window] screen];
    return screen != nil ? screen : [NSScreen mainScreen];
}

/** The display `view` is on, as CoreGraphics names it.
    @param view  the view; may be on no window or no screen
    @return      the display's id, or kCGNullDirectDisplay where the view is on no screen */
CGDirectDisplayID DisplayOf(NSView *view)
{
    NSScreen *screen = ScreenOf(view);
    NSNumber *number = screen != nil ? [screen deviceDescription][@"NSScreenNumber"] : nil;
    return number != nil ? static_cast<CGDirectDisplayID>([number unsignedIntValue]) : kCGNullDirectDisplay;
}

/** What the window server and the display mode say about the rate of the display `view` is on. The window server's
    own answer (NSScreen's maximumFramesPerSecond) is the compositor's, since that is the rate it composites the
    screen at; the mode's (CGDisplayModeGetRefreshRate) is the cross-check, and is 0 on the displays CoreGraphics
    reports no rate for, where the policy then has the window server's alone.
    @param view  the view whose display is read
    @return      the reading, with nothing yet said about whether the period holds still */
DisplayClockReading DisplayClockReadingOf(NSView *view)
{
    DisplayClockReading reading;
    if (@available(macOS 14.0, *))
    {
        reading.has_clock = true; // Core Animation's link is made from the view itself, whatever screen it is on
    }
    else
    {
        reading.has_clock = DisplayOf(view) != kCGNullDirectDisplay; // CoreVideo's has to be given a display
    }
    if (NSScreen *screen = ScreenOf(view); screen != nil)
    {
        reading.compositor_hertz = static_cast<double>([screen maximumFramesPerSecond]);
    }
    if (const CGDirectDisplayID display = DisplayOf(view); display != kCGNullDirectDisplay)
    {
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display))
        {
            reading.display_hertz = CGDisplayModeGetRefreshRate(mode);
            CGDisplayModeRelease(mode);
        }
    }
    return reading;
}

/** What a CoreVideo display link's callback and the main thread share. The callback runs on a thread of CoreVideo's
    own, so each tick is handed to the main thread, and a tick already on its way as the clock stops finds
    `is_pacing` false rather than a window that has gone. */
struct CoreVideoTicks
{
    std::function<void(double tick_seconds, double period_seconds)> ticked; ///< run on the main thread, per tick
    std::atomic<bool> is_pacing{true};    ///< false once the clock has stopped: a tick in flight is then dropped
    std::atomic<int>  ticks_in_flight{0}; ///< ticks handed to the main thread and not yet run
};

/** A CoreVideo display link's callback, on a thread of CoreVideo's own: the tick handed to the main thread, unless one
    is already waiting there - a main thread that is busy is given no backlog to run in a burst, and a tick dropped
    costs nothing, the subdivision being counted from the ticks' own timestamps rather than from how many arrived.
    @param output  the time the frame being timed is for
    @param user    a pointer to the shared CoreVideoTicks, which outlives every tick in flight
    @return        kCVReturnSuccess always: nothing here can refuse a tick */
CVReturn CoreVideoTicked(CVDisplayLinkRef, const CVTimeStamp *, const CVTimeStamp *output, CVOptionFlags,
                         CVOptionFlags *, void *user)
{
    const std::shared_ptr<CoreVideoTicks> ticks = *static_cast<std::shared_ptr<CoreVideoTicks> *>(user);
    const double                          tick =
        (output->flags & kCVTimeStampHostTimeValid) != 0 ? SecondsOfHostTime(output->hostTime) : CACurrentMediaTime();
    const double period =
        (output->flags & kCVTimeStampVideoRefreshPeriodValid) != 0 && output->videoTimeScale > 0
            ? static_cast<double>(output->videoRefreshPeriod) / static_cast<double>(output->videoTimeScale)
            : 0.0;
    if (ticks->ticks_in_flight.fetch_add(1, std::memory_order_acq_rel) > 0)
    {
        ticks->ticks_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return kCVReturnSuccess;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      ticks->ticks_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      if (ticks->is_pacing.load(std::memory_order_acquire) && ticks->ticked)
      {
          ticks->ticked(tick, period);
      }
    });
    return kCVReturnSuccess;
}

// CoreVideo's display link is deprecated from macOS 15. It is reached only below macOS 14, where Core Animation's is
// not there to use: the toolkit's floor is 13.3, and a Mac on 13 is owed the display's clock as much as one on 15.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/** A CoreVideo display link on `display`, with CoreVideoTicked as its callback, started.
    @param display  the display to pace on
    @param held     the pointer the callback is given, which outlives every tick in flight
    @return         the link, which the caller releases; null where CoreVideo refused any step */
CVDisplayLinkRef MakeCoreVideoLink(CGDirectDisplayID display, std::shared_ptr<CoreVideoTicks> *held)
{
    CVDisplayLinkRef link = nullptr;
    if (CVDisplayLinkCreateWithCGDisplay(display, &link) != kCVReturnSuccess || link == nullptr)
    {
        return nullptr;
    }
    if (CVDisplayLinkSetOutputCallback(link, &CoreVideoTicked, held) != kCVReturnSuccess ||
        CVDisplayLinkStart(link) != kCVReturnSuccess)
    {
        CVDisplayLinkRelease(link);
        return nullptr;
    }
    return link;
}

/** A CoreVideo display link stopped - which returns only once a tick of CoreVideo's own thread has finished, so
    nothing reads what the callback was given after it - and released.
    @param link  the link; nothing is done for null */
void EndCoreVideoLink(CVDisplayLinkRef link)
{
    if (link != nullptr)
    {
        CVDisplayLinkStop(link);
        CVDisplayLinkRelease(link);
    }
}

#pragma clang diagnostic pop

/**
    The display's own clock on macOS: a callback once a refresh of the display the view is on, which the window counts
    to deliver a frame every Nth. This is the clean case of the whole design - there is nothing to wait for and
    nothing to snap, because the platform is already calling once a refresh, and "shown for exactly N refresh
    periods" is a count.

    Two links, by what the Mac has. **Core Animation's**, from macOS 14, is made from the view itself
    (`-[NSView displayLinkWithTarget:selector:]`), follows that view's own screen as the window is moved between
    displays, and calls back on the run loop it is added to - the window's own thread, with no hop. **CoreVideo's** is
    the fallback down to the toolkit's floor of 13.3: it is made for one display by id, calls back on a thread of its
    own, and so each tick is handed to the main thread (CoreVideoTicked). Its calls are deprecated from macOS 15,
    which is why they are only ever reached below 14.
*/
class MacDisplayClock
{
  public:
    MacDisplayClock() = default;

    /** Stopped, its link invalidated or released, before the window it ticks can go. */
    ~MacDisplayClock()
    {
        Stop();
    }

    MacDisplayClock(const MacDisplayClock &)            = delete;
    MacDisplayClock &operator=(const MacDisplayClock &) = delete;

    /** Started on the display `view` is on. Core Animation's link is made from the view, with the view as its target
        (the view's display-link method hands each tick to the surface), and CoreVideo's below macOS 14 with `ticked`
        as its destination on the main thread.
        @param view    the view whose display is paced on; outlives the clock
        @param ticked  run on the main thread for each tick of a CoreVideo link, with the tick's time and the
                       display's refresh period, both in seconds; unused by a Core Animation link, whose ticks reach
                       the view
        @return        true where a link was made and started */
    bool Start(NSView *view, std::function<void(double tick_seconds, double period_seconds)> ticked)
    {
        Stop();
        if (@available(macOS 14.0, *))
        {
            CADisplayLink *link = [view displayLinkWithTarget:view selector:DisplayLinkTickedSelector()];
            if (link == nil)
            {
                return false;
            }
            [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
            animation_link_ = link;
            return true;
        }
        const CGDirectDisplayID display = DisplayOf(view);
        if (display == kCGNullDirectDisplay)
        {
            return false;
        }
        ticks_         = std::make_shared<CoreVideoTicks>();
        ticks_->ticked = std::move(ticked);
        ticks_held_    = new std::shared_ptr<CoreVideoTicks>(ticks_);
        video_link_    = MakeCoreVideoLink(display, ticks_held_);
        if (video_link_ == nullptr)
        {
            Stop();
            return false;
        }
        return true;
    }

    /** Stopped: Core Animation's link invalidated, or CoreVideo's stopped - which waits for a tick of its own that is
        running - and released, and any tick already handed to the main thread left with nothing to reach. Calling it
        on a clock that is not running does nothing. */
    void Stop()
    {
        if (animation_link_ != nil)
        {
            if (@available(macOS 14.0, *))
            {
                [static_cast<CADisplayLink *>(animation_link_) invalidate];
            }
            animation_link_ = nil;
        }
        if (video_link_ != nullptr)
        {
            EndCoreVideoLink(video_link_); // waits for a tick of CoreVideo's own thread, so `ticks_held_` is then free
            video_link_ = nullptr;
        }
        if (ticks_ != nullptr)
        {
            ticks_->is_pacing.store(false, std::memory_order_release);
            ticks_.reset();
        }
        delete ticks_held_;
        ticks_held_ = nullptr;
    }

    /** Whether a link is running.
        @return  true while one is */
    bool is_running() const noexcept
    {
        return animation_link_ != nil || video_link_ != nullptr;
    }

  private:
    /** Core Animation's link, held as `id` so that a build whose floor is below macOS 14 names its class only where
        it is available; nil where there is none. */
    id animation_link_{nil};

    /** CoreVideo's link, below macOS 14; null where there is none. */
    CVDisplayLinkRef video_link_{nullptr};

    /** What CoreVideo's callback and the main thread share, while CoreVideo's link runs. */
    std::shared_ptr<CoreVideoTicks> ticks_;

    /** The pointer CoreVideo's callback is given, which it copies the shared pointer out of: deleted only after the
        link has been stopped, which waits for any tick of its own that is running. */
    std::shared_ptr<CoreVideoTicks> *ticks_held_{nullptr};
};

/** "*.wav" as "wav"; an extension the dialog filters on.
    @param pattern  a file pattern, "*.wav" or an extension alone
    @return         the text after the last dot, or the whole pattern where there is none */
NSString *ExtensionOf(const std::string &pattern)
{
    const auto dot = pattern.rfind('.');
    return StringOf(dot == std::string::npos ? pattern : pattern.substr(dot + 1));
}
} // namespace

/** A view tree drawn in an NSView: the raster, the drawing, the mouse, keys and drops to a router, and the host the
    tree lives in. The child window and every popup are one. */
class MacSurface : public view::Host
{
  public:
    /** Make the NSView of the surface and put the tree in it.
        @param root   the view tree drawn; owned by the caller, which outlives the surface
        @param scale  design pixels to points, 1 for the tree at its design size
        @param frame  the NSView's frame in points, in its parent's coordinates */
    MacSurface(view::View &root, float scale, NSRect frame) : root_(root), router_(root, this), scale_(scale)
    {
        view_ = [[PanelViewClass() alloc] initWithFrame:frame];
        SetOwner(view_, this);
        [view_ setWantsLayer:YES];
        tracking_ = [[NSTrackingArea alloc]
            initWithRect:NSZeroRect
                 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                         NSTrackingInVisibleRect | NSTrackingCursorUpdate
                   owner:view_
                userInfo:nil];
        [view_ addTrackingArea:tracking_];
        [view_ registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        root_.SetHost(this);
    }

    /** The surface released (Release): its popups closed, the tree's host dropped, the view's lifetime ended. */
    ~MacSurface() override
    {
        Release();
    }

    /** Every popup opened over the surface closed, and theirs, each `closed_by_window` gathered into `heard` for
        whoever let the surface go to run once everything is closed.
        @param heard  receives each closed popup's `closed_by_window`, where it has one */
    void EndPopups(std::vector<std::function<void()>> &heard);

    /** `popup`, going on its own, off the list of those opened over the surface.
        @param popup  the popup that is going; no longer reached by the surface afterwards */
    void Forget(const MacPopup *popup)
    {
        popups_.erase(std::remove(popups_.begin(), popups_.end(), popup), popups_.end());
    }

    // ---- view::Host -----------------------------------------------------------------------------------------------

    /** The part of the root to paint again, handed to the view in backing pixels.
        @param area  the part, in the root's coordinates */
    void Invalidate(gfx::RectI area) override
    {
        [view_ setNeedsDisplayInRect:NSMakeRect(area.x() * scale_, area.y() * scale_, area.width() * scale_,
                                                area.height() * scale_)];
    }

    /** How wide `text` is in `font`, measured by the canvas that draws it.
        @param font  the font the text is drawn in
        @param text  the UTF-8 text
        @return      the width, in pixels at the root's scale */
    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        return gfx::MeasureText(font, text);
    }

    /** `call` on the main queue once the event in hand is done - and not at all if the surface has gone by then, since
        what it reaches (a view, the panel round it) went with it.
        @param call  what to run, on the main thread */
    void CallSoon(std::function<void()> call) override
    {
        auto                      shared = std::make_shared<std::function<void()>>(std::move(call));
        const std::weak_ptr<char> alive  = lifetime_;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (!alive.expired())
          {
              (*shared)();
          }
        });
    }

    /** The cursor over the surface, set now and kept for the NSView's cursor rects.
        @param cursor  the cursor the view under the pointer asked for */
    void SetCursor(view::Cursor cursor) override
    {
        cursor_ = cursor;
        [CursorFor(cursor) set];
        [[view_ window] invalidateCursorRectsForView:view_];
    }

    /** Hand the keyboard to a view of the tree, through the router.
        @param view  the view to focus; nullptr takes the focus away */
    void RequestKeyboardFocus(view::View *view) override
    {
        router_.Focus(view);
    }

    void KeyboardFocusChanged(bool a_view_has_it) override
    {
        wants_keys_    = a_view_has_it;
        NSWindow *host = [view_ window];
        if (!a_view_has_it)
        {
            LetGoOfHeldRawKeys(); // the keyboard is going elsewhere: nothing it said was down stays down
        }
        if (a_view_has_it)
        {
            previous_responder_ = [host firstResponder];
            [host makeFirstResponder:view_];
        }
        else if ([host firstResponder] == view_)
        {
            [host makeFirstResponder:previous_responder_]; // the keyboard back to whoever had it
            previous_responder_ = nil;
        }
    }

    /** Every raw key edge told to `raw_key` (ChildWindow::SetRawKeyCallback): nothing while it is empty, which it is
        until an owner asks. With the first callback the surface also starts watching for its window losing the
        keyboard - another app brought to the front with a key held, which leaves the view first responder and so
        tells it nothing - on which everything held is let go. The keys held are let go first, so a callback replaced
        mid-press leaves nothing owed and nothing held.
        @param raw_key  told of each edge; empty for none */
    void TakeRawKeys(RawKeyCallback raw_key)
    {
        LetGoOfHeldRawKeys();
        const bool had_none = !raw_key_;
        raw_key_            = std::move(raw_key);
        if (!had_none || !raw_key_ || released_ || key_resign_watch_ != nil)
        {
            return;
        }
        const std::weak_ptr<char> alive = lifetime_;
        key_resign_watch_ =
            [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
                                                              object:nil
                                                               queue:nil
                                                          usingBlock:^(NSNotification *note) {
                                                            if (!alive.expired() && note.object == [view_ window])
                                                            {
                                                                LetGoOfHeldRawKeys();
                                                            }
                                                          }];
    }

    /** One edge of one key told to whoever asked for the raw keys, and nothing where no one has. A down of a key
        already held is auto-repeat's (`is_repeat`) - the key's own `isARepeat` is not read, since it is set for a key
        that was already down when the view took the keyboard, whose down nothing was told of - and an up is told only
        for a key whose down was.
        @param key  the edge, its repeat flag unset */
    void TellRawKey(RawKeyEvent key)
    {
        if (!raw_key_)
        {
            return;
        }
        if (key.is_down)
        {
            key.is_repeat = raw_keys_held_.NoteDown(key);
            raw_key_(key);
        }
        else if (raw_keys_held_.NoteUp(key.physical_key_code))
        {
            raw_key_(key);
        }
    }

    /** One edge of one modifier key told to whoever asked for the raw keys: `flagsChanged:` says a modifier changed
        and which key it was, never which way it went, and its flag stays set while the other key of the pair is held.
        So the edge is a down where the flag is set and this key is not held already, and an up otherwise - which is
        also right for caps lock, whose flag follows the lamp and so is set by one press and cleared by the next.
        @param key          the edge, its `is_down` and repeat flag unset
        @param is_flag_held whether the key's own modifier flag is set now */
    void TellRawModifierKey(RawKeyEvent key, bool is_flag_held)
    {
        if (!raw_key_)
        {
            return;
        }
        key.is_down = is_flag_held && !raw_keys_held_.IsHeld(key.physical_key_code);
        TellRawKey(key);
    }

    /** An up told for every key the raw key callback was told is down, and nothing held afterwards: what the surface
        does as the keyboard goes elsewhere, so no key it said was down stays down. */
    void LetGoOfHeldRawKeys()
    {
        for (const RawKeyEvent &up : raw_keys_held_.TakeUps())
        {
            if (raw_key_)
            {
                raw_key_(up);
            }
        }
    }

    /** A view of the tree is going: the router forgets it, so no press, hover or focus outlives it.
        @param view  the view that is leaving the tree */
    void ViewLeaving(const view::View &view) override
    {
        router_.Forget(view);
    }

    /** A popup, a `MacPopup`, opened over this surface.
        @param content          the tree the popup shows; owned by the caller, which outlives the popup
        @param at               the popup's top left in this surface's design pixels
        @param pressed_outside  told, with the press's screen point, of each mouse press outside the popup
        @param takes_mouse      whether the popup receives mouse events itself
        @return                 the popup, or nullptr when the surface has gone or is not in a window */
    std::unique_ptr<view::Popup> OpenPopup(view::View &content, gfx::PointI at,
                                           std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse) override;

    /** The general pasteboard's text, for a paste.
        @return  its string in UTF-8, or empty when it holds none */
    std::string ClipboardText() override
    {
        NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return text != nil ? std::string([text UTF8String]) : std::string();
    }

    /** `text` on the general pasteboard, for a copy (mac::WritePasteboardText).
        @param text  the text, UTF-8
        @return      true when the pasteboard took it */
    bool SetClipboardText(const std::string &text) override
    {
        return mac::WritePasteboardText([NSPasteboard generalPasteboard], text);
    }

    /** The system open or save dialog, not modal; cancelled if the surface goes while it is up.
        @param request  what to ask: open or save, the title, the folder, the file name and the extension patterns
        @param picked   called on the main thread with the chosen path, or an empty string for a cancel; never called
                        once the surface has gone */
    void ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> picked) override
    {
        if (released_)
        {
            return; // let go: nothing hears an answer
        }
        auto         callback = std::make_shared<std::function<void(const std::string &)>>(std::move(picked));
        NSSavePanel *panel    = request.save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        if (!request.save)
        {
            NSOpenPanel *open            = (NSOpenPanel *)panel;
            open.canChooseFiles          = YES;
            open.canChooseDirectories    = NO;
            open.allowsMultipleSelection = NO;
        }
        else if (!request.file_name.empty())
        {
            panel.nameFieldStringValue = StringOf(request.file_name);
        }
        if (!request.title.empty())
        {
            panel.message = StringOf(request.title);
        }
        if (!request.folder.empty())
        {
            panel.directoryURL = [NSURL fileURLWithPath:StringOf(request.folder) isDirectory:YES];
        }
        if (!request.patterns.empty())
        {
            NSMutableArray<UTType *> *types = [NSMutableArray array];
            for (const auto &pattern : request.patterns)
            {
                if (UTType *type = [UTType typeWithFilenameExtension:ExtensionOf(pattern)])
                {
                    [types addObject:type];
                }
            }
            if (types.count > 0)
            {
                panel.allowedContentTypes = types;
            }
        }
        // The panel is not modal and the application keeps it, so it can still be up when the window goes - with an
        // answer to come that `picked`, holding a view or the panel round it, must not hear: the surface cancels it as
        // it goes, and the answer finds the lifetime gone.
        [open_panels_ addObject:panel];
        MacSurface               *surface = this;
        const std::weak_ptr<char> alive   = lifetime_;
        [panel beginWithCompletionHandler:^(NSModalResponse response) {
          if (alive.expired())
          {
              return;
          }
          [surface->open_panels_ removeObject:panel];
          const bool chosen = response == NSModalResponseOK && panel.URL != nil;
          (*callback)(chosen ? std::string([panel.URL.path UTF8String]) : std::string());
        }];
    }

    // ---- From the NSView ------------------------------------------------------------------------------------------

    /** The tree drawn into `context` over `dirty` (points).
        @param context  the NSView's graphics context
        @param dirty    the part to draw again, in the view's points */
    void Draw(CGContextRef context, NSRect dirty)
    {
        const CGFloat backing = [[view_ window] backingScaleFactor] > 0 ? [[view_ window] backingScaleFactor] : 2.0;
        const NSRect  bounds  = [view_ bounds];
        const int     pw      = static_cast<int>(std::ceil(bounds.size.width * backing));
        const int     ph      = static_cast<int>(std::ceil(bounds.size.height * backing));
        if (pw <= 0 || ph <= 0)
        {
            return;
        }
        if (!surface_ || surface_->width() != pw || surface_->height() != ph)
        {
            surface_ = SkSurfaces::Raster(SkImageInfo::Make(pw, ph, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
        }
        SkCanvas *sk = surface_->getCanvas();
        sk->save();
        sk->resetMatrix();
        const float total = static_cast<float>(backing) * scale_;
        sk->scale(total, total);
        // Only the dirty part is drawn again: the rest of the raster still holds the last frame.
        const gfx::RectI clip =
            gfx::RectF{static_cast<float>(dirty.origin.x / scale_), static_cast<float>(dirty.origin.y / scale_),
                       static_cast<float>(dirty.size.width / scale_), static_cast<float>(dirty.size.height / scale_)}
                .SmallestIntegerContainer();
        {
            gfx::SkiaCanvas canvas(*sk, 1.0f);
            canvas.ClipToRect(clip);
            if (clear_first_)
            {
                canvas.FillAll(gfx::Color()); // a popup's corners stay see-through
            }
            view::PaintTree(root_, canvas);
        }
        sk->restore();

        SkPixmap pixmap;
        if (!surface_->peekPixels(&pixmap))
        {
            return;
        }
        CGColorSpaceRef   space = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, pixmap.addr(), pixmap.computeByteSize(), nullptr);
        CGImageRef image =
            CGImageCreate(static_cast<size_t>(pw), static_cast<size_t>(ph), 8, 32, pixmap.rowBytes(), space,
                          static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                              static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst),
                          provider, nullptr, false, kCGRenderingIntentDefault);
        CGContextSaveGState(context);
        CGContextClipToRect(context, dirty);
        // The view is flipped (y down), as is the raster; CGContextDrawImage draws y up, so flip it back.
        CGContextTranslateCTM(context, 0, bounds.size.height);
        CGContextScaleCTM(context, 1.0, -1.0);
        CGContextDrawImage(context, CGRectMake(0, 0, bounds.size.width, bounds.size.height), image);
        CGContextRestoreGState(context);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(space);
    }

    /** A point in the window's coordinates as a point in the root's.
        @param window_point  a point in the window, in points, y up
        @return              the point in design pixels, y down */
    gfx::PointF ToDesign(NSPoint window_point) const
    {
        const NSPoint p = [view_ convertPoint:window_point fromView:nil];
        return {static_cast<float>(p.x / scale_), static_cast<float>(p.y / scale_)};
    }

    /** Where a mouse event fell, in the root's coordinates.
        @param event  a mouse event delivered to the NSView
        @return       its location in design pixels, y down */
    gfx::PointF ToDesign(NSEvent *event) const
    {
        return ToDesign([event locationInWindow]);
    }

    /** A point in the root's coordinates as a point on the screen.
        @param at  a point in design pixels
        @return    the point in Cocoa's screen coordinates, y up from the main screen's bottom */
    NSPoint ToScreen(gfx::PointI at) const
    {
        const NSPoint in_view   = NSMakePoint(at.x * scale_, at.y * scale_);
        const NSPoint in_window = [view_ convertPoint:in_view toView:nil];
        return [[view_ window] convertPointToScreen:in_window];
    }

    /** The modifier keys of an event in the views' form.
        @param flags  an event's modifier flags
        @return       shift, command and option, each true while held */
    static view::Modifiers ModifiersOf(NSEventModifierFlags flags)
    {
        return {(flags & NSEventModifierFlagShift) != 0, (flags & NSEventModifierFlagCommand) != 0,
                (flags & NSEventModifierFlagOption) != 0};
    }

    /** The router the surface's mouse, key and drop events go to.
        @return the router, owned by the surface */
    view::MouseRouter &router() noexcept
    {
        return router_;
    }

    /** Whether the NSView takes key events.
        @return true while a view of the tree holds the keyboard focus */
    bool wants_keys() const noexcept
    {
        return wants_keys_;
    }

    /** The cursor last asked for, which the NSView's cursor rects set.
        @return the cursor; the arrow until a view asks */
    view::Cursor cursor() const noexcept
    {
        return cursor_;
    }

    /** The NSView the surface draws in.
        @return the view, or nil once the surface has let it go */
    NSView *view() const noexcept
    {
        return view_;
    }

    /** Design pixels to points, as the surface lays the tree out.
        @return the scale, 1 at the design size */
    float scale() const noexcept
    {
        return scale_;
    }

    /** The view's display link ticked: told to whoever asked for it, and nothing where no one has. Called by the
        view's display-link method (ViewDisplayLinkTicked), so it is public as the view's other answers are.
        @param tick_seconds    the tick's own time, in seconds on mach's clock
        @param period_seconds  the display's refresh period, in seconds; 0 where the link did not say */
    void DisplayLinkTicked(double tick_seconds, double period_seconds)
    {
        if (display_tick_)
        {
            display_tick_(tick_seconds, period_seconds);
        }
    }

    /** Whether every pixel is drawn opaque: the child window's panel is, a popup's rounded corners are not.
        @return true when the raster is not cleared before each draw */
    bool opaque() const noexcept
    {
        return !clear_first_;
    }

  protected:
    /** The surface let go, once - what its destruction does, and what a popup's closing with the window it opened
        over does early. From here on nothing it deferred (CallSoon) and no dialog's answer (ChooseFile) runs, each
        holding the lifetime weakly; a dialog still up goes, its answer (which a cancel gives at once) finding the
        lifetime gone; the popups opened over it close, and theirs; the tree loses its host. Then each closed popup's
        `closed_by_window` runs - a menu ends dismissed, its owner free to drop it there - when nothing of the surface
        is left for it to reach. */
    void Release()
    {
        std::vector<std::function<void()>> heard;
        ReleaseInto(heard);
        for (const auto &closed : heard)
        {
            closed();
        }
    }

    /** Release's letting go, the closed popups' `closed_by_window` gathered into `heard` for the caller to run.
        @param heard  receives each closed popup's `closed_by_window`, where it has one */
    void ReleaseInto(std::vector<std::function<void()>> &heard)
    {
        if (released_)
        {
            return;
        }
        released_ = true;
        lifetime_.reset();
        if (key_resign_watch_ != nil)
        {
            [[NSNotificationCenter defaultCenter] removeObserver:key_resign_watch_];
            key_resign_watch_ = nil;
        }
        NSArray<NSSavePanel *> *panels = [open_panels_ copy];
        [open_panels_ removeAllObjects];
        for (NSSavePanel *panel in panels)
        {
            [panel cancel:nil];
        }
        EndPopups(heard);
        root_.SetHost(nullptr);
        if (view_ == nil)
        {
            return;
        }
        if (wants_keys_ && [[view_ window] firstResponder] == view_)
        {
            [[view_ window] makeFirstResponder:previous_responder_];
        }
        SetOwner(view_, nullptr);
        [view_ removeTrackingArea:tracking_];
        [view_ unregisterDraggedTypes];
        [view_ removeFromSuperview];
        view_ = nil;
    }

    view::View       &root_;                         ///< The tree drawn; the caller's.
    view::MouseRouter router_;                       ///< Takes the mouse, key and drop events.
    float             scale_;                        ///< Design pixels to points.
    NSView           *view_{nil};                    ///< The NSView drawn in; nil once let go.
    NSTrackingArea   *tracking_{nil};                ///< Delivers moves, enters and leaves over the whole view.
    NSResponder      *previous_responder_{nil};      ///< Who had the keyboard before the tree took it.
    sk_sp<SkSurface>  surface_;                      ///< The raster, sized to the view's backing pixels.
    bool              wants_keys_{false};            ///< True while a view of the tree holds the keyboard focus.
    bool              clear_first_{false};           ///< True when each draw starts transparent (a popup).
    view::Cursor      cursor_{view::Cursor::kArrow}; ///< The cursor last asked for.

    /** Told of each tick of the display link pacing the frame callback, with the tick's own time and the display's
        refresh period, both in seconds on mach's clock (ChildWindow::SetFramePacing). **Empty until a product asks**,
        and no link is made until then, so the view's display-link method is never called and nothing of this is
        reached by a window that was never asked. */
    std::function<void(double tick_seconds, double period_seconds)> display_tick_;

  private:
    /** Held weakly by every call the surface defers and every dialog it opens: gone with the surface, so neither
        reaches it, or the views it held, afterwards. */
    std::shared_ptr<char>          lifetime_{std::make_shared<char>()};
    NSMutableArray<NSSavePanel *> *open_panels_{[NSMutableArray array]}; ///< the file dialogs up, cancelled as it goes
    std::vector<MacPopup *>        popups_;          ///< the popups opened over it, which go with it
    bool                           released_{false}; ///< True once Release has let the surface go.
    /** Told of every raw key edge (ChildWindow::SetRawKeyCallback); empty until an owner asks, until when nothing of
        the raw keys is read or delivered and the view's key up, flags and resignation are AppKit's own. */
    RawKeyCallback raw_key_;
    /** The keys the raw key callback has been told are down, which answer whether a down is auto-repeat's, whether an
        up is owed, and what is to be let go as the keyboard goes elsewhere. */
    RawKeysHeld raw_keys_held_;
    /** The watch on the window losing the keyboard, taken with the first raw key callback and dropped as the surface
        goes; nil while there is none. */
    id key_resign_watch_{nil};
};

/** A popup: the surface in a borderless panel that never takes the host's window's place, over everything but
    menus, and a local event monitor telling its opener of every press outside it. It goes with the surface it opened
    over (CloseInto) if that goes first. */
class MacPopup final : public view::Popup, public MacSurface
{
  public:
    /** The popup's panel on the screen, with its monitor of presses outside it.
        @param opener           the surface it opened over; told when the popup goes
        @param content          the tree shown; the caller's
        @param screen_top_left  the top left in Cocoa's screen coordinates, moved in to keep the popup on its screen
        @param scale            design pixels to points
        @param outside          told, with the press's screen point, of each mouse press outside the popup
        @param takes_mouse      whether the panel receives mouse events itself */
    MacPopup(MacSurface &opener, view::View &content, NSPoint screen_top_left, float scale,
             std::function<void(gfx::PointI)> outside, bool takes_mouse)
        : MacSurface(content, scale, NSMakeRect(0, 0, content.width() * scale, content.height() * scale)),
          opener_(&opener), outside_(std::move(outside))
    {
        clear_first_ = true;
        NSRect frame = NSMakeRect(screen_top_left.x, screen_top_left.y - content.height() * scale,
                                  content.width() * scale, content.height() * scale);
        // Kept whole on the screen it opens on: moved in from an edge it would cross.
        for (NSScreen *screen in [NSScreen screens])
        {
            if (NSPointInRect(screen_top_left, screen.frame))
            {
                const NSRect usable = screen.visibleFrame;
                frame.origin.x = std::max(NSMinX(usable), std::min(frame.origin.x, NSMaxX(usable) - frame.size.width));
                frame.origin.y = std::max(NSMinY(usable), std::min(frame.origin.y, NSMaxY(usable) - frame.size.height));
                break;
            }
        }
        panel_ = [[NSPanel alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
        panel_.level              = NSPopUpMenuWindowLevel;
        panel_.opaque             = NO;
        panel_.backgroundColor    = [NSColor clearColor];
        panel_.hasShadow          = YES;
        panel_.ignoresMouseEvents = !takes_mouse;
        panel_.releasedWhenClosed = NO;
        panel_.contentView        = view_;
        [panel_ orderFront:nil];
        MacPopup *self = this;
        monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown |
                                                                 NSEventMaskOtherMouseDown
                                                         handler:^NSEvent *(NSEvent *event) {
                                                           const NSPoint     p = [NSEvent mouseLocation];
                                                           const gfx::PointI at{static_cast<int>(std::lround(p.x)),
                                                                                static_cast<int>(std::lround(p.y))};
                                                           if (!self->Contains(at) && self->outside_)
                                                           {
                                                               self->outside_(at);
                                                           }
                                                           return event;
                                                         }];
    }

    /** The popup taken off its opener's list and its panel and monitor released. */
    ~MacPopup() override
    {
        if (opener_ != nullptr)
        {
            opener_->Forget(this);
        }
        ReleasePanel();
    }

    bool Contains(gfx::PointI screen) const override
    {
        return panel_ != nil && NSPointInRect(NSMakePoint(screen.x, screen.y), panel_.frame);
    }

    /** Closed with the surface it opened over: its panel and its monitor gone, then the surface let go - its own
        popups with it - and its `closed_by_window` gathered into `heard` after theirs, the handle left inert for its
        owner to drop, its destruction then reaching nothing.
        @param heard  receives each closed popup's `closed_by_window`, its own after those of its popups */
    void CloseInto(std::vector<std::function<void()>> &heard)
    {
        if (closed_)
        {
            return;
        }
        closed_ = true;
        opener_ = nullptr;
        ReleasePanel();
        ReleaseInto(heard);
        if (closed_by_window)
        {
            heard.push_back(closed_by_window);
        }
    }

  private:
    /** The panel and the event monitor gone; safe to call again. */
    void ReleasePanel()
    {
        if (panel_ == nil)
        {
            return;
        }
        [NSEvent removeMonitor:monitor_];
        monitor_ = nil;
        [panel_ orderOut:nil];
        panel_.contentView = nil;
        [panel_ close];
        panel_ = nil;
    }

    MacSurface                      *opener_;        ///< The surface it opened over; nullptr once closed.
    NSPanel                         *panel_{nil};    ///< The borderless panel; nil once released.
    id                               monitor_{nil};  ///< The local event monitor of outside presses.
    std::function<void(gfx::PointI)> outside_;       ///< Told of each press outside the popup.
    bool                             closed_{false}; ///< True once CloseInto has run.
};

std::unique_ptr<view::Popup> MacSurface::OpenPopup(view::View &content, gfx::PointI at,
                                                   std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse)
{
    if (released_ || [view_ window] == nil)
    {
        return nullptr;
    }
    auto popup =
        std::make_unique<MacPopup>(*this, content, ToScreen(at), scale_, std::move(pressed_outside), takes_mouse);
    popups_.push_back(popup.get());
    return popup;
}

void MacSurface::EndPopups(std::vector<std::function<void()>> &heard)
{
    for (MacPopup *popup : std::exchange(popups_, {}))
    {
        popup->CloseInto(heard);
    }
}

/** The child window inside the host's editor: the surface as a subview of the host's NSView, with the frame timer. */
class MacChildWindow final : public ChildWindow, public MacSurface
{
  public:
    /** The surface as a subview of the host's view.
        @param parent  the host's NSView; the surface is added to it
        @param root    the tree drawn; the caller's
        @param width   the design width in design pixels
        @param height  the design height in design pixels
        @param scale   design pixels to points */
    MacChildWindow(NSView *parent, view::View &root, int width, int height, float scale)
        : MacSurface(root, scale, NSMakeRect(0, 0, width * scale, height * scale))
    {
        [parent addSubview:view_];
    }

    /** The frame timer invalidated and the display clock stopped, before the view they tick can go. */
    ~MacChildWindow() override
    {
        clock_.Stop();
        [timer_ invalidate];
        timer_ = nil;
    }

    void SetScale(float scale) override
    {
        scale_ = scale;
        [view_ setFrameSize:NSMakeSize(std::lround(root_.width() * scale), std::lround(root_.height() * scale))];
        [view_ setNeedsDisplay:YES];
    }

    void SetFrameCallback(std::function<void()> frame) override
    {
        frame_ = std::move(frame);
        ArrangePacing();
    }

    /** The frame callback paced as `request` asks: the display's own link - Core Animation's from macOS 14, made from
        this window's view and following the display the window is moved to, and CoreVideo's below that - whose
        refreshes are counted, or the timer the window has always used where no link can be made or no rate believed.
        @param request  what the product asks for; FrameRequest{} for the platform's timer
        @return         what the window is pacing on, and why where it fell back */
    FramePacingState SetFramePacing(const FrameRequest &request) override
    {
        request_ = request;
        pacing_  = ChosenPacing(request_, DisplayClockReadingOf(view_));
        if (pacing_.pacing == FramePacing::kDisplayClock && !display_tick_)
        {
            // Installed once a product has asked and never taken back: a tick still on its way as the link stops
            // reaches a window that is simply no longer paced on it.
            display_tick_ = [this](double tick_seconds, double period_seconds) {
                PacedTick(tick_seconds, period_seconds);
            };
        }
        ArrangePacing();
        return pacing_;
    }

    /** What the window is pacing on now, with the rate of the display it is on now: a Core Animation link follows the
        window between displays, so the rate here is not always the one SetFramePacing answered with.
        @return  the pacing in force */
    FramePacingState FramePacingNow() const override
    {
        return pacing_;
    }

    /** In a window that is on screen and not in the Dock, and not hidden in it by the host.
        @return true when the window shows */
    bool Showing() const override
    {
        NSWindow *window = [view_ window];
        return window != nil && [window isVisible] && ![window isMiniaturized] && ![view_ isHiddenOrHasHiddenAncestor];
    }

    /** Every raw key edge told to `raw_key`: the key downs the views are given too (`keyDown:`), the key ups AppKit
        alone answered before (`keyUp:`) and the modifier keys, which a Mac reports no other way (`flagsChanged:`),
        with everything let go as the keyboard goes elsewhere.
        @param raw_key  told of each edge, on the window's thread; empty to ask for no more */
    void SetRawKeyCallback(RawKeyCallback raw_key) override
    {
        TakeRawKeys(std::move(raw_key));
    }

  private:
    /** The frame callback's source arranged for the pacing in force: the display's own link where that is what was
        decided and a link can be made, and the timer the window has always used otherwise - which is what a window
        with no request of its own always reaches, as the invalidate and the NSTimer alone. Nothing runs while there
        is no callback. */
    void ArrangePacing()
    {
        clock_.Stop();
        [timer_ invalidate];
        timer_ = nil;
        if (!frame_)
        {
            return;
        }
        if (pacing_.pacing == FramePacing::kDisplayClock)
        {
            period_divided_ = 0.0;
            display_        = kCGNullDirectDisplay;
            steadiness_.Forget();
            unsteady_ = 0;
            if (clock_.Start(view_, [this](double tick_seconds, double period_seconds) {
                    PacedTick(tick_seconds, period_seconds);
                }))
            {
                return;
            }
            // No link to be had on this Mac: the timer it has always used, and the reason said rather than a rate
            // invented for it.
            pacing_ = {FramePacing::kPlatformTimer, 0, pacing_.display_hertz, 0.0,
                       FramePacingFallback::kNoDisplayClock};
        }
        MacChildWindow *self = this;
        timer_               = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                                               repeats:YES
                                                                 block:^(NSTimer *) {
                                                     if (self->frame_)
                                                     {
                                                         self->frame_();
                                                     }
                                                                 }];
        [[NSRunLoop currentRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
    }

    /** One tick of the display link, on the window's thread: a refresh of the display the window is on. The frame
        callback is run on the tick the schedule's next frame belongs on and on no other, so each frame drawn is shown
        for exactly the divisor's refresh periods; where the display has stopped being one to divide, the window goes
        back to its timer with the reason kept for the product to read.
        @param tick_seconds    the tick's own time, in seconds on mach's clock
        @param period_seconds  the display's refresh period, in seconds; 0 where the link did not say */
    void PacedTick(double tick_seconds, double period_seconds)
    {
        // A link that named no period: the rate being divided, which is the display's own answer for it.
        const double period =
            period_seconds > 0.0 ? period_seconds : (pacing_.display_hertz > 0.0 ? 1.0 / pacing_.display_hertz : 0.0);
        const CGDirectDisplayID under          = DisplayOf(view_);
        const bool              is_display_new = under != display_;
        // A period further from the one being divided than one period read twice could be: the first tick, the mode
        // changed, or the window arrived on a display of another rate.
        const bool has_period_moved =
            period > 0.0 && std::fabs(period - period_divided_) > kRefreshPeriodsHoldStillWithin * period;
        if (is_display_new)
        {
            // Nothing of the display being left is judged together with the one being arrived at.
            display_  = under;
            unsteady_ = 0;
            steadiness_.Forget();
        }
        if (is_display_new || has_period_moved)
        {
            // The rates are read again rather than carried over - a mode change on the one display is caught here -
            // and reading them only when something has moved keeps CGDisplayCopyDisplayMode off every tick's path.
            reading_ = DisplayClockReadingOf(view_);
        }
        steadiness_.Note(period);
        unsteady_ = steadiness_.does_hold_still() ? 0 : unsteady_ + 1;

        DisplayClockReading reading = reading_;
        // A period that will not hold still for as many readings as are judged together is a panel whose rate varies;
        // fewer than that is one mode or display change, whose jump is still inside the readings being judged.
        reading.does_rate_hold_still = unsteady_ < kRefreshPeriodsJudged;
        const FramePacingState state = ChosenPacing(request_, reading);
        if (state.pacing != FramePacing::kDisplayClock)
        {
            pacing_ = {FramePacing::kPlatformTimer, 0, state.display_hertz, 0.0, state.why};
            ArrangePacing();
            return;
        }
        pacing_ = state;
        if (has_period_moved)
        {
            // The schedule starts again on what the display is doing now rather than dividing what it was doing.
            period_divided_ = period;
            frames_.Start(tick_seconds, period * request_.refresh_divisor, request_.most_frames_behind);
            return; // this tick is the schedule's start, not a frame of it
        }
        if (!IsFrameDueOnTick(tick_seconds, frames_.next_frame_seconds(), period))
        {
            return; // a refresh the frame drawn last is still being shown for
        }
        frames_.Advance(tick_seconds);
        // Back onto the display's own grid, so a period that wanders a little is followed rather than drifted from.
        frames_.MoveNextFrameTo(OnTheRefreshGrid(frames_.next_frame_seconds(), tick_seconds, period));
        if (frame_)
        {
            frame_();
        }
    }

    NSTimer              *timer_{nil}; ///< The platform's frame timer; nil while there is no callback, or a link runs.
    std::function<void()> frame_;      ///< The frame callback; empty for none.
    FrameRequest      request_{};   ///< What the product asked for; FrameRequest{} until it asks, which is the default.
    FramePacingState  pacing_{};    ///< What the window is pacing on, and why where it fell back.
    MacDisplayClock   clock_;       ///< The display link; stopped, and holding nothing, until a product asks.
    FrameClock        frames_;      ///< When the next frame is due, while a link is counted.
    RefreshSteadiness steadiness_;  ///< The refresh periods the link has lately reported.
    std::size_t       unsteady_{0}; ///< Consecutive ticks whose periods would not hold still.
    double            period_divided_{0.0}; ///< The refresh period the schedule in force divides; 0 before the first
                                            ///< tick, which starts it.
    /** The rates the display the window is on reports, read as the display or its mode changes rather than per tick. */
    DisplayClockReading reading_;
    CGDirectDisplayID   display_{kCGNullDirectDisplay}; ///< The display the readings are of.
};

float DisplayScaleOf(void *)
{
    return 1.0f;
}

void RevealInFileManager(const std::string &path)
{
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:StringOf(path)] ]];
}

void OpenInBrowser(const std::string &url)
{
    NSURL *page = [NSURL URLWithString:StringOf(url)];
    if (page != nil && ([page.scheme isEqualToString:@"https"] || [page.scheme isEqualToString:@"http"]))
    {
        [[NSWorkspace sharedWorkspace] openURL:page];
    }
}

std::unique_ptr<ChildWindow> CreateChildWindow(void *parent, view::View &root, int width, int height, float scale,
                                               std::unique_ptr<RunLoop>)
{
    NSView *parent_view = (__bridge NSView *)parent;
    if (parent_view == nil)
    {
        return nullptr;
    }
    return std::make_unique<MacChildWindow>(parent_view, root, width, height, scale);
}
} // namespace reckless::window

namespace
{
using reckless::window::MacSurface;

/** The toolkit's name for the key a virtual key code stands for: the editing and moving keys, and Key::kNone for every
    other, which names a key the views know only by the character it types. The one table the views' keys (KeyOf) and
    the raw keys (RawKeyOf) are both named from.
    @param code  the key's virtual key code, an NSEvent's `keyCode`
    @return      the key, or Key::kNone where the toolkit has no name for it */
reckless::view::Key NamedKeyOfMacKeyCode(unsigned short code)
{
    using reckless::view::Key;
    switch (code)
    {
    case 53:
        return Key::kEscape;
    case 36:
    case 76:
        return Key::kReturn;
    case 48:
        return Key::kTab;
    case 51:
        return Key::kBackspace;
    case 117:
        return Key::kDelete;
    case 123:
        return Key::kLeft;
    case 124:
        return Key::kRight;
    case 125:
        return Key::kDown;
    case 126:
        return Key::kUp;
    case 115:
        return Key::kHome;
    case 119:
        return Key::kEnd;
    case 116:
        return Key::kPageUp;
    case 121:
        return Key::kPageDown;
    default:
        return Key::kNone;
    }
}

/** The modifier flag a modifier key's virtual key code carries, for the key edges `flagsChanged:` brings: the left and
    the right key of each pair carry one flag between them, and the key code says which key it was.
    @param code  the key's virtual key code, an NSEvent's `keyCode`
    @return      the flag, or 0 where the code is no modifier key's */
NSEventModifierFlags ModifierFlagOfMacKeyCode(unsigned short code)
{
    switch (code)
    {
    case 56: // the left shift
    case 60: // the right shift
        return NSEventModifierFlagShift;
    case 59: // the left control
    case 62: // the right control
        return NSEventModifierFlagControl;
    case 58: // the left option
    case 61: // the right option
        return NSEventModifierFlagOption;
    case 55: // the left command
    case 54: // the right command
        return NSEventModifierFlagCommand;
    case 57: // caps lock, whose flag follows the lamp rather than the key
        return NSEventModifierFlagCapsLock;
    case 63: // the function key
        return NSEventModifierFlagFunction;
    default:
        return 0;
    }
}

/** One edge of one key as a raw key event: the virtual key code, which names the physical key whatever the layout
    makes it type, and the toolkit's name for it where it has one.
    @param event    a key down, a key up or a `flagsChanged:` event
    @param is_down  true for the key going down, false for it being let go
    @return         the edge, its repeat flag left for the surface to answer */
reckless::window::RawKeyEvent RawKeyOf(NSEvent *event, bool is_down)
{
    reckless::window::RawKeyEvent key;
    key.is_down           = is_down;
    key.named_key         = NamedKeyOfMacKeyCode([event keyCode]);
    key.platform_key_code = [event keyCode];
    key.physical_key_code = [event keyCode]; // a Mac's key code is the physical key: there is no second code beside it
    key.modifiers         = MacSurface::ModifiersOf([event modifierFlags]);
    return key;
}

/** An NSEvent's key as the views see it: the editing and moving keys by their key codes, the rest as the character
    they type; nothing for a function key or a key with no character.
    @param event  a key-down event
    @return       the key and the modifiers held; the key unset for one the views do not take */
reckless::view::KeyEvent KeyOf(NSEvent *event)
{
    using reckless::view::Key;
    reckless::view::KeyEvent key;
    key.modifiers = MacSurface::ModifiersOf([event modifierFlags]);
    if (const Key named = NamedKeyOfMacKeyCode([event keyCode]); named != Key::kNone)
    {
        key.key = named;
        return key;
    }
    NSString *characters = [event characters];
    if (characters.length > 0)
    {
        const unichar c = [characters characterAtIndex:0];
        if (c >= 32 && (c < 0xf700 || c > 0xf8ff)) // the function keys are private-use characters
        {
            key.key       = Key::kCharacter;
            key.character = static_cast<char32_t>(c);
        }
    }
    return key;
}

/** The files a drag carries.
    @param sender  the drag, from the dragging destination's methods
    @return        the paths of its file URLs, in UTF-8; empty for a drag of anything else */
std::vector<std::string> PathsOf(id<NSDraggingInfo> sender)
{
    std::vector<std::string> paths;
    NSArray<NSURL *> *urls = [[sender draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                                        options:@{
                                                                            NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                                        }];
    for (NSURL *url in urls)
    {
        paths.emplace_back([url.path UTF8String]);
    }
    return paths;
}
} // namespace

namespace reckless::window
{
namespace
{
// The view's methods, added to its class as the class is registered: what an NSView subclass's @implementation
// would hold, as functions of the view and the selector.

/** `isFlipped`: the view's y runs down, as the views' do.
    @return YES */
BOOL ViewIsFlipped(id, SEL)
{
    return YES;
}

/** `acceptsFirstMouse:`: the press that brings the window forward is the view's too.
    @return YES */
BOOL ViewAcceptsFirstMouse(id, SEL, NSEvent *)
{
    return YES;
}

/** `isOpaque`: whether the surface paints every pixel of the view.
    @param self  the view
    @return      YES when the surface's root is opaque; NO once the surface has gone */
BOOL ViewIsOpaque(id self, SEL)
{
    MacSurface *owner = OwnerOf(self);
    return owner != nullptr && owner->opaque();
}

/** `acceptsFirstResponder`: the view takes the keyboard while a view of the tree holds the focus.
    @param self  the view
    @return      YES when a view of the surface wants keys */
BOOL ViewAcceptsFirstResponder(id self, SEL)
{
    MacSurface *owner = OwnerOf(self);
    return owner != nullptr && owner->wants_keys();
}

/** `drawRect:`: the surface drawn into the view's graphics context.
    @param self   the view
    @param dirty  the rectangle to draw, in the view's points */
void ViewDrawRect(id self, SEL, NSRect dirty)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->Draw([[NSGraphicsContext currentContext] CGContext], dirty);
    }
}

/** `recklessDisplayLinkTicked:`: the display link made from this view (MacDisplayClock, from macOS 14) ticked, which
    is once a refresh of the display the view is on. The link's own timestamps are handed on, so the subdivision is
    counted from what the display did rather than from how many callbacks arrived.
    @param self  the view
    @param link  the CADisplayLink that ticked, as `id` so that a build whose floor is below macOS 14 names its class
                 only where it is available */
void ViewDisplayLinkTicked(id self, SEL, id link)
{
    MacSurface *owner = OwnerOf(self);
    if (owner == nullptr)
    {
        return;
    }
    double tick_seconds   = CACurrentMediaTime();
    double period_seconds = 0.0;
    if (@available(macOS 14.0, *))
    {
        CADisplayLink *ticked = static_cast<CADisplayLink *>(link);
        tick_seconds          = [ticked timestamp];
        period_seconds        = [ticked targetTimestamp] - [ticked timestamp];
    }
    owner->DisplayLinkTicked(tick_seconds, period_seconds);
}

/** `resetCursorRects`: the whole view takes the cursor the views last asked for.
    @param self  the view */
void ViewResetCursorRects(id self, SEL)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        [self addCursorRect:[self bounds] cursor:CursorFor(owner->cursor())];
    }
}

/** `cursorUpdate:`: the cursor the views last asked for is set.
    @param self  the view */
void ViewCursorUpdate(id self, SEL, NSEvent *)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        [CursorFor(owner->cursor()) set];
    }
}

/** `mouseDown:`: a press to the router; a Control-click is a right press.
    @param self   the view
    @param event  the press: its place, click count and modifiers */
void ViewMouseDown(id self, SEL, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        const bool right = ([event modifierFlags] & NSEventModifierFlagControl) != 0;
        owner->router().Down(owner->ToDesign(event), static_cast<int>([event clickCount]), right,
                             MacSurface::ModifiersOf([event modifierFlags]));
    }
}

/** `rightMouseDown:`: a right press to the router.
    @param self   the view
    @param event  the press: its place and modifiers */
void ViewRightMouseDown(id self, SEL, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->router().Down(owner->ToDesign(event), 1, true, MacSurface::ModifiersOf([event modifierFlags]));
    }
}

/** `mouseDragged:` and `rightMouseDragged:`: a drag to the router, to the view the press began on.
    @param self   the view
    @param event  the drag: its place and modifiers */
void ViewMouseDragged(id self, SEL, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->router().Drag(owner->ToDesign(event), MacSurface::ModifiersOf([event modifierFlags]));
    }
}

/** `mouseUp:` and `rightMouseUp:`: a release to the router.
    @param self   the view
    @param event  the release: its place and modifiers */
void ViewMouseUp(id self, SEL, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->router().Up(owner->ToDesign(event), MacSurface::ModifiersOf([event modifierFlags]));
    }
}

/** `mouseMoved:` and `mouseEntered:`: a move to the router.
    @param self   the view
    @param event  the move: its place and modifiers */
void ViewMouseMoved(id self, SEL, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->router().Move(owner->ToDesign(event), MacSurface::ModifiersOf([event modifierFlags]));
    }
}

/** `mouseExited:`: the pointer left the view; the router is told.
    @param self  the view */
void ViewMouseExited(id self, SEL, NSEvent *)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->router().Exit();
    }
}

/** `scrollWheel:`: the wheel to the router, a wheel's deltas times ten and a trackpad's as they are, with
    whether the system inverted the device's direction.
    @param self   the view
    @param event  the scroll: its place, deltas and modifiers */
void ViewScrollWheel(id self, SEL, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        // The deltas are the content's direction; under natural scrolling the system inverts the device's, and says
        // so, for a control that follows the hand rather than the content (a knob turns the way the wheel does).
        const float scale = [event hasPreciseScrollingDeltas] ? 1.0f : 10.0f;
        owner->router().Wheel(owner->ToDesign(event), static_cast<float>([event scrollingDeltaX]) * scale,
                              static_cast<float>([event scrollingDeltaY]) * scale,
                              MacSurface::ModifiersOf([event modifierFlags]), [event isDirectionInvertedFromDevice]);
    }
}

/** `keyDown:`: a key to the router, else to NSView's own implementation, so the host's shortcuts still reach it.
    @param self     the view
    @param command  the selector, `keyDown:`
    @param event    the key */
void ViewKeyDown(id self, SEL command, NSEvent *event)
{
    MacSurface *owner = OwnerOf(self);
    if (owner != nullptr)
    {
        // Whoever asked for the raw keys told first; the views are then given what they always were.
        owner->TellRawKey(RawKeyOf(event, true));
    }
    if (owner == nullptr || !owner->router().Key(KeyOf(event)))
    {
        // Not the views': NSView's own, so the host's shortcuts still reach it.
        objc_super super_class{self, [NSView class]};
        reinterpret_cast<void (*)(objc_super *, SEL, NSEvent *)>(objc_msgSendSuper)(&super_class, command, event);
    }
}

/** `keyUp:`: the key let go told to whoever asked for the raw keys, then NSView's own implementation, which is all
    that happened before anyone could ask - the views have no key up, and nothing here gives them one.
    @param self     the view
    @param command  the selector, `keyUp:`
    @param event    the key */
void ViewKeyUp(id self, SEL command, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->TellRawKey(RawKeyOf(event, false));
    }
    objc_super super_class{self, [NSView class]};
    reinterpret_cast<void (*)(objc_super *, SEL, NSEvent *)>(objc_msgSendSuper)(&super_class, command, event);
}

/** `flagsChanged:`: a modifier key pressed or let go told to whoever asked for the raw keys - the only way a Mac
    reports one, since a modifier key sends no key down - then NSView's own implementation.
    @param self     the view
    @param command  the selector, `flagsChanged:`
    @param event    the event, whose key code says which modifier key changed */
void ViewFlagsChanged(id self, SEL command, NSEvent *event)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        const NSEventModifierFlags flag = ModifierFlagOfMacKeyCode([event keyCode]);
        owner->TellRawModifierKey(RawKeyOf(event, false), flag != 0 && ([event modifierFlags] & flag) != 0);
    }
    objc_super super_class{self, [NSView class]};
    reinterpret_cast<void (*)(objc_super *, SEL, NSEvent *)>(objc_msgSendSuper)(&super_class, command, event);
}

/** `resignFirstResponder`: the keyboard going to another view, on which every key the raw key callback was told is
    down is let go, then NSView's own answer.
    @param self     the view
    @param command  the selector, `resignFirstResponder`
    @return         NSView's own answer: YES */
BOOL ViewResignFirstResponder(id self, SEL command)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->LetGoOfHeldRawKeys();
    }
    objc_super super_class{self, [NSView class]};
    return reinterpret_cast<BOOL (*)(objc_super *, SEL)>(objc_msgSendSuper)(&super_class, command);
}

/** `draggingEntered:` and `draggingUpdated:`: files dragged over the view, offered to the router.
    @param self    the view
    @param sender  the drag
    @return        a copy where the view under the pointer takes the files, else none */
NSDragOperation ViewDraggingUpdated(id self, SEL, id<NSDraggingInfo> sender)
{
    MacSurface *owner = OwnerOf(self);
    if (owner == nullptr)
    {
        return NSDragOperationNone;
    }
    const auto at = owner->ToDesign([sender draggingLocation]);
    return owner->router().DragFiles(at, PathsOf(sender)) ? NSDragOperationCopy : NSDragOperationNone;
}

/** `draggingExited:`: the drag left the view, or ended where nothing takes it; the router is told.
    @param self  the view */
void ViewDraggingExited(id self, SEL, id<NSDraggingInfo>)
{
    if (MacSurface *owner = OwnerOf(self))
    {
        owner->router().EndFileDrag(); // gone, or let go somewhere that takes nothing
    }
}

/** `performDragOperation:`: the files dropped on the view, handed to the router.
    @param self    the view
    @param sender  the drag
    @return        YES when the view under the pointer took the files */
BOOL ViewPerformDragOperation(id self, SEL, id<NSDraggingInfo> sender)
{
    MacSurface *owner = OwnerOf(self);
    if (owner == nullptr)
    {
        return NO;
    }
    return owner->router().DropFiles(owner->ToDesign([sender draggingLocation]), PathsOf(sender)) ? YES : NO;
}

/** Where a view of the class keeps its surface: the offset of its one instance variable, known once registered. */
std::ptrdiff_t owner_offset = 0;

/** A method added to the class under `selector`, typed as NSView's own (or an optional method of
    NSDraggingDestination) types it.
    @param cls             the class being registered
    @param selector        the method's selector
    @param implementation  the function that implements it, one of the View functions above */
void AddMethod(Class cls, SEL selector, IMP implementation)
{
    const char *types = nullptr;
    if (Method inherited = class_getInstanceMethod([NSView class], selector))
    {
        types = method_getTypeEncoding(inherited);
    }
    else
    {
        types = protocol_getMethodDescription(@protocol(NSDraggingDestination), selector, NO, YES).types;
    }
    class_addMethod(cls, selector, implementation, types);
}

/** A method added to the view's class with a type encoding of its own, for a selector NSView has not got and no
    protocol describes: the display link's (DisplayLinkTickedSelector).
    @param cls             the class being registered
    @param selector        the selector
    @param implementation  the function that implements it
    @param types           the method's Objective-C type encoding */
void AddMethod(Class cls, SEL selector, IMP implementation, const char *types)
{
    class_addMethod(cls, selector, implementation, types);
}

Class PanelViewClass()
{
    static Class registered = [] {
        // The name: the module's (RECKLESS_PANEL_VIEW) and the address of this image's own variable, which no other
        // image loaded beside it shares - so neither another module nor another build of this one holds it.
        char name[256];
        std::snprintf(name, sizeof(name), "%s_%p", RECKLESS_PANEL_VIEW_TEXT(RECKLESS_PANEL_VIEW),
                      static_cast<const void *>(&owner_offset));
        Class cls = objc_allocateClassPair([NSView class], name, 0);
        class_addIvar(cls, "owner", sizeof(MacSurface *), static_cast<std::uint8_t>(std::log2(alignof(MacSurface *))),
                      "^v");
        AddMethod(cls, @selector(isFlipped), reinterpret_cast<IMP>(ViewIsFlipped));
        AddMethod(cls, @selector(acceptsFirstMouse:), reinterpret_cast<IMP>(ViewAcceptsFirstMouse));
        AddMethod(cls, @selector(isOpaque), reinterpret_cast<IMP>(ViewIsOpaque));
        AddMethod(cls, @selector(acceptsFirstResponder), reinterpret_cast<IMP>(ViewAcceptsFirstResponder));
        AddMethod(cls, @selector(drawRect:), reinterpret_cast<IMP>(ViewDrawRect));
        AddMethod(cls, DisplayLinkTickedSelector(), reinterpret_cast<IMP>(ViewDisplayLinkTicked), "v@:@");
        AddMethod(cls, @selector(resetCursorRects), reinterpret_cast<IMP>(ViewResetCursorRects));
        AddMethod(cls, @selector(cursorUpdate:), reinterpret_cast<IMP>(ViewCursorUpdate));
        AddMethod(cls, @selector(mouseDown:), reinterpret_cast<IMP>(ViewMouseDown));
        AddMethod(cls, @selector(rightMouseDown:), reinterpret_cast<IMP>(ViewRightMouseDown));
        AddMethod(cls, @selector(mouseDragged:), reinterpret_cast<IMP>(ViewMouseDragged));
        AddMethod(cls, @selector(rightMouseDragged:), reinterpret_cast<IMP>(ViewMouseDragged));
        AddMethod(cls, @selector(mouseUp:), reinterpret_cast<IMP>(ViewMouseUp));
        AddMethod(cls, @selector(rightMouseUp:), reinterpret_cast<IMP>(ViewMouseUp));
        AddMethod(cls, @selector(mouseMoved:), reinterpret_cast<IMP>(ViewMouseMoved));
        AddMethod(cls, @selector(mouseEntered:), reinterpret_cast<IMP>(ViewMouseMoved));
        AddMethod(cls, @selector(mouseExited:), reinterpret_cast<IMP>(ViewMouseExited));
        AddMethod(cls, @selector(scrollWheel:), reinterpret_cast<IMP>(ViewScrollWheel));
        AddMethod(cls, @selector(keyDown:), reinterpret_cast<IMP>(ViewKeyDown));
        AddMethod(cls, @selector(keyUp:), reinterpret_cast<IMP>(ViewKeyUp));
        AddMethod(cls, @selector(flagsChanged:), reinterpret_cast<IMP>(ViewFlagsChanged));
        AddMethod(cls, @selector(resignFirstResponder), reinterpret_cast<IMP>(ViewResignFirstResponder));
        AddMethod(cls, @selector(draggingEntered:), reinterpret_cast<IMP>(ViewDraggingUpdated));
        AddMethod(cls, @selector(draggingUpdated:), reinterpret_cast<IMP>(ViewDraggingUpdated));
        AddMethod(cls, @selector(draggingExited:), reinterpret_cast<IMP>(ViewDraggingExited));
        AddMethod(cls, @selector(performDragOperation:), reinterpret_cast<IMP>(ViewPerformDragOperation));
        class_addProtocol(cls, @protocol(NSDraggingDestination));
        objc_registerClassPair(cls);
        owner_offset = ivar_getOffset(class_getInstanceVariable(cls, "owner"));
        return cls;
    }();
    return registered;
}

MacSurface *OwnerOf(id view)
{
    return *reinterpret_cast<MacSurface **>(static_cast<char *>((__bridge void *)view) + owner_offset);
}

void SetOwner(NSView *view, MacSurface *owner)
{
    *reinterpret_cast<MacSurface **>(static_cast<char *>((__bridge void *)view) + owner_offset) = owner;
}
} // namespace
} // namespace reckless::window
