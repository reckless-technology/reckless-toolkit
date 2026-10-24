// The Standalone's platform on macOS (app/AppPlatform.h): an NSApplication with the app menu - About, Hide, Quit
// - and the app's own menu beside it, its windows NSWindows whose content views hold the child windows the panel and
// the settings are drawn in (window/mac/WindowMac.mm), timers on the run loop's common modes so they keep
// running while a menu is tracked, the calls other threads post run by a block on the main queue, and the loop ended
// by stopping the application. Screen points are Cocoa's (y up from
// the bottom of the main screen); a window's position is handed over as its top left, y down from the top of the main
// screen, as a JUCE Standalone keeps it, and the places are worked out that way round (WindowPlace.h): a window put
// back wholly on the screen it is most on, a dialog centred over its owner and kept inside that screen. A dialog is a
// child window of its owner, so it stays over it; the main window is resized by its frame, each size dragged to held to
// the owner's constraint on its content (windowWillResize), zoomed to the most the constraint allows on the screen
// (windowWillUseStandardFrame), and each size taken told (windowDidResize).
#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../window/mac/ClipboardMac.h" // the child window's too: not on an include path
#include "AppPlatform.h"
#include "WindowPlace.h"

namespace reckless::app
{
class MacPlatform;
}

/** The application's delegate: the app's own ending when it is asked to quit from outside its windows. */
@interface RecklessAppDelegate : NSObject <NSApplicationDelegate>
{
  @public
    std::function<void()>
         quit_handler; ///< the app's own ending, run once when a quit comes from outside; may be empty.
    bool ending;       ///< true once quit_handler has run, so it runs only once.
}
@end

@implementation RecklessAppDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    // The app menu's Quit, the Dock's, a logout: the app saves what it keeps, and the process ends here.
    if (!ending && quit_handler)
    {
        ending = true;
        quit_handler();
    }
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    (void)sender;
    return NO; // the main window's close button ends the app itself, after saving
}

@end

/** A menu item's target: the item's tag handed to the app. */
@interface RecklessMenuTarget : NSObject
{
  @public
    std::function<void(int)> chosen; ///< told the chosen item's id; may be empty.
}
/** A menu item was chosen: its tag goes to `chosen`.
    @param sender  the NSMenuItem chosen, whose tag is the app's command id */
- (void)choose:(id)sender;
@end

@implementation RecklessMenuTarget

- (void)choose:(id)sender
{
    if (chosen)
    {
        chosen(static_cast<int>([(NSMenuItem *)sender tag]));
    }
}

@end

/** A window that tells its owner when the person closes it - its close button, or Escape on a dialog - and lets the
    owner close it; resizable, a size dragged to or zoomed to is held by the owner's constraint on its content and the
    size taken told. */
@interface RecklessAppWindow : NSWindow <NSWindowDelegate>
{
  @public
    std::function<void()> closing; ///< what the owner does when the person closes the window; may be empty.
    /** The owner's constraint on the content size, in points, for a size dragged to or zoomed to; empty for a window
        of one size. */
    std::function<reckless::gfx::PointF(reckless::gfx::PointF)> constrain;
    std::function<void(int, int)> resized; ///< told the content's size, in points, after each resize; may be empty.
    bool                          dialog;  ///< a dialog: Escape closes it.
    /** A key that reached the window - its own, or one no view took, up the responder chain - offered to the menu
        bar's shortcuts: true when it was one. May be empty. */
    std::function<bool(NSEvent *)> shortcut;
    /** Whether a key is one of the menu bar's shortcuts, which the window takes before the menu bar does (its
        performKeyEquivalent) to hand it to the views first. May be empty. */
    std::function<bool(NSEvent *)> is_shortcut;
}
@end

@implementation RecklessAppWindow

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    if (closing)
    {
        const auto f = closing;
        f();
    }
    return NO; // the owner destroys the window when it is done with it
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize
{
    if (!constrain)
    {
        return frameSize;
    }
    // The content the frame would hold, held to the owner's constraint, and the frame round that.
    NSRect content = [sender contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    const reckless::gfx::PointF held =
        constrain({static_cast<float>(content.size.width), static_cast<float>(content.size.height)});
    content.size = NSMakeSize(std::round(held.x), std::round(held.y));
    return [sender frameRectForContentRect:content].size;
}

- (NSRect)windowWillUseStandardFrame:(NSWindow *)sender defaultFrame:(NSRect)newFrame
{
    if (!constrain)
    {
        return newFrame;
    }
    // Zoomed, as JUCE's window zooms: the most content the owner's constraint allows inside the frame the screen
    // offers, its top left where the window's stands, moved only as far as keeps it inside. Zoomed again, AppKit puts
    // back the size it had.
    const NSRect                room = [sender contentRectForFrameRect:newFrame];
    const reckless::gfx::PointF held =
        constrain({static_cast<float>(room.size.width), static_cast<float>(room.size.height)});
    NSRect       frame = [sender
        frameRectForContentRect:NSMakeRect(0, 0, std::min(std::floor(CGFloat{held.x}), std::floor(room.size.width)),
                                           std::min(std::floor(CGFloat{held.y}), std::floor(room.size.height)))];
    const NSRect now   = sender.frame;
    frame.origin.x =
        std::clamp(NSMinX(now), NSMinX(newFrame), std::max(NSMinX(newFrame), NSMaxX(newFrame) - NSWidth(frame)));
    frame.origin.y = std::clamp(NSMaxY(now) - NSHeight(frame), NSMinY(newFrame),
                                std::max(NSMinY(newFrame), NSMaxY(newFrame) - NSHeight(frame)));
    return frame;
}

- (void)windowDidResize:(NSNotification *)notification
{
    (void)notification;
    if (resized)
    {
        const NSRect content = [self contentRectForFrameRect:self.frame];
        const auto   f       = resized;
        f(static_cast<int>(std::lround(content.size.width)), static_cast<int>(std::lround(content.size.height)));
    }
}

- (void)keyDown:(NSEvent *)event
{
    if (dialog && [event keyCode] == 53) // Escape
    {
        [self windowShouldClose:self];
        return;
    }
    if (shortcut)
    {
        const auto f = shortcut;
        if (f(event))
        {
            return;
        }
    }
    [super keyDown:event];
}

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    // AppKit asks the key window for a key equivalent before the menu bar, whose items show the shortcuts as theirs:
    // a key that is one of the bar's shortcuts goes to the views first, as a key down - the view with the keyboard
    // taking it or handing it up the responder chain to this window's keyDown, which chooses the entry - and the menu
    // bar never sees it. Any other key, the app menu's Command-Q among them, is AppKit's as before.
    if (is_shortcut)
    {
        const auto f = is_shortcut;
        if (f(event))
        {
            NSResponder *first = self.firstResponder;
            if (first != nil && first != self)
            {
                [first keyDown:event];
            }
            else
            {
                [self keyDown:event];
            }
            return YES;
        }
    }
    return [super performKeyEquivalent:event];
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

@end

/** The content view a child window is added to: flipped, as the child window's own view is. */
@interface RecklessAppContent : NSView
@end

@implementation RecklessAppContent

- (BOOL)isFlipped
{
    return YES;
}

@end

namespace reckless::app
{
namespace
{
/** A string in Cocoa's form.
    @param utf8  the text, in UTF-8
    @return      the autoreleased NSString */
NSString *StringOf(const std::string &utf8)
{
    return [NSString stringWithUTF8String:utf8.c_str()];
}

/** The main screen's height: Cocoa's y runs up from its bottom.
    @return the height, in points; 0 with no screen */
CGFloat MainScreenHeight()
{
    NSArray<NSScreen *> *screens = [NSScreen screens];
    return screens.count > 0 ? NSMaxY(screens[0].frame) : 0.0;
}

/** A rectangle in Cocoa's screen points as WindowPlace.h takes one: y down from the top of the main screen.
    @param rect  the rectangle, y up from the bottom of the main screen
    @return      the same rectangle, whole points, its y down from the top of the main screen */
gfx::RectI DownwardsOf(NSRect rect)
{
    return {static_cast<int>(std::lround(rect.origin.x)),
            static_cast<int>(std::lround(MainScreenHeight() - NSMaxY(rect))),
            static_cast<int>(std::lround(rect.size.width)), static_cast<int>(std::lround(rect.size.height))};
}

/** Every screen's visible frame - what the menu bar and the Dock leave of it - y down.
    @return the frames, in points, the main screen's first */
std::vector<gfx::RectI> VisibleAreas()
{
    std::vector<gfx::RectI> areas;
    for (NSScreen *screen in [NSScreen screens])
    {
        areas.push_back(DownwardsOf(screen.visibleFrame));
    }
    return areas;
}

/** The key a key down names, as the shortcuts match it: the function keys and the editing and moving keys by their key
    codes, any other key by the character it types with no modifier applied - the keyboard's layout's own - and
    Command as the primary modifier, Shift, and Option as Alt. A key with Control held names none, Control being no
    modifier a shortcut is written with.
    @param event  the key down
    @return       the key pressed; empty for a key that names none */
Shortcut StrokeOf(NSEvent *event)
{
    Shortcut                   stroke;
    const NSEventModifierFlags flags = [event modifierFlags];
    if ((flags & NSEventModifierFlagControl) != 0)
    {
        return stroke;
    }
    stroke.primary = (flags & NSEventModifierFlagCommand) != 0;
    stroke.shift   = (flags & NSEventModifierFlagShift) != 0;
    stroke.alt     = (flags & NSEventModifierFlagOption) != 0;
    switch ([event keyCode])
    {
    case 53:
        stroke.key = ShortcutKey::kEscape;
        return stroke;
    case 36:
    case 76:
        stroke.key = ShortcutKey::kReturn;
        return stroke;
    case 48:
        stroke.key = ShortcutKey::kTab;
        return stroke;
    case 49:
        stroke.key = ShortcutKey::kSpace;
        return stroke;
    case 51:
        stroke.key = ShortcutKey::kBackspace;
        return stroke;
    case 117:
        stroke.key = ShortcutKey::kDelete;
        return stroke;
    case 123:
        stroke.key = ShortcutKey::kLeft;
        return stroke;
    case 124:
        stroke.key = ShortcutKey::kRight;
        return stroke;
    case 125:
        stroke.key = ShortcutKey::kDown;
        return stroke;
    case 126:
        stroke.key = ShortcutKey::kUp;
        return stroke;
    case 115:
        stroke.key = ShortcutKey::kHome;
        return stroke;
    case 119:
        stroke.key = ShortcutKey::kEnd;
        return stroke;
    case 116:
        stroke.key = ShortcutKey::kPageUp;
        return stroke;
    case 121:
        stroke.key = ShortcutKey::kPageDown;
        return stroke;
    default:
        break;
    }
    // The function keys' codes, F1 to F12, as the keyboard sends them.
    static constexpr unsigned short kFunctionCodes[] = {122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111};
    for (int i = 0; i < 12; ++i)
    {
        if ([event keyCode] == kFunctionCodes[i])
        {
            stroke.key = static_cast<ShortcutKey>(static_cast<int>(ShortcutKey::kF1) + i);
            return stroke;
        }
    }
    NSString *typed = [event charactersByApplyingModifiers:0];
    if (typed.length == 1 && [typed characterAtIndex:0] >= 32 && [typed characterAtIndex:0] < 0xF700)
    {
        stroke.key       = ShortcutKey::kCharacter;
        stroke.character = Shortcut::Lowered(static_cast<char32_t>([typed characterAtIndex:0]));
    }
    return stroke;
}

/** A shortcut as an NSMenuItem's key equivalent, which the item shows beside its title.
    @param shortcut  the shortcut
    @param mask      set to the modifiers' mask: Command for the primary modifier, Shift, Option for Alt
    @return          the key equivalent; empty for no shortcut */
NSString *KeyEquivalentOf(const Shortcut &shortcut, NSEventModifierFlags &mask)
{
    mask = 0;
    if (shortcut.primary)
    {
        mask |= NSEventModifierFlagCommand;
    }
    if (shortcut.shift)
    {
        mask |= NSEventModifierFlagShift;
    }
    if (shortcut.alt)
    {
        mask |= NSEventModifierFlagOption;
    }
    unichar key = 0;
    switch (shortcut.key)
    {
    case ShortcutKey::kNone:
        return @"";
    case ShortcutKey::kCharacter:
        if (shortcut.character == 0 || shortcut.character > 0xFFFF)
        {
            return @"";
        }
        key = static_cast<unichar>(shortcut.character);
        break;
    case ShortcutKey::kEscape:
        key = 0x1B;
        break;
    case ShortcutKey::kReturn:
        key = '\r';
        break;
    case ShortcutKey::kTab:
        key = '\t';
        break;
    case ShortcutKey::kSpace:
        key = ' ';
        break;
    case ShortcutKey::kBackspace:
        key = 0x08;
        break;
    case ShortcutKey::kDelete:
        key = NSDeleteFunctionKey;
        break;
    case ShortcutKey::kLeft:
        key = NSLeftArrowFunctionKey;
        break;
    case ShortcutKey::kRight:
        key = NSRightArrowFunctionKey;
        break;
    case ShortcutKey::kUp:
        key = NSUpArrowFunctionKey;
        break;
    case ShortcutKey::kDown:
        key = NSDownArrowFunctionKey;
        break;
    case ShortcutKey::kHome:
        key = NSHomeFunctionKey;
        break;
    case ShortcutKey::kEnd:
        key = NSEndFunctionKey;
        break;
    case ShortcutKey::kPageUp:
        key = NSPageUpFunctionKey;
        break;
    case ShortcutKey::kPageDown:
        key = NSPageDownFunctionKey;
        break;
    default:
        key = static_cast<unichar>(NSF1FunctionKey +
                                   (static_cast<int>(shortcut.key) - static_cast<int>(ShortcutKey::kF1)));
        break;
    }
    return [NSString stringWithCharacters:&key length:1];
}

/** The style a window of `options` has: titled and closable, minimisable unless a dialog, resizable where it is given
    a constraint.
    @param options  the window's options
    @return         the style mask */
NSWindowStyleMask StyleOf(const WindowOptions &options)
{
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
    if (!options.dialog)
    {
        style |= NSWindowStyleMaskMiniaturizable;
    }
    if (options.constrain)
    {
        style |= NSWindowStyleMaskResizable;
    }
    return style;
}
} // namespace

/** A top-level NSWindow of the app: titled and closable, minimisable unless a dialog, resizable where the owner gives a
    constraint, its content view the one the child window is made in. A dialog is a child window of its owner. Made
    by MacPlatform::OpenWindow. Main thread. */
class MacWindow final : public AppWindow
{
  public:
    /** Create the window, put it where it stood, else centred over its owner, else centred on the screen, and show it.
        @param options  the title, the content's size in points, the owner, the constraint and the callbacks */
    explicit MacWindow(const WindowOptions &options)
    {
        const NSWindowStyleMask style   = StyleOf(options);
        const NSRect            content = NSMakeRect(0, 0, options.width, options.height);
        window_                         = [[RecklessAppWindow alloc] initWithContentRect:content
                                                                               styleMask:style
                                                                                 backing:NSBackingStoreBuffered
                                                                                   defer:NO];
        window_->closing                = options.closing;
        window_->constrain              = options.constrain;
        window_->resized                = options.resized;
        window_->dialog                 = options.dialog;
        if (options.constrain)
        {
            // Resized by its frame and zoomed, as a JUCE Standalone's window is, but never a full-screen space of its
            // own: JUCE's window with no maximise button, whose zoom button zooms (windowWillUseStandardFrame).
            window_.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
        }
        window_.delegate           = window_;
        window_.releasedWhenClosed = NO;
        window_.title              = StringOf(options.title);
        window_.contentView        = [[RecklessAppContent alloc] initWithFrame:content];
        const gfx::PointI             size{static_cast<int>(std::lround(window_.frame.size.width)),
                                           static_cast<int>(std::lround(window_.frame.size.height))};
        const std::vector<gfx::RectI> areas = VisibleAreas();
        const auto                   *owner = dynamic_cast<const MacWindow *>(options.owner);
        if (options.position)
        {
            // Where it stood, wholly on the screen it is most on.
            MoveTo(PlaceWithin({options.position->first, options.position->second, size.x, size.y}, areas));
        }
        else if (owner != nullptr)
        {
            MoveTo(CentredOver(DownwardsOf(owner->window_.frame), size, areas));
        }
        else
        {
            [window_ center];
        }
        [window_ makeKeyAndOrderFront:nil];
        if (options.dialog && owner != nullptr)
        {
            // A child of its owner: kept over it, as a Windows owned window and an X11 transient one are, and hidden
            // with it when it is minimised. A child moves with its owner, as AppKit has it.
            [owner->window_ addChildWindow:window_ ordered:NSWindowAbove];
        }
    }

    /** Unhook the window's callbacks and delegate, detach it from its parent window and close it. */
    ~MacWindow() override
    {
        window_->closing     = nullptr;
        window_->constrain   = nullptr;
        window_->resized     = nullptr;
        window_->shortcut    = nullptr;
        window_->is_shortcut = nullptr;
        window_.delegate     = nil;
        if (NSWindow *parent = window_.parentWindow)
        {
            [parent removeChildWindow:window_];
        }
        [window_ orderOut:nil];
        [window_ close];
        window_ = nil;
    }

    void *Handle() const override
    {
        return (__bridge void *)window_.contentView;
    }

    float Scale() const override
    {
        return 1.0f; // points
    }

    std::pair<int, int> Position() const override
    {
        const NSRect frame = window_.frame;
        return {static_cast<int>(std::lround(frame.origin.x)),
                static_cast<int>(std::lround(MainScreenHeight() - NSMaxY(frame)))};
    }

    void BringToFront() override
    {
        if (window_.miniaturized)
        {
            [window_ deminiaturize:nil];
        }
        [window_ makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    void SetContentSize(int width, int height) override
    {
        // The content's top left kept: Cocoa's frame grows from its bottom left, so the bottom moves by the change.
        NSRect content = [window_ contentRectForFrameRect:window_.frame];
        content.origin.y += content.size.height - height;
        content.size.width  = width;
        content.size.height = height;
        [window_ setFrame:[window_ frameRectForContentRect:content] display:YES];
    }

    void SetContentSizeOnScreen(int width, int height) override
    {
        // The screen the window is most on now, before it grows; the frame at the new size, its content's top left
        // kept, moved onto that screen's visible frame in from any edge it would cross. AppKit tells the owner the size
        // taken (windowDidResize), as a drag's.
        const std::vector<gfx::RectI> areas   = VisibleAreas();
        const gfx::RectI             *area    = AreaFor(DownwardsOf(window_.frame), areas);
        NSRect                        content = [window_ contentRectForFrameRect:window_.frame];
        content.origin.y += content.size.height - height;
        content.size.width  = width;
        content.size.height = height;
        NSRect frame        = [window_ frameRectForContentRect:content];
        if (area != nullptr)
        {
            const gfx::RectI moved = MovedOnto(DownwardsOf(frame), *area);
            frame.origin.x         = moved.x();
            frame.origin.y         = MainScreenHeight() - moved.y() - NSHeight(frame);
        }
        [window_ setFrame:frame display:YES];
    }

    gfx::PointI LargestContent() const override
    {
        // The visible frame of the screen the window is most on, less the frame's own title bar.
        const std::vector<gfx::RectI> areas = VisibleAreas();
        const gfx::RectI             *area  = AreaFor(DownwardsOf(window_.frame), areas);
        if (area == nullptr)
        {
            return {0, 0};
        }
        const NSRect frame   = window_.frame;
        const NSRect content = [window_ contentRectForFrameRect:frame];
        return {std::max(1, area->width() - static_cast<int>(std::ceil(NSWidth(frame) - NSWidth(content)))),
                std::max(1, area->height() - static_cast<int>(std::ceil(NSHeight(frame) - NSHeight(content))))};
    }

    /** The keys that reach the window offered to the menu bar's shortcuts, and those that are shortcuts taken before
        the menu bar sees them, to go to the views first.
        @param shortcut     chooses the entry a key is the shortcut of: true when it was one; may be empty
        @param is_shortcut  whether a key is an enabled entry's shortcut, choosing nothing; may be empty */
    void SetShortcuts(std::function<bool(NSEvent *)> shortcut, std::function<bool(NSEvent *)> is_shortcut)
    {
        window_->shortcut    = std::move(shortcut);
        window_->is_shortcut = std::move(is_shortcut);
    }

    void SetTitle(const std::string &title) override
    {
        NSString *text = StringOf(title); // nil for text that is not UTF-8, which a window's title may not be
        window_.title  = text != nil ? text : @"";
    }

    std::string Title() const override
    {
        const char *utf8 = window_.title.UTF8String;
        return utf8 != nullptr ? std::string(utf8) : std::string();
    }

  private:
    /** The window's frame put at `frame`'s top left, y down from the top of the main screen.
        @param frame  the frame wanted, in points; only its top left is used */
    void MoveTo(gfx::RectI frame)
    {
        [window_ setFrameTopLeftPoint:NSMakePoint(frame.x(), MainScreenHeight() - frame.y())];
    }

    RecklessAppWindow *window_{nil}; ///< the NSWindow, which this object owns and closes in its destructor.
};

/** The Standalone's platform on macOS: the shared NSApplication with its menu bar, its windows and its timers on the
    run loop. Made by AppPlatform::Create. Main thread. */
class MacPlatform final : public AppPlatform
{
  public:
    /** Start the application, make its delegate and menu target, and build the menu bar with the app menu alone.
        @param name  the app's name, in the app menu's items */
    explicit MacPlatform(const std::string &name) : name_(name)
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate_         = [[RecklessAppDelegate alloc] init];
        delegate_->ending = false;
        NSApp.delegate    = delegate_;
        target_           = [[RecklessMenuTarget alloc] init];
        bar_target_       = [[RecklessMenuTarget alloc] init];
        // A bar item's tag is its command number; the controller is held weakly, the target outliving no platform.
        const std::weak_ptr<MenuBarController> menu = menu_;
        bar_target_->chosen                         = [menu](int command) {
            if (const std::shared_ptr<MenuBarController> held = menu.lock())
            {
                held->Choose(command);
            }
        };
        BuildMenu({});
        // A post that finds the queue empty dispatches one block to the main queue, which runs the batch: the main
        // queue is served on the main thread in the run loop's common modes, so posts run while a menu is tracked or
        // a window dragged too. The block holds the queue weakly, and finds it closed once Run has returned.
        const std::weak_ptr<core::PostQueue> weak = posts_;
        posts_->SetWake([weak] {
            dispatch_async(dispatch_get_main_queue(), ^{
              if (const std::shared_ptr<core::PostQueue> queue = weak.lock())
              {
                  queue->RunPending();
              }
            });
        });
    }

    /** Close the posts' queue, invalidate every timer the app made and drop the application delegate. */
    ~MacPlatform() override
    {
        posts_->Close();
        for (NSTimer *timer : timers_)
        {
            [timer invalidate];
        }
        NSApp.delegate = nil;
    }

    gfx::PointI LargestContent(const WindowOptions &options) const override
    {
        const NSRect      content = NSMakeRect(0, 0, options.width, options.height);
        const NSRect      frame   = [NSWindow frameRectForContentRect:content styleMask:StyleOf(options)];
        const gfx::PointI decoration{static_cast<int>(std::lround(frame.size.width - content.size.width)),
                                     static_cast<int>(std::lround(frame.size.height - content.size.height))};
        const std::vector<gfx::RectI> areas = VisibleAreas();
        const gfx::RectI  placed{options.position ? options.position->first : 0,
                                 options.position ? options.position->second : 0, options.width + decoration.x,
                                 options.height + decoration.y};
        const gfx::RectI *area = OpeningArea(placed, options.position.has_value(), areas);
        return area != nullptr ? FittedContent({options.width, options.height}, decoration, *area) : gfx::PointI{0, 0};
    }

    std::unique_ptr<AppWindow> OpenWindow(const WindowOptions &options) override
    {
        auto window = std::make_unique<MacWindow>(options);
        // Every window offers the keys that reach it to the bar's shortcuts, the bar being the app's, not a window's.
        const std::weak_ptr<MenuBarController> menu = menu_;
        window->SetShortcuts(
            [menu](NSEvent *event) {
                const std::shared_ptr<MenuBarController> held   = menu.lock();
                const Shortcut                           stroke = StrokeOf(event);
                return held != nullptr && !stroke.empty() && held->PressShortcut(stroke);
            },
            [menu](NSEvent *event) {
                const std::shared_ptr<MenuBarController> held   = menu.lock();
                const Shortcut                           stroke = StrokeOf(event);
                return held != nullptr && !stroke.empty() && held->FindShortcut(stroke) != nullptr;
            });
        return window;
    }

    /** The bar set, and the menu bar built from it after the app menu, in place of anything SetMenu built.
        @param bar     the menus
        @param chosen  told each chosen entry's id */
    void SetMenuBar(const MenuBar &bar, std::function<void(int id)> chosen) override
    {
        menu_->Set(bar, std::move(chosen));
        BuildBar();
    }

    /** The screen's menu bar shows the menus.
        @return  true */
    bool HasNativeMenuBar() const override
    {
        return true;
    }

    std::unique_ptr<window::RunLoop> ChildLoop() override
    {
        return nullptr; // AppKit runs the child windows' events and timers itself
    }

    void StartTimer(int milliseconds, std::function<void()> tick) override
    {
        auto     shared = std::make_shared<std::function<void()>>(std::move(tick));
        NSTimer *timer  = [NSTimer timerWithTimeInterval:milliseconds / 1000.0
                                                 repeats:YES
                                                   block:^(NSTimer *) {
                                                    (*shared)();
                                                   }];
        [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
        timers_.push_back(timer);
    }

    void CallSoon(std::function<void()> call) override
    {
        auto shared = std::make_shared<std::function<void()>>(std::move(call));
        dispatch_async(dispatch_get_main_queue(), ^{
          (*shared)();
        });
    }

    void SetMenu(const std::vector<view::MenuItem> &items, std::function<void(int)> chosen) override
    {
        target_->chosen = std::move(chosen);
        BuildMenu(items);
    }

    std::shared_ptr<core::PostQueue> Posts() override
    {
        return posts_;
    }

    bool SetClipboardText(const std::string &text) override
    {
        return window::mac::WritePasteboardText([NSPasteboard generalPasteboard], text);
    }

    std::string ClipboardText() override
    {
        return window::mac::ReadPasteboardText([NSPasteboard generalPasteboard]);
    }

    int Run() override
    {
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        posts_->Close(); // the app has quit: what other threads post from here on never runs
        return 0;
    }

    void Quit() override
    {
        // The loop stops after the event being handled; an event of no meaning wakes it to see that.
        [NSApp stop:nil];
        NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:0
                                              data1:0
                                              data2:0];
        [NSApp postEvent:wake atStart:NO];
    }

    void SetQuitHandler(std::function<void()> handler) override
    {
        delegate_->quit_handler = std::move(handler);
    }

  private:
    /** The menu bar: the app menu, and the app's own menu, "Options", where it has items.
        @param items  the app's commands; none leaves the app menu alone */
    void BuildMenu(const std::vector<view::MenuItem> &items)
    {
        NSMenu *bar = BarWithAppMenu();
        if (!items.empty())
        {
            NSMenu *options = [[NSMenu alloc] initWithTitle:@"Options"];
            for (const view::MenuItem &item : items)
            {
                if (item.separator)
                {
                    [options addItem:[NSMenuItem separatorItem]];
                    continue;
                }
                NSMenuItem *entry = [options addItemWithTitle:StringOf(item.text)
                                                       action:@selector(choose:)
                                                keyEquivalent:@""];
                entry.target      = target_;
                entry.tag         = item.id;
                entry.enabled     = item.enabled;
            }
            NSMenuItem *options_item = [[NSMenuItem alloc] init];
            options_item.submenu     = options;
            [bar addItem:options_item];
        }
        NSApp.mainMenu = bar;
    }

    /** The menu bar from the controller's bar (SetMenuBar): the app menu, then each of the bar's menus, each item
        tagged with its command number, enabled or not as the entry says (the menus enable nothing of their own),
        ticked where it is checked and showing its shortcut as its key equivalent - which the app's windows take
        before the bar does (their performKeyEquivalent), so the views have each key first. With no window of the
        app's key, the bar's items take their keys themselves, through the same controller. */
    void BuildBar()
    {
        NSMenu *bar = BarWithAppMenu();
        for (const MenuEntry &menu : menu_->bar().menus)
        {
            NSMenu     *submenu = MenuOf(menu);
            NSMenuItem *item    = [[NSMenuItem alloc] init];
            item.title          = StringOf(menu.title);
            item.submenu        = submenu;
            item.enabled        = menu.enabled;
            [bar addItem:item];
        }
        bar.autoenablesItems = NO;
        NSApp.mainMenu       = bar;
    }

    /** One of the bar's menus, or a submenu, as an NSMenu.
        @param menu  the menu's entry: its title and its entries
        @return      the menu */
    NSMenu *MenuOf(const MenuEntry &menu)
    {
        NSMenu *built          = [[NSMenu alloc] initWithTitle:StringOf(menu.title)];
        built.autoenablesItems = NO;
        for (const MenuEntry &entry : menu.submenu)
        {
            if (entry.separator)
            {
                [built addItem:[NSMenuItem separatorItem]];
                continue;
            }
            NSMenuItem *item = [[NSMenuItem alloc] init];
            item.title       = StringOf(entry.title);
            item.enabled     = entry.enabled;
            item.state       = entry.checked ? NSControlStateValueOn : NSControlStateValueOff;
            if (!entry.submenu.empty())
            {
                item.submenu = MenuOf(entry);
            }
            else
            {
                NSEventModifierFlags mask      = 0;
                item.keyEquivalent             = KeyEquivalentOf(entry.shortcut, mask);
                item.keyEquivalentModifierMask = mask;
                item.target                    = bar_target_;
                item.action                    = @selector(choose:);
                item.tag                       = menu_->CommandOf(entry);
            }
            [built addItem:item];
        }
        return built;
    }

    /** A menu bar holding the app menu alone - About, Hide, Hide Others, Show All, Quit - for the app's menus to
       follow. About is the system's standard panel, or, where the bar names an entry for it (MenuBar::about_id),
       that entry chosen through the bar as its own click chooses it.
        @return  the bar */
    NSMenu *BarWithAppMenu()
    {
        NSString   *name     = StringOf(name_);
        NSMenu     *bar      = [[NSMenu alloc] init];
        NSMenu     *app_menu = [[NSMenu alloc] initWithTitle:name];
        NSMenuItem *about    = [app_menu addItemWithTitle:[@"About " stringByAppendingString:name]
                                                   action:@selector(orderFrontStandardAboutPanel:)
                                            keyEquivalent:@""];
        if (const int command = menu_->CommandOfId(menu_->bar().about_id); command != 0)
        {
            about.target = bar_target_;
            about.action = @selector(choose:);
            about.tag    = command;
        }
        [app_menu addItem:[NSMenuItem separatorItem]];
        [app_menu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
        NSMenuItem *others               = [app_menu addItemWithTitle:@"Hide Others"
                                                               action:@selector(hideOtherApplications:)
                                                        keyEquivalent:@"h"];
        others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        [app_menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
        [app_menu addItem:[NSMenuItem separatorItem]];
        [app_menu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                            action:@selector(terminate:)
                     keyEquivalent:@"q"];
        NSMenuItem *app_item = [[NSMenuItem alloc] init];
        app_item.submenu     = app_menu;
        [bar addItem:app_item];
        return bar;
    }

    /** The calls other threads post (Posts): woken by a block on the main queue, closed when Run returns. */
    std::shared_ptr<core::PostQueue> posts_{std::make_shared<core::PostQueue>()};
    std::string                      name_;          ///< the app's name.
    RecklessAppDelegate             *delegate_{nil}; ///< the application's delegate, which holds the quit handler.
    RecklessMenuTarget              *target_{nil};   ///< the target of the app's menu items.
    RecklessMenuTarget *bar_target_{nil}; ///< the target of the menu bar's items (SetMenuBar), by command number.
    /** The menu bar, its choosing and its shortcuts, shared with every window's keyDown. */
    std::shared_ptr<MenuBarController> menu_{std::make_shared<MenuBarController>()};
    std::vector<NSTimer *>             timers_; ///< the timers StartTimer made, invalidated in the destructor.
};

std::unique_ptr<AppPlatform> AppPlatform::Create(const std::string &name, std::span<const std::uint8_t>)
{
    return std::make_unique<MacPlatform>(name); // the Dock's icon is the bundle's
}
} // namespace reckless::app
