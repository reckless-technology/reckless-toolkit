#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"
#include "View.h"

namespace reckless::view
{
/**
    A panel's tooltips as JUCE's tooltip window shows them: when a tip appears, where, and how it looks.

    When (TooltipTracker): the JUCE window's timer, ticked every 123 ms, and its rules. The tip under the pointer is the
    hovered view's, or the nearest of its parents' that has one. A tip appears once the pointer has rested - the tip
    and the view under it unchanged and the pointer moved no more than 12 pixels between ticks - for the delay (the
    series' 500 ms, look::kTooltipDelayMs); while one is up, or within half a second of one going, a different tip
    replaces it at the next tick. A press or the wheel dismisses it at the next tick, no tip shows while a button is
    held, none at the very point the last press landed until the pointer moves, and none once the pointer has left the
    window or rests where no view has a tip. A tip that shows stays where it appeared until it goes.

    Where (TooltipBounds): a tip appears beside the pointer, 24 pixels to its right and 6 below - or, past the middle
    of the window, ending 12 pixels to its left or 6 above - held inside the window, as the series' shell holds its
    tooltip window inside the editor; and it hides when the pointer moves onto it.

    How (TooltipView): JUCE's tooltip in the panel's palette (the palette's panel a shade brighter, the ink, the accent
    at half strength round it) - a rounded box of corner 5, its outline a pixel inside the edge, the text in the bold
    13-pixel serif, centred, on lines balanced to at most 400 pixels as JUCE's tooltip balances them, 7 pixels clear
    at each side and 3 above and below. The window that shows it draws it at its own scale and not the panel's zoom,
    as the series' shell draws its tooltip window.
*/

/** What a tooltip looks like: the JUCE tooltip window's colours from the panel's palette, and its font. */
struct TooltipStyle
{
    gfx::Color background{gfx::Color::FromRgb(40, 40, 40)};      ///< the box's fill
    gfx::Color outline{gfx::Color::FromArgb(128, 200, 160, 60)}; ///< the outline a pixel inside the box's edge
    gfx::Color text{gfx::Color::FromRgb(240, 230, 200)};         ///< the text
    gfx::Font  font{gfx::Font(13.0f).Bold()};                    ///< the text's font, bold, 13 pixels
    float      max_width{400.0f}; ///< the widest a line may be before the text wraps, in pixels
    float      corner{5.0f};      ///< the box's corner radius, in pixels
};

/** A tip's text on its lines: each line's text and width, the widest line's width and the lines' height together. */
struct TooltipLayout
{
    std::vector<std::string> lines;        ///< each line's text, the spaces at its ends taken off
    std::vector<float>       widths;       ///< each line's width, in pixels, in the same order
    float                    width{0.0f};  ///< the widest line's width, in pixels
    float                    height{0.0f}; ///< the lines' height together, in pixels
};

namespace tooltip
{
/** The text's words with the space before each kept on it, so a line is the words it holds joined as they came.
    @param text  the tip's text
    @return      the words, each with the spaces before it; they join back to `text` */
inline std::vector<std::string> Words(std::string_view text)
{
    std::vector<std::string> words;
    std::size_t              start = 0;
    while (start < text.size())
    {
        std::size_t end = start;
        while (end < text.size() && text[end] == ' ')
        {
            ++end;
        }
        while (end < text.size() && text[end] != ' ')
        {
            ++end;
        }
        words.emplace_back(text.substr(start, end - start));
        start = end;
    }
    return words;
}

/** `text` with the spaces at its ends taken off: what a line's width is measured on, as a line's bounds are.
    @param text  the text, which must outlive the result
    @return      the part of `text` between its first and its last non-space; empty when it has none */
inline std::string_view Trimmed(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

/** The text wrapped at its spaces to lines of at most `max_width`, each line as wide as `measure` says its words are,
    a word wider than the limit on a line of its own.
    @param text         the tip's text
    @param max_width    the widest a line may be, in pixels
    @param line_height  one line's height, in pixels
    @param measure      the width of a text in the tip's font, in pixels
    @return             the lines, their widths, the widest width and the height of all of them */
inline TooltipLayout Wrap(std::string_view text, float max_width, float line_height,
                          const std::function<float(std::string_view)> &measure)
{
    TooltipLayout layout;
    std::string   line;
    const auto    finish = [&]() {
        const std::string_view kept = Trimmed(line);
        if (!kept.empty() || layout.lines.empty())
        {
            layout.lines.emplace_back(kept);
            layout.widths.push_back(measure(kept));
        }
        line.clear();
    };
    for (const std::string &word : Words(text))
    {
        const std::string longer = line + word;
        if (!Trimmed(line).empty() && measure(Trimmed(longer)) > max_width)
        {
            finish();
            line = std::string(Trimmed(word));
        }
        else
        {
            line = longer;
        }
    }
    finish();
    for (const float w : layout.widths)
    {
        layout.width = std::max(layout.width, w);
    }
    layout.height = line_height * static_cast<float>(layout.lines.size());
    return layout;
}
} // namespace tooltip

/** The tip's lines as JUCE's tooltip lays them out (TextLayout::createLayoutWithBalancedLineLengths): wrapped at the
    style's widest, then at ten pixels narrower each time down to half of it while the last two lines differ by more
    than a tenth, keeping the width whose last two lines were the most unequal - JUCE's own choice - when none comes
    within the tenth. Each line is the font's height, `measure` the text's width in the font.
    @param text     the tip's text
    @param style    the tooltip's look: its widest line and its font
    @param measure  the width of a text in the style's font, in pixels
    @return         the tip's lines, their widths, the widest width and the height of all of them */
inline TooltipLayout LayOutTooltip(std::string_view text, const TooltipStyle &style,
                                   const std::function<float(std::string_view)> &measure)
{
    const float line_height = style.font.height;
    float       max_width   = style.max_width;
    const float minimum     = max_width / 2.0f;
    float       best_width  = max_width;
    float       best        = 0.0f;
    while (max_width > minimum)
    {
        TooltipLayout layout = tooltip::Wrap(text, max_width, line_height, measure);
        if (layout.lines.size() < 2)
        {
            return layout;
        }
        const float last       = layout.widths[layout.widths.size() - 1];
        const float before     = layout.widths[layout.widths.size() - 2];
        const float shortest   = std::min(last, before);
        const float longest    = std::max(last, before);
        const float proportion = shortest > 0.0f ? longest / shortest : 1.0f;
        if (proportion > 0.9f && proportion < 1.1f)
        {
            return layout;
        }
        if (proportion > best)
        {
            best       = proportion;
            best_width = max_width;
        }
        max_width -= 10.0f;
    }
    return tooltip::Wrap(text, best_width, line_height, measure);
}

/** Where a tip of `layout` appears for the pointer at `pointer` in `area` (the window's, in its units), as JUCE's
    tooltip window places it: the layout's width and height plus 14 and 6, whole pixels; to the pointer's right by 24
    and below it by 6, or, past the area's middle, ending 12 to its left and 6 above it; held inside the area.
    @param layout   the tip's lines, from LayOutTooltip
    @param pointer  the pointer, in the area's units
    @param area     the window's rectangle the tip is held inside, in the same units
    @return         the tip's rectangle, in whole units */
inline gfx::RectI TooltipBounds(const TooltipLayout &layout, gfx::PointI pointer, gfx::RectI area)
{
    const int w = static_cast<int>(layout.width + 14.0f);
    const int h = static_cast<int>(layout.height + 6.0f);
    const int x = pointer.x > area.CentreX() ? pointer.x - (w + 12) : pointer.x + 24;
    const int y = pointer.y > area.CentreY() ? pointer.y - (h + 6) : pointer.y + 6;
    // Rectangle::constrainedWithin: the position held to the room the area leaves - which, for a tip wider than the
    // area, lies left of the area, as JUCE's limit gives it - and the size to the area's.
    const auto limit = [](int lower, int upper, int value) {
        return value < lower ? lower : (upper < value ? upper : value);
    };
    return {limit(area.x(), area.x() + area.width() - w, x), limit(area.y(), area.y() + area.height() - h, y),
            std::min(w, area.width()), std::min(h, area.height())};
}

/** The tip a view under the pointer has: its own, or the nearest of its parents' that has one; empty for none.
    @param view  the view under the pointer; may be null
    @return      the tip's text, a copy; empty when neither the view nor a parent has one */
inline std::string TipOf(const View *view)
{
    for (const View *v = view; v != nullptr; v = v->parent())
    {
        if (!v->tooltip.empty())
        {
            return v->tooltip;
        }
    }
    return {};
}

/** A tip on the window, sized to its text: drawn as JUCE's LookAndFeel_V4 draws a tooltip, taking no mouse. */
class TooltipView final : public View
{
  public:
    /** A hidden tip that takes no mouse.
        @param style  the tip's look */
    explicit TooltipView(TooltipStyle style = {}) : style_(std::move(style))
    {
        SetInterceptsMouse(false, false);
        SetVisible(false);
    }

    /** Dress the tip in another look and paint it again.
        @param style  the tip's look */
    void SetStyle(TooltipStyle style)
    {
        style_ = std::move(style);
        Repaint();
    }

    /** The tip's look.
        @return  the style in force */
    const TooltipStyle &style() const noexcept
    {
        return style_;
    }

    /** The tip `text`, laid out, at `bounds` (TooltipBounds), shown.
        @param text    the tip's text
        @param layout  the text on its lines, from LayOutTooltip
        @param bounds  the tip's rectangle in its parent's coordinates, in pixels */
    void Show(std::string text, TooltipLayout layout, gfx::RectI bounds)
    {
        text_   = std::move(text);
        layout_ = std::move(layout);
        SetBounds(bounds);
        SetVisible(true);
        Repaint();
    }

    /** Hide the tip and forget its text. */
    void Hide()
    {
        SetVisible(false);
        text_.clear();
    }

    /** The text shown.
        @return  the tip's text; empty while hidden */
    const std::string &text() const noexcept
    {
        return text_;
    }

    /** The text's layout.
        @return  the lines last shown */
    const TooltipLayout &layout() const noexcept
    {
        return layout_;
    }

    /** The rounded box, its outline and the lines of text, the block centred in the box and each line on the widest.
        @param c  the canvas, the tip's top left at its origin */
    void Paint(gfx::Canvas &c) override
    {
        const gfx::RectF all = LocalBounds().ToFloat();
        c.SetColor(style_.background);
        c.FillRoundedRect(all, style_.corner);
        c.SetColor(style_.outline);
        c.DrawRoundedRect(all.Reduced(0.5f), style_.corner, 1.0f);
        // TextLayout::draw: the block centred in the box, each line centred on the widest.
        const float left = (all.width() - layout_.width) * 0.5f;
        float       top  = (all.height() - layout_.height) * 0.5f;
        c.SetFont(style_.font);
        c.SetColor(style_.text);
        for (std::size_t i = 0; i < layout_.lines.size(); ++i)
        {
            const float x = left + (layout_.width - layout_.widths[i]) * 0.5f;
            c.DrawText(layout_.lines[i], gfx::RectF{x, top, layout_.widths[i] + 1.0f, style_.font.height},
                       gfx::Justify::kCentredLeft, false);
            top += style_.font.height;
        }
    }

  private:
    TooltipStyle  style_;  ///< the tip's look
    std::string   text_;   ///< the text shown
    TooltipLayout layout_; ///< the text on its lines
};

/** What a window's views report of the pointer, for the tooltips: each mouse event they take, by kind. */
enum class PointerEvent
{
    kMove, ///< moved over the view, or onto it
    kExit, ///< left it
    kDown, ///< a button pressed
    kUp,   ///< released
    kWheel ///< the wheel turned over it
};

/** A view's report of a mouse event, its position in the window's root coordinates. */
using PointerWatch = std::function<void(PointerEvent, gfx::PointF position)>;

/**
    When a window's tip shows: the JUCE tooltip window's timer callback, its state and its rules (the comment at the
    top of this file), on what the window reports - where the pointer is and whether it is over the window, each press,
    release and wheel - and, at each tick, what is under the pointer: which of the window's views, and its tip. Times
    are seconds on a steady clock. The tracker decides; the window places and draws (TooltipBounds, TooltipView).
*/
class TooltipTracker
{
  public:
    static constexpr double kTickSeconds   = 0.123; ///< the JUCE window's timer
    static constexpr double kRecentSeconds = 0.5;   ///< a tip gone less than this long is replaced at once
    static constexpr float  kQuickMove     = 12.0f; ///< more than this between ticks is a pointer still moving

    /** What is under the pointer at a tick: the window's view there (any identity), and the tip it shows. */
    struct Under
    {
        const void *view{nullptr}; ///< the view under the pointer, by identity; null for none
        std::string tip;           ///< the tip it shows; empty for none
    };

    /** A tracker with no tip showing and the pointer outside the window.
        @param delay_seconds  how long the pointer must rest before a tip appears, in seconds */
    explicit TooltipTracker(double delay_seconds) : delay_(delay_seconds)
    {
    }

    // ---- What the window reports --------------------------------------------------------------------------------

    /** The pointer is over the window at `position`.
        @param position  the pointer, in the window's root coordinates, in pixels */
    void PointerAt(gfx::PointF position) noexcept
    {
        position_ = position;
        over_     = true;
    }

    /** The pointer left the window. */
    void PointerLeft() noexcept
    {
        over_ = false;
    }

    /** A button went down at `position`: no tip shows while it is held, and one showing goes at the next tick.
        @param position  the press, in the window's root coordinates, in pixels */
    void Pressed(gfx::PointF position) noexcept
    {
        position_   = position;
        last_press_ = position;
        held_       = true;
        Dismiss();
    }

    /** The button went up. */
    void Released() noexcept
    {
        held_ = false;
    }

    /** The wheel turned: a tip showing goes at the next tick. */
    void Wheeled() noexcept
    {
        Dismiss();
    }

    /** The tip goes now: the pointer moved onto it.
        @param now  the time, in seconds on the steady clock */
    void Hide(double now) noexcept
    {
        if (showing_)
        {
            showing_   = false;
            shown_     = {};
            dismissed_ = false;
            last_hide_ = now;
        }
    }

    /** One tick at `now`, with what is under the pointer: JUCE's TooltipWindow::timerCallback.
        @param now    the time, in seconds on the steady clock
        @param under  the window's view under the pointer and its tip */
    void Tick(double now, const Under &under)
    {
        const void       *view = over_ ? under.view : nullptr;
        const std::string tip  = view != nullptr && !held_ ? under.tip : std::string();
        const bool        moved_quickly =
            std::hypot(position_.x - last_position_.x, position_.y - last_position_.y) > kQuickMove;
        last_position_         = position_;
        const bool tip_changed = tip != last_tip_ || view != last_view_;
        last_view_             = view;
        last_tip_              = tip;
        if (tip_changed || dismissed_ || moved_quickly)
        {
            last_change_ = now;
        }
        const auto same = [](float a, float b) { return !(a < b) && !(b < a); };
        const auto show = [&] {
            if (!(last_press_.has_value() && same(last_press_->x, position_.x) && same(last_press_->y, position_.y)))
            {
                Display(tip);
            }
        };
        if (showing_ || now < last_hide_ + kRecentSeconds)
        {
            if (view == nullptr || dismissed_ || tip.empty())
            {
                Hide(now);
            }
            else if (tip_changed)
            {
                show();
            }
        }
        else if (!tip.empty() && tip != shown_ && now > last_change_ + delay_)
        {
            show();
        }
    }

    // ---- What it decided ------------------------------------------------------------------------------------------

    /** Whether a tip is up.
        @return  true while one shows */
    bool showing() const noexcept
    {
        return showing_;
    }

    /** The tip showing, or empty.
        @return  the tip's text */
    const std::string &tip() const noexcept
    {
        return shown_;
    }

    /** Where the pointer was when the tip appeared, rounded: what it is placed beside.
        @return  the pointer then, in the window's root coordinates, in pixels */
    gfx::PointI shown_at() const noexcept
    {
        return shown_at_;
    }

    /** How many times a tip has appeared: a window lays the tip out again when this moves.
        @return  the count of tips shown */
    unsigned shows() const noexcept
    {
        return shows_;
    }

  private:
    /** A tip showing is marked to go at the next tick. */
    void Dismiss() noexcept
    {
        if (showing_)
        {
            dismissed_ = true;
        }
    }

    /** `tip` appears where the pointer is.
        @param tip  the tip's text */
    void Display(const std::string &tip)
    {
        shown_     = tip;
        shown_at_  = {gfx::RoundToInt(position_.x), gfx::RoundToInt(position_.y)};
        showing_   = true;
        dismissed_ = false;
        ++shows_;
    }

    double                     delay_;              ///< how long the pointer rests before a tip appears, in seconds
    gfx::PointF                position_{};         ///< the pointer, in the window's root coordinates
    gfx::PointF                last_position_{};    ///< the pointer at the previous tick
    std::optional<gfx::PointF> last_press_;         ///< where the last press landed; no tip there until it moves
    bool                       over_{false};        ///< whether the pointer is over the window
    bool                       held_{false};        ///< whether a button is held
    bool                       showing_{false};     ///< whether a tip is up
    bool                       dismissed_{false};   ///< a press or the wheel marked the tip to go at the next tick
    const void                *last_view_{nullptr}; ///< the view under the pointer at the previous tick
    std::string                last_tip_;           ///< its tip at the previous tick
    std::string                shown_;              ///< the tip showing; empty for none
    gfx::PointI                shown_at_{};         ///< where the pointer was when it appeared
    double                     last_change_{0.0};   ///< the time the tip, its view or the pointer last moved on
    double                     last_hide_{-std::numeric_limits<double>::infinity()}; ///< the time a tip last went
    unsigned                   shows_{0};                                            ///< how many tips have appeared
};
} // namespace reckless::view
