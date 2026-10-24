#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "LookGeometry.h"
#include "LookImages.h"
#include "LookPalette.h"
#include "LookTrademarks.h"
#include "ParameterControl.h"
#include "View.h"

namespace reckless::app
{
/**
    A list of named choices as the control a series plate (view::Switch) drives - an audio API, a device, a rate - so
    the settings dialog's choices are the series' legend plates, menu and all: the choices and the one it is on, and
    what choosing another does (`on_choose`, once a gesture ends, with the choice's index). A choice the plate is on
    already is no gesture, as on every plate. The plate's legend follows SetChoices through its owner, which sets it.
*/
class ChoiceControl final : public view::ParameterControl
{
  public:
    /** Called when the person chooses a choice the plate was not on, at the gesture's end.
        The argument `index` is the new choice's index; the call is made on a copy, since it may rebuild the owner. */
    std::function<void(int index)> on_choose;

    /** Replace the choices and the one the plate is on.
        @param choices  the legends, in menu order; may be empty
        @param current  the index of the choice in force, clamped to the list (0 for an empty one) */
    void SetChoices(std::vector<std::string> choices, int current)
    {
        choices_ = std::move(choices);
        current_ = choices_.empty() ? 0 : std::clamp(current, 0, static_cast<int>(choices_.size()) - 1);
    }

    /** The choice in force.
        @return  its index, 0 for an empty list */
    int current() const noexcept
    {
        return current_;
    }

    /** The legends.
        @return  the choices in menu order, valid until the next SetChoices() */
    const std::vector<std::string> &choices() const noexcept
    {
        return choices_;
    }

    double Normalized() const override
    {
        return NormalizedForChoice(current_);
    }

    double DefaultNormalized() const override
    {
        return 0.0;
    }

    std::string Text() const override
    {
        return ChoiceText(current_);
    }

    void BeginGesture() override
    {
        pending_.reset();
    }

    void SetNormalized(double value) override
    {
        const int count = ChoiceCount();
        pending_ = count > 1
                       ? std::clamp(static_cast<int>(std::lround(value * static_cast<double>(count - 1))), 0, count - 1)
                       : 0;
    }

    void EndGesture() override
    {
        if (pending_ && *pending_ != current_)
        {
            current_     = *pending_;
            const auto f = on_choose; // a copy: the choice may rebuild what owns this
            if (f)
            {
                f(current_);
            }
        }
        pending_.reset();
    }

    int ChoiceCount() const override
    {
        return static_cast<int>(choices_.size());
    }

    std::string ChoiceText(int index) const override
    {
        return index >= 0 && index < ChoiceCount() ? choices_[static_cast<std::size_t>(index)] : std::string();
    }

  private:
    std::vector<std::string> choices_;    ///< the legends, in menu order
    int                      current_{0}; ///< the index of the choice in force
    std::optional<int>       pending_;    ///< the index the open gesture has reached, empty before it moves
};

/** How the dialog's toggles look: a box with a tick in it when on, the box, the tick and the text in the ink - which
    every palette prints legibly on its panel, where the amber accent vanishes on a light plate - the row under the
    pointer lit by a wash of the ink, and the list's frame in the ink, as a knob's frame is. */
struct ToggleStyle
{
    gfx::Color text;           ///< the row's text
    gfx::Color box;            ///< the tick box's outline
    gfx::Color tick;           ///< the tick
    gfx::Color highlight;      ///< the wash under the pointer's row
    gfx::Color frame;          ///< the list's frame and scroll bar
    gfx::Font  font{15.0f};    ///< the row's font
    int        row_height{22}; ///< a row's height, in design pixels
    int        box_size{13};   ///< the tick box's side, in design pixels

    /** The style in a palette's ink.
        @param palette  the plugin's palette; only its ink is used
        @return         the style, with the default font and metrics */
    static ToggleStyle Of(const look::Palette &palette)
    {
        ToggleStyle style;
        style.text      = palette.ink;
        style.box       = palette.ink.WithAlpha(0.7f);
        style.tick      = palette.ink;
        style.highlight = palette.ink.WithAlpha(0.1f);
        style.frame     = palette.ink.WithAlpha(0.4f);
        return style;
    }
};

/** One toggle's row: its box and its text in `row`, ticked when `on`, lit when `lit`.
    @param c        the canvas to paint on
    @param style    the colours, font and metrics
    @param row      the row's bounds, in the canvas's coordinates
    @param text     the row's text
    @param on       true to draw the tick
    @param lit      true to wash the row, as under the pointer; ignored when not enabled
    @param enabled  false to draw the row at 40 % strength with no wash */
inline void PaintToggleRow(gfx::Canvas &c, const ToggleStyle &style, gfx::RectI row, std::string_view text, bool on,
                           bool lit, bool enabled = true)
{
    if (lit && enabled)
    {
        c.SetColor(style.highlight);
        c.FillRect(row);
    }
    const float alpha = enabled ? 1.0f : 0.4f;
    const auto  box   = gfx::RectI{row.x() + 6, row.CentreY() - style.box_size / 2, style.box_size, style.box_size};
    c.SetColor(style.box.WithMultipliedAlpha(alpha));
    c.DrawRoundedRect(box.ToFloat().Reduced(0.5f), 2.5f, 1.0f);
    if (on)
    {
        const gfx::RectF inner = box.ToFloat().Reduced(3.0f);
        gfx::Path        tick;
        tick.MoveTo({inner.x(), inner.y() + inner.height() * 0.55f});
        tick.LineTo({inner.x() + inner.width() * 0.4f, inner.bottom()});
        tick.LineTo({inner.right(), inner.y()});
        c.SetColor(style.tick.WithMultipliedAlpha(alpha));
        c.StrokePath(tick, gfx::Stroke(2.0f, gfx::Stroke::Join::kCurved, gfx::Stroke::Cap::kRounded));
    }
    c.SetColor(style.text.WithMultipliedAlpha(alpha));
    c.SetFont(style.font);
    c.DrawText(text, row.WithTrimmedLeft(box.right() - row.x() + 8).ToFloat(), gfx::Justify::kCentredLeft);
}

/** One toggle as a view - the input's muting - flipped by a click through `on_click`, which sets it again. */
class ToggleRow final : public view::View
{
  public:
    /** A row with its text and its click.
        @param text      the row's text
        @param style     the row's look
        @param on_click  called on a copy when a click lands on the row; it sets the row through SetOn() */
    ToggleRow(std::string text, ToggleStyle style, std::function<void()> on_click)
        : text_(std::move(text)), style_(std::move(style)), on_click_(std::move(on_click))
    {
        cursor = view::Cursor::kPointingHand;
    }

    /** Tick or clear the row, repainting if it changed.
        @param on  true to tick */
    void SetOn(bool on)
    {
        if (on != on_)
        {
            on_ = on;
            Repaint();
        }
    }

    /** Whether the row is ticked.
        @return  true when on */
    bool on() const noexcept
    {
        return on_;
    }

    void Paint(gfx::Canvas &c) override
    {
        PaintToggleRow(c, style_, LocalBounds(), text_, on_, lit_);
    }

    void MouseEnter(const view::MouseEvent &) override
    {
        SetLit(true);
    }

    void MouseExit(const view::MouseEvent &) override
    {
        SetLit(false);
    }

    void MouseUp(const view::MouseEvent &e) override
    {
        if (LocalBounds().ToFloat().Contains(e.position) && on_click_)
        {
            const auto f = on_click_;
            f();
        }
    }

  private:
    /** Light or unlight the row under the pointer, repainting if it changed.
        @param lit  true while the pointer is over the row */
    void SetLit(bool lit)
    {
        if (lit != lit_)
        {
            lit_ = lit;
            Repaint();
        }
    }

    std::string           text_;       ///< the row's text
    ToggleStyle           style_;      ///< the row's look
    std::function<void()> on_click_;   ///< what a click calls
    bool                  on_{false};  ///< whether the row is ticked
    bool                  lit_{false}; ///< whether the pointer is over the row
};

/**
    A column of toggles in a frame - a device's channels listed one by one, the MIDI inputs: a row each, ticked when on,
    lit under the pointer, a click on one handing its index to `on_click`; with more rows than it shows, the wheel
    scrolls it a row a notch and a bar at the right says where it is. `empty_text` stands in the frame when there are no
    rows.
*/
class ToggleList final : public view::View
{
  public:
    /** An empty list.
        @param style       the rows' look
        @param empty_text  the text shown in the frame when there are no rows
        @param on_click    called on a copy with the row's index when a click lands on an enabled row */
    ToggleList(ToggleStyle style, std::string empty_text, std::function<void(int)> on_click)
        : style_(std::move(style)), empty_text_(std::move(empty_text)), on_click_(std::move(on_click))
    {
    }

    /** The rows' names and which are on; the scroll kept where it still fits.
        @param names  the rows' text, top to bottom
        @param on     which rows are ticked, by index; padded with false or cut to the names' count */
    void SetRows(std::vector<std::string> names, std::vector<bool> on)
    {
        on.resize(names.size(), false);
        if (names != names_ || on != on_)
        {
            names_ = std::move(names);
            on_    = std::move(on);
            first_ = std::clamp(first_, 0, std::max(0, static_cast<int>(names_.size()) - VisibleRows()));
            Repaint();
        }
    }

    /** The rows' text.
        @return  the names, top to bottom, valid until the next SetRows() */
    const std::vector<std::string> &names() const noexcept
    {
        return names_;
    }

    /** Which rows are ticked.
        @return  a flag a row, by index, valid until the next SetRows() */
    const std::vector<bool> &on() const noexcept
    {
        return on_;
    }

    /** The rows a frame of `rows` rows' height takes, frame included.
        @param rows   the number of rows to show
        @param style  the rows' look, whose row height is used
        @return       the list's height, in design pixels */
    static int HeightFor(int rows, const ToggleStyle &style) noexcept
    {
        return rows * style.row_height + 2;
    }

    /** The rows the list's height shows at once.
        @return  the count, at least 1 */
    int VisibleRows() const noexcept
    {
        return std::max(1, (height() - 2) / style_.row_height);
    }

    /** The row at the top of the list, as scrolled.
        @return  its index, 0 when not scrolled */
    int first_row() const noexcept
    {
        return first_;
    }

    /** Where row `index` is drawn, in the list's coordinates; empty when it is scrolled out of sight.
        @param index  the row's index
        @return       the row's bounds, in design pixels, or an empty rectangle */
    gfx::RectI RowBounds(int index) const noexcept
    {
        const int shown = index - first_;
        if (shown < 0 || shown >= VisibleRows())
        {
            return {};
        }
        return {1, 1 + shown * style_.row_height, width() - 2 - (Scrolls() ? kBarWidth : 0), style_.row_height};
    }

    void Paint(gfx::Canvas &c) override
    {
        if (names_.empty())
        {
            c.SetColor(style_.text.WithAlpha(0.5f));
            c.SetFont(style_.font);
            c.DrawText(empty_text_, LocalBounds().Reduced(8, 0).ToFloat(), gfx::Justify::kCentredLeft);
        }
        for (int i = first_; i < static_cast<int>(names_.size()) && i < first_ + VisibleRows(); ++i)
        {
            PaintToggleRow(c, style_, RowBounds(i), names_[static_cast<std::size_t>(i)],
                           on_[static_cast<std::size_t>(i)], i == hover_, IsEnabled());
        }
        if (Scrolls())
        {
            const float track = static_cast<float>(height() - 6);
            const float share = static_cast<float>(VisibleRows()) / static_cast<float>(names_.size());
            const float top   = static_cast<float>(first_) / static_cast<float>(names_.size()) * track;
            c.SetColor(style_.frame);
            c.FillRoundedRect({static_cast<float>(width() - kBarWidth - 1), 3.0f + top,
                               static_cast<float>(kBarWidth - 2), std::max(6.0f, share * track)},
                              2.0f);
        }
        c.SetColor(style_.frame);
        c.DrawRoundedRect(LocalBounds().ToFloat().Reduced(0.5f), 3.0f, 1.0f);
    }

    void MouseMove(const view::MouseEvent &e) override
    {
        SetHover(RowAt(e.position));
    }

    void MouseEnter(const view::MouseEvent &e) override
    {
        SetHover(RowAt(e.position));
    }

    void MouseExit(const view::MouseEvent &) override
    {
        SetHover(-1);
    }

    void MouseUp(const view::MouseEvent &e) override
    {
        const int row = RowAt(e.position);
        if (row >= 0 && row == RowAt(e.down) && IsEnabled() && on_click_)
        {
            const auto f = on_click_;
            f(row);
        }
    }

    /** The wheel scrolls a list longer than its rows show; a list that shows every row hands it on.
        @param e  the wheel event
        @return   true when the list scrolls and took it, false when it shows every row */
    bool MouseWheel(const view::MouseEvent &e) override
    {
        if (!Scrolls())
        {
            return false;
        }
        wheel_ += e.wheel_y;
        const int notches = static_cast<int>(wheel_ / kWheelPixelsPerRow);
        if (notches != 0)
        {
            wheel_ -= static_cast<float>(notches) * kWheelPixelsPerRow;
            const int last = static_cast<int>(names_.size()) - VisibleRows();
            first_         = std::clamp(first_ - notches, 0, last);
            SetHover(RowAt(e.position));
            Repaint();
        }
        return true;
    }

  private:
    static constexpr int kBarWidth = 6; ///< the scroll bar's width, in design pixels
    /** The wheel's pixels that scroll one row; a wheel's notch is 10 to 20 pixels in the windows. */
    static constexpr float kWheelPixelsPerRow = 10.0f;

    /** Whether there are more rows than the list shows.
        @return  true when the wheel scrolls it and the bar is drawn */
    bool Scrolls() const noexcept
    {
        return static_cast<int>(names_.size()) > VisibleRows();
    }

    /** The row under a point.
        @param p  a point in the list's coordinates
        @return   the row's index, or -1 for none */
    int RowAt(gfx::PointF p) const noexcept
    {
        for (int i = first_; i < static_cast<int>(names_.size()) && i < first_ + VisibleRows(); ++i)
        {
            if (RowBounds(i).ToFloat().Contains(p))
            {
                return i;
            }
        }
        return -1;
    }

    /** Light the row under the pointer, repainting if it changed.
        @param row  the row's index, or -1 for none */
    void SetHover(int row)
    {
        if (row != hover_)
        {
            hover_ = row;
            Repaint();
        }
    }

    ToggleStyle              style_;       ///< the rows' look
    std::string              empty_text_;  ///< what the frame says with no rows
    std::function<void(int)> on_click_;    ///< what a click on a row calls
    std::vector<std::string> names_;       ///< the rows' text
    std::vector<bool>        on_;          ///< which rows are ticked
    int                      first_{0};    ///< the index of the top row shown
    int                      hover_{-1};   ///< the index of the row under the pointer, -1 for none
    float                    wheel_{0.0f}; ///< the wheel's pixels not yet taken up as a row
};

/**
    Steinberg's ASIO-compatible logo with its attribution line beside it (look::kAsioTrademarkLine, word for word), as
    Steinberg's usage guidelines (ASIO SDK 2.3.4) ask in every dialog that turns ASIO on: the logo, white lettering on a
    transparent ground as Steinberg ships it, on a black box kLogoWidth by kLogoHeight, 21 by 13.5 mm at the 96 pixels
    an inch a design pixel is, shrunk by halving to the physical pixels it lands on (look::ShrunkImage) and drawn on
    whole ones, kGap before the line, set in kLineHeight on up to two lines; the logo left out where the window shows it
    smaller than Steinberg's smallest, 15 mm, and the line standing alone. Only a build with ASIO shows one, the one
    place the logo is drawn.
*/
class AsioNoticeView final : public view::View
{
  public:
    static constexpr int   kLogoWidth     = 80;          ///< the logo's black box, in design pixels
    static constexpr int   kLogoHeight    = 51;          ///< the logo's black box, in design pixels
    static constexpr int   kGap           = 12;          ///< the gap between the logo and the line, in design pixels
    static constexpr float kLineHeight    = 13.0f;       ///< the line's type size, in design pixels
    static constexpr float kMinShownWidth = 57.0f;       ///< the least width the logo is shown at, in physical pixels
    static constexpr int   kHeight        = kLogoHeight; ///< the view's height, in design pixels

    /** `logo` (Steinberg's artwork, or an invalid image for the line alone), shrunk through `images`, which outlives
        the view; the line in `ink`.
        @param logo    the logo's image, or an invalid one for the line alone
        @param images  what shrinking the image needs of the toolkit; outlives the view
        @param ink     the line's colour */
    AsioNoticeView(gfx::Image logo, const look::ImageBackend *images, gfx::Color ink)
        : logo_(std::move(logo)), images_(images), ink_(ink)
    {
        SetInterceptsMouse(false, false);
    }

    /** Whether the logo is drawn at the zoom the window shows the dialog at: not under kMinShownWidth.
        @return  true when the logo is valid and shows at least kMinShownWidth wide */
    bool ShowsLogo() const
    {
        const view::Host *h    = host();
        const float       zoom = h != nullptr ? h->Zoom() : 1.0f;
        return logo_.IsValid() && images_ != nullptr &&
               static_cast<float>(kLogoWidth) * zoom >= kMinShownWidth - 1.0e-3f;
    }

    /** The attribution line.
        @return  look::kAsioTrademarkLine */
    std::string_view line() const noexcept
    {
        return look::kAsioTrademarkLine;
    }

    void Paint(gfx::Canvas &c) override
    {
        gfx::RectI area = LocalBounds();
        if (ShowsLogo())
        {
            const auto logo = area.RemoveFromLeft(kLogoWidth).WithSizeKeepingCentre(kLogoWidth, kLogoHeight).ToFloat();
            c.SetColor(gfx::colors::kBlack);
            c.FillRect(logo);
            const auto placed = look::CentredFit(
                gfx::RectF(static_cast<float>(logo_.width()), static_cast<float>(logo_.height())), logo);
            const float scale  = c.PhysicalPixelScale();
            const int   width  = std::max(1, gfx::RoundToInt(placed.width() * scale));
            const int   height = std::max(1, gfx::RoundToInt(placed.height() * scale));
            if (shrunk_.width() != width || shrunk_.height() != height)
            {
                shrunk_ = look::ShrunkImage(*images_, logo_, width, height);
            }
            c.SetResampling(gfx::Resampling::kHigh);
            c.DrawImage(shrunk_, look::OnPhysicalPixels(placed, width, height, scale));
            area.RemoveFromLeft(kGap);
        }
        c.SetColor(ink_);
        c.SetFont(gfx::Font(kLineHeight));
        c.DrawFittedText(look::kAsioTrademarkLine, area, gfx::Justify::kCentredLeft, 2, 1.0f);
    }

  private:
    gfx::Image                logo_;   ///< Steinberg's artwork, or an invalid image
    const look::ImageBackend *images_; ///< what shrinking the logo needs of the toolkit
    gfx::Color                ink_;    ///< the line's colour
    gfx::Image                shrunk_; ///< the logo at the physical size it was last drawn at
};
} // namespace reckless::app
