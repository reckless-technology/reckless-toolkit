#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"
#include "Text.h"

namespace reckless::view
{
/** The modifier keys held with a mouse or key event. */
struct Modifiers
{
    bool shift{false};   ///< a shift key
    bool command{false}; ///< the command key on a Mac, control elsewhere
    bool alt{false};     ///< an alt (option) key
};

/** A mouse event as the view it reaches sees it: positions in that view's coordinates, in pixels. */
struct MouseEvent
{
    gfx::PointF position;      ///< in the receiving view's coordinates
    gfx::PointF down;          ///< where the press began, in the same coordinates
    int         clicks{1};     ///< 2 for the press of a double click
    bool        right{false};  ///< the secondary button, or a control-click on a Mac
    Modifiers   modifiers{};   ///< the modifier keys held
    float       wheel_x{0.0f}; ///< the wheel's horizontal travel, in pixels, right positive; 0 outside MouseWheel
    float       wheel_y{0.0f}; ///< the wheel's vertical travel, in pixels, up positive; 0 outside MouseWheel
    bool        wheel_reversed{false}; ///< the device's direction inverted by the system (a Mac's natural scrolling)
};

/** A key, as the views see it: the editing and moving keys by name, and anything that types a character as that
    character. */
enum class Key
{
    kNone,      ///< no key: an empty event
    kCharacter, ///< `KeyEvent::character` says which
    kEscape,    ///< the escape key
    kReturn,    ///< the return or enter key
    kTab,       ///< the tab key
    kBackspace, ///< the backspace key
    kDelete,    ///< the forward delete key
    kLeft,      ///< the left arrow
    kRight,     ///< the right arrow
    kUp,        ///< the up arrow
    kDown,      ///< the down arrow
    kHome,      ///< the home key
    kEnd,       ///< the end key
    kPageUp,    ///< the page up key
    kPageDown,  ///< the page down key
};

/** A key press as the view with the keyboard sees it. */
struct KeyEvent
{
    Key       key{Key::kNone}; ///< which key; kCharacter for anything that types
    char32_t  character{0};    ///< the Unicode scalar typed, for Key::kCharacter
    Modifiers modifiers{};     ///< the modifier keys held
};

/** The pointer's shape over a view. */
enum class Cursor
{
    kArrow,        ///< the standard arrow
    kPointingHand, ///< over something that can be pressed
    kIBeam,        ///< over text
    kGrab,         ///< over something that can be dragged
    kGrabbing,     ///< while it is
    kUpDown,       ///< a vertical drag
    kLeftRight,    ///< a horizontal one
    kCrosshair,    ///< a crosshair
};

class View;

/** What a file dialog asks for: a file to open (one of `patterns`, "*.wav" style) or a place to save one. */
struct FileRequest
{
    std::string              title;       ///< the dialog's title
    std::string              folder;      ///< where the dialog opens; empty for wherever the platform last left it
    std::vector<std::string> patterns;    ///< the files it offers, "*.wav", "*.nam"; empty for any
    bool                     save{false}; ///< true for a place to save a file, false for a file to open
    std::string              file_name;   ///< a save's suggested name
};

/** A window of its own over a host's, showing a view tree - a menu, a tooltip - until it is destroyed, or until the
    window it opened over goes first, which closes it. */
class Popup
{
  public:
    virtual ~Popup() = default;

    /** Whether `screen`, a point in the screen's coordinates, lies in the popup's window: never once it has closed.
        @param screen  the point, in the screen's coordinates, in pixels
        @return        true when the point lies in the popup's window; false for any point once it has closed */
    virtual bool Contains(gfx::PointI screen) const = 0;

    /** Run once if the window the popup opened over goes first, as it closes the popup: what the popup showed has
        ended, and the handle is inert - its window gone, nothing of the window it opened over reached through it -
        for its owner to drop there or whenever it likes. Never run from inside one of the popup's own events. Set by
        whoever opened it: a menu ends dismissed on it. */
    std::function<void()> closed_by_window;
};

/** What a root view lives in: told which part of the root to paint again, and what the views ask of the window - the
    pointer's shape, the keyboard, a popup, and when a view leaves so the window's router forgets it. */
class Host
{
  public:
    virtual ~Host() = default;

    /** `content` (a root of its own, sized by its bounds) in a borderless window over this host's, its top left at
        `at` in this root's coordinates - it may reach past the root's bounds, and the screen keeps it whole -
        painted and given the mouse as a root is; `pressed_outside` runs with the press's screen point whenever a press
        lands outside it, and decides what that means (a menu checks its submenus' windows first). `takes_mouse`
        false for a popup that only shows (a tooltip). Null where the host has no popups. The popup closes when the
        handle goes, which may not be from inside `pressed_outside` or one of the content's own events (CallSoon) - or
        when this host's window goes first, which leaves the handle inert and runs its `closed_by_window`.
        @param content          the view tree to show, a root of its own; the caller owns it and keeps it alive while
       the popup is open
        @param at               the popup's top left, in this root's coordinates, in pixels
        @param pressed_outside  run with the press's screen point, in pixels, whenever a press lands outside the popup
        @param takes_mouse      false for a popup that only shows (a tooltip), which is given no mouse
        @return                 the popup, owned by the caller; null where the host has no popups */
    virtual std::unique_ptr<Popup> OpenPopup(View &content, gfx::PointI at,
                                             std::function<void(gfx::PointI screen)> pressed_outside,
                                             bool                                    takes_mouse = true)
    {
        (void)content;
        (void)at;
        (void)pressed_outside;
        (void)takes_mouse;
        return nullptr;
    }

    /** Part of the root needs painting again.
        @param root_area  the rectangle to paint again, in the root's coordinates, in pixels */
    virtual void Invalidate(gfx::RectI root_area) = 0;

    /** The pointer's shape, as the view under it asks.
        @param cursor  the shape */
    virtual void SetCursor(Cursor cursor)
    {
        (void)cursor;
    }

    /** A view asks for the keyboard (`view`, in this host's tree) or gives it up (null): the window hands the request
        to its router (MouseRouter::Focus).
        @param view  the view taking the keyboard, in this host's tree; null for none */
    virtual void RequestKeyboardFocus(View *view)
    {
        (void)view;
    }

    /** The router's word that a view has the keyboard now, or none has: the window takes the platform's keyboard
        focus while one does and gives it back when none does.
        @param a_view_has_it  true when a view has the keyboard now, false when none does */
    virtual void KeyboardFocusChanged(bool a_view_has_it)
    {
        (void)a_view_has_it;
    }

    /** Whether the window holds the platform's keyboard focus: a view the router gave the keyboard draws as focused
        (its caret, its lit outline) only while the window does, as a JUCE component draws only while it has the
        focus. A window that takes the focus whenever a view asks answers true.
        @return  true while the window holds the platform's keyboard focus */
    virtual bool WindowHasKeyboardFocus() const
    {
        return true;
    }

    /** The scale the window shows the root at on top of the display's own density - the shell's zoom - for a view
        that picks how it resamples an image by it, as the series' panels pick the face plate's; 1 for a window that
        shows the panel at its design size.
        @return  the shell's zoom, 1 at the design size */
    virtual float Zoom() const
    {
        return 1.0f;
    }

    /** How wide `text` is in `font`, as the window's canvas would draw it - for a view laid out before it paints (a
        menu sizing its popup).
        @param font  the font the text is drawn in
        @param text  the UTF-8 text
        @return      the width, in pixels at the root's scale */
    virtual float TextWidth(const gfx::Font &font, std::string_view text) const
    {
        return 0.5f * font.height * static_cast<float>(text.size()); // a host with no canvas (a test): an estimate
    }

    /** `call` on the window's thread once the event being handled is done - for a view that must not be destroyed
        while it is still handling one (a menu closing on the click that chose an item).
        @param call  what to run, once; a host with no loop of its own runs it at once */
    virtual void CallSoon(std::function<void()> call)
    {
        call(); // a host with no loop of its own (a test) runs it at once
    }

    /** The platform's own file dialog for `request`; `picked` runs once on the window's thread with the path chosen,
        or an empty one when the person cancels or the host has no dialog.
        @param request  what the dialog asks for
        @param picked   run once, on the window's thread, with the path chosen, or an empty string for none */
    virtual void ChooseFile(const FileRequest &request, std::function<void(const std::string &path)> picked)
    {
        (void)request;
        picked({});
    }

    /** The text on the system's clipboard, for a paste: UTF-8, or empty when it holds no text or the host has none.
        Waits on the clipboard's owner at most half a second where the platform asks it (X11).
        @return  the clipboard's text */
    virtual std::string ClipboardText()
    {
        return {}; // a host with no clipboard (a test)
    }

    /** `text` put on the system's clipboard, for a copy: UTF-8, what a paste anywhere reads until something else is
        copied. On X11 the copy is held by a connection of this process, the app's where the window runs on an app's
        loop, else the window's own while it lives (window::RunLoop::SetClipboardText).
        @param text  the text, UTF-8
        @return      true when the clipboard took it; false where the host has none (a test) or the system refused */
    virtual bool SetClipboardText(const std::string &text)
    {
        (void)text;
        return false; // a host with no clipboard (a test)
    }

    /** `view` and every view under it are leaving the tree (or going): forget them.
        @param view  the view leaving, still in the tree for the call */
    virtual void ViewLeaving(const View &view)
    {
        (void)view;
    }
};

/**
    The panels' components, written against no toolkit: a View is a rectangle of a panel - its bounds in its parent's
    coordinates, its children in front of it in the order added - that paints itself on the suite's Canvas and takes
    the mouse and the keyboard. A window (window/'s child window, drawing through Skia) is the root's Host: it
    is told which part of the root needs painting again, paints the tree through PaintTree when it can, and hands its
    mouse, its keys and the files dropped on it to a MouseRouter, which finds the view under the pointer, keeps a
    press on the view it began on until it ends, tells views when the pointer enters and leaves them, and gives the
    keyboard to the view a press lands on when that view wants it. A view owns no child: the panel that adds one owns
    it, as a member, and outlives the tree; a view leaving the tree, or going, tells the host, so its router forgets
    it.

    A view paints as a JUCE component paints, so a panel lands on the pixels the releases built on JUCE drew: each
    child clipped to its bounds and moved to its origin, its painting and its painting over its children each in a
    saved state of their own, and a view less than opaque (SetAlpha) drawn into a layer composited at its opacity. A
    disabled view (SetEnabled), or one under a disabled parent, draws as disabled; what it does with the mouse is its
    own to decide, as a JUCE component's is.
*/
class View
{
  public:
    /** A view with empty bounds, visible, enabled, not opaque, fully opaque in alpha, in no tree. */
    View() = default;

    /** The view taken out of its parent; or, for a root, its host told the view is leaving. Its children are let go
        (they have no parent afterwards), not destroyed. */
    virtual ~View()
    {
        if (parent_ != nullptr)
        {
            parent_->RemoveChild(*this);
        }
        else if (host_ != nullptr)
        {
            host_->ViewLeaving(*this); // a root going while its host still holds it
        }
        for (View *child : children_)
        {
            child->parent_ = nullptr;
        }
    }

    View(const View &)            = delete;
    View &operator=(const View &) = delete;

    // ---- Geometry -------------------------------------------------------------------------------------------------

    /** Place the view. Repaints the old and the new area, and calls Resized() when the size changed.
        @param bounds  the view's rectangle, in its parent's coordinates, in pixels */
    void SetBounds(gfx::RectI bounds)
    {
        if (bounds == bounds_)
        {
            return;
        }
        Repaint();
        const bool resized = bounds.width() != bounds_.width() || bounds.height() != bounds_.height();
        bounds_            = bounds;
        Repaint();
        if (resized)
        {
            Resized();
        }
    }

    /** The view's rectangle in its parent's coordinates.
        @return  the bounds, in pixels */
    gfx::RectI bounds() const noexcept
    {
        return bounds_;
    }

    /** The view's rectangle in its own coordinates.
        @return  the bounds with the origin at zero */
    gfx::RectI LocalBounds() const noexcept
    {
        return bounds_.WithZeroOrigin();
    }

    /** The view's width.
        @return  the width, in pixels */
    int width() const noexcept
    {
        return bounds_.width();
    }

    /** The view's height.
        @return  the height, in pixels */
    int height() const noexcept
    {
        return bounds_.height();
    }

    /** A point in this view's coordinates in the root's.
        @param p  the point, in this view's coordinates, in pixels
        @return   the same point in the root's coordinates */
    gfx::PointI ToRoot(gfx::PointI p) const noexcept
    {
        for (const View *v = this; v != nullptr && v->parent_ != nullptr; v = v->parent_)
        {
            p = p + v->bounds_.TopLeft();
        }
        return p;
    }

    /** A point in this view's coordinates in the root's, in fractions of a pixel.
        @param p  the point, in this view's coordinates, in pixels
        @return   the same point in the root's coordinates */
    gfx::PointF ToRoot(gfx::PointF p) const noexcept
    {
        const gfx::PointI offset = ToRoot(gfx::PointI{0, 0});
        return {p.x + static_cast<float>(offset.x), p.y + static_cast<float>(offset.y)};
    }

    // ---- The tree -------------------------------------------------------------------------------------------------

    /** Add `child` in front of the children already added, taking it from its previous parent. The child stays the
        caller's: the panel that adds it owns it and outlives the tree.
        @param child  the view to add, owned by the caller */
    void AddChild(View &child)
    {
        if (child.parent_ != nullptr)
        {
            child.parent_->RemoveChild(child);
        }
        child.parent_ = this;
        children_.push_back(&child);
        child.Repaint();
    }

    /** Take `child` out of the tree, telling the host so its router forgets it; nothing if it is not a child.
        @param child  the child to remove */
    void RemoveChild(View &child)
    {
        const auto it = std::find(children_.begin(), children_.end(), &child);
        if (it != children_.end())
        {
            child.Repaint();
            if (Host *h = host())
            {
                h->ViewLeaving(child); // while the child is still in the tree, so the router can tell it is
            }
            children_.erase(it);
            child.parent_ = nullptr;
        }
    }

    /** Whether `view` is this view or under it.
        @param view  the view asked about
        @return      true when it is this view or one of its descendants */
    bool Contains(const View &view) const noexcept
    {
        for (const View *v = &view; v != nullptr; v = v->parent_)
        {
            if (v == this)
            {
                return true;
            }
        }
        return false;
    }

    /** The view this one is a child of.
        @return  the parent; null for a root and for a view in no tree */
    View *parent() const noexcept
    {
        return parent_;
    }

    /** The children, from the back to the front.
        @return  the children, owned by whoever added them */
    const std::vector<View *> &children() const noexcept
    {
        return children_;
    }

    /** Show or hide the view and everything under it.
        @param visible  false to hide; a hidden view is neither painted nor given the mouse */
    void SetVisible(bool visible)
    {
        if (visible != visible_)
        {
            visible_ = visible;
            Repaint(LocalBounds(), true);
        }
    }

    /** Whether the view itself is shown.
        @return  true unless SetVisible(false) hid it */
    bool visible() const noexcept
    {
        return visible_;
    }

    /** How opaque the view and everything under it is drawn, 0 to 1: under 1 the tree paints it into a layer and
        composites the layer at that opacity, and at 0 not at all. Kept in 256 steps, rounded as a JUCE component
        rounds its alpha, so a view composites as the component it stands in for does.
        @param alpha  the opacity, 0 to 1, clamped */
    void SetAlpha(float alpha)
    {
        const double scaled       = 255.0 * static_cast<double>(std::clamp(alpha, 0.0f, 1.0f));
        const auto   transparency = static_cast<std::uint8_t>(255 - std::clamp(gfx::RoundToInt(scaled), 0, 255));
        if (transparency != transparency_)
        {
            transparency_ = transparency;
            Repaint();
        }
    }

    /** The opacity the view is drawn at.
        @return  the opacity, 0 to 1, in 256 steps */
    float alpha() const noexcept
    {
        return static_cast<float>(255 - transparency_) / 255.0f;
    }

    /** 0 for an opaque view, 255 for one not drawn at all.
        @return  the transparency, 0 to 255, 255 less the opacity in 256 steps */
    std::uint8_t transparency() const noexcept
    {
        return transparency_;
    }

    /** Whether the view takes part: a disabled view draws as disabled and leaves the mouse alone, each view deciding
        what that means, as a disabled JUCE component does.
        @param enabled  false to disable the view */
    void SetEnabled(bool enabled)
    {
        if (enabled != enabled_)
        {
            enabled_ = enabled;
            Repaint();
        }
    }

    /** Whether the view and every parent above it is enabled.
        @return  true when none of them has been disabled */
    bool IsEnabled() const noexcept
    {
        for (const View *v = this; v != nullptr; v = v->parent_)
        {
            if (!v->enabled_)
            {
                return false;
            }
        }
        return true;
    }

    /** Whether the view paints every pixel of its bounds, nothing showing through it - a JUCE component's opaque flag:
        the painting of its parent and every parent above it, and of each sibling under it, is clipped round it
        (PaintTree), as JUCE clips round an opaque component.
        @param opaque  true when the view paints every pixel of its bounds */
    void SetOpaque(bool opaque) noexcept
    {
        opaque_ = opaque;
    }

    /** Whether the view paints every pixel of its bounds.
        @return  the flag SetOpaque set */
    bool opaque() const noexcept
    {
        return opaque_;
    }

    /** Whether the view itself takes the mouse, and whether its children may.
        @param self      true when the view itself may be the view under the pointer
        @param children  true when its children may be */
    void SetInterceptsMouse(bool self, bool children) noexcept
    {
        intercepts_self_     = self;
        intercepts_children_ = children;
    }

    /** Whether the view itself takes the mouse.
        @return  the flag SetInterceptsMouse set for the view */
    bool intercepts_self() const noexcept
    {
        return intercepts_self_;
    }

    /** Whether the view's children may take the mouse.
        @return  the flag SetInterceptsMouse set for the children */
    bool intercepts_children() const noexcept
    {
        return intercepts_children_;
    }

    /** Make this view a root that lives in `host`; only a root's host is read (host()).
        @param host  the host the tree lives in, owned by the caller and outliving the tree; null for none */
    void SetHost(Host *host) noexcept
    {
        host_ = host;
    }

    /** The host of the tree this view is in: the root's.
        @return  the root's host; null where the tree has none */
    Host *host() const noexcept
    {
        const View *v = this;
        while (v->parent_ != nullptr)
        {
            v = v->parent_;
        }
        return v->host_;
    }

    /** All of the view painted again. */
    void Repaint()
    {
        Repaint(LocalBounds());
    }

    /** `area` of the view painted again, when the view and its parents are visible.
        @param area            the rectangle, in the view's coordinates, in pixels
        @param even_if_hidden  true to paint it again although the view itself is hidden: the view has just been hidden
                               and what it covered needs painting */
    void Repaint(gfx::RectI area, bool even_if_hidden = false)
    {
        if (!visible_ && !even_if_hidden)
        {
            return;
        }
        for (const View *v = this; v->parent_ != nullptr; v = v->parent_)
        {
            if (!v->parent_->visible_)
            {
                return;
            }
        }
        if (Host *h = host())
        {
            const gfx::PointI origin = ToRoot(gfx::PointI{0, 0});
            h->Invalidate(area.Translated(origin));
        }
    }

    std::string tooltip; ///< the text a tooltip shows while the pointer rests on the view; empty for none

    /** The pointer's shape over the view. */
    Cursor cursor{Cursor::kArrow};

    /** `content` in a popup over the window, its top left at `at` in this view's coordinates (Host::OpenPopup); null
        when the view is in no host's tree or the host has no popups.
        @param content          the view tree to show, owned by the caller, which keeps it alive while the popup is open
        @param at               the popup's top left, in this view's coordinates, in pixels
        @param pressed_outside  run with the press's screen point, in pixels, whenever a press lands outside the popup
        @param takes_mouse      false for a popup that only shows (a tooltip)
        @return                 the popup, owned by the caller; null when there is none */
    std::unique_ptr<Popup> OpenPopup(View &content, gfx::PointI at,
                                     std::function<void(gfx::PointI screen)> pressed_outside, bool takes_mouse = true)
    {
        Host *h = host();
        return h != nullptr ? h->OpenPopup(content, ToRoot(at), std::move(pressed_outside), takes_mouse) : nullptr;
    }

    /** The platform's file dialog, through the host (Host::ChooseFile); cancelled at once in no host's tree.
        @param request  what the dialog asks for
        @param picked   run once with the path chosen, or an empty string when cancelled */
    void ChooseFile(const FileRequest &request, std::function<void(const std::string &path)> picked)
    {
        if (Host *h = host())
        {
            h->ChooseFile(request, std::move(picked));
        }
        else
        {
            picked({});
        }
    }

    /** The view takes the keyboard, as a press on it would when it wants it (WantsKeyboardFocus). */
    void GrabKeyboardFocus()
    {
        if (Host *h = host())
        {
            h->RequestKeyboardFocus(this);
        }
    }

    /** No view keeps the keyboard: what a view does when it is done with it (a box typed into, closing). */
    void ReleaseKeyboardFocus()
    {
        if (Host *h = host())
        {
            h->RequestKeyboardFocus(nullptr);
        }
    }

    // ---- What a view does -----------------------------------------------------------------------------------------

    /** Paint the view's own content, under its children, on a canvas whose origin is the view's top left and whose
        clip is the view's bounds less the opaque views over it. Message thread.
        @param c  the canvas, in the view's own design pixels */
    virtual void Paint(gfx::Canvas &c)
    {
        (void)c;
    }

    /** Paint over the children, on the canvas as Paint() is given it.
        @param c  the canvas, in the view's own design pixels */
    virtual void PaintOverChildren(gfx::Canvas &c)
    {
        (void)c;
    }

    /** The view's size changed (SetBounds): the place to lay the children out. */
    virtual void Resized()
    {
    }

    /** Whether `point`, in the view's coordinates and inside its bounds, is the view's.
        @param point  the point, in the view's coordinates, in pixels
        @return       true when the point is the view's, false to let the views behind it have it */
    virtual bool HitTest(gfx::PointI point) const
    {
        (void)point;
        return true;
    }

    /** A press on the view; the drags and the release that follow go to it wherever the pointer goes. The event says
        which button and how many clicks.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseDown(const MouseEvent &e)
    {
        (void)e;
    }

    /** The pointer moved while the press that began on the view is held.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseDrag(const MouseEvent &e)
    {
        (void)e;
    }

    /** The press that began on the view ended.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseUp(const MouseEvent &e)
    {
        (void)e;
    }

    /** The release of the second press of a double click, after MouseUp(): neither moved 4 pixels nor lasted 300 ms.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseDoubleClick(const MouseEvent &e)
    {
        (void)e;
    }

    /** The pointer moved over the view with no press held.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseMove(const MouseEvent &e)
    {
        (void)e;
    }

    /** The pointer came onto the view.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseEnter(const MouseEvent &e)
    {
        (void)e;
    }

    /** The pointer left the view, with no press held.
        @param e  the event: where, which button and how many clicks */
    virtual void MouseExit(const MouseEvent &e)
    {
        (void)e;
    }

    /** The wheel over the view (MouseEvent::wheel_x, wheel_y): true when the view used it, false to hand it to its
        parent, as a JUCE component hands a wheel it has no use for up to its parent - so a viewport round a panel
        scrolls under a view that takes no wheel.
        @param e  the event: where, and the wheel's travel (wheel_x, wheel_y)
        @return   true when the view used the wheel, false to hand it to the parent */
    virtual bool MouseWheel(const MouseEvent &e)
    {
        (void)e;
        return false;
    }

    /** Whether a press on the view gives it the keyboard: a box being typed into, a text field.
        @return  true when a press on the view takes the keyboard */
    virtual bool WantsKeyboardFocus() const
    {
        return false;
    }

    /** Whether the view, while it has the keyboard, keeps it against a press on a view that wants none - the box it
        sits in, the box's title, the screen round it - and against the window's losing the keyboard to another, as a
        JUCE editor keeps its focus against a press on a component that takes none: a box open to be typed into, which
        a press on another view that wants the keyboard still takes it from.
        @return  true when the view keeps the keyboard against a press on a view that wants none */
    virtual bool KeepsKeyboardFocus() const
    {
        return false;
    }

    /** The router gave the view the keyboard. */
    virtual void FocusGained()
    {
    }

    /** The view lost the keyboard to another view, or to none. */
    virtual void FocusLost()
    {
    }

    /** A key while the view has the keyboard; true when it used it, false to let the window's host have it.
        @param key  the key and its modifiers
        @return     true when the view used the key */
    virtual bool KeyPressed(const KeyEvent &key)
    {
        (void)key;
        return false;
    }

    /** Whether files dragged over the view, by their paths, are ones it would take.
        @param paths  the paths of the files dragged, as the platform names them
        @return       true when the view would take them */
    virtual bool IsInterestedInFiles(const std::vector<std::string> &paths) const
    {
        (void)paths;
        return false;
    }

    /** Files dropped on the view (one that is interested in them), at `position` in its coordinates.
        @param paths     the paths of the files dropped
        @param position  where they were dropped, in the view's coordinates, in pixels */
    virtual void FilesDropped(const std::vector<std::string> &paths, gfx::PointF position)
    {
        (void)paths;
        (void)position;
    }

    /** Files the view is interested in are being dragged over it, the drag come in at `position` in its coordinates:
        what a view that lights to say it would take them hears, as a JUCE drop target hears fileDragEnter.
        @param paths     the paths of the files dragged
        @param position  where the drag came in, in the view's coordinates, in pixels */
    virtual void FileDragEnter(const std::vector<std::string> &paths, gfx::PointF position)
    {
        (void)paths;
        (void)position;
    }

    /** The drag that came in (FileDragEnter) moved on over the view, to `position` in its coordinates, as a JUCE drop
        target hears fileDragMove: what a view that shows another tree (ZoomView) follows the drag through it by.
        @param paths     the paths of the files dragged
        @param position  where the drag is, in the view's coordinates, in pixels */
    virtual void FileDragMove(const std::vector<std::string> &paths, gfx::PointF position)
    {
        (void)paths;
        (void)position;
    }

    /** The drag that came in (FileDragEnter) went on to another view, left the window, or ended with nothing dropped;
        not heard before FilesDropped, as a JUCE drop target hears no fileDragExit before filesDropped. */
    virtual void FileDragExit()
    {
    }

  private:
    gfx::RectI          bounds_{};                  ///< the view's rectangle in its parent's coordinates
    View               *parent_{nullptr};           ///< the view this one is a child of; null for a root
    std::vector<View *> children_;                  ///< the children, back to front; not owned
    Host               *host_{nullptr};             ///< a root's host; not owned
    bool                visible_{true};             ///< false after SetVisible(false)
    bool                enabled_{true};             ///< false after SetEnabled(false)
    bool                opaque_{false};             ///< SetOpaque's flag
    std::uint8_t        transparency_{0};           ///< 255 less the opacity in 256 steps
    bool                intercepts_self_{true};     ///< whether the view itself takes the mouse
    bool                intercepts_children_{true}; ///< whether its children may take the mouse
};

/** Whether an opaque view under `view` lies over `clip` (the view's own coordinates), each one there left out of the
    canvas's clip where it meets it - JUCE's clipObscuredRegions: the children from the frontmost back, a visible one
    that meets the clip cut out of it where it does when it is opaque and drawn whole, and searched for opaque views of
    its own when it is not. `delta` places the view in the canvas's coordinates.
    @param view    the view whose children are searched
    @param canvas  the canvas whose clip each opaque view found is cut out of
    @param clip    the area of interest, in the view's coordinates
    @param delta   the offset of the view's origin in the canvas's coordinates
    @return        true when any opaque view was cut out of the clip */
inline bool ExcludeOpaqueViews(const View &view, gfx::Canvas &canvas, gfx::RectI clip, gfx::PointI delta)
{
    bool        clipped  = false;
    const auto &children = view.children();
    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        const View &child = **it;
        if (!child.visible())
        {
            continue;
        }
        const gfx::RectI meets = clip.Intersection(child.bounds());
        if (meets.IsEmpty())
        {
            continue;
        }
        if (child.opaque() && child.transparency() == 0)
        {
            canvas.ExcludeClipRect(meets.Translated(delta));
            clipped = true;
        }
        else if (ExcludeOpaqueViews(child, canvas, meets.Translated(-child.bounds().x(), -child.bounds().y()),
                                    delta + child.bounds().TopLeft()))
        {
            clipped = true;
        }
    }
    return clipped;
}

/** `view` and its visible children onto `canvas`, `view` at the canvas's origin, in the order and states a JUCE
    component paints in: the view's Paint in a saved state of its own, clipped round the opaque views under it
    (ExcludeOpaqueViews); each child behind its later siblings - clipped to its bounds and round its later siblings
    that are opaque, moved to its own origin and, when less than opaque, drawn into a layer composited at its alpha -
    and the view's PaintOverChildren over them all, in a saved state of its own. A child outside the canvas's clip is
    skipped, and so is a painting whose clip the opaque views have taken whole. The view's own alpha is its parent's to
    apply, as a component's is: the root is painted as it is.
    @param view    the root of the tree to paint
    @param canvas  the canvas, the view at its origin */
inline void PaintTree(View &view, gfx::Canvas &canvas)
{
    if (!view.visible())
    {
        return;
    }
    {
        gfx::ScopedState state(canvas);
        if (!(ExcludeOpaqueViews(view, canvas, canvas.ClipBounds(), {}) && canvas.ClipBounds().IsEmpty()))
        {
            view.Paint(canvas);
        }
    }
    const auto &children = view.children();
    for (std::size_t i = 0; i < children.size(); ++i)
    {
        View *child = children[i];
        if (!child->visible() || child->bounds().IsEmpty() || child->transparency() == 255)
        {
            continue;
        }
        gfx::ScopedState state(canvas);
        if (!canvas.ClipToRect(child->bounds()))
        {
            continue;
        }
        bool covered = false;
        for (std::size_t j = i + 1; j < children.size(); ++j)
        {
            if (children[j]->opaque() && children[j]->visible())
            {
                covered = true;
                canvas.ExcludeClipRect(children[j]->bounds());
            }
        }
        if (covered && canvas.ClipBounds().IsEmpty())
        {
            continue;
        }
        canvas.AddTransform(
            gfx::Affine::Translation(static_cast<float>(child->bounds().x()), static_cast<float>(child->bounds().y())));
        if (child->transparency() > 0)
        {
            canvas.BeginTransparencyLayer(child->alpha());
            PaintTree(*child, canvas);
            canvas.EndTransparencyLayer();
        }
        else
        {
            PaintTree(*child, canvas);
        }
    }
    gfx::ScopedState state(canvas);
    view.PaintOverChildren(canvas);
}

/** The view under `point` (in `root`'s coordinates) that takes the mouse: the frontmost visible child that contains
    it and hits, searched depth first, else the root itself; null when even the root declines.
    @param root   the root of the tree searched
    @param point  the point, in the root's coordinates, in pixels
    @return       the view under the point; null when none takes the mouse */
inline View *ViewAt(View &root, gfx::PointI point)
{
    if (!root.visible() || !root.LocalBounds().Contains(point))
    {
        return nullptr;
    }
    if (root.intercepts_children())
    {
        const auto &children = root.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it)
        {
            View *child = *it;
            if (child->visible() && child->bounds().Contains(point))
            {
                if (View *found = ViewAt(*child, point - child->bounds().TopLeft()))
                {
                    return found;
                }
            }
        }
    }
    return root.intercepts_self() && root.HitTest(point) ? &root : nullptr;
}

/** The first of `view` and its parents that wants the keyboard, or null.
    @param view  the view pressed; may be null
    @return      the view that takes the keyboard; null when none of them wants it */
inline View *KeyboardTaker(View *view)
{
    for (View *v = view; v != nullptr; v = v->parent())
    {
        if (v->WantsKeyboardFocus())
        {
            return v;
        }
    }
    return nullptr;
}

/** The view under `point` (the root's coordinates) that would take `paths`: the deepest one interested, or null.
    @param root   the root of the tree searched
    @param point  the point, in the root's coordinates, in pixels
    @param paths  the paths of the files dragged or dropped
    @return       the view that would take them; null when none would */
inline View *FileTakerAt(View &root, gfx::PointI point, const std::vector<std::string> &paths)
{
    for (View *v = ViewAt(root, point); v != nullptr; v = v->parent())
    {
        if (v->IsInterestedInFiles(paths))
        {
            return v;
        }
    }
    return nullptr;
}

/** Whether `view`, or a visible view under it, would take `paths` anywhere: what a toolkit's drop target that asks
    before it says where (a JUCE component's isInterestedInFileDrag) answers for a tree.
    @param view   the root of the tree searched
    @param paths  the paths of the files dragged
    @return       true when `view` or a visible view under it would take them */
inline bool AnyViewTakesFiles(const View &view, const std::vector<std::string> &paths)
{
    if (!view.visible())
    {
        return false;
    }
    if (view.IsInterestedInFiles(paths))
    {
        return true;
    }
    return std::any_of(view.children().begin(), view.children().end(),
                       [&paths](const View *child) { return AnyViewTakesFiles(*child, paths); });
}

/**
    A window's input handed to its views. The mouse: a press goes to the view under the pointer and every drag and the
    release after it to that same view, wherever the pointer goes, and a double click comes after the release of its
    second press, as JUCE's does, unless that press moved 4 pixels or lasted past 300 ms; a move tells the view under
    it, and the view it left; the wheel goes to the view under it and up its parents until one uses it. The keyboard: a
    press gives it to the first of the pressed view and its parents that wants it, or takes it from whoever had it -
    unless that view keeps it (View::KeepsKeyboardFocus) and nothing pressed wants it - and keys go to that view. The
    pointer's shape follows the view under it, or the pressed one. Files dropped go to the view under them that is
    interested, and files dragged over the window tell that view as the drag comes in and goes. Points in the root's
    coordinates; each event reaches its view in the view's. The window forwards its host's ViewLeaving here (Forget), so
    the router never holds a view that has left.
*/
class MouseRouter
{
  public:
    /** `host` is told the pointer's shape and whether a view has the keyboard; it may be null (a test).
        @param root  the root of the tree the input goes to; the caller owns it and it outlives the router
        @param host  the host told the pointer's shape and the keyboard's state; not owned; null for none */
    explicit MouseRouter(View &root, Host *host = nullptr) : root_(root), host_(host)
    {
    }

    /** A press: to the view under the pointer, which keeps the press until it ends; the keyboard to the first view
        from it up that wants it.
        @param point      the pointer, in the root's coordinates, in pixels
        @param clicks     1 for a press, 2 for the second press of a double click
        @param right      true for the secondary button, or a control-click on a Mac
        @param modifiers  the modifier keys held */
    void Down(gfx::PointF point, int clicks, bool right, Modifiers modifiers)
    {
        Hover(point, modifiers);
        pressed_ = ViewAt(root_, point.ToNearestInt());
        down_    = point;
        right_   = right; // the press's button, for its drags and its release
        if (View *taker = KeyboardTaker(pressed_); taker != nullptr || !IsKeyboardKept())
        {
            Focus(taker); // a view that keeps the keyboard keeps it from a press on one that wants none
        }
        double_click_ = nullptr;
        if (pressed_ != nullptr)
        {
            View *view = pressed_;
            view->MouseDown(EventFor(*view, point, clicks, right, modifiers));
            if (clicks == 2 && pressed_ == view)
            {
                double_click_    = view; // decided at the release
                double_click_at_ = std::chrono::steady_clock::now();
            }
        }
        ShowCursor();
    }

    /** The pointer moved with a press held: to the pressed view, wherever the pointer is.
        @param point      the pointer, in the root's coordinates, in pixels
        @param modifiers  the modifier keys held */
    void Drag(gfx::PointF point, Modifiers modifiers)
    {
        if (double_click_ != nullptr && std::hypot(point.x - down_.x, point.y - down_.y) >= kDoubleClickSlop)
        {
            double_click_ = nullptr; // a drag, not a double click
        }
        if (pressed_ != nullptr)
        {
            pressed_->MouseDrag(EventFor(*pressed_, point, 1, right_, modifiers));
        }
    }

    /** The release, to the pressed view - and then, for the second press of a double click that neither moved
        kDoubleClickSlop nor lasted past kLongPress, the view's double click, as JUCE decides one at the release, so a
        double click's second press dragged or held is the press it was and nothing more.
        @param point      the pointer, in the root's coordinates, in pixels
        @param modifiers  the modifier keys held */
    void Up(gfx::PointF point, Modifiers modifiers)
    {
        if (View *view = pressed_)
        {
            pressed_         = nullptr;
            const bool quick = std::chrono::steady_clock::now() - double_click_at_ <= kLongPress;
            view->MouseUp(EventFor(*view, point, 1, right_, modifiers));
            if (double_click_ == view && quick) // still in the tree: Forget lets go of one that leaves
            {
                double_click_ = nullptr;
                view->MouseDoubleClick(EventFor(*view, point, 2, right_, modifiers));
            }
        }
        double_click_ = nullptr;
        Hover(point, modifiers);
        ShowCursor();
    }

    /** The pointer moved with no press held: the view it left and the view it entered are told, and the view under it
        hears the move.
        @param point      the pointer, in the root's coordinates, in pixels
        @param modifiers  the modifier keys held */
    void Move(gfx::PointF point, Modifiers modifiers)
    {
        Hover(point, modifiers);
        if (hovered_ != nullptr)
        {
            hovered_->MouseMove(EventFor(*hovered_, point, 1, false, modifiers));
        }
        ShowCursor();
    }

    /** The pointer left the window: the hovered view is told it left, unless a press holds the pointer. */
    void Exit()
    {
        if (hovered_ != nullptr && pressed_ == nullptr)
        {
            View *view = hovered_;
            hovered_   = nullptr;
            view->MouseExit(MouseEvent{});
        }
    }

    /** The wheel, in pixels (up and right positive); `reversed` when the system inverts the device's direction: to
        the view under the pointer and up its parents until one uses it (View::MouseWheel). Whether one did - false for
        the window to hand the wheel on.
        @param point      the pointer, in the root's coordinates, in pixels
        @param dx         the horizontal travel, in pixels, right positive
        @param dy         the vertical travel, in pixels, up positive
        @param modifiers  the modifier keys held
        @param reversed   true when the system inverts the device's direction
        @return           true when a view used the wheel */
    bool Wheel(gfx::PointF point, float dx, float dy, Modifiers modifiers, bool reversed = false)
    {
        for (View *view = ViewAt(root_, point.ToNearestInt()); view != nullptr; view = view->parent())
        {
            MouseEvent e     = EventFor(*view, point, 1, false, modifiers);
            e.wheel_x        = dx;
            e.wheel_y        = dy;
            e.wheel_reversed = reversed;
            if (view->MouseWheel(e))
            {
                return true;
            }
        }
        return false;
    }

    /** A key: to the view with the keyboard; false when none took it, for the window to hand on.
        @param key  the key event
        @return     true when the view with the keyboard used it */
    bool Key(const KeyEvent &key)
    {
        return focused_ != nullptr && focused_->KeyPressed(key);
    }

    /** `view` takes the keyboard, or no view does (null): the one that had it is told it lost it, the new one that it
        has it, and the host whether a view has it now. While the one losing it hears so, no view has it: a release
        it asks for then - a ZoomView giving up the window's keyboard as its content's goes - is of a keyboard it no
        longer holds and changes nothing, and a view that takes the keyboard then has it, told so, in `view`'s place.
        @param view  the view taking the keyboard; null for none */
    void Focus(View *view)
    {
        if (view == focused_)
        {
            return;
        }
        View *lost = focused_;
        if (lost != nullptr)
        {
            focused_ = nullptr;
            lost->FocusLost();
            if (focused_ != nullptr)
            {
                return; // taken while it heard: that Focus told the view and the host
            }
        }
        focused_ = view;
        if (focused_ != nullptr)
        {
            focused_->FocusGained();
        }
        if (host_ != nullptr && (lost == nullptr) != (focused_ == nullptr))
        {
            host_->KeyboardFocusChanged(focused_ != nullptr);
        }
    }

    /** Whether files dragged over `point` would be taken.
        @param point  the pointer, in the root's coordinates, in pixels
        @param paths  the paths of the files dragged
        @return       true when a view under the point would take them */
    bool WouldTakeFiles(gfx::PointF point, const std::vector<std::string> &paths)
    {
        return FileTakerAt(root_, point.ToNearestInt(), paths) != nullptr;
    }

    /** Files dropped at `point`: to the view under it that takes them; false when none does. The drag over it ends
        with no FileDragExit, as a JUCE drop target's ends before filesDropped.
        @param point  where the files were dropped, in the root's coordinates, in pixels
        @param paths  the paths of the files dropped
        @return       true when a view took them */
    bool DropFiles(gfx::PointF point, const std::vector<std::string> &paths)
    {
        dragged_over_ = nullptr;
        View *view    = FileTakerAt(root_, point.ToNearestInt(), paths);
        if (view == nullptr)
        {
            return false;
        }
        const gfx::PointF here = view->ToRoot(gfx::PointF{0.0f, 0.0f});
        view->FilesDropped(paths, {point.x - here.x, point.y - here.y});
        return true;
    }

    /** Files dragged to `point`, as the window's platform reports each move of the drag: the view under it that takes
        them told the drag came in (FileDragEnter), the one it was over before told it left (FileDragExit) when that is
        another, and the one it stays over told it moved (FileDragMove); whether any takes them, which the window
        answers its platform with.
        @param point  the drag, in the root's coordinates, in pixels
        @param paths  the paths of the files dragged
        @return       true when a view under the point takes them */
    bool DragFiles(gfx::PointF point, const std::vector<std::string> &paths)
    {
        View *view = FileTakerAt(root_, point.ToNearestInt(), paths);
        if (view == nullptr)
        {
            EndFileDrag();
            return false;
        }
        const gfx::PointF here = view->ToRoot(gfx::PointF{0.0f, 0.0f});
        const gfx::PointF at{point.x - here.x, point.y - here.y};
        if (view == dragged_over_)
        {
            view->FileDragMove(paths, at);
            return true;
        }
        EndFileDrag();
        dragged_over_ = view;
        view->FileDragEnter(paths, at);
        return true;
    }

    /** The drag left the window, or ended with nothing dropped: the view it was over told it left. */
    void EndFileDrag()
    {
        if (View *left = dragged_over_)
        {
            dragged_over_ = nullptr;
            left->FileDragExit();
        }
    }

    /** The view files are being dragged over, or null: for the suites.
        @return  the view, not owned; null when no drag is over a view */
    View *dragged_over() const noexcept
    {
        return dragged_over_;
    }

    /** `view` and everything under it are leaving the tree: forget whichever of them the router holds.
        @param view  the view leaving, with the views under it */
    void Forget(const View &view) noexcept
    {
        if (pressed_ != nullptr && view.Contains(*pressed_))
        {
            pressed_ = nullptr;
        }
        if (double_click_ != nullptr && view.Contains(*double_click_))
        {
            double_click_ = nullptr;
        }
        if (hovered_ != nullptr && view.Contains(*hovered_))
        {
            hovered_ = nullptr;
        }
        if (dragged_over_ != nullptr && view.Contains(*dragged_over_))
        {
            dragged_over_ = nullptr;
        }
        if (focused_ != nullptr && view.Contains(*focused_))
        {
            focused_ = nullptr; // not told: it is going
            if (host_ != nullptr)
            {
                host_->KeyboardFocusChanged(false);
            }
        }
    }

    /** The view a press began on, until it ends.
        @return  the pressed view, not owned; null with no press held */
    View *pressed() const noexcept
    {
        return pressed_;
    }

    /** The view under the pointer when no press holds it.
        @return  the hovered view, not owned; null for none */
    View *hovered() const noexcept
    {
        return hovered_;
    }

    /** The view that has the keyboard.
        @return  the view, not owned; null for none */
    View *focused() const noexcept
    {
        return focused_;
    }

    /** Whether the view with the keyboard keeps it (View::KeepsKeyboardFocus): the window's own losing the keyboard
        leaves it there, for when the window has it again.
        @return  true when a view has the keyboard and keeps it */
    bool IsKeyboardKept() const noexcept
    {
        return focused_ != nullptr && focused_->KeepsKeyboardFocus();
    }

  private:
    /** JUCE's rule for a press that is no click: moved this many pixels from where it began, or held past this long. */
    static constexpr float                     kDoubleClickSlop = 4.0f;
    static constexpr std::chrono::milliseconds kLongPress{300}; ///< a press held past this long is no double click

    /** The event `view` hears at `root_point`, in its own coordinates: made of its arguments alone, so a wheel or a
       move during a press changes nothing of the press.
        @param view        the view the event goes to
        @param root_point  the pointer, in the root's coordinates, in pixels
        @param clicks      1, or 2 for the press of a double click
        @param right       true for the secondary button
        @param modifiers   the modifier keys held
        @return            the event, in `view`'s coordinates */
    MouseEvent EventFor(const View &view, gfx::PointF root_point, int clicks, bool right, Modifiers modifiers) const
    {
        const gfx::PointF here = view.ToRoot(gfx::PointF{0.0f, 0.0f});
        MouseEvent        event;
        event.position  = {root_point.x - here.x, root_point.y - here.y};
        event.down      = {down_.x - here.x, down_.y - here.y};
        event.clicks    = clicks;
        event.right     = right;
        event.modifiers = modifiers;
        return event;
    }

    /** The view under the pointer changes to the one at `point`, the one left told it left and the new one that it
        came, unless a press holds the pointer.
        @param point      the pointer, in the root's coordinates, in pixels
        @param modifiers  the modifier keys held */
    void Hover(gfx::PointF point, Modifiers modifiers)
    {
        if (pressed_ != nullptr)
        {
            return; // a press keeps its view until it ends
        }
        View *now = ViewAt(root_, point.ToNearestInt());
        if (now != hovered_)
        {
            View *left = hovered_;
            hovered_   = now;
            if (left != nullptr)
            {
                left->MouseExit(EventFor(*left, point, 1, false, modifiers));
            }
            if (hovered_ != nullptr)
            {
                hovered_->MouseEnter(EventFor(*hovered_, point, 1, false, modifiers));
            }
        }
    }

    /** The pointer's shape: the pressed view's while a press lasts, else the hovered one's. */
    void ShowCursor()
    {
        const View  *over  = pressed_ != nullptr ? pressed_ : hovered_;
        const Cursor shape = over != nullptr ? over->cursor : Cursor::kArrow;
        if (host_ != nullptr && shape != cursor_)
        {
            cursor_ = shape;
            host_->SetCursor(shape);
        }
    }

    View                                 &root_;             ///< the root of the tree the input goes to
    Host                                 *host_;             ///< told the pointer's shape and the keyboard; may be null
    View                                 *pressed_{nullptr}; ///< the view a press began on, until it ends
    View                                 *hovered_{nullptr}; ///< the view under the pointer
    View                                 *focused_{nullptr}; ///< the view with the keyboard
    View                                 *dragged_over_{nullptr}; ///< the view files are being dragged over (DragFiles)
    View                                 *double_click_{nullptr}; ///< a double click's second press, until its release
    std::chrono::steady_clock::time_point double_click_at_{};     ///< when the second press of a double click came
    gfx::PointF                           down_{};                ///< where the press began, in the root's coordinates
    bool                                  right_{false};          ///< whether the press is of the secondary button
    Cursor                                cursor_{Cursor::kArrow}; ///< the shape last told to the host
};
} // namespace reckless::view
