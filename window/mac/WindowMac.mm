// The child window on macOS: an NSView drawing a view tree through Skia into a raster at the backing scale, put on
// screen through a CGImage, with the mouse, the keys and dropped files handed to a MouseRouter and a display-rate
// timer for the frame callback. A popup is the same surface in a borderless, non-activating panel over the host's
// window, watched by a local event monitor for presses outside it. Screen points are Cocoa's (y up from the main
// screen's bottom), opaque to the views, which only hand them back to Popup::Contains.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
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

/** Text in Cocoa's form.
    @param utf8  the text, in UTF-8
    @return      the autoreleased NSString */
NSString *StringOf(const std::string &utf8)
{
    return [NSString stringWithUTF8String:utf8.c_str()];
}

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

  private:
    /** Held weakly by every call the surface defers and every dialog it opens: gone with the surface, so neither
        reaches it, or the views it held, afterwards. */
    std::shared_ptr<char>          lifetime_{std::make_shared<char>()};
    NSMutableArray<NSSavePanel *> *open_panels_{[NSMutableArray array]}; ///< the file dialogs up, cancelled as it goes
    std::vector<MacPopup *>        popups_;          ///< the popups opened over it, which go with it
    bool                           released_{false}; ///< True once Release has let the surface go.
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

    /** The frame timer invalidated. */
    ~MacChildWindow() override
    {
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
        [timer_ invalidate];
        timer_ = nil;
        if (frame_)
        {
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
    }

    /** In a window that is on screen and not in the Dock, and not hidden in it by the host.
        @return true when the window shows */
    bool Showing() const override
    {
        NSWindow *window = [view_ window];
        return window != nil && [window isVisible] && ![window isMiniaturized] && ![view_ isHiddenOrHasHiddenAncestor];
    }

  private:
    NSTimer              *timer_{nil}; ///< The 60 Hz frame timer; nil while there is no callback.
    std::function<void()> frame_;      ///< The frame callback; empty for none.
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

/** An NSEvent's key as the views see it: the editing and moving keys by their key codes, the rest as the character
    they type; nothing for a function key or a key with no character.
    @param event  a key-down event
    @return       the key and the modifiers held; the key unset for one the views do not take */
reckless::view::KeyEvent KeyOf(NSEvent *event)
{
    using reckless::view::Key;
    reckless::view::KeyEvent key;
    key.modifiers = MacSurface::ModifiersOf([event modifierFlags]);
    switch ([event keyCode])
    {
    case 53:
        key.key = Key::kEscape;
        return key;
    case 36:
    case 76:
        key.key = Key::kReturn;
        return key;
    case 48:
        key.key = Key::kTab;
        return key;
    case 51:
        key.key = Key::kBackspace;
        return key;
    case 117:
        key.key = Key::kDelete;
        return key;
    case 123:
        key.key = Key::kLeft;
        return key;
    case 124:
        key.key = Key::kRight;
        return key;
    case 125:
        key.key = Key::kDown;
        return key;
    case 126:
        key.key = Key::kUp;
        return key;
    case 115:
        key.key = Key::kHome;
        return key;
    case 119:
        key.key = Key::kEnd;
        return key;
    case 116:
        key.key = Key::kPageUp;
        return key;
    case 121:
        key.key = Key::kPageDown;
        return key;
    default:
        break;
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
    if (owner == nullptr || !owner->router().Key(KeyOf(event)))
    {
        // Not the views': NSView's own, so the host's shortcuts still reach it.
        objc_super super_class{self, [NSView class]};
        reinterpret_cast<void (*)(objc_super *, SEL, NSEvent *)>(objc_msgSendSuper)(&super_class, command, event);
    }
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
