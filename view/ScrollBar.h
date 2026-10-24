#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "Canvas.h"
#include "Geometry.h"
#include "Tooltip.h"
#include "View.h"

namespace reckless::view
{
/**
    A scroll bar as the shell shows one beside a panel too big for its window, drawn and worked as JUCE's ScrollBar
    under its LookAndFeel_V4, so a window in scroll mode reads and works as it did in the releases built on JUCE.

    A range limited to [minimum, maximum] and the part of it showing (start and size), the thumb that part along the
    bar: its length the showing share of the bar, at least twice the bar's thickness (one pixel short of the bar where
    that is longer), the bar's whole length once the bar is too short for a thumb and 32 pixels beside it - when no
    thumb is drawn. No arrows (the V4 look has none). It shows only while some of the range is hidden. A press on the
    thumb drags it, the range following the thumb's travel; a press before or after it moves a page (the showing size)
    that way, and, held, a page every 40 ms after the first 400 ms while the thumb has not reached the pointer. The
    wheel moves ten steps a notch, at least one. The thumb is a rounded rectangle (corner 4) a pixel inside its bounds,
    in the V4 dark scheme's fill unless its owner gives it a colour (SetThumbColour), a quarter brighter under the
    pointer; the track is not drawn.

    `on_move` hears each move the person makes, with the range's new start; `watch` each mouse event, as the window's
    tooltips want it. The page repeat runs on Step(now), which the window calls once a display frame.
*/
class ScrollBar final : public View
{
  public:
    /** The thumb's colour: the JUCE V4 dark colour scheme's default fill, which the series' look leaves as it is. */
    static constexpr gfx::Color kThumb = gfx::Color::FromRgb(0x42, 0xa2, 0xc8);

    static constexpr double kFirstRepeatSeconds = 0.4;  ///< how long a press before or after the thumb waits to repeat
    static constexpr double kRepeatSeconds      = 0.04; ///< the gap between repeated pages, in seconds

    /** @param vertical  true for a bar down the right edge, false for one along the bottom */
    explicit ScrollBar(bool vertical) : vertical_(vertical)
    {
    }

    /** Told each move the person makes, with the range's new start; empty for none. */
    std::function<void(double start)> on_move;

    /** Told each mouse event the bar takes, for the window's tooltips; empty for none. */
    PointerWatch watch;

    /** @return  true for a vertical bar */
    bool vertical() const noexcept
    {
        return vertical_;
    }

    /** The whole range the bar scrolls over; the showing part is held inside it.
        @param minimum  the range's start
        @param maximum  the range's end; at least `minimum` is used */
    void SetRangeLimits(double minimum, double maximum)
    {
        if (minimum < total_min_ || total_min_ < minimum || maximum < total_max_ || total_max_ < maximum)
        {
            total_min_ = minimum;
            total_max_ = std::max(minimum, maximum);
            SetCurrentRange(start_, size_);
            UpdateThumb();
        }
    }

    /** The showing part, held inside the limits; true when it moved.
        @param start  the showing part's start
        @param size   the showing part's length
        @return       true when the part moved or changed size */
    bool SetCurrentRange(double start, double size)
    {
        const double total = total_max_ - total_min_;
        size               = std::clamp(size, 0.0, total);
        start              = std::clamp(start, total_min_, total_max_ - size);
        if (start < start_ || start_ < start || size < size_ || size_ < size)
        {
            start_ = start;
            size_  = size;
            UpdateThumb();
            return true;
        }
        return false;
    }

    /** The thumb's colour in place of the V4 dark scheme's fill (kThumb) a bar is made with - an app's accent - drawn
        a quarter brighter under the pointer, as the fill is.
        @param colour  the thumb's colour */
    void SetThumbColour(gfx::Color colour)
    {
        if (!(colour == thumb_colour_))
        {
            thumb_colour_ = colour;
            Repaint();
        }
    }

    /** @return  the thumb's colour: kThumb unless the owner gave it another */
    gfx::Color thumb_colour() const noexcept
    {
        return thumb_colour_;
    }

    /** The distance one step of the wheel moves the range.
        @param step  the step, in the range's units */
    void SetSingleStepSize(double step) noexcept
    {
        step_ = step;
    }

    /** @return  the showing part's start, in the range's units */
    double start() const noexcept
    {
        return start_;
    }

    /** @return  the showing part's length, in the range's units */
    double size() const noexcept
    {
        return size_;
    }

    /** @return  the range's end, in the range's units */
    double maximum() const noexcept
    {
        return total_max_;
    }

    /** Whether the bar shows: some of the range hidden.
        @return  true when part of the range is hidden */
    bool Needed() const noexcept
    {
        return total_max_ - total_min_ > size_ && size_ > 0.0;
    }

    /** @return  the thumb's start along the bar, in design pixels */
    int thumb_start() const noexcept
    {
        return thumb_start_;
    }

    /** @return  the thumb's length along the bar, in design pixels */
    int thumb_size() const noexcept
    {
        return thumb_size_;
    }

    /** Once a display frame, the window's steady clock in seconds: a page moved while a press before or after the
        thumb is held, on JUCE's timing.
        @param now  the window's steady clock, in seconds */
    void Step(double now)
    {
        now_ = now;
        if (!held_ || repeat_due_ < 0.0 || now < repeat_due_)
        {
            return;
        }
        repeat_due_ = now + kRepeatSeconds;
        if (last_pointer_ < thumb_start_)
        {
            Move(start_ - size_);
        }
        else if (last_pointer_ > thumb_start_ + thumb_size_)
        {
            Move(start_ + size_);
        }
    }

    /** Whether the bar may show at all - a Viewport's word on it - beside whether the range needs it.
        @param shown  true to let the bar show when the range needs it */
    void SetShown(bool shown)
    {
        shown_ = shown;
        View::SetVisible(shown_ && Needed());
    }

    // ---- View ------------------------------------------------------------------------------------------------------

    /** Places the thumb's area: the whole length, or none when the bar is too short for a thumb. */
    void Resized() override
    {
        const int length = vertical_ ? height() : width();
        if (length < 32 + MinimumThumb())
        {
            thumb_area_start_ = length / 2;
            thumb_area_size_  = 0;
        }
        else
        {
            thumb_area_start_ = 0;
            thumb_area_size_  = length;
        }
        UpdateThumb();
    }

    /** Draws the thumb as a rounded rectangle; the track is not drawn.
        @param c  the canvas, in the bar's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        if (thumb_area_size_ <= 0)
        {
            return;
        }
        const int        thumb = thumb_area_size_ > MinimumThumb() ? thumb_size_ : 0;
        const gfx::RectI bounds =
            vertical_ ? gfx::RectI{0, thumb_start_, width(), thumb} : gfx::RectI{thumb_start_, 0, thumb, height()};
        const gfx::RectI inside = bounds.Reduced(1);
        if (inside.width() <= 0 || inside.height() <= 0)
        {
            return;
        }
        c.SetColor(over_ ? thumb_colour_.Brighter(0.25f) : thumb_colour_);
        c.FillRoundedRect(inside.ToFloat(), 4.0f);
    }

    /** Brightens the thumb and reports the pointer to `watch`.
        @param e  the move: its position */
    void MouseEnter(const MouseEvent &e) override
    {
        Tell(PointerEvent::kMove, e);
        SetOver(true);
    }

    /** Reports the pointer to `watch`.
        @param e  the move: its position */
    void MouseMove(const MouseEvent &e) override
    {
        Tell(PointerEvent::kMove, e);
    }

    /** Dims the thumb and reports the pointer to `watch`.
        @param e  the move: its position */
    void MouseExit(const MouseEvent &e) override
    {
        Tell(PointerEvent::kExit, e);
        SetOver(false);
    }

    /** A press on the thumb begins a drag; before or after it, moves a page that way and arms the repeat.
        @param e  the press: its position */
    void MouseDown(const MouseEvent &e) override
    {
        Tell(PointerEvent::kDown, e);
        held_          = true;
        dragging_      = false;
        last_pointer_  = Along(e.position);
        drag_start_    = last_pointer_;
        start_at_down_ = start_;
        repeat_due_    = -1.0;
        if (drag_start_ < thumb_start_)
        {
            Move(start_ - size_);
            repeat_due_ = now_ + kFirstRepeatSeconds;
        }
        else if (drag_start_ >= thumb_start_ + thumb_size_)
        {
            Move(start_ + size_);
            repeat_due_ = now_ + kFirstRepeatSeconds;
        }
        else
        {
            dragging_ = thumb_area_size_ > MinimumThumb() && thumb_area_size_ > thumb_size_;
        }
        Repaint();
    }

    /** Drags the thumb, the range following its travel.
        @param e  the drag: its position */
    void MouseDrag(const MouseEvent &e) override
    {
        Tell(PointerEvent::kMove, e);
        const int pointer = Along(e.position);
        if (dragging_ && pointer != last_pointer_ && thumb_area_size_ > thumb_size_)
        {
            const double travel = static_cast<double>(pointer - drag_start_);
            Move(start_at_down_ +
                 travel * (total_max_ - total_min_ - size_) / static_cast<double>(thumb_area_size_ - thumb_size_));
        }
        last_pointer_ = pointer;
    }

    /** Lets go of the thumb and stops the page repeat.
        @param e  the release: its position */
    void MouseUp(const MouseEvent &e) override
    {
        Tell(PointerEvent::kUp, e);
        held_       = false;
        dragging_   = false;
        repeat_due_ = -1.0;
        Repaint();
    }

    /** JUCE's wheel on a scroll bar: its delta in JUCE's units (100 of the views' pixels one, as JUCE's view host
        handed a unit on), ten steps a unit - two for a notch, which the child windows hand on as 20 pixels on Windows
        and Linux - at least one step either way; always the bar's, as JUCE's bar keeps it.
        @param e  the wheel event: its deltas, in the views' pixels
        @return   always true */
    bool MouseWheel(const MouseEvent &e) override
    {
        Tell(PointerEvent::kWheel, e);
        float increment = 10.0f * (vertical_ ? e.wheel_y : e.wheel_x) / 100.0f;
        if (increment < 0.0f)
        {
            increment = std::min(increment, -1.0f);
        }
        else if (increment > 0.0f)
        {
            increment = std::max(increment, 1.0f);
        }
        Move(start_ - step_ * static_cast<double>(increment));
        return true;
    }

  private:
    /** @return  the thumb's shortest length, twice the bar's thickness, in design pixels */
    int MinimumThumb() const noexcept
    {
        return std::min(width(), height()) * 2;
    }

    /** @param p  a point, in the bar's local coordinates
        @return   its whole-pixel position along the bar */
    int Along(gfx::PointF p) const noexcept
    {
        return gfx::RoundToInt(vertical_ ? p.y : p.x); // a JUCE mouse event's whole-pixel position
    }

    /** Report a mouse event to `watch`, in the window's root coordinates.
        @param event  the kind of event
        @param e      the event */
    void Tell(PointerEvent event, const MouseEvent &e) const
    {
        if (watch)
        {
            watch(event, ToRoot(e.position));
        }
    }

    /** Note whether the pointer is over the bar; repaints when it changes.
        @param over  true when it is */
    void SetOver(bool over)
    {
        if (over != over_)
        {
            over_ = over;
            Repaint();
        }
    }

    /** The range's start moved by the person, and told.
        @param start  the new start of the showing part */
    void Move(double start)
    {
        if (SetCurrentRange(start, size_) && on_move)
        {
            on_move(start_);
        }
    }

    /** Recompute the thumb from the range and the bar's length, and the bar's visibility; repaints on a change. */
    void UpdateThumb()
    {
        const double total = total_max_ - total_min_;
        int          new_size =
            gfx::RoundToInt(total > 0.0 ? size_ * thumb_area_size_ / total : static_cast<double>(thumb_area_size_));
        const int minimum = MinimumThumb();
        if (new_size < minimum)
        {
            new_size = std::min(minimum, thumb_area_size_ - 1);
        }
        if (new_size > thumb_area_size_)
        {
            new_size = thumb_area_size_;
        }
        int new_start = thumb_area_start_;
        if (total > size_)
        {
            new_start += gfx::RoundToInt((start_ - total_min_) * (thumb_area_size_ - new_size) / (total - size_));
        }
        View::SetVisible(shown_ && Needed());
        if (new_start != thumb_start_ || new_size != thumb_size_)
        {
            thumb_start_ = new_start;
            thumb_size_  = new_size;
            Repaint();
        }
    }

    bool   vertical_;            ///< a vertical bar
    double total_min_{0.0};      ///< the range's start
    double total_max_{1.0};      ///< the range's end
    double start_{0.0};          ///< the showing part's start
    double size_{1.0};           ///< the showing part's length
    double step_{16.0};          ///< a Viewport's single step
    int    thumb_area_start_{0}; ///< where the thumb's travel starts along the bar, in design pixels
    int    thumb_area_size_{0};  ///< the thumb's travel, in design pixels; 0 when the bar is too short for a thumb
    int    thumb_start_{0};      ///< the thumb's start along the bar, in design pixels
    int    thumb_size_{0};       ///< the thumb's length, in design pixels
    bool   shown_{true};         ///< the bar may show, when the range needs it
    bool   over_{false};         ///< the pointer is over the bar
    bool   held_{false};         ///< a press is down
    bool   dragging_{false};     ///< the press is dragging the thumb
    int    last_pointer_{0};     ///< the pointer's position along the bar at the last event, in design pixels
    int    drag_start_{0};       ///< the pointer's position along the bar at the press, in design pixels
    double start_at_down_{0.0};  ///< the range's start at the press
    double repeat_due_{-1.0};    ///< the time of the next page, in seconds; negative for none armed
    double now_{0.0};            ///< the last Step's time, in seconds

    gfx::Color thumb_colour_{kThumb}; ///< the thumb's colour
};
} // namespace reckless::view
