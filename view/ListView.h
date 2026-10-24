#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"
#include "ScrollBar.h"
#include "Text.h"
#include "View.h"

namespace reckless::view
{
/** A column of a ListView: its header's title, its width - fixed, or a share of what the fixed columns leave - and
    where its cells' text sits. */
struct ListColumn
{
    std::string  title;                               ///< the header's text, UTF-8
    int          width{0};                            ///< a fixed width, in pixels; 0 for a share of what is left
    float        weight{1.0f};                        ///< the share of what is left, for a column of no fixed width
    gfx::Justify justify{gfx::Justify::kCentredLeft}; ///< where the cells' text sits in each cell
};

/** What a ListView shows, supplied by its owner: how many rows, and each row's cells as text in a colour, or the whole
    row painted by the owner. The view asks only for the rows it paints, so a model of tens of thousands of rows costs
    what the rows on the screen cost. Read on the message thread. */
class ListModel
{
  public:
    virtual ~ListModel() = default;

    /** How many rows there are.
        @return  the number of rows */
    virtual std::size_t RowCount() const = 0;

    /** The text of a cell, for the view's own cell painting and for a copy of the selection.
        @param row     the row, from 0
        @param column  the column, from 0; 0 for a list with no columns
        @return        the cell's text, UTF-8; empty for none */
    virtual std::string CellText(std::size_t row, std::size_t column) const
    {
        (void)row;
        (void)column;
        return {};
    }

    /** The colour a cell's text is drawn in - a score's, a loss in red - or the style's.
        @param row     the row, from 0
        @param column  the column, from 0
        @return        the colour; empty for the style's (its text, or its selected text on a selected row) */
    virtual std::optional<gfx::Color> CellColour(std::size_t row, std::size_t column) const
    {
        (void)row;
        (void)column;
        return std::nullopt;
    }

    /** A fill behind a cell - the move a game stands at - over the row's stripe and selection.
        @param row     the row, from 0
        @param column  the column, from 0
        @return        the fill; empty for none */
    virtual std::optional<gfx::Color> CellBackground(std::size_t row, std::size_t column) const
    {
        (void)row;
        (void)column;
        return std::nullopt;
    }

    /** The whole row painted by the model, over the row's stripe and selection fill, in place of the cells.
        @param c         the canvas, in the list's coordinates, clipped to the rows' area
        @param row       the row, from 0
        @param bounds    the row's rectangle, in the list's coordinates
        @param selected  whether the row is selected
        @return          true when the model painted the row; false for the view to paint its cells */
    virtual bool PaintRow(gfx::Canvas &c, std::size_t row, gfx::RectI bounds, bool selected)
    {
        (void)c;
        (void)row;
        (void)bounds;
        (void)selected;
        return false;
    }
};

/** A set of rows, as sorted ranges that neither overlap nor touch, so a selection of every row of a long list is one
    range. */
class RowSelection
{
  public:
    /** The rows [first, last). */
    struct Range
    {
        std::size_t first{0}; ///< the first row
        std::size_t last{0};  ///< one past the last row

        /** Whether two ranges are the same rows.
            @param a  one range
            @param b  the other
            @return   true when both ends agree */
        friend bool operator==(const Range &a, const Range &b) noexcept = default;
    };

    /** Whether a row is in the set.
        @param row  the row
        @return     true when it is */
    bool Contains(std::size_t row) const noexcept
    {
        const auto it = std::upper_bound(ranges_.begin(), ranges_.end(), row,
                                         [](std::size_t r, const Range &range) { return r < range.first; });
        return it != ranges_.begin() && row < std::prev(it)->last;
    }

    /** Whether no row is in the set.
        @return  true when it is empty */
    bool empty() const noexcept
    {
        return ranges_.empty();
    }

    /** How many rows are in the set.
        @return  the count */
    std::size_t Count() const noexcept
    {
        std::size_t count = 0;
        for (const Range &range : ranges_)
        {
            count += range.last - range.first;
        }
        return count;
    }

    /** The set's ranges, sorted, none overlapping or touching another.
        @return  the ranges */
    const std::vector<Range> &ranges() const noexcept
    {
        return ranges_;
    }

    /** The first row in the set.
        @return  the row; empty for an empty set */
    std::optional<std::size_t> First() const noexcept
    {
        return ranges_.empty() ? std::nullopt : std::optional<std::size_t>(ranges_.front().first);
    }

    /** Nothing in the set. */
    void Clear() noexcept
    {
        ranges_.clear();
    }

    /** The rows [first, last) and nothing else; empty when `last` is not past `first`.
        @param first  the first row
        @param last   one past the last row */
    void Set(std::size_t first, std::size_t last)
    {
        ranges_.clear();
        Add(first, last);
    }

    /** The rows [first, last) added to the set, merged with any they meet or touch.
        @param first  the first row
        @param last   one past the last row; nothing is added unless it is past `first` */
    void Add(std::size_t first, std::size_t last)
    {
        if (last <= first)
        {
            return;
        }
        std::vector<Range> merged;
        merged.reserve(ranges_.size() + 1);
        Range incoming{first, last};
        bool  placed = false;
        for (const Range &range : ranges_)
        {
            if (range.last < incoming.first)
            {
                merged.push_back(range);
            }
            else if (incoming.last < range.first)
            {
                if (!placed)
                {
                    merged.push_back(incoming);
                    placed = true;
                }
                merged.push_back(range);
            }
            else
            {
                incoming = {std::min(incoming.first, range.first), std::max(incoming.last, range.last)};
            }
        }
        if (!placed)
        {
            merged.push_back(incoming);
        }
        ranges_ = std::move(merged);
    }

    /** The rows [first, last) taken out of the set, splitting a range they fall inside.
        @param first  the first row
        @param last   one past the last row */
    void Remove(std::size_t first, std::size_t last)
    {
        if (last <= first)
        {
            return;
        }
        std::vector<Range> kept;
        kept.reserve(ranges_.size() + 1);
        for (const Range &range : ranges_)
        {
            if (range.last <= first || last <= range.first)
            {
                kept.push_back(range);
                continue;
            }
            if (range.first < first)
            {
                kept.push_back({range.first, first});
            }
            if (last < range.last)
            {
                kept.push_back({last, range.last});
            }
        }
        ranges_ = std::move(kept);
    }

    /** A row put in the set when it is not in it, and taken out when it is.
        @param row  the row */
    void Toggle(std::size_t row)
    {
        if (Contains(row))
        {
            Remove(row, row + 1);
        }
        else
        {
            Add(row, row + 1);
        }
    }

    /** Every row at or past `count` taken out: what is left when the list shrinks to `count` rows.
        @param count  the number of rows the list has */
    void ClampTo(std::size_t count)
    {
        Remove(count, static_cast<std::size_t>(-1));
    }

    /** Whether two sets hold the same rows.
        @param a  one set
        @param b  the other
        @return   true when they do */
    friend bool operator==(const RowSelection &a, const RowSelection &b) noexcept
    {
        return a.ranges_ == b.ranges_;
    }

  private:
    std::vector<Range> ranges_; ///< sorted, none overlapping or touching another
};

/**
    A list of many rows of one height, scrolled - an engine's log, a game's moves, an engine's analysis lines, a
    tournament's crosstable. Virtualised: a paint asks the model (ListModel) for the rows the screen shows and no
    others, so tens of thousands of rows cost what the visible ones cost.

    The rows are a model's, as text in cells or painted whole; optional columns, each a fixed width or a share of what
    the fixed ones leave, carry a header row across the top. A selection - none, one row, or any rows in ranges - is
    made by the mouse (a press selects, Shift extends from the anchor, Command toggles, a drag extends) and by the keys
    (Up and Down, Page Up and Page Down, Home and End, Shift extending; Command-A selects every row; Return activates
   the current row; Command-C copies the selection, its cells parted by tabs and its rows by line breaks, to `on_copy`).
   A click, a double click, a right click and a press on a header each have a callback.

    The wheel and the series' ScrollBar beside the rows scroll it, three rows a notch; ScrollToRow brings a row into
    view. With `stick_to_bottom` set, a list scrolled to its bottom stays there as rows arrive (RowsChanged) or the view
    is resized - an engine's log following its output - and stays where it is when the person has scrolled up.
*/
class ListView final : public View
{
  public:
    /** What the list lets the person select. */
    enum class SelectionMode
    {
        kNone,     ///< nothing: the list is read and scrolled
        kSingle,   ///< one row at a time
        kMultiple, ///< any rows, in ranges
    };

    /** What a list is dressed in. */
    struct Style
    {
        gfx::Font  font{14.0f};                          ///< the cells' font
        gfx::Font  header_font{gfx::Font(14.0f).Bold()}; ///< the header's font
        int        row_height{20};                       ///< each row's height, in pixels, at least 1
        int        header_height{22};    ///< the header row's height, in pixels; 0 for none even with columns
        int        padding{4};           ///< the room inside each cell at its sides, in pixels
        int        scroll_bar_width{10}; ///< the scroll bar's thickness, as the shell's, in pixels
        gfx::Color background{gfx::Color::FromRgb(20, 20, 20)};        ///< behind every row
        gfx::Color stripe{};                                           ///< over every other row; transparent for none
        gfx::Color text{gfx::Color::FromRgb(240, 240, 240)};           ///< the cells' text
        gfx::Color selection{gfx::Color::FromArgb(110, 200, 160, 60)}; ///< behind a selected row
        gfx::Color selected_text{gfx::Color::FromRgb(255, 255, 255)};  ///< a selected row's text
        gfx::Color header_background{gfx::Color::FromRgb(36, 36, 36)}; ///< behind the header row
        gfx::Color header_text{gfx::Color::FromRgb(200, 200, 200)};    ///< the header's text
        gfx::Color grid{gfx::Color::FromArgb(60, 255, 255, 255)};      ///< the lines between columns; transparent: none
        gfx::Color focused_outline{}; ///< round the list while it has the keyboard; transparent for none
    };

    /** An empty list with no model, selecting one row at a time.
        @param style  the list's dress */
    explicit ListView(Style style) : style_(std::move(style)), bar_(true)
    {
        style_.row_height = std::max(1, style_.row_height);
        bar_.SetSingleStepSize(static_cast<double>(style_.row_height) * 1.5); // three rows a 20-pixel notch
        bar_.on_move = [this](double start) { SetScroll(gfx::RoundToInt(start)); };
        bar_.SetShown(true); // hidden until a range hides some of the content
        AddChild(bar_);
    }

    ListView(const ListView &)            = delete;
    ListView &operator=(const ListView &) = delete;

    /** Told with the row and the column of a click: a press and a release on the same row with the primary button. */
    std::function<void(std::size_t row, std::size_t column)> on_click;
    /** Told with the row and the column of a double click on a row. */
    std::function<void(std::size_t row, std::size_t column)> on_double_click;
    /** Told with the current row when Return is pressed on it. */
    std::function<void(std::size_t row)> on_activate;
    /** Told with the row under a right click (empty below the last row) and where it landed, in the list's
        coordinates - for a context menu; the row is selected first unless it already is. */
    std::function<void(std::optional<std::size_t> row, gfx::PointI at)> on_context_click;
    /** Told with the column whose header was pressed and released. */
    std::function<void(std::size_t column)> on_header_click;
    /** Told whenever the selection changes. */
    std::function<void()> on_selection_changed;
    /** Told with the selection's text on Command-C (CopySelection): its rows' cells parted by tabs, its rows by line
        breaks - for the owner to put on the clipboard. */
    std::function<void(std::string text)> on_copy;

    /** Whether a list scrolled to its bottom stays there as rows arrive and as it is resized. */
    bool stick_to_bottom{false};

    /** The list's dress.
        @return  the style the list was made with */
    const Style &style() const noexcept
    {
        return style_;
    }

    /** The model the list shows, or none; the list asks it for its rows from here on.
        @param model  the model, owned by the caller and outliving its use here; null for none */
    void SetModel(ListModel *model)
    {
        model_     = model;
        row_count_ = 0;
        selection_.Clear();
        current_.reset();
        anchor_.reset();
        scroll_ = 0;
        pinned_ = true;
        RowsChanged();
    }

    /** The model the list shows.
        @return  the model, not owned; null for none */
    ListModel *model() const noexcept
    {
        return model_;
    }

    /** The columns, and with them the header row; none for one column across the list and no header.
        @param columns  the columns, left to right */
    void SetColumns(std::vector<ListColumn> columns)
    {
        columns_ = std::move(columns);
        LayOut();
        Repaint();
    }

    /** The columns.
        @return  the columns, left to right */
    const std::vector<ListColumn> &columns() const noexcept
    {
        return columns_;
    }

    /** How many columns the cells are in: at least 1.
        @return  the number of columns */
    std::size_t ColumnCount() const noexcept
    {
        return std::max<std::size_t>(1, columns_.size());
    }

    /** How the person may select rows: a change clears the selection.
        @param mode  none, one row, or any rows */
    void SetSelectionMode(SelectionMode mode)
    {
        if (mode != mode_)
        {
            mode_ = mode;
            ClearSelection();
        }
    }

    /** How the person may select rows.
        @return  the mode */
    SelectionMode selection_mode() const noexcept
    {
        return mode_;
    }

    /** The model's rows have changed - rows added, removed or rewritten: the count read again, the selection held to
        it, the scroll range set, the list kept at its bottom when it was there and `stick_to_bottom` is set, and the
        rows painted again. */
    void RowsChanged()
    {
        const std::size_t count = model_ != nullptr ? model_->RowCount() : 0;
        if (count < row_count_)
        {
            const RowSelection before = selection_;
            selection_.ClampTo(count);
            if (current_ && *current_ >= count)
            {
                current_ = count > 0 ? std::optional<std::size_t>(count - 1) : std::nullopt;
            }
            if (anchor_ && *anchor_ >= count)
            {
                anchor_ = current_;
            }
            if (!(before == selection_))
            {
                SelectionChanged();
            }
        }
        row_count_ = count;
        LayOut();
        Repaint();
    }

    /** How many rows the list holds, as last read from the model (RowsChanged).
        @return  the number of rows */
    std::size_t row_count() const noexcept
    {
        return row_count_;
    }

    // ---- Geometry -------------------------------------------------------------------------------------------------

    /** The header row's height.
        @return  the height, in pixels; 0 when the list has no columns or the style no header */
    int HeaderHeight() const noexcept
    {
        return !columns_.empty() ? std::max(0, style_.header_height) : 0;
    }

    /** Where the rows are drawn: under the header and left of the scroll bar while it shows.
        @return  the area, in the list's coordinates */
    gfx::RectI RowsArea() const noexcept
    {
        const int header = std::min(HeaderHeight(), height());
        const int bar    = bar_.visible() ? style_.scroll_bar_width : 0;
        return {0, header, std::max(0, width() - bar), height() - header};
    }

    /** Each column's cell across the rows' width, as a header cell.
        @return  the columns' rectangles in the header row, in the list's coordinates; one for a list with no columns */
    std::vector<gfx::RectI> ColumnBounds() const
    {
        const gfx::RectI area  = RowsArea();
        const int        total = area.width();
        std::vector<int> widths(ColumnCount(), 0);
        if (columns_.empty())
        {
            widths[0] = total;
        }
        else
        {
            int   fixed  = 0;
            float shares = 0.0f;
            for (const ListColumn &column : columns_)
            {
                fixed += std::max(0, column.width);
                shares += column.width > 0 ? 0.0f : std::max(0.0f, column.weight);
            }
            const int left_over = std::max(0, total - fixed);
            int       given     = 0;
            int       last_flex = -1;
            for (std::size_t i = 0; i < columns_.size(); ++i)
            {
                if (columns_[i].width > 0)
                {
                    widths[i] = columns_[i].width;
                }
                else if (shares > 0.0f)
                {
                    widths[i] =
                        gfx::RoundToInt(static_cast<float>(left_over) * std::max(0.0f, columns_[i].weight) / shares);
                    given += widths[i];
                    last_flex = static_cast<int>(i);
                }
            }
            if (last_flex >= 0)
            {
                widths[static_cast<std::size_t>(last_flex)] += left_over - given; // the rounding's pixels to the last
            }
        }
        std::vector<gfx::RectI> bounds;
        bounds.reserve(widths.size());
        int x = 0;
        for (int w : widths)
        {
            bounds.push_back({x, 0, std::max(0, w), HeaderHeight()});
            x += std::max(0, w);
        }
        return bounds;
    }

    /** The column at `x`.
        @param x  a position across the list, in its coordinates, in pixels
        @return   the column; empty past the last column's right edge or left of the first */
    std::optional<std::size_t> ColumnAt(int x) const
    {
        const std::vector<gfx::RectI> bounds = ColumnBounds();
        for (std::size_t i = 0; i < bounds.size(); ++i)
        {
            if (x >= bounds[i].x() && x < bounds[i].right())
            {
                return i;
            }
        }
        return std::nullopt;
    }

    /** The row under `point`.
        @param point  the point, in the list's coordinates, in pixels
        @return       the row; empty outside the rows' area or below the last row */
    std::optional<std::size_t> RowAt(gfx::PointI point) const
    {
        const gfx::RectI area = RowsArea();
        if (!area.Contains(point))
        {
            return std::nullopt;
        }
        const auto row = static_cast<std::size_t>((point.y - area.y() + scroll_) / style_.row_height);
        return row < row_count_ ? std::optional<std::size_t>(row) : std::nullopt;
    }

    /** A row's rectangle where it is drawn now, scrolled - above or below the rows' area for a row out of view.
        @param row  the row
        @return     its rectangle, in the list's coordinates */
    gfx::RectI RowBounds(std::size_t row) const
    {
        const gfx::RectI area = RowsArea();
        const long long  top  = static_cast<long long>(row) * style_.row_height - scroll_ + area.y();
        return {0, static_cast<int>(std::clamp<long long>(top, -(1LL << 30), 1LL << 30)), area.width(),
                style_.row_height};
    }

    /** The rows any of which shows in the rows' area: those a paint asks the model for.
        @return  [first, last) */
    std::pair<std::size_t, std::size_t> VisibleRows() const
    {
        const gfx::RectI area = RowsArea();
        if (area.IsEmpty() || row_count_ == 0)
        {
            return {0, 0};
        }
        const auto first = static_cast<std::size_t>(scroll_ / style_.row_height);
        const auto last =
            static_cast<std::size_t>((scroll_ + area.height() + style_.row_height - 1) / style_.row_height);
        return {std::min(first, row_count_), std::min(last, row_count_)};
    }

    // ---- Scrolling ------------------------------------------------------------------------------------------------

    /** How far the rows are scrolled.
        @return  the distance of the rows' area's top below the first row's top, in pixels */
    int scroll() const noexcept
    {
        return scroll_;
    }

    /** How far the rows can be scrolled: the last row's bottom at the rows' area's bottom.
        @return  the distance, in pixels, never negative */
    int MaxScroll() const noexcept
    {
        const long long content = static_cast<long long>(row_count_) * style_.row_height;
        return static_cast<int>(std::clamp<long long>(content - RowsArea().height(), 0, 1LL << 30));
    }

    /** Whether the list is scrolled to its bottom - every row showing counting as at it.
        @return  true when it is */
    bool AtBottom() const noexcept
    {
        return scroll_ >= MaxScroll();
    }

    /** Scroll the rows, held between the top and the bottom.
        @param pixels  the distance of the rows' area's top below the first row's top, in pixels */
    void SetScroll(int pixels)
    {
        pixels  = std::clamp(pixels, 0, MaxScroll());
        pinned_ = pixels >= MaxScroll();
        if (pixels != scroll_)
        {
            scroll_ = pixels;
            bar_.SetCurrentRange(static_cast<double>(scroll_), static_cast<double>(RowsArea().height()));
            Repaint();
        }
    }

    /** Scroll as little as brings a row wholly into view.
        @param row  the row; one past the last is taken as the last */
    void ScrollToRow(std::size_t row)
    {
        if (row_count_ == 0)
        {
            return;
        }
        row                    = std::min(row, row_count_ - 1);
        const long long top    = static_cast<long long>(row) * style_.row_height;
        const long long bottom = top + style_.row_height;
        const int       shown  = RowsArea().height();
        if (top < scroll_)
        {
            SetScroll(static_cast<int>(top));
        }
        else if (bottom > static_cast<long long>(scroll_) + shown)
        {
            SetScroll(static_cast<int>(bottom - shown));
        }
    }

    /** Scroll to the last row. */
    void ScrollToBottom()
    {
        SetScroll(MaxScroll());
    }

    /** The scroll bar beside the rows, for a window that watches it for its tooltips or steps its page repeat.
        @return  the bar */
    ScrollBar &scroll_bar() noexcept
    {
        return bar_;
    }

    /** Once a display frame, the window's steady clock: a press held before or after the scroll bar's thumb pages on.
        @param now  the window's steady clock, in seconds */
    void Step(double now)
    {
        bar_.Step(now);
    }

    // ---- Selection ------------------------------------------------------------------------------------------------

    /** The selected rows.
        @return  the selection */
    const RowSelection &selection() const noexcept
    {
        return selection_;
    }

    /** Whether a row is selected.
        @param row  the row
        @return     true when it is */
    bool IsSelected(std::size_t row) const noexcept
    {
        return selection_.Contains(row);
    }

    /** The row the keys move from - the last pressed or moved to.
        @return  the row; empty before any */
    std::optional<std::size_t> current_row() const noexcept
    {
        return current_;
    }

    /** One row selected, and current, and brought into view; nothing while the mode selects nothing.
        @param row  the row; nothing past the last */
    void Select(std::size_t row)
    {
        if (mode_ == SelectionMode::kNone || row >= row_count_)
        {
            return;
        }
        anchor_ = row;
        MoveTo(row, false);
    }

    /** The rows from `anchor` to `current` selected - either may be the larger - with `current` the current row and
        brought into view; one row while the mode selects one, nothing while it selects none.
        @param anchor   the row the range grows from
        @param current  the row it reaches */
    void SelectRange(std::size_t anchor, std::size_t current)
    {
        if (mode_ != SelectionMode::kMultiple)
        {
            Select(current);
            return;
        }
        if (row_count_ == 0)
        {
            return;
        }
        anchor_ = std::min(anchor, row_count_ - 1);
        MoveTo(std::min(current, row_count_ - 1), true);
    }

    /** Every row selected, while the mode selects any rows. */
    void SelectAll()
    {
        if (mode_ != SelectionMode::kMultiple || row_count_ == 0)
        {
            return;
        }
        const RowSelection before = selection_;
        selection_.Set(0, row_count_);
        if (!(before == selection_))
        {
            SelectionChanged();
        }
    }

    /** No row selected and none current. */
    void ClearSelection()
    {
        current_.reset();
        anchor_.reset();
        if (!selection_.empty())
        {
            selection_.Clear();
            SelectionChanged();
        }
    }

    /** The selected rows' text: each row's cells parted by tabs, the rows by line breaks, top to bottom.
        @return  the text, UTF-8; empty for no selection or no model */
    std::string SelectedText() const
    {
        std::string text;
        if (model_ == nullptr)
        {
            return text;
        }
        bool first_row = true;
        for (const RowSelection::Range &range : selection_.ranges())
        {
            for (std::size_t row = range.first; row < range.last && row < row_count_; ++row)
            {
                if (!first_row)
                {
                    text.push_back('\n');
                }
                first_row = false;
                for (std::size_t column = 0; column < ColumnCount(); ++column)
                {
                    if (column > 0)
                    {
                        text.push_back('\t');
                    }
                    text += model_->CellText(row, column);
                }
            }
        }
        return text;
    }

    /** The selection's text (SelectedText) to `on_copy`, when anything is selected. */
    void CopySelection()
    {
        if (on_copy && !selection_.empty())
        {
            const auto copy = on_copy; // a copy: the owner may replace it as it hears
            copy(SelectedText());
        }
    }

    // ---- View -----------------------------------------------------------------------------------------------------

    /** A list that selects takes the keyboard on a press.
        @return  true unless the mode selects nothing */
    bool WantsKeyboardFocus() const override
    {
        return mode_ != SelectionMode::kNone;
    }

    void FocusGained() override
    {
        focused_ = true;
        Repaint();
    }

    void FocusLost() override
    {
        focused_ = false;
        Repaint();
    }

    void Resized() override
    {
        LayOut();
    }

    void Paint(gfx::Canvas &c) override
    {
        c.SetColor(style_.background);
        c.FillRect(LocalBounds());
        const std::vector<gfx::RectI> columns = ColumnBounds();
        PaintHeader(c, columns);
        const gfx::RectI area = RowsArea();
        gfx::ScopedState state(c);
        if (!c.ClipToRect(area) || model_ == nullptr)
        {
            return;
        }
        const gfx::RectI clip = c.ClipBounds();
        auto [first, last]    = VisibleRows();
        // Of the rows that show, only those the clip reaches: a repaint of one row asks for one.
        const long long clip_first = (static_cast<long long>(clip.y()) - area.y() + scroll_) / style_.row_height;
        const long long clip_last =
            (static_cast<long long>(clip.bottom()) - area.y() + scroll_ + style_.row_height - 1) / style_.row_height;
        first = std::max(first, static_cast<std::size_t>(std::max(0LL, clip_first)));
        last  = std::min(last, static_cast<std::size_t>(std::max(0LL, clip_last)));
        for (std::size_t row = first; row < last; ++row)
        {
            PaintOneRow(c, row, RowBounds(row), columns);
        }
    }

    void PaintOverChildren(gfx::Canvas &c) override
    {
        const Host *h = host();
        if (focused_ && style_.focused_outline.alpha() > 0 && (h == nullptr || h->WindowHasKeyboardFocus()))
        {
            c.SetColor(style_.focused_outline);
            c.DrawRect(LocalBounds().ToFloat(), 1.0f);
        }
    }

    void MouseDown(const MouseEvent &e) override
    {
        const gfx::PointI at = e.position.ToNearestInt();
        pressed_header_.reset();
        pressed_row_.reset();
        drag_selecting_ = false;
        if (at.y < HeaderHeight())
        {
            pressed_header_ = ColumnAt(at.x);
            return;
        }
        const std::optional<std::size_t> row = RowAt(at);
        if (e.right)
        {
            if (row && !IsSelected(*row))
            {
                Select(*row);
            }
            if (on_context_click)
            {
                const auto context = on_context_click;
                context(row, at);
            }
            return;
        }
        pressed_row_ = row;
        if (!row || mode_ == SelectionMode::kNone)
        {
            return;
        }
        if (mode_ == SelectionMode::kMultiple && e.modifiers.shift && anchor_)
        {
            MoveTo(*row, true);
        }
        else if (mode_ == SelectionMode::kMultiple && e.modifiers.command)
        {
            const RowSelection before = selection_;
            selection_.Toggle(*row);
            anchor_  = *row;
            current_ = *row;
            if (!(before == selection_))
            {
                SelectionChanged();
            }
            return; // a toggle is no drag
        }
        else
        {
            Select(*row);
        }
        drag_selecting_ = true;
    }

    void MouseDrag(const MouseEvent &e) override
    {
        if (!drag_selecting_ || row_count_ == 0)
        {
            return;
        }
        const gfx::RectI area = RowsArea();
        const long long  y    = static_cast<long long>(gfx::RoundToInt(e.position.y)) - area.y() + scroll_;
        const auto       row  = static_cast<std::size_t>(
            std::clamp<long long>(y / style_.row_height, 0, static_cast<long long>(row_count_) - 1));
        if (mode_ == SelectionMode::kMultiple && anchor_)
        {
            MoveTo(row, true);
        }
        else
        {
            Select(row);
        }
    }

    void MouseUp(const MouseEvent &e) override
    {
        const gfx::PointI at = e.position.ToNearestInt();
        if (pressed_header_)
        {
            const std::optional<std::size_t> column = at.y < HeaderHeight() ? ColumnAt(at.x) : std::nullopt;
            if (column == pressed_header_ && on_header_click)
            {
                const auto header = on_header_click;
                header(*column);
            }
        }
        else if (pressed_row_ && !e.right)
        {
            const std::optional<std::size_t> row    = RowAt(at);
            const std::optional<std::size_t> column = ColumnAt(at.x);
            if (row == pressed_row_ && column && on_click)
            {
                const auto click = on_click;
                click(*row, *column);
            }
        }
        pressed_header_.reset();
        pressed_row_.reset();
        drag_selecting_ = false;
    }

    void MouseDoubleClick(const MouseEvent &e) override
    {
        const gfx::PointI                at     = e.position.ToNearestInt();
        const std::optional<std::size_t> row    = RowAt(at);
        const std::optional<std::size_t> column = ColumnAt(at.x);
        if (row && column && !e.right && on_double_click)
        {
            const auto double_click = on_double_click;
            double_click(*row, *column);
        }
    }

    /** The wheel scrolls the rows while some are hidden, and is handed up otherwise.
        @param e  the wheel's travel
        @return   true when the list scrolled, or could have */
    bool MouseWheel(const MouseEvent &e) override
    {
        if (MaxScroll() <= 0 || e.wheel_y == 0.0f)
        {
            return false;
        }
        MouseEvent along = e;
        along.wheel_x    = 0.0f;
        return bar_.MouseWheel(along);
    }

    bool KeyPressed(const KeyEvent &key) override
    {
        if (mode_ == SelectionMode::kNone)
        {
            return false;
        }
        const bool extend = key.modifiers.shift && mode_ == SelectionMode::kMultiple;
        if (key.key == Key::kCharacter)
        {
            if (!key.modifiers.command)
            {
                return false;
            }
            if (key.character == U'a' || key.character == U'A')
            {
                SelectAll();
                return mode_ == SelectionMode::kMultiple;
            }
            if (key.character == U'c' || key.character == U'C')
            {
                CopySelection();
                return true;
            }
            return false;
        }
        if (key.key == Key::kReturn)
        {
            if (current_ && on_activate)
            {
                const auto activate = on_activate;
                activate(*current_);
            }
            return current_.has_value();
        }
        if (row_count_ == 0)
        {
            return key.key == Key::kUp || key.key == Key::kDown || key.key == Key::kPageUp ||
                   key.key == Key::kPageDown || key.key == Key::kHome || key.key == Key::kEnd;
        }
        const std::size_t last = row_count_ - 1;
        const std::size_t page = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::max(0, RowsArea().height() / style_.row_height)) - 1);
        const bool        none = !current_;
        const std::size_t from = current_.value_or(0);
        std::size_t       to   = from;
        switch (key.key)
        {
        case Key::kUp:
            to = none ? 0 : (from > 0 ? from - 1 : 0);
            break;
        case Key::kDown:
            to = none ? 0 : std::min(last, from + 1);
            break;
        case Key::kPageUp:
            to = from > page ? from - page : 0;
            break;
        case Key::kPageDown:
            to = std::min(last, from + page);
            break;
        case Key::kHome:
            to = 0;
            break;
        case Key::kEnd:
            to = last;
            break;
        default:
            return false;
        }
        if (extend && anchor_)
        {
            MoveTo(to, true);
        }
        else
        {
            Select(to);
        }
        return true;
    }

  private:
    /** The current row moved to `row`, brought into view, and the selection that row alone - or, `extending`, the
        rows from the anchor to it.
        @param row        the row, inside the list
        @param extending  true to select from the anchor to the row */
    void MoveTo(std::size_t row, bool extending)
    {
        const RowSelection before = selection_;
        current_                  = row;
        if (extending && anchor_)
        {
            selection_.Set(std::min(*anchor_, row), std::max(*anchor_, row) + 1);
        }
        else
        {
            anchor_ = row;
            selection_.Set(row, row + 1);
        }
        ScrollToRow(row);
        if (!(before == selection_))
        {
            SelectionChanged();
        }
    }

    /** The selection painted again and its listener told. */
    void SelectionChanged()
    {
        Repaint();
        if (on_selection_changed)
        {
            const auto changed = on_selection_changed;
            changed();
        }
    }

    /** The scroll bar placed and ranged, the scroll held to the range, and the list kept at its bottom when it was
        there and sticks to it. */
    void LayOut()
    {
        const int header = std::min(HeaderHeight(), height());
        const int shown  = std::max(0, height() - header);
        bar_.SetBounds({std::max(0, width() - style_.scroll_bar_width), header, style_.scroll_bar_width, shown});
        bar_.SetRangeLimits(0.0, static_cast<double>(row_count_) * style_.row_height);
        const bool keep_bottom = stick_to_bottom && pinned_;
        scroll_                = std::clamp(scroll_, 0, MaxScroll());
        if (keep_bottom)
        {
            scroll_ = MaxScroll();
        }
        pinned_ = scroll_ >= MaxScroll();
        bar_.SetCurrentRange(static_cast<double>(scroll_), static_cast<double>(shown));
    }

    /** The header row: its fill, each column's title and the lines between the columns.
        @param c        the canvas, in the list's coordinates
        @param columns  the columns' rectangles in the header row */
    void PaintHeader(gfx::Canvas &c, const std::vector<gfx::RectI> &columns)
    {
        const int header = HeaderHeight();
        if (header <= 0)
        {
            return;
        }
        c.SetColor(style_.header_background);
        c.FillRect(gfx::RectI{0, 0, width(), header});
        c.SetFont(style_.header_font);
        for (std::size_t i = 0; i < columns_.size() && i < columns.size(); ++i)
        {
            const gfx::RectI cell = columns[i];
            c.SetColor(style_.header_text);
            c.DrawText(columns_[i].title, cell.Reduced(style_.padding, 0).ToFloat(), columns_[i].justify, true);
            if (style_.grid.alpha() > 0 && i + 1 < columns_.size())
            {
                c.SetColor(style_.grid);
                c.FillRect(gfx::RectI{cell.right() - 1, 0, 1, height()});
            }
        }
        if (style_.grid.alpha() > 0)
        {
            c.SetColor(style_.grid);
            c.FillRect(gfx::RectI{0, header - 1, width(), 1});
        }
    }

    /** One row: its stripe, its selection, then the model's painting or its cells.
        @param c        the canvas, in the list's coordinates, clipped to the rows' area
        @param row      the row
        @param bounds   the row's rectangle
        @param columns  the columns' rectangles in the header row, their x and width the cells' */
    void PaintOneRow(gfx::Canvas &c, std::size_t row, gfx::RectI bounds, const std::vector<gfx::RectI> &columns)
    {
        const bool selected = selection_.Contains(row);
        if (style_.stripe.alpha() > 0 && row % 2 == 1)
        {
            c.SetColor(style_.stripe);
            c.FillRect(bounds);
        }
        if (selected)
        {
            c.SetColor(style_.selection);
            c.FillRect(bounds);
        }
        if (model_->PaintRow(c, row, bounds, selected))
        {
            return;
        }
        c.SetFont(style_.font);
        for (std::size_t column = 0; column < columns.size(); ++column)
        {
            const gfx::RectI cell{columns[column].x(), bounds.y(), columns[column].width(), bounds.height()};
            if (const std::optional<gfx::Color> fill = model_->CellBackground(row, column))
            {
                c.SetColor(*fill);
                c.FillRect(cell);
            }
            const std::string text = model_->CellText(row, column);
            if (text.empty())
            {
                continue;
            }
            const gfx::Color colour =
                model_->CellColour(row, column).value_or(selected ? style_.selected_text : style_.text);
            const gfx::Justify justify =
                column < columns_.size() ? columns_[column].justify : gfx::Justify::kCentredLeft;
            c.SetColor(colour);
            c.DrawText(text, cell.Reduced(style_.padding, 0).ToFloat(), justify, true);
        }
    }

    Style                      style_;                        ///< the list's dress
    ScrollBar                  bar_;                          ///< the scroll bar beside the rows
    ListModel                 *model_{nullptr};               ///< the rows; not owned
    std::vector<ListColumn>    columns_;                      ///< the columns; none for one across the list
    SelectionMode              mode_{SelectionMode::kSingle}; ///< what the person may select
    RowSelection               selection_;                    ///< the selected rows
    std::optional<std::size_t> current_;                      ///< the row the keys move from
    std::optional<std::size_t> anchor_;                       ///< the row a range grows from
    std::size_t                row_count_{0};                 ///< the model's rows, as last read
    int                        scroll_{0};                    ///< the rows' area's top below the first row's
    bool                       pinned_{true};                 ///< scrolled to the bottom when last moved
    bool                       focused_{false};               ///< the router has given the list the keyboard
    bool                       drag_selecting_{false};        ///< a press on a row is extending the selection
    std::optional<std::size_t> pressed_row_;                  ///< the row a press began on
    std::optional<std::size_t> pressed_header_;               ///< the header column a press began on
};
} // namespace reckless::view
