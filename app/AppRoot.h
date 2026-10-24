#pragma once

#include <functional>
#include <optional>

#include "Geometry.h"
#include "Tooltip.h"
#include "View.h"
#include "ZoomView.h"

namespace reckless::app
{
/**
    An app window's root: the content at the window's own size inside a zoom, which hands it the mouse, and the
    series' tooltip in front of it, shown, moved and hidden as the series' tracker decides from what the zoom's watch
    hears - the delay, the drift, the press that hides it - as every Reckless window's tooltip is. The content is laid
    out to the window's size rather than scaled to it - an app's window is a document, not a face plate - down to the
    least its layout takes (`layout`); a window smaller than that shows the layout at that size zoomed down to fit, so a
    small laptop's screen shows all of it, smaller, rather than a layout squeezed past its least. With `layout` at
    {0, 0} the content is always at the window's size.

    An app may instead hold the content at one size and zoom (SetFixedZoom) - a size from a View menu, "125%": the
    content laid out at its design size and drawn at exactly that zoom, the root the design size times the zoom - for as
    long as the root keeps that size. Sized to anything else - the person dragging the window's frame, a display too
    small for it - the root lets the fixed zoom go and lays the content out as above.
*/
class AppRoot final : public view::View
{
  public:
    /** Seconds on a steady scale; a suite's own clock stands in. */
    using Clock = std::function<double()>;

    /** The root round `content`, which is sized with it.
        @param content  the window's views; outlive the root
        @param style    the tooltip's look (Theme::Tooltip)
        @param clock    the tracker's clock; the steady clock when empty
        @param layout   the least size the content is laid out at, in design pixels; {0, 0} for none, the content
                        always at the window's size */
    AppRoot(view::View &content, view::TooltipStyle style, Clock clock = {}, gfx::PointI layout = {0, 0});

    /** One display frame: the tooltip's tick, at the tracker's own rate. */
    void Frame();

    /** @return  the tooltip view */
    const view::TooltipView &tooltip() const noexcept
    {
        return tooltip_;
    }

    /** @return  the zoom the content sits in, which the suites drive the mouse through */
    view::ZoomView &zoom() noexcept
    {
        return zoom_;
    }

    /** The content held at `design` and drawn at `zoom` while the root is the size this returns - `design` times
        `zoom`, rounded - and laid out so at once where the root is that size already. The root's own size is not set:
        the app sizes the window to it (AppWindow::SetContentSizeOnScreen), and the root follows the window, so the
        child window drawing it follows too; before the window opens, the app sets the root's bounds to it itself.
        @param design  the size the content is laid out at, in design pixels; each side at least one
        @param zoom    the zoom it is drawn at, above zero
        @return        the root's size at which the fixed zoom holds, in design pixels */
    gfx::PointI SetFixedZoom(gfx::PointI design, float zoom);

    /** @return  the zoom held while the root is the size SetFixedZoom gave; none before it reaches that size, once it
                 has been sized to another, and before any was set */
    std::optional<float> fixed_zoom() const noexcept
    {
        return fixed_ && width() == fixed_->size.x && height() == fixed_->size.y ? std::optional<float>(fixed_->zoom)
                                                                                 : std::nullopt;
    }

    /** The content and the zoom brought to the window's size: at a fixed zoom's size, the content at its design size
        and that zoom; at any other, the fixed zoom let go and the content laid out at the window's size, or at the
        layout's least and zoomed down to it where the window is smaller. */
    void Resized() override;

  private:
    /** A zoom an app holds the content at (SetFixedZoom). */
    struct FixedZoom
    {
        gfx::PointI design; ///< the size the content is laid out at, in design pixels
        float       zoom;   ///< the zoom it is drawn at
        gfx::PointI size;   ///< the root's size at which it holds: the design times the zoom, rounded
    };

    /** The tip brought to what the tracker shows. */
    void ShowTracked();

    view::View              &content_;         ///< the window's views
    Clock                    clock_;           ///< seconds on a steady scale
    gfx::PointI              layout_;          ///< the least the content is laid out at; {0, 0} for none
    std::optional<FixedZoom> fixed_;           ///< the zoom an app holds the content at; none for the layout's own
    view::ZoomView           zoom_;            ///< the content, at its zoom
    view::TooltipView        tooltip_;         ///< the tip, in front of everything
    view::TooltipTracker     tracker_;         ///< when the tip shows, moves and hides
    gfx::PointF              pointer_;         ///< where the pointer was last
    double                   last_tick_{-1.0}; ///< the tracker's last tick
    unsigned                 shows_{0};        ///< the tracker's count of tips when the view last showed one
};
} // namespace reckless::app
