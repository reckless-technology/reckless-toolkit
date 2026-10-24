#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"
#include "Tooltip.h"
#include "View.h"

namespace reckless::view
{
/**
    Another view tree shown larger or smaller: the content - a root of its own at its own size, a panel at its design
    size - drawn `zoom` times as large inside this view, which is the content's Host. The content lays itself out,
    paints and hit-tests in its own pixels and never learns its scale: this view paints it under a scale, hands it the
    mouse through a router of its own in the content's coordinates (the position over the zoom), and passes on what
    the content asks of its window - a part to paint again (scaled, rounded out, a pixel more each way for the
    antialiased edge), the pointer's shape (which this view then shows as its own), the keyboard (this view takes the
    window's while a view of the content has it, and hands each key on), a popup (the popup's content shown at the
    same zoom in a popup of the window's, as a JUCE menu scales with its target), a file dialog, the text's measure, a
    call soon - to the window this view lives in. Files dragged over it are the content's view's under the pointer.

    `watch`, when set, hears each mouse event this view takes, its position in the window's root coordinates, before
    the content does: what a window's tooltips follow the pointer by.
*/
class ZoomView final : public View, private Host
{
  public:
    /** A zoom of `shown`, which becomes this view's content with this view its host.
        @param shown  the content, a root of its own at its own size; the caller owns it and it outlives the zoom
        @param scale  the zoom, 1 for the content's own size */
    explicit ZoomView(View &shown, float scale = 1.0f) : content_(shown), router_(shown, this), zoom_(scale)
    {
        content_.SetHost(this);
    }

    /** The content is let go of: its host is cleared. */
    ~ZoomView() override
    {
        content_.SetHost(nullptr);
    }

    /** The view shown.
        @return  the content, owned by the caller */
    View &content() noexcept
    {
        return content_;
    }

    /** The content's router: whichever of its views is hovered, pressed or has the keyboard.
        @return  the router, in the content's coordinates */
    const MouseRouter &router() const noexcept
    {
        return router_;
    }

    /** The scale the content is shown at.
        @return  the zoom, 1 for the content's own size */
    float zoom() const noexcept
    {
        return zoom_;
    }

    /** Show the content at another scale; painted again when it differs.
        @param scale  the zoom, 1 for the content's own size */
    void SetZoom(float scale)
    {
        if (scale < zoom_ || zoom_ < scale)
        {
            zoom_ = scale;
            Repaint();
        }
    }

    /** The size the content covers here: its size times the zoom, whole pixels rounded out.
        @return  the rectangle in this view's coordinates, in pixels, at the origin */
    gfx::RectI ContentArea() const noexcept
    {
        return gfx::RectF{0.0f, 0.0f, static_cast<float>(content_.width()) * zoom_,
                          static_cast<float>(content_.height()) * zoom_}
            .SmallestIntegerContainer();
    }

    /** A point of this view's as the content's.
        @param p  the point, in this view's coordinates, in pixels
        @return   the same point in the content's coordinates: `p` over the zoom */
    gfx::PointF ToContent(gfx::PointF p) const noexcept
    {
        return {p.x / zoom_, p.y / zoom_};
    }

    /** The pointer still at `position`, in this view's coordinates, while this view or its zoom changed under it: the
        content's view under it found again and told of a move there - the one it left that it left, the one now under
        it that it entered - as JUCE sends a fake mouse move when a showing component's bounds change. Nothing while a
        press holds a view, as JUCE fakes no move during a drag; the watch hears nothing, the pointer having not moved.
        @param position  where the pointer rests, in this view's coordinates, in pixels */
    void PointerStill(gfx::PointF position)
    {
        if (router_.pressed() == nullptr)
        {
            router_.Move(ToContent(position), {});
        }
    }

    /** The pointer still where it was, and this view moved out from under it: the content's view it was over told it
        left. */
    void PointerGone()
    {
        router_.Exit();
    }

    PointerWatch watch; ///< hears each mouse event this view takes, before the content does; empty for none

    // ---- View ------------------------------------------------------------------------------------------------------

    /** The content, under the zoom, clipped to its own bounds.
        @param c  the canvas, this view's top left at its origin */
    void Paint(gfx::Canvas &c) override
    {
        c.AddTransform(gfx::Affine::Scale(zoom_));
        if (c.ClipToRect(content_.LocalBounds()))
        {
            PaintTree(content_, c);
        }
    }

    /** A press: told to the watch and handed to the content's router. A double click's press arrives with two clicks.
        @param e  the press, in this view's coordinates */
    void MouseDown(const MouseEvent &e) override
    {
        Tell(PointerEvent::kDown, e);
        router_.Down(ToContent(e.position), e.clicks, e.right, e.modifiers);
    }

    /** A drag: told to the watch as a move and handed to the content's router.
        @param e  the event, in this view's coordinates */
    void MouseDrag(const MouseEvent &e) override
    {
        Tell(PointerEvent::kMove, e);
        router_.Drag(ToContent(e.position), e.modifiers);
    }

    /** The release: told to the watch and handed to the content's router.
        @param e  the event, in this view's coordinates */
    void MouseUp(const MouseEvent &e) override
    {
        Tell(PointerEvent::kUp, e);
        router_.Up(ToContent(e.position), e.modifiers);
    }

    /** A move: told to the watch and handed to the content's router.
        @param e  the event, in this view's coordinates */
    void MouseMove(const MouseEvent &e) override
    {
        Tell(PointerEvent::kMove, e);
        router_.Move(ToContent(e.position), e.modifiers);
    }

    /** The pointer came on: told to the watch as a move and handed to the content's router as one.
        @param e  the event, in this view's coordinates */
    void MouseEnter(const MouseEvent &e) override
    {
        Tell(PointerEvent::kMove, e);
        router_.Move(ToContent(e.position), e.modifiers);
    }

    /** The pointer left: told to the watch and to the content's router.
        @param e  the event, in this view's coordinates */
    void MouseExit(const MouseEvent &e) override
    {
        Tell(PointerEvent::kExit, e);
        router_.Exit();
    }

    /** The wheel to the content's view under it, and the content's word whether a view used it: a wheel none of the
        content's views uses goes on up to the view round this one - a shell's viewport scrolls under it.
        @param e  the wheel event, in this view's coordinates
        @return   true when a view of the content used the wheel */
    bool MouseWheel(const MouseEvent &e) override
    {
        Tell(PointerEvent::kWheel, e);
        return router_.Wheel(ToContent(e.position), e.wheel_x, e.wheel_y, e.modifiers, e.wheel_reversed);
    }

    // The router of the window calls a double click's press MouseDown with two clicks, which the content's router
    // turns into the content's own double click: nothing more here.

    /** The window's keyboard while a view of the content has the content's.
        @return  true while a view of the content has the keyboard */
    bool WantsKeyboardFocus() const override
    {
        return router_.focused() != nullptr;
    }

    /** A key, handed to the content's view that has the keyboard.
        @param key  the key event
        @return     true when that view used it */
    bool KeyPressed(const KeyEvent &key) override
    {
        return router_.Key(key);
    }

    /** The window's keyboard went elsewhere, so the content's goes too - unless the content's view that has it keeps
        it (a box open to be typed into), which has it again with the window's next press. */
    void FocusLost() override
    {
        if (!router_.IsKeyboardKept())
        {
            router_.Focus(nullptr);
        }
    }

    /** Every point is this view's; the last one asked is where files dragged over it are asked about.
        @param point  the point, in this view's coordinates, in pixels
        @return       true */
    bool HitTest(gfx::PointI point) const override
    {
        last_hit_ = point;
        return true;
    }

    /** Whether the content's view at the point last hit-tested would take the files.
        @param paths  the paths of the files dragged
        @return       true when it would */
    bool IsInterestedInFiles(const std::vector<std::string> &paths) const override
    {
        const gfx::PointF at = ToContent(last_hit_.ToFloat());
        return FileTakerAt(content_, at.ToNearestInt(), paths) != nullptr;
    }

    /** Files dropped, to the content's view under them that takes them.
        @param paths     the paths of the files dropped
        @param position  where they were dropped, in this view's coordinates, in pixels */
    void FilesDropped(const std::vector<std::string> &paths, gfx::PointF position) override
    {
        router_.DropFiles(ToContent(position), paths);
    }

    // A drag over the content followed through it, so the content's view under the files hears it come, move and go
    // as it would under the window's own router - the NAM's screen and the Space's light while one hovers.

    /** A drag came in: followed through the content's router.
        @param paths     the paths of the files dragged
        @param position  where the drag is, in this view's coordinates, in pixels */
    void FileDragEnter(const std::vector<std::string> &paths, gfx::PointF position) override
    {
        router_.DragFiles(ToContent(position), paths);
    }

    /** The drag moved: followed through the content's router.
        @param paths     the paths of the files dragged
        @param position  where the drag is, in this view's coordinates, in pixels */
    void FileDragMove(const std::vector<std::string> &paths, gfx::PointF position) override
    {
        router_.DragFiles(ToContent(position), paths);
    }

    /** The drag left or ended: the content's view it was over is told it left. */
    void FileDragExit() override
    {
        router_.EndFileDrag();
    }

  private:
    /** Report a mouse event to the watch, if one is set.
        @param event  the kind of event
        @param e      the event, in this view's coordinates; reported in the window's root coordinates */
    void Tell(PointerEvent event, const MouseEvent &e) const
    {
        if (watch)
        {
            watch(event, ToRoot(e.position));
        }
    }

    // ---- Host: what the content asks of its window ----------------------------------------------------------------

    /** A part of the content to paint again: scaled, rounded out and a pixel more each way for the antialiased edge.
        @param area  the rectangle, in the content's coordinates, in pixels */
    void Invalidate(gfx::RectI area) override
    {
        const gfx::RectF scaled{static_cast<float>(area.x()) * zoom_, static_cast<float>(area.y()) * zoom_,
                                static_cast<float>(area.width()) * zoom_, static_cast<float>(area.height()) * zoom_};
        Repaint(scaled.SmallestIntegerContainer().Expanded(1));
    }

    /** A popup of the content's: its content shown at this zoom (ZoomedPopup) in a popup of the window's.
        @param popup_content    the popup's view tree; the caller owns it and keeps it alive while the popup is open
        @param at               the popup's top left, in the content's coordinates, in pixels
        @param pressed_outside  run with the press's screen point whenever a press lands outside the popup
        @param takes_mouse      false for a popup that only shows
        @return                 the popup, owned by the caller; null where the window has none */
    std::unique_ptr<Popup> OpenPopup(View &popup_content, gfx::PointI at,
                                     std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse) override;

    /** The content's pointer shape becomes this view's, and goes on to the window.
        @param shape  the shape over the content's view under the pointer */
    void SetCursor(Cursor shape) override
    {
        cursor = shape; // the window's router shows this view's shape after each event it hands on
        if (Host *h = host())
        {
            h->SetCursor(shape);
        }
    }

    /** A view of the content takes the keyboard, or none does: through the content's router.
        @param view  the content's view taking the keyboard; null for none */
    void RequestKeyboardFocus(View *view) override
    {
        router_.Focus(view);
    }

    /** The router's word that a view of the content has the keyboard or none does: this view takes the window's
       keyboard or lets it go.
        @param a_view_has_it  true when a view of the content has the keyboard now */
    void KeyboardFocusChanged(bool a_view_has_it) override
    {
        if (a_view_has_it)
        {
            GrabKeyboardFocus();
        }
        else
        {
            ReleaseKeyboardFocus();
        }
    }

    /** Whether the window holds the keyboard focus: the window's word; true in no window.
        @return  true while the window holds it or there is none */
    bool WindowHasKeyboardFocus() const override
    {
        const Host *h = host();
        return h == nullptr || h->WindowHasKeyboardFocus();
    }

    /** The window's zoom times this view's.
        @return  the content's scale on the screen on top of the display's density */
    float Zoom() const override
    {
        const Host *h = host();
        return (h != nullptr ? h->Zoom() : 1.0f) * zoom_;
    }

    /** The window's measure of the text; an estimate in no window.
        @param font  the font the text is drawn in
        @param text  the UTF-8 text
        @return      the width, in pixels at the root's scale */
    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        const Host *h = host();
        return h != nullptr ? h->TextWidth(font, text) : 0.5f * font.height * static_cast<float>(text.size());
    }

    /** The window's call soon; run at once in no window.
        @param call  what to run, once */
    void CallSoon(std::function<void()> call) override
    {
        if (Host *h = host())
        {
            h->CallSoon(std::move(call));
        }
        else
        {
            call();
        }
    }

    /** The window's file dialog, asked through the host this view lives in.
        @param request  what the dialog asks for
        @param picked   run once with the path chosen, or an empty string when cancelled */
    void ChooseFile(const FileRequest &request, std::function<void(const std::string &)> picked) override
    {
        View::ChooseFile(request, std::move(picked));
    }

    /** The window's clipboard, read through the host this view lives in: a paste into a view of the content.
        @return  the clipboard's text; empty in no window */
    std::string ClipboardText() override
    {
        Host *h = host();
        return h != nullptr ? h->ClipboardText() : std::string();
    }

    /** The window's clipboard, written through the host this view lives in: a copy from a view of the content.
        @param text  the text, UTF-8
        @return      true when the clipboard took it; false in no window */
    bool SetClipboardText(const std::string &text) override
    {
        Host *h = host();
        return h != nullptr && h->SetClipboardText(text);
    }

    /** A view of the content is leaving: the content's router forgets it.
        @param view  the view leaving */
    void ViewLeaving(const View &view) override
    {
        router_.Forget(view);
    }

    View               &content_;    ///< the view shown; owned by the caller
    MouseRouter         router_;     ///< hands the mouse and the keyboard to the content's views
    float               zoom_;       ///< the scale the content is shown at
    mutable gfx::PointI last_hit_{}; ///< the point last hit-tested, where files dragged over the view are asked about
};

/** A popup of a zoomed content's, shown at the same zoom: the window's popup holding a zoom of the popup's content. */
class ZoomedPopup final : public Popup
{
  public:
    /** A zoom of the popup's content, sized to cover it.
        @param shown  the popup's view tree; the caller owns it and it outlives the popup
        @param scale  the zoom the content is shown at */
    ZoomedPopup(View &shown, float scale) : view_(shown, scale)
    {
        view_.SetBounds(view_.ContentArea());
    }

    /** The window's popup closes first, while the view it shows lives. */
    ~ZoomedPopup() override
    {
        window_.reset(); // the window first, while the view it shows lives
    }

    /** The zoom the window's popup shows.
        @return  the zoom view, owned by the popup */
    ZoomView &view() noexcept
    {
        return view_;
    }

    /** Take the window's popup, and pass its closing by the window on to whoever opened this zoom of it.
        @param window  the window's popup showing the zoom's view; null when the window has none */
    void Adopt(std::unique_ptr<Popup> window)
    {
        window_ = std::move(window);
        if (window_ != nullptr)
        {
            // The window's word that it closed the popup, passed on to whoever opened this zoom of it.
            const std::weak_ptr<bool> alive = alive_;
            ZoomedPopup              *self  = this;
            window_->closed_by_window       = [alive, self] {
                if (!alive.expired() && self->closed_by_window)
                {
                    const std::function<void()> closed = self->closed_by_window; // its owner may drop the zoom in it
                    closed();
                }
            };
        }
    }

    /** Whether the window's popup is held.
        @return  true once a window's popup has been adopted */
    bool open() const noexcept
    {
        return window_ != nullptr;
    }

    /** Whether the point lies in the window's popup.
        @param screen  the point, in the screen's coordinates, in pixels
        @return        true when it does; false before a popup is adopted */
    bool Contains(gfx::PointI screen) const override
    {
        return window_ != nullptr && window_->Contains(screen);
    }

  private:
    ZoomView               view_;                                ///< the popup's content at the zoom
    std::unique_ptr<Popup> window_;                              ///< the window's popup showing the zoom's view
    std::shared_ptr<bool>  alive_{std::make_shared<bool>(true)}; ///< expires with the zoom
};

inline std::unique_ptr<Popup> ZoomView::OpenPopup(View &popup_content, gfx::PointI at,
                                                  std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse)
{
    auto              popup = std::make_unique<ZoomedPopup>(popup_content, zoom_);
    const gfx::PointI corner{gfx::RoundToInt(static_cast<float>(at.x) * zoom_),
                             gfx::RoundToInt(static_cast<float>(at.y) * zoom_)};
    popup->Adopt(View::OpenPopup(popup->view(), corner, std::move(pressed_outside), takes_mouse));
    if (!popup->open())
    {
        return nullptr;
    }
    return popup;
}
} // namespace reckless::view
