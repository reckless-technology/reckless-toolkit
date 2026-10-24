#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"
#include "ScrollBar.h"
#include "Text.h"
#include "TextField.h"
#include "View.h"

namespace reckless::view
{
/** The byte offset of the code point after the one at `at` in well-formed UTF-8: never inside a character.
    @param text  well-formed UTF-8
    @param at    a byte offset at a character's start, or the end
    @return      the next character's start; the end at the end */
inline std::size_t NextCodePointStart(std::string_view text, std::size_t at) noexcept
{
    if (at >= text.size())
    {
        return text.size();
    }
    const int step = gfx::detail::Utf8Step(text, at);
    return std::min(text.size(), at + static_cast<std::size_t>(step > 0 ? step : -step));
}

/** The byte offset of the code point before `at` in well-formed UTF-8: back over its continuation bytes.
    @param text  well-formed UTF-8
    @param at    a byte offset at a character's start, or the end
    @return      the previous character's start; 0 at the start */
inline std::size_t PreviousCodePointStart(std::string_view text, std::size_t at) noexcept
{
    at = std::min(at, text.size());
    if (at == 0)
    {
        return 0;
    }
    --at;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0u) == 0x80u)
    {
        --at;
    }
    return at;
}

/** Text as a text view keeps it: well-formed UTF-8 (each broken part one U+FFFD, gfx::RepairedUtf8) with every line
    break one line feed - a carriage return and line feed, or a carriage return alone, made one.
    @param text  the text, in any encoding and with any line breaks
    @return      the text, UTF-8, its line breaks line feeds */
inline std::string NormalisedText(std::string_view text)
{
    const std::string repaired = gfx::RepairedUtf8(text);
    std::string       out;
    out.reserve(repaired.size());
    for (std::size_t i = 0; i < repaired.size(); ++i)
    {
        if (repaired[i] == '\r')
        {
            out.push_back('\n');
            if (i + 1 < repaired.size() && repaired[i + 1] == '\n')
            {
                ++i;
            }
        }
        else
        {
            out.push_back(repaired[i]);
        }
    }
    return out;
}

/** A line of a text view as it is laid out: the bytes [start, end) of the text it shows, with no line feed - a
    paragraph's end, a wrap's point, or the text's end at `end`. */
struct TextLine
{
    std::size_t start{0};    ///< the line's first byte in the text
    std::size_t end{0};      ///< one past its last byte; a line feed or the next line's start there
    bool        soft{false}; ///< true when the line was wrapped at `end`, so the next line goes on its paragraph

    /** Whether two lines are the same bytes, broken the same way.
        @param a  one line
        @param b  the other
        @return   true when all three agree */
    friend bool operator==(const TextLine &a, const TextLine &b) noexcept = default;
};

/** `text` broken into lines: at every line feed, and - with `width` above zero - wherever the next word would pass
    `width`, the spaces before the break hanging at the end of the line before it; a word wider than `width` alone
    broken between characters, at least one a line. Never inside a character.
    @param text     well-formed UTF-8, its line breaks line feeds
    @param width    the room across, in pixels; zero or less for no wrapping
    @param measure  the width of a run of the text, in pixels, as it would be drawn
    @return         the lines, at least one */
inline std::vector<TextLine> WrapLines(std::string_view text, float width,
                                       const std::function<float(std::string_view)> &measure)
{
    std::vector<TextLine> lines;
    std::size_t           paragraph = 0;
    while (true)
    {
        const std::size_t feed = text.find('\n', paragraph);
        const std::size_t stop = feed == std::string_view::npos ? text.size() : feed;
        std::size_t       pos  = paragraph;
        if (width <= 0.0f || pos == stop)
        {
            lines.push_back({pos, stop, false});
        }
        else
        {
            while (pos < stop)
            {
                const std::size_t start    = pos;
                std::size_t       at       = pos;
                bool              has_word = false;
                bool              broke    = false;
                while (at < stop)
                {
                    const bool  space = text[at] == ' ';
                    std::size_t run   = at;
                    while (run < stop && (text[run] == ' ') == space)
                    {
                        ++run;
                    }
                    if (space)
                    {
                        at = run; // spaces hang at the end of the line
                        continue;
                    }
                    if (measure(text.substr(start, run - start)) <= width)
                    {
                        has_word = true;
                        at       = run;
                        continue;
                    }
                    if (has_word)
                    {
                        lines.push_back({start, at, true}); // the word goes to the next line
                    }
                    else
                    {
                        // A word wider than the line: as many characters as fit, at least one.
                        std::size_t cut = NextCodePointStart(text, at);
                        while (cut < run)
                        {
                            const std::size_t next = NextCodePointStart(text, cut);
                            if (measure(text.substr(start, next - start)) > width)
                            {
                                break;
                            }
                            cut = next;
                        }
                        at = cut;
                        lines.push_back({start, at, at < stop});
                    }
                    broke = true;
                    break;
                }
                if (!broke)
                {
                    lines.push_back({start, stop, false});
                    at = stop;
                }
                pos = at;
            }
        }
        if (feed == std::string_view::npos)
        {
            break;
        }
        paragraph = feed + 1;
    }
    return lines;
}

/**
    Many lines of text, scrolled: a game's score as text, a position's notation, a message - read-only by default, or
    editable. The text is UTF-8 with line feeds between its paragraphs; it is wrapped at the view's width between words
    (WrapLines), or laid out a paragraph a line, and every width is measured as the window's canvas draws it
    (Host::TextWidth), so any font - the serif, or a fixed-pitch face - lays out as it is drawn. A paint draws only the
    lines that show.

    Every place in the text is a byte offset at a character's start, never inside one. The selection runs from an
    anchor to the caret: a press puts the caret (Shift extends from the anchor), a drag extends, a double click selects
    a word; Left and Right (a word with Alt), Up and Down (at the column the caret was in), Home and End (the line's
    ends; with Command, the text's), Page Up and Page Down move it, Shift extending; Command-A selects everything and
    Command-C hands the selection to `on_copy`, for the owner to put on the clipboard. Editable, it also takes typing in
    place of the selection, Return for a line feed, Backspace and Delete (a word with Alt), Command-X (the selection to
    `on_copy`, then out) and Command-V (the clipboard's text, Host::ClipboardText), and tells `on_change` after each
    edit; the caret shows as a 2 px bar while it has the keyboard. The wheel and the series' ScrollBar scroll it, and
    every move of the caret scrolls as little as keeps it in view.
*/
class TextView final : public View
{
  public:
    /** What a text view is dressed in. */
    struct Style
    {
        gfx::Font  font{14.0f};          ///< the text's font
        int        line_height{0};       ///< each line's height, in pixels; 0 for 1.3 times the font's
        int        padding{4};           ///< the room between the edge and the text, in pixels
        int        scroll_bar_width{10}; ///< the scroll bar's thickness, as the shell's, in pixels
        bool       word_wrap{true};      ///< wrap at the view's width; false for a paragraph a line, cut at the edge
        gfx::Color text{gfx::Color::FromRgb(240, 240, 240)};             ///< the text's colour
        gfx::Color background{gfx::Color::FromRgb(20, 20, 20)};          ///< the fill behind everything
        gfx::Color outline{};                                            ///< 1 px round the view; transparent for none
        gfx::Color focused_outline{gfx::Color::FromRgb(200, 160, 60)};   ///< round it while it has the keyboard
        gfx::Color highlight{gfx::Color::FromArgb(110, 200, 160, 60)};   ///< behind the selection
        gfx::Color highlighted_text{gfx::Color::FromRgb(255, 255, 255)}; ///< the selected text's colour
        gfx::Color caret{gfx::Color::FromRgb(240, 240, 240)};            ///< the caret's bar
    };

    /** An empty, read-only view with the I-beam over it.
        @param style  the view's dress */
    explicit TextView(Style style) : style_(std::move(style)), bar_(true)
    {
        cursor = Cursor::kIBeam;
        bar_.SetSingleStepSize(static_cast<double>(LineHeight()) * 1.5); // three lines a 20-pixel notch
        bar_.on_move = [this](double start) { SetScroll(gfx::RoundToInt(start)); };
        bar_.SetShown(true); // hidden until a range hides some of the content
        AddChild(bar_);
    }

    TextView(const TextView &)            = delete;
    TextView &operator=(const TextView &) = delete;

    /** Told with the selection's text on Command-C and Command-X (Copy, Cut), for the owner to put on the clipboard. */
    std::function<void(std::string text)> on_copy;
    /** Told with the whole text after each edit the person makes (typing, a line feed, a deletion, a cut, a paste) -
       not after SetText. */
    std::function<void(const std::string &text)> on_change;

    /** The view's dress.
        @return  the style the view was made with */
    const Style &style() const noexcept
    {
        return style_;
    }

    /** The text replaced (NormalisedText), the selection emptied and the caret kept where it was - held to the text and
        to a character's start - or, if it was at the end, put at the new end. on_change is not told.
        @param text  the text, UTF-8, its line breaks any of the three */
    void SetText(std::string_view text)
    {
        const bool at_end = caret_ >= text_.size();
        text_             = NormalisedText(text);
        caret_            = at_end ? text_.size() : AtBoundary(caret_);
        anchor_           = caret_;
        preferred_x_      = -1.0f;
        Invalidate();
    }

    /** The text.
        @return  the text, UTF-8, its line breaks line feeds */
    const std::string &text() const noexcept
    {
        return text_;
    }

    /** Whether the person may edit the text.
        @param editable  true to take typing, line feeds, deletions, cuts and pastes */
    void SetEditable(bool editable)
    {
        if (editable != editable_)
        {
            editable_ = editable;
            Repaint();
        }
    }

    /** Whether the person may edit the text.
        @return  true when they may */
    bool editable() const noexcept
    {
        return editable_;
    }

    /** Wrap at the view's width, or lay a paragraph out a line.
        @param wrap  true to wrap */
    void SetWordWrap(bool wrap)
    {
        if (wrap != style_.word_wrap)
        {
            style_.word_wrap = wrap;
            Invalidate();
        }
    }

    /** The caret's place.
        @return  a byte offset into the text, at a character's start or the end */
    std::size_t caret() const noexcept
    {
        return caret_;
    }

    /** The selection as [first, last), empty when the caret stands alone.
        @return  the byte offsets of the first selected character and one past the last */
    std::pair<std::size_t, std::size_t> selection() const noexcept
    {
        return {std::min(anchor_, caret_), std::max(anchor_, caret_)};
    }

    /** The selection from `anchor` to `caret`, each held to the text and to a character's start, the caret scrolled
        into view.
        @param anchor  where the selection grows from, a byte offset
        @param caret   where the caret goes, a byte offset */
    void Select(std::size_t anchor, std::size_t caret)
    {
        anchor_      = AtBoundary(anchor);
        caret_       = AtBoundary(caret);
        preferred_x_ = -1.0f;
        ScrollToCaret();
        Repaint();
    }

    /** Everything selected, the caret at the end. */
    void SelectAll()
    {
        anchor_ = 0;
        caret_  = text_.size();
        Repaint();
    }

    /** The selected text.
        @return  the bytes of the selection, UTF-8 */
    std::string SelectedText() const
    {
        const auto [first, last] = selection();
        return text_.substr(first, last - first);
    }

    /** The selection's text to `on_copy`, when anything is selected. */
    void Copy()
    {
        const auto [first, last] = selection();
        if (on_copy && first < last)
        {
            const auto copy = on_copy; // a copy: the owner may replace it as it hears
            copy(SelectedText());
        }
    }

    /** The selection's text to `on_copy`, then taken out - while the text may be edited. */
    void Cut()
    {
        if (!editable_ || selection().first == selection().second)
        {
            return;
        }
        Copy();
        Replace({});
    }

    /** The clipboard's text in place of the selection (Host::ClipboardText), while the text may be edited. */
    void Paste()
    {
        Host *h = host();
        if (!editable_ || h == nullptr)
        {
            return;
        }
        const std::string pasted = h->ClipboardText();
        if (!pasted.empty())
        {
            Replace(pasted);
        }
    }

    /** `text` in place of the selection, the caret after it, as typing it would put it - while the text may be edited.
        @param text  the text, UTF-8, its line breaks any of the three */
    void InsertText(std::string_view text)
    {
        if (editable_)
        {
            Replace(text);
        }
    }

    // ---- Layout and scrolling -------------------------------------------------------------------------------------

    /** Each line's height.
        @return  the style's, or 1.3 times the font's height, in pixels, at least 1 */
    int LineHeight() const noexcept
    {
        return std::max(1, style_.line_height > 0 ? style_.line_height : gfx::RoundToInt(style_.font.height * 1.3f));
    }

    /** Where the lines are drawn: inside the padding and left of the scroll bar while it shows.
        @return  the area, in the view's coordinates */
    gfx::RectI TextArea()
    {
        EnsureLayout();
        return AreaFor(bar_.visible());
    }

    /** The text laid out in lines, at the view's width.
        @return  the lines, at least one */
    const std::vector<TextLine> &Lines()
    {
        EnsureLayout();
        return lines_;
    }

    /** The line a place in the text is drawn on: a place where a wrapped line breaks is the start of the next.
        @param position  a byte offset
        @return          the line's index in Lines() */
    std::size_t LineOf(std::size_t position)
    {
        EnsureLayout();
        const auto it = std::upper_bound(lines_.begin(), lines_.end(), position,
                                         [](std::size_t p, const TextLine &line) { return p < line.start; });
        return it == lines_.begin() ? 0 : static_cast<std::size_t>(std::prev(it) - lines_.begin());
    }

    /** Where a place in the text is drawn: the left of the character there, at its line's top.
        @param position  a byte offset at a character's start
        @return          the point, in the view's coordinates, scrolled */
    gfx::PointF PointOf(std::size_t position)
    {
        EnsureLayout();
        const std::size_t index = LineOf(position);
        const gfx::RectI  area  = AreaFor(bar_.visible());
        return {static_cast<float>(area.x()) + XOf(lines_[index], position),
                static_cast<float>(area.y() + static_cast<int>(index) * LineHeight() - scroll_)};
    }

    /** The place in the text nearest a point: the line under it (the first above the lines, the last below them), and
        the nearer side of the character under it on that line.
        @param point  the point, in the view's coordinates
        @return       a byte offset at a character's start */
    std::size_t PositionAt(gfx::PointF point)
    {
        EnsureLayout();
        const gfx::RectI area  = AreaFor(bar_.visible());
        const long long  y     = static_cast<long long>(gfx::RoundToInt(point.y)) - area.y() + scroll_;
        const auto       index = static_cast<std::size_t>(
            std::clamp<long long>(y < 0 ? 0 : y / LineHeight(), 0, static_cast<long long>(lines_.size()) - 1));
        return PositionOnLine(index, point.x - static_cast<float>(area.x()));
    }

    /** How far the lines are scrolled.
        @return  the distance of the text area's top below the first line's top, in pixels */
    int scroll() const noexcept
    {
        return scroll_;
    }

    /** How far the lines can be scrolled: the last line's bottom at the text area's bottom.
        @return  the distance, in pixels, never negative */
    int MaxScroll()
    {
        EnsureLayout();
        const long long content = static_cast<long long>(lines_.size()) * LineHeight();
        return static_cast<int>(std::clamp<long long>(content - AreaFor(bar_.visible()).height(), 0, 1LL << 30));
    }

    /** Scroll the lines, held between the top and the bottom.
        @param pixels  the distance of the text area's top below the first line's top, in pixels */
    void SetScroll(int pixels)
    {
        pixels = std::clamp(pixels, 0, MaxScroll());
        if (pixels != scroll_)
        {
            scroll_ = pixels;
            bar_.SetCurrentRange(static_cast<double>(scroll_), static_cast<double>(AreaFor(bar_.visible()).height()));
            Repaint();
        }
    }

    /** Scroll as little as shows the caret's line whole. */
    void ScrollToCaret()
    {
        EnsureLayout();
        const long long top    = static_cast<long long>(LineOf(caret_)) * LineHeight();
        const long long bottom = top + LineHeight();
        const int       shown  = AreaFor(bar_.visible()).height();
        if (top < scroll_)
        {
            SetScroll(static_cast<int>(top));
        }
        else if (bottom > static_cast<long long>(scroll_) + shown)
        {
            SetScroll(static_cast<int>(bottom - shown));
        }
    }

    /** The lines any of which shows in the text area: those a paint draws.
        @return  [first, last) of Lines() */
    std::pair<std::size_t, std::size_t> VisibleLines()
    {
        EnsureLayout();
        const gfx::RectI area  = AreaFor(bar_.visible());
        const int        lh    = LineHeight();
        const auto       first = static_cast<std::size_t>(scroll_ / lh);
        const auto       last  = static_cast<std::size_t>((scroll_ + std::max(0, area.height()) + lh - 1) / lh);
        return {std::min(first, lines_.size()), std::min(last, lines_.size())};
    }

    /** The scroll bar beside the lines, laid out for the text as it is, for a window that watches it for its tooltips
        or steps its page repeat.
        @return  the bar */
    ScrollBar &scroll_bar()
    {
        EnsureLayout();
        return bar_;
    }

    /** Once a display frame, the window's steady clock: a press held before or after the scroll bar's thumb pages on.
        @param now  the window's steady clock, in seconds */
    void Step(double now)
    {
        bar_.Step(now);
    }

    // ---- View -----------------------------------------------------------------------------------------------------

    bool WantsKeyboardFocus() const override
    {
        return true;
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
        Invalidate();
    }

    void Paint(gfx::Canvas &c) override
    {
        EnsureLayout();
        c.SetColor(style_.background);
        c.FillRect(LocalBounds());
        const gfx::RectI area = AreaFor(bar_.visible());
        gfx::ScopedState state(c);
        const int        right = bar_.visible() ? bar_.bounds().x() : width() - 1;
        if (!c.ClipToRect(gfx::RectI::LeftTopRightBottom(1, 1, right, height() - 1)))
        {
            return;
        }
        const auto [first, last] = VisibleLines();
        const auto [from, to]    = selection();
        const int lh             = LineHeight();
        c.SetFont(style_.font);
        for (std::size_t i = first; i < last; ++i)
        {
            const TextLine        &line  = lines_[i];
            const float            y     = static_cast<float>(area.y() + static_cast<int>(i) * lh - scroll_);
            const float            left  = static_cast<float>(area.x());
            const std::string_view shown = std::string_view(text_).substr(line.start, line.end - line.start);
            const float            run   = Measure(shown);
            // The selection's part of the line, and the line feed after it when that is selected too.
            gfx::RectF selected{};
            if (from < to && from <= line.end && to > line.start)
            {
                const std::size_t a  = std::max(from, line.start);
                const std::size_t b  = std::min(to, line.end);
                float             x0 = XOf(line, a);
                float             x1 = XOf(line, b);
                if (to > line.end && !line.soft && line.end < text_.size())
                {
                    x1 += style_.font.height * 0.3f; // the line feed, selected
                }
                if (x1 > x0)
                {
                    selected = {left + x0, y, x1 - x0, static_cast<float>(lh)};
                    c.SetColor(style_.highlight.WithMultipliedAlpha(ShowsFocus() ? 1.0f : 0.5f));
                    c.FillRect(selected);
                }
            }
            if (shown.empty())
            {
                continue;
            }
            const gfx::RectF box{left, y, run + 16.0f, static_cast<float>(lh)}; // wide enough that nothing is cut
            c.SetColor(style_.text);
            c.DrawText(shown, box, gfx::Justify::kCentredLeft, false);
            if (!selected.IsEmpty() && !(style_.highlighted_text == style_.text))
            {
                gfx::ScopedState over(c);
                if (c.ClipToRect(selected.SmallestIntegerContainer()))
                {
                    c.SetColor(style_.highlighted_text);
                    c.DrawText(shown, box, gfx::Justify::kCentredLeft, false);
                }
            }
        }
        if (editable_ && ShowsFocus())
        {
            const gfx::PointF at  = PointOf(caret_);
            const float       top = at.y + 0.5f * (static_cast<float>(lh) - style_.font.height);
            c.SetColor(style_.caret);
            c.FillRect(gfx::RectF{at.x, top, 2.0f, style_.font.height}.SmallestIntegerContainer());
        }
    }

    void PaintOverChildren(gfx::Canvas &c) override
    {
        if (ShowsFocus() && style_.focused_outline.alpha() > 0)
        {
            c.SetColor(style_.focused_outline);
            c.DrawRect(LocalBounds().ToFloat(), 1.0f);
        }
        else if (style_.outline.alpha() > 0)
        {
            c.SetColor(style_.outline);
            c.DrawRect(LocalBounds().ToFloat(), 1.0f);
        }
    }

    void MouseDown(const MouseEvent &e) override
    {
        if (e.right)
        {
            return;
        }
        MoveCaret(PositionAt(e.position), e.modifiers.shift);
    }

    void MouseDrag(const MouseEvent &e) override
    {
        if (e.right)
        {
            return;
        }
        MoveCaret(PositionAt(e.position), true);
    }

    void MouseDoubleClick(const MouseEvent &e) override
    {
        if (e.right)
        {
            return;
        }
        // The run of one class under the pointer; a press just past a word's end - on the space or the line's end after
        // it - takes the word.
        std::size_t at = PositionAt(e.position);
        if ((at == text_.size() || ClassAt(at) == 0) && at > 0 && ClassAt(PreviousCodePointStart(text_, at)) != 0)
        {
            at = PreviousCodePointStart(text_, at);
        }
        if (at == text_.size() || text_[at] == '\n')
        {
            Select(at, at);
            return;
        }
        const int   kind  = ClassAt(at);
        std::size_t start = at;
        while (start > 0 && ClassAt(PreviousCodePointStart(text_, start)) == kind &&
               text_[PreviousCodePointStart(text_, start)] != '\n')
        {
            start = PreviousCodePointStart(text_, start);
        }
        std::size_t end = at;
        while (end < text_.size() && ClassAt(end) == kind && text_[end] != '\n')
        {
            end = NextCodePointStart(text_, end);
        }
        Select(start, end);
    }

    /** The wheel scrolls the lines while some are hidden, and is handed up otherwise.
        @param e  the wheel's travel
        @return   true when the view scrolled, or could have */
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
        const bool shift   = key.modifiers.shift;
        const bool command = key.modifiers.command;
        const bool words   = key.modifiers.alt;
        switch (key.key)
        {
        case Key::kCharacter:
            if (command)
            {
                switch (key.character)
                {
                case U'a':
                case U'A':
                    SelectAll();
                    return true;
                case U'c':
                case U'C':
                    Copy();
                    return true;
                case U'x':
                case U'X':
                    Cut();
                    return editable_;
                case U'v':
                case U'V':
                    Paste();
                    return editable_;
                default:
                    return false;
                }
            }
            if (!editable_ || key.character < U' ')
            {
                return false;
            }
            Replace(Utf8Of(std::u32string(1, key.character)));
            return true;
        case Key::kReturn:
            if (!editable_)
            {
                return false;
            }
            Replace("\n");
            return true;
        case Key::kBackspace:
        case Key::kDelete:
            if (!editable_)
            {
                return false;
            }
            if (anchor_ == caret_)
            {
                const bool back = key.key == Key::kBackspace;
                anchor_         = back ? (words ? WordStartBefore(caret_) : PreviousCodePointStart(text_, caret_))
                                       : (words ? WordEndAfter(caret_) : NextCodePointStart(text_, caret_));
            }
            if (anchor_ != caret_)
            {
                Replace({});
            }
            return true;
        case Key::kLeft:
            if (!shift && anchor_ != caret_)
            {
                MoveCaret(selection().first, false);
            }
            else
            {
                MoveCaret(command ? LineHome(caret_)
                          : words ? WordStartBefore(caret_)
                                  : PreviousCodePointStart(text_, caret_),
                          shift);
            }
            return true;
        case Key::kRight:
            if (!shift && anchor_ != caret_)
            {
                MoveCaret(selection().second, false);
            }
            else
            {
                MoveCaret(command ? LineEnd(caret_)
                          : words ? WordEndAfter(caret_)
                                  : NextCodePointStart(text_, caret_),
                          shift);
            }
            return true;
        case Key::kUp:
            if (command)
            {
                MoveCaret(0, shift);
            }
            else
            {
                MoveVertically(-1, shift);
            }
            return true;
        case Key::kDown:
            if (command)
            {
                MoveCaret(text_.size(), shift);
            }
            else
            {
                MoveVertically(1, shift);
            }
            return true;
        case Key::kPageUp:
        case Key::kPageDown: {
            const int page = std::max(1, AreaFor(bar_.visible()).height() / LineHeight() - 1);
            MoveVertically(key.key == Key::kPageUp ? -page : page, shift);
            return true;
        }
        case Key::kHome:
            MoveCaret(command ? 0 : LineHome(caret_), shift);
            return true;
        case Key::kEnd:
            MoveCaret(command ? text_.size() : LineEnd(caret_), shift);
            return true;
        case Key::kEscape:
        case Key::kTab:
        case Key::kNone:
            return false;
        }
        return false;
    }

  private:
    /** Whether the caret and the lit outline show: the view has the keyboard and the window holds it too.
        @return  true when they show */
    bool ShowsFocus() const
    {
        const Host *h = host();
        return focused_ && (h == nullptr || h->WindowHasKeyboardFocus());
    }

    /** `position` held to the text and moved back to a character's start.
        @param position  a byte offset
        @return          the offset at or before it that starts a character, or the end */
    std::size_t AtBoundary(std::size_t position) const noexcept
    {
        position = std::min(position, text_.size());
        while (position > 0 && position < text_.size() &&
               (static_cast<unsigned char>(text_[position]) & 0xC0u) == 0x80u)
        {
            --position;
        }
        return position;
    }

    /** How wide a run of text is in the view's font, as the window draws it (an estimate in no window: half the
        font's height a character).
        @param run  the UTF-8 run
        @return     the width, in pixels */
    float Measure(std::string_view run) const
    {
        if (const Host *h = host())
        {
            return h->TextWidth(style_.font, run);
        }
        std::size_t count = 0;
        for (std::size_t i = 0; i < run.size(); ++count)
        {
            i = NextCodePointStart(run, i);
        }
        return 0.5f * style_.font.height * static_cast<float>(count);
    }

    /** Where the text is drawn for a scroll bar shown or not.
        @param with_bar  true when the bar takes its width at the right
        @return          the area, in the view's coordinates */
    gfx::RectI AreaFor(bool with_bar) const noexcept
    {
        const int p = std::max(0, style_.padding);
        const int w = width() - 2 * p - (with_bar ? style_.scroll_bar_width : 0);
        return {p, p, std::max(0, w), std::max(0, height() - 2 * p)};
    }

    /** The layout dropped, to be made again before it is next read, and the view painted again. */
    void Invalidate()
    {
        layout_valid_ = false;
        Repaint();
    }

    /** The lines laid out at the area's width - with the scroll bar's width taken when the lines without it do not fit
        - the bar placed and ranged, and the scroll held to the range: made again only when the text, the size, the
        wrapping or the host that measures has changed. */
    void EnsureLayout()
    {
        if (layout_valid_ && measured_by_ == host())
        {
            return;
        }
        layout_valid_      = true;
        measured_by_       = host();
        const auto measure = [this](std::string_view run) { return Measure(run); };
        const int  lh      = LineHeight();
        const auto lay_out = [&](bool with_bar) {
            const float room = style_.word_wrap ? std::max(1.0f, static_cast<float>(AreaFor(with_bar).width())) : 0.0f;
            lines_           = WrapLines(text_, room, measure);
        };
        lay_out(false);
        const int shown = AreaFor(false).height();
        bool      bar   = static_cast<long long>(lines_.size()) * lh > shown && shown > 0;
        if (bar && style_.word_wrap)
        {
            lay_out(true);
        }
        const double content = static_cast<double>(lines_.size()) * lh;
        bar_.SetBounds({std::max(0, width() - style_.scroll_bar_width), 0, style_.scroll_bar_width, height()});
        bar_.SetRangeLimits(0.0, bar ? content : static_cast<double>(shown));
        const long long most = std::max<long long>(0, static_cast<long long>(content) - shown);
        scroll_              = static_cast<int>(std::clamp<long long>(scroll_, 0, bar ? most : 0));
        bar_.SetCurrentRange(static_cast<double>(scroll_), static_cast<double>(shown));
    }

    /** The x of a place on a line, from the line's left.
        @param line      the line
        @param position  a byte offset on it, at a character's start; held to the line
        @return          the width of the line's text before it, in pixels */
    float XOf(const TextLine &line, std::size_t position) const
    {
        position = std::clamp(position, line.start, line.end);
        return Measure(std::string_view(text_).substr(line.start, position - line.start));
    }

    /** The last place the caret may stand on a line: its end, or, on a wrapped line, before its last character, since
        its end is drawn at the next line's start.
        @param line  the line
        @return      a byte offset */
    std::size_t LastPlaceOn(const TextLine &line) const noexcept
    {
        return line.soft && line.end > line.start ? PreviousCodePointStart(text_, line.end) : line.end;
    }

    /** The place on line `index` nearest `x`: the nearer side of the character under it.
        @param index  the line's index in lines_
        @param x      the x, from the line's left, in pixels
        @return       a byte offset at a character's start */
    std::size_t PositionOnLine(std::size_t index, float x) const
    {
        const TextLine   &line = lines_[index];
        const std::size_t last = LastPlaceOn(line);
        float             left = 0.0f;
        for (std::size_t p = line.start; p < last;)
        {
            const std::size_t next  = NextCodePointStart(text_, p);
            const float       right = XOf(line, next);
            if (x < 0.5f * (left + right))
            {
                return p;
            }
            left = right;
            p    = next;
        }
        return last;
    }

    /** The start of the caret's line.
        @param position  a byte offset
        @return          its line's first byte */
    std::size_t LineHome(std::size_t position)
    {
        return lines_[LineOf(position)].start;
    }

    /** The last place on the caret's line (LastPlaceOn).
        @param position  a byte offset
        @return          a byte offset */
    std::size_t LineEnd(std::size_t position)
    {
        return LastPlaceOn(lines_[LineOf(position)]);
    }

    /** The class of the character at a place, for the word keys and the double click.
        @param position  a byte offset at a character's start, inside the text
        @return          2 for a letter, a digit or anything past ASCII, 0 for a space, a tab or a line feed, 1 for
                         anything else */
    int ClassAt(std::size_t position) const noexcept
    {
        const auto c = static_cast<unsigned char>(text_[position]);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80)
        {
            return 2;
        }
        return c == ' ' || c == '\t' || c == '\n' ? 0 : 1;
    }

    /** The start of the word before a place: the spaces just before it skipped, then a run of one class.
        @param position  a byte offset at a character's start
        @return          the start of that run */
    std::size_t WordStartBefore(std::size_t position) const noexcept
    {
        std::size_t i = position;
        while (i > 0 && ClassAt(PreviousCodePointStart(text_, i)) == 0)
        {
            i = PreviousCodePointStart(text_, i);
        }
        if (i > 0)
        {
            const int kind = ClassAt(PreviousCodePointStart(text_, i));
            while (i > 0 && ClassAt(PreviousCodePointStart(text_, i)) == kind)
            {
                i = PreviousCodePointStart(text_, i);
            }
        }
        return i;
    }

    /** The end of the word after a place: the spaces just after it skipped, then a run of one class.
        @param position  a byte offset at a character's start
        @return          the end of that run */
    std::size_t WordEndAfter(std::size_t position) const noexcept
    {
        std::size_t i = position;
        while (i < text_.size() && ClassAt(i) == 0)
        {
            i = NextCodePointStart(text_, i);
        }
        if (i < text_.size())
        {
            const int kind = ClassAt(i);
            while (i < text_.size() && ClassAt(i) == kind)
            {
                i = NextCodePointStart(text_, i);
            }
        }
        return i;
    }

    /** The caret to a place, the selection extended from the anchor or emptied there, the column forgotten, the caret
        scrolled into view.
        @param position   a byte offset at a character's start
        @param extending  true to keep the anchor */
    void MoveCaret(std::size_t position, bool extending)
    {
        caret_ = AtBoundary(position);
        if (!extending)
        {
            anchor_ = caret_;
        }
        preferred_x_ = -1.0f;
        ScrollToCaret();
        Repaint();
    }

    /** The caret moved `lines` lines up (negative) or down, at the column it stood in when the vertical moves began,
        to the text's start or end past the first or the last line.
        @param lines      how many lines, down positive
        @param extending  true to keep the anchor */
    void MoveVertically(int lines, bool extending)
    {
        EnsureLayout();
        const std::size_t index = LineOf(caret_);
        const float       x     = preferred_x_ >= 0.0f ? preferred_x_ : XOf(lines_[index], caret_);
        const long long   to    = static_cast<long long>(index) + lines;
        std::size_t       place = 0;
        if (to < 0)
        {
            place = 0;
        }
        else if (to >= static_cast<long long>(lines_.size()))
        {
            place = text_.size();
        }
        else
        {
            place = PositionOnLine(static_cast<std::size_t>(to), x);
        }
        MoveCaret(place, extending);
        preferred_x_ = x;
    }

    /** `incoming` in place of the selection, the caret after it, the layout made again, the caret scrolled into view
        and `on_change` told.
        @param incoming  the text put in, any line breaks; empty to take the selection out alone */
    void Replace(std::string_view incoming)
    {
        const std::string text   = NormalisedText(incoming);
        const auto [first, last] = selection();
        text_.replace(first, last - first, text);
        caret_       = first + text.size();
        anchor_      = caret_;
        preferred_x_ = -1.0f;
        Invalidate();
        ScrollToCaret();
        if (on_change)
        {
            const auto change = on_change; // a copy: the owner may replace it as it hears
            change(text_);
        }
    }

    Style                 style_;                ///< the view's dress
    ScrollBar             bar_;                  ///< the scroll bar beside the lines
    std::string           text_;                 ///< the text, well-formed UTF-8, its line breaks line feeds
    std::size_t           caret_{0};             ///< the caret, a byte offset at a character's start
    std::size_t           anchor_{0};            ///< where the selection grows from
    float                 preferred_x_{-1.0f};   ///< the column vertical moves keep, in pixels; negative for none
    bool                  editable_{false};      ///< whether the person may edit the text
    bool                  focused_{false};       ///< the router has given the view the keyboard
    int                   scroll_{0};            ///< the text area's top below the first line's, in pixels
    std::vector<TextLine> lines_;                ///< the text laid out
    bool                  layout_valid_{false};  ///< whether lines_ and the bar follow the text and the size
    const Host           *measured_by_{nullptr}; ///< the host whose measure laid the lines out
};
} // namespace reckless::view
