#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "View.h"

namespace reckless::view
{
/** UTF-8 to code points and back, for a field that moves its caret a character at a time: what is not UTF-8 - a
    preset's name from a file name in a code page - reads as U+FFFD for each broken part, the standard's practice
    (gfx::NextCodePoint), the good bytes after it kept; the suite's own text is ASCII, and what a person types arrives
    as code points.
    @param utf8  the text, UTF-8, or bytes that are not
    @return      its code points, U+FFFD for each broken part */
inline std::u32string CodePointsOf(std::string_view utf8)
{
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();)
    {
        out.push_back(gfx::NextCodePoint(utf8, i));
    }
    return out;
}

/** Code points to UTF-8, a surrogate or a value past U+10FFFF - no character - written as U+FFFD.
    @param text  the code points
    @return      the UTF-8 text */
inline std::string Utf8Of(std::u32string_view text)
{
    std::string out;
    for (char32_t cp : text)
    {
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        {
            cp = U'\xFFFD';
        }
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

/**
    One line of text to type into - a knob's value box being edited, a preset's name - laid out and drawn as JUCE's
    text editor lays out and draws one line, so a field lands on the pixels the releases built on JUCE drew.
    The text is split into words and runs of space, each measured on its own and drawn from where the one before it
    ended, at the justification's offset; the field's border holds a viewport whose text scrolls sideways to keep the
    caret in view (out past the edge by a fifth of the field's width, back by ten pixels short of it) and centres its
    one line in the height there is; the selection is a fill behind the selected words, at half strength while the
    window does not hold the keyboard, and they draw in the highlighted colour; the caret is a 2 px bar; the outline
    is 2 px in the focused colour while the field has the keyboard and a hairline otherwise; the background fills all
    of it. Every step the layout takes - the caret moved, text put in or taken out, the field resized - scrolls as the
    JUCE editor's does, so the same sequence of steps lands the text on the same pixel.

    Its keys are the editor's for one line: typing replaces the selection; Left and Right (a word at a time with Alt),
    Home and End, and Up and Down (to the start and the end) move the caret, Shift extending the selection; Backspace
    and Delete remove the selection or the character beside the caret (Alt: back to the word's start); Command-A
    selects everything. A press puts the caret where it lands (Shift: extends the selection), a drag selects, a double
    click selects the word. Return commits, as losing the keyboard does unless the owner says otherwise; Escape
    cancels, and a field cancelled commits nothing until it has the keyboard again - what either means is the
    owner's. The partly selected word is drawn in two colours split where the selected characters' width ends, which
    is where the JUCE editor splits its glyphs to within the kerning between them.
*/
class TextField final : public View
{
  public:
    /** What a field is dressed in: its colours, its font, its border and its indents. */
    struct Style
    {
        gfx::Font    font{14.0f};                                 ///< the text's font
        gfx::Color   text{gfx::Color::FromRgb(240, 240, 240)};    ///< the text's colour
        gfx::Color   background{gfx::Color::FromRgb(20, 20, 20)}; ///< the fill behind everything
        gfx::Color   outline{gfx::Color::FromRgb(120, 120, 120)}; ///< the hairline round a field without the keyboard
        gfx::Color   focused_outline{gfx::Color::FromRgb(200, 160, 60)};   ///< 2 px round one with it
        gfx::Color   highlight{gfx::Color::FromArgb(110, 200, 160, 60)};   ///< behind the selection
        gfx::Color   highlighted_text{gfx::Color::FromRgb(255, 255, 255)}; ///< the colour of the selected characters
        gfx::Color   caret{gfx::Color::FromRgb(240, 240, 240)};            ///< the caret's bar
        gfx::Justify justify{gfx::Justify::kTopLeft};                      ///< where the line sits in the field
        int          border_top{1};    ///< the border round the viewport the text scrolls in, in pixels
        int          border_left{1};   ///< the left border, in pixels
        int          border_bottom{1}; ///< the bottom border, in pixels
        int          border_right{3};  ///< the right border, in pixels
        int          left_indent{4};   ///< the text's start inside the viewport, in pixels
        int          top_indent{4};    ///< the text's top inside the viewport, in pixels
        /** What typing may put in, as juce::TextEditor::setInputRestrictions lets it: at most `max_length` characters
            in the field (0 for no limit), and only those in `allowed` (UTF-8; empty for any) - a seed's four digits. */
        std::size_t max_length{0};
        std::string allowed{}; ///< the characters that may be typed, UTF-8; empty for any
    };

    /** An empty field with the I-beam over it.
        @param style  the field's dress */
    explicit TextField(Style style) : style_(std::move(style))
    {
        cursor = Cursor::kIBeam;
    }

    /** What Return or losing the keyboard sends: called with the text, UTF-8. Message thread. */
    std::function<void(const std::string &)> on_commit;

    /** What Escape sends. */
    std::function<void()> on_cancel;

    /** Whether losing the keyboard commits, as Return does; false where a press elsewhere means something else (a
        dialog's Cancel button takes the keyboard from its field before it is clicked). */
    bool commit_on_focus_lost{true};
    /** Keep the keyboard, while the field has it, against a press on a view that wants none and the window's losing the
        keyboard (View::KeepsKeyboardFocus): a field in a box open to be typed into. */
    bool keeps_keyboard_focus{false};

    /** Whether gaining the keyboard selects all of the text, the caret at its end. */
    bool select_all_when_focused{false};

    /** The field's dress.
        @return  the style the field was made with */
    const Style &style() const noexcept
    {
        return style_;
    }

    /** The text replaced, the caret kept where it was or, if it was at the end, put at the new end.
        @param utf8  the new text, UTF-8; bytes that are not read as U+FFFD for each broken part */
    void SetText(std::string_view utf8)
    {
        std::u32string incoming = CodePointsOf(utf8);
        if (incoming == text_)
        {
            return;
        }
        int        old_caret = caret_;
        const bool at_end    = old_caret >= Length();
        if (!text_.empty())
        {
            text_.clear();
            CheckLayout();
            MoveCaretTo(caret_, false);
        }
        if (!incoming.empty())
        {
            text_ = std::move(incoming);
            CheckLayout();
            MoveCaretTo(caret_, false);
        }
        if (at_end)
        {
            old_caret = Length();
        }
        MoveCaretTo(old_caret, false);
        CheckLayout();
        ScrollToCaret();
        Repaint();
    }

    /** The text in the field.
        @return  the text, UTF-8 */
    std::string text() const
    {
        return Utf8Of(text_);
    }

    /** The caret's place.
        @return  the caret's index, in characters (code points) from the start of the text */
    std::size_t caret() const noexcept
    {
        return static_cast<std::size_t>(caret_);
    }

    /** Whether the router has given the field the keyboard.
        @return  true while the field has the keyboard */
    bool has_focus() const noexcept
    {
        return focused_;
    }

    /** The selection as [first, last), empty when the caret stands alone.
        @return  the first and the one past the last selected character, in characters from the start of the text */
    std::pair<std::size_t, std::size_t> selection() const noexcept
    {
        return {static_cast<std::size_t>(selection_.start), static_cast<std::size_t>(selection_.end)};
    }

    /** [first, last) selected: the caret at the end the selection grew from, as the editor puts it.
        @param first  the first character selected, in characters from the start of the text
        @param last   one past the last character selected */
    void SetHighlightedRegion(std::size_t first, std::size_t last)
    {
        const Range wanted{static_cast<int>(first), static_cast<int>(last)};
        if (wanted == selection_)
        {
            return;
        }
        const bool caret_at_start = wanted.end == selection_.start || wanted.end == selection_.end;
        MoveCaretTo(caret_at_start ? wanted.end : wanted.start, false);
        MoveCaretTo(caret_at_start ? wanted.start : wanted.end, true);
    }

    /** All of the text selected, the caret at its start. */
    void SelectAll()
    {
        MoveCaretTo(Length(), false);
        MoveCaretTo(0, true);
    }

    /** Where the text starts, in the field's coordinates: the indent and the border, less the viewport's scroll.
        @return  the text's origin, in pixels */
    gfx::PointI TextOffset() const
    {
        return {
            style_.left_indent + style_.border_left + content_x_,
            gfx::RoundToInt(static_cast<float>(style_.top_indent) + static_cast<float>(style_.border_top) + YOffset()) +
                content_y_};
    }

    bool WantsKeyboardFocus() const override
    {
        return true;
    }

    bool KeepsKeyboardFocus() const override
    {
        return keeps_keyboard_focus;
    }

    void FocusGained() override
    {
        focused_   = true;
        cancelled_ = false;
        if (select_all_when_focused)
        {
            MoveCaretTo(0, false);
            MoveCaretTo(Length(), true);
        }
        Repaint();
    }

    void FocusLost() override
    {
        focused_ = false;
        Repaint();
        if (!cancelled_ && commit_on_focus_lost)
        {
            Commit();
        }
    }

    bool KeyPressed(const KeyEvent &key) override
    {
        const bool shift = key.modifiers.shift;
        const bool words = key.modifiers.alt;
        switch (key.key)
        {
        case Key::kCharacter:
            if (key.modifiers.command && (key.character == U'a' || key.character == U'A'))
            {
                SelectAll();
            }
            else if (key.modifiers.command && (key.character == U'v' || key.character == U'V'))
            {
                Paste();
            }
            else if (key.character >= U' ' && !key.modifiers.command)
            {
                Insert(std::u32string(1, key.character));
            }
            else
            {
                return false;
            }
            break;
        case Key::kBackspace:
            if (words)
            {
                MoveCaretTo(WordBreakBefore(caret_), true);
            }
            else if (selection_.empty() && selection_.start > 0)
            {
                SetSelection({selection_.end - 1, selection_.end});
            }
            Cut();
            break;
        case Key::kDelete:
            if (selection_.empty() && selection_.start < Length())
            {
                SetSelection({selection_.start, selection_.start + 1});
            }
            Cut();
            break;
        case Key::kLeft:
            MoveCaretTo(key.modifiers.command ? 0 : (words ? WordBreakBefore(caret_) : caret_ - 1), shift);
            break;
        case Key::kRight:
            MoveCaretTo(key.modifiers.command ? Length() : (words ? WordBreakAfter(caret_) : caret_ + 1), shift);
            break;
        case Key::kHome:
        case Key::kUp:
        case Key::kPageUp:
            MoveCaretTo(0, shift);
            break;
        case Key::kEnd:
        case Key::kDown:
        case Key::kPageDown:
            MoveCaretTo(Length(), shift);
            break;
        case Key::kReturn:
            Commit();
            return true;
        case Key::kEscape:
            MoveCaretTo(caret_, false);
            cancelled_ = true;
            if (on_cancel)
            {
                const auto cancel = on_cancel; // a copy: cancelling may close the field
                cancel();
            }
            return true;
        case Key::kTab:
        case Key::kNone:
            return false;
        }
        return true;
    }

    void MouseDown(const MouseEvent &e) override
    {
        MoveCaretTo(IndexAt(e.position), e.modifiers.shift);
    }

    void MouseDrag(const MouseEvent &e) override
    {
        MoveCaretTo(IndexAt(e.position), true);
    }

    void MouseDoubleClick(const MouseEvent &e) override
    {
        // The word under the pointer: letters, digits and anything past ASCII, as the editor counts a word.
        const auto in_word = [](char32_t c) {
            return (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || c > 128;
        };
        int end = IndexAt(e.position);
        while (end < Length() && in_word(text_[static_cast<std::size_t>(end)]))
        {
            ++end;
        }
        int start = end;
        while (start > 0 && in_word(text_[static_cast<std::size_t>(start - 1)]))
        {
            --start;
        }
        MoveCaretTo(end, false);
        MoveCaretTo(start, true);
    }

    void Resized() override
    {
        holder_width_  = std::max(0, width() - style_.border_left - style_.border_right);
        holder_height_ = std::max(0, height() - style_.border_top - style_.border_bottom);
        Reclamp();
        CheckLayout();
        ScrollToCaret();
    }

    void Paint(gfx::Canvas &c) override
    {
        c.FillAll(style_.background);
        // The viewport, inside the border; its content holder, the same size; the text holder, scrolled within it.
        gfx::ScopedState viewport(c);
        if (!c.ClipToRect({style_.border_left, style_.border_top, holder_width_, holder_height_}))
        {
            return;
        }
        c.AddTransform(
            gfx::Affine::Translation(static_cast<float>(style_.border_left), static_cast<float>(style_.border_top)));
        if (!c.ClipToRect({0, 0, holder_width_, holder_height_}) ||
            !c.ClipToRect({content_x_, content_y_, text_width_, text_height_}))
        {
            return;
        }
        c.AddTransform(gfx::Affine::Translation(static_cast<float>(content_x_), static_cast<float>(content_y_)));
        {
            gfx::ScopedState content(c);
            DrawContent(c);
        }
        if (ShowsFocus())
        {
            // The caret, a child of the text holder: clipped to its bar and filled.
            const gfx::RectI bar =
                CaretRect().Translated(style_.left_indent, style_.top_indent + gfx::RoundToInt(YOffset()));
            gfx::ScopedState caret(c);
            if (c.ClipToRect(bar))
            {
                c.AddTransform(gfx::Affine::Translation(static_cast<float>(bar.x()), static_cast<float>(bar.y())));
                c.SetColor(style_.caret);
                c.FillRect(gfx::RectI{bar.width(), bar.height()});
            }
        }
    }

    void PaintOverChildren(gfx::Canvas &c) override
    {
        if (!IsEnabled())
        {
            return;
        }
        if (ShowsFocus())
        {
            c.SetColor(style_.focused_outline);
            c.DrawRect(LocalBounds().ToFloat(), 2.0f);
        }
        else
        {
            c.SetColor(style_.outline);
            c.DrawRect(LocalBounds().ToFloat(), 1.0f);
        }
    }

  private:
    /** A run of characters [start, end): the selection, or an atom's span. */
    struct Range
    {
        int start{0}; ///< the first character, in characters from the start of the text
        int end{0};   ///< one past the last character

        /** Whether the range holds no character.
            @return  true when `end` is not past `start` */
        bool empty() const noexcept
        {
            return end <= start;
        }

        /** Whether the range shares a character with `other`.
            @param other  the range compared
            @return       true when they overlap */
        bool Intersects(Range other) const noexcept
        {
            return other.start < end && start < other.end;
        }

        /** Whether two ranges are the same run.
            @param a  one range
            @param b  the other
            @return   true when both ends agree */
        friend bool operator==(Range a, Range b) noexcept
        {
            return a.start == b.start && a.end == b.end;
        }
    };

    /** A word or a run of space: where it starts in the text, how many characters, and its measured width. */
    struct Atom
    {
        int   start{0};     ///< the first character, in characters from the start of the text
        int   length{0};    ///< how many characters
        bool  space{false}; ///< true for a run of spaces or tabs, false for a word
        float width{0.0f};  ///< the measured width, in pixels
    };

    /** The one line laid out: its atoms and where each starts, and where the caret stands past the last. */
    struct Line
    {
        std::vector<Atom>  atoms;       ///< the words and runs of space, in order
        std::vector<float> x;           ///< where each atom starts, in pixels from the text's start
        float              start{0.0f}; ///< where the first atom starts: the justification's offset
        float              end{0.0f};   ///< where the caret stands past the last atom
    };

    /** Which end of the selection a selecting move extends. */
    enum class Drag
    {
        kNone,           ///< no selection is being extended
        kSelectionStart, ///< the selection's start follows the caret
        kSelectionEnd,   ///< the selection's end follows the caret
    };

    static constexpr int kRightEdgeSpace = 2; ///< room for the caret at the text's end

    /** The text's length.
        @return  the number of characters (code points) */
    int Length() const noexcept
    {
        return static_cast<int>(text_.size());
    }

    /** Whether the caret and the focused outline show: the field has the keyboard and the window holds it too.
        @return  true when they show */
    bool ShowsFocus() const
    {
        const Host *h = host();
        return focused_ && (h == nullptr || h->WindowHasKeyboardFocus());
    }

    /** How wide `text` is in the field's font, as the window draws it (an estimate in no window).
        @param text  the characters measured
        @return      the width, in pixels */
    float Measure(std::u32string_view text) const
    {
        const std::string utf8 = Utf8Of(text);
        if (const Host *h = host())
        {
            return h->TextWidth(style_.font, utf8);
        }
        return 0.5f * style_.font.height * static_cast<float>(text.size());
    }

    /** The line's atoms measured and placed, from the justification's offset.
        @return  the line, in pixels from the text's start */
    Line LayOut() const
    {
        Line line;
        for (int i = 0; i < Length();)
        {
            const bool space =
                text_[static_cast<std::size_t>(i)] == U' ' || text_[static_cast<std::size_t>(i)] == U'\t';
            int j = i;
            while (j < Length() &&
                   (text_[static_cast<std::size_t>(j)] == U' ' || text_[static_cast<std::size_t>(j)] == U'\t') == space)
            {
                ++j;
            }
            line.atoms.push_back({i, j - i, space,
                                  Measure(std::u32string_view(text_).substr(static_cast<std::size_t>(i),
                                                                            static_cast<std::size_t>(j - i)))});
            i = j;
        }
        float line_width = 0.0f;
        for (const Atom &atom : line.atoms)
        {
            line_width += atom.width;
        }
        line.start = JustificationOffset(line_width);
        float x    = line.start;
        for (const Atom &atom : line.atoms)
        {
            line.x.push_back(x);
            x += atom.width;
        }
        line.end = x;
        return line;
    }

    /** Where the line starts for the justification: 0 for the left, the spare room's half or all of it otherwise.
        @param line_width  the line's width, in pixels
        @return            the offset of the first atom, in pixels, never negative */
    float JustificationOffset(float line_width) const
    {
        const auto right = static_cast<float>(MaximumTextWidth());
        if (gfx::Has(style_.justify, gfx::Justify::kHorizontallyCentred))
        {
            return std::max(0.0f, (right - line_width) * 0.5f);
        }
        if (gfx::Has(style_.justify, gfx::Justify::kRight))
        {
            return std::max(0.0f, right - line_width);
        }
        return 0.0f;
    }

    /** The room the line has across the viewport, less the left indent and the room for the caret at its end.
        @return  the width, in pixels, at least 1 */
    int MaximumTextWidth() const noexcept
    {
        return std::max(1, holder_width_ - style_.left_indent - kRightEdgeSpace);
    }

    /** The room the line has down the viewport, less the top indent.
        @return  the height, in pixels, at least 1 */
    int MaximumTextHeight() const noexcept
    {
        return std::max(1, holder_height_ - style_.top_indent);
    }

    /** How far the line is moved down to sit as the justification asks in the height there is.
        @return  the offset, in pixels; 0 for a top-justified line */
    float YOffset() const
    {
        if (gfx::Has(style_.justify, gfx::Justify::kTop))
        {
            return 0.0f;
        }
        const auto bottom = static_cast<float>(MaximumTextHeight());
        if (!(0.0f < bottom))
        {
            return 0.0f;
        }
        const float spare = std::max(0.0f, bottom - 0.0f - style_.font.height);
        return gfx::Has(style_.justify, gfx::Justify::kBottom) ? spare : spare * 0.5f;
    }

    /** The x where character `index` starts on `line`: an atom's start, the end of the last, or inside a word where
        the characters before it end.
        @param line   the line laid out
        @param index  the character's index, in characters from the start of the text
        @return       the x, in pixels from the text's start */
    float IndexToX(const Line &line, int index) const
    {
        for (std::size_t i = 0; i < line.atoms.size(); ++i)
        {
            const Atom &atom = line.atoms[i];
            if (atom.start + atom.length > index)
            {
                return XWithin(line, i, index);
            }
        }
        return line.end;
    }

    /** Index `index`'s x as atom `i` places it: its start at or before it, its end at or past it.
        @param line   the line laid out
        @param i      the atom's place in the line
        @param index  the character's index, in characters from the start of the text
        @return       the x, in pixels from the text's start */
    float XWithin(const Line &line, std::size_t i, int index) const
    {
        const Atom &atom = line.atoms[i];
        if (index <= atom.start)
        {
            return line.x[i];
        }
        const float right = line.x[i] + atom.width;
        if (index >= atom.start + atom.length)
        {
            return right;
        }
        const float within = Measure(std::u32string_view(text_).substr(static_cast<std::size_t>(atom.start),
                                                                       static_cast<std::size_t>(index - atom.start)));
        return std::min(right, line.x[i] + within);
    }

    /** The caret's bar relative to the text's start: 2 px wide and the line's height, on whole pixels.
        @return  the bar, in pixels */
    gfx::RectI CaretRect() const
    {
        const Line line = LayOut();
        return gfx::RectF{IndexToX(line, caret_), 0.0f, 2.0f, style_.font.height}.SmallestIntegerContainer();
    }

    /** The line's right edge.
        @return  the farthest right any atom reaches, rounded, in pixels from the text's start */
    int TextRight() const
    {
        const Line line  = LayOut();
        float      right = 0.0f;
        for (std::size_t i = 0; i < line.atoms.size(); ++i)
        {
            right = std::max(right, line.x[i] + line.atoms[i].width);
        }
        return gfx::RoundToInt(right);
    }

    /** The line's height with the offset that sits it in the field.
        @return  the height, rounded, in pixels */
    int TotalTextHeight() const
    {
        return gfx::RoundToInt(0.0f + style_.font.height + YOffset());
    }

    /** The text holder sized to the text, and the scroll held inside what it allows. */
    void CheckLayout()
    {
        text_width_  = std::max(holder_width_, TextRight() + style_.left_indent + kRightEdgeSpace);
        text_height_ = TotalTextHeight() + style_.top_indent;
        Reclamp();
    }

    /** The text holder kept where the viewport allows it: never right of or below its origin, never so far left or
        up that space opens past its end. */
    void Reclamp()
    {
        content_x_ = std::max(std::min(0, holder_width_ - text_width_), content_x_);
        content_y_ = std::max(std::min(0, holder_height_ - text_height_), content_y_);
    }

    /** Scroll the viewport so (`x`, `y`) of the text holder is at its top left, held inside what the holder allows.
        @param x  the text holder's x at the viewport's left, in pixels
        @param y  the text holder's y at the viewport's top, in pixels */
    void SetViewPosition(int x, int y)
    {
        content_x_ = std::max(std::min(0, holder_width_ - text_width_), std::min(0, -x));
        content_y_ = std::max(std::min(0, holder_height_ - text_height_), std::min(0, -y));
    }

    /** A share of the field's width.
        @param proportion  the share, 0 to 1
        @return            that share of the width, rounded, in pixels */
    int ProportionOfWidth(float proportion) const
    {
        return gfx::RoundToInt(proportion * static_cast<float>(width()));
    }

    /** The scroll that keeps the caret in view and the line centred in the height. */
    void ScrollToCaret()
    {
        int              view_x   = -content_x_;
        const gfx::RectI bar      = CaretRect().Translated(style_.left_indent, style_.top_indent);
        const int        relative = bar.x() - view_x;
        if (relative < std::max(1, ProportionOfWidth(0.05f)))
        {
            view_x += relative - ProportionOfWidth(0.2f);
        }
        else if (relative > std::max(0, holder_width_ - 10))
        {
            view_x += relative + 10 - holder_width_;
        }
        view_x = std::clamp(view_x, 0, std::max(0, text_width_ + 8 - holder_width_));
        SetViewPosition(view_x, (height() - text_height_ - style_.top_indent) / -2);
    }

    /** The caret to `position`, scrolled into view; the selection is left as it is.
        @param position  the new place, in characters from the start of the text, clamped to the text */
    void MoveCaret(int position)
    {
        position = std::clamp(position, 0, Length());
        if (position == caret_)
        {
            return;
        }
        caret_ = position;
        ScrollToCaret();
        Repaint();
    }

    /** The selection to `range`, painted again when it changed.
        @param range  the characters selected; empty for a caret alone */
    void SetSelection(Range range)
    {
        if (!(range == selection_))
        {
            selection_ = range;
            Repaint();
        }
    }

    /** The caret to `position`, the selection extended from the end it grew from with `selecting`, emptied there
        otherwise.
        @param position   the new place, in characters from the start of the text, clamped to the text
        @param selecting  true to extend the selection to it, false to empty the selection there */
    void MoveCaretTo(int position, bool selecting)
    {
        if (selecting)
        {
            MoveCaret(position);
            if (drag_ == Drag::kNone)
            {
                drag_ = std::abs(caret_ - selection_.start) < std::abs(caret_ - selection_.end) ? Drag::kSelectionStart
                                                                                                : Drag::kSelectionEnd;
            }
            if (drag_ == Drag::kSelectionStart)
            {
                if (caret_ >= selection_.end)
                {
                    drag_ = Drag::kSelectionEnd;
                }
                SetSelection({std::min(caret_, selection_.end), std::max(caret_, selection_.end)});
            }
            else
            {
                if (caret_ < selection_.start)
                {
                    drag_ = Drag::kSelectionStart;
                }
                SetSelection({std::min(caret_, selection_.start), std::max(caret_, selection_.start)});
            }
        }
        else
        {
            drag_ = Drag::kNone;
            MoveCaret(position);
            SetSelection({caret_, caret_});
        }
    }

    /** `typed` as the field's restrictions let it in: the characters it allows, and no more of them than its length
        leaves room for once the selection is gone - juce::TextEditor's LengthAndCharacterRestriction.
        @param typed  the characters typed
        @return       those of them the style's `allowed` and `max_length` let in */
    std::u32string Admitted(std::u32string typed) const
    {
        if (!style_.allowed.empty())
        {
            const std::u32string allowed = CodePointsOf(style_.allowed);
            std::erase_if(typed, [&allowed](char32_t c) { return allowed.find(c) == std::u32string::npos; });
        }
        if (style_.max_length > 0)
        {
            const auto room = static_cast<long long>(style_.max_length) -
                              (static_cast<long long>(Length()) - (selection_.end - selection_.start));
            typed.resize(
                static_cast<std::size_t>(std::clamp<long long>(room, 0, static_cast<long long>(typed.size()))));
        }
        return typed;
    }

    /** `typed` in place of the selection, the caret after it: what the restrictions let in of it, the selection taken
        out whether or not any was, as the JUCE editor inserts at its caret.
        @param incoming  the characters typed; empty to take the selection out alone */
    void Insert(const std::u32string &incoming)
    {
        const std::u32string typed     = Admitted(incoming);
        const int            at        = selection_.start;
        const int            new_caret = at + static_cast<int>(typed.size());
        if (!selection_.empty())
        {
            text_.erase(static_cast<std::size_t>(selection_.start),
                        static_cast<std::size_t>(selection_.end - selection_.start));
            CheckLayout();
            MoveCaretTo(typed.empty() ? new_caret : new_caret - 1, false);
        }
        if (!typed.empty())
        {
            text_.insert(static_cast<std::size_t>(at), typed);
            CheckLayout();
            MoveCaretTo(new_caret, false);
        }
        Repaint();
    }

    /** The clipboard's text in place of the selection, as typing it would put it - what the restrictions let in, its
        line breaks and tabs as spaces: a licence pasted from an email, wrapped, lands as the one line it is. */
    void Paste()
    {
        Host *h = host();
        if (h == nullptr)
        {
            return;
        }
        std::u32string pasted;
        for (char32_t c : CodePointsOf(h->ClipboardText()))
        {
            pasted.push_back(c == U'\n' || c == U'\r' || c == U'\t' ? U' ' : c);
        }
        if (!pasted.empty())
        {
            Insert(pasted);
        }
    }

    /** The selection taken out, the caret where it was, nothing put in its place. */
    void Cut()
    {
        MoveCaret(selection_.end);
        Insert({});
    }

    /** The class a character has for the word keys.
        @param c  the character
        @return   2 for a letter, a digit or anything past ASCII, 0 for a space or a tab, 1 for anything else */
    static int CategoryOf(char32_t c)
    {
        const bool letter_or_digit =
            (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || c > 127;
        return letter_or_digit ? 2 : (c == U' ' || c == U'\t' ? 0 : 1);
    }

    /** The start of the word before `position`: the spaces just before it skipped, then a run of one class.
        @param position  a place in the text, in characters, clamped to the text
        @return          the start of that run */
    int WordBreakBefore(int position) const
    {
        int i = std::clamp(position, 0, Length());
        while (i > 0 && CategoryOf(text_[static_cast<std::size_t>(i - 1)]) == 0)
        {
            --i;
        }
        if (i > 0)
        {
            const int category = CategoryOf(text_[static_cast<std::size_t>(i - 1)]);
            while (i > 0 && CategoryOf(text_[static_cast<std::size_t>(i - 1)]) == category)
            {
                --i;
            }
        }
        return i;
    }

    /** The end of the word after `position`: the spaces just after it skipped, then a run of one class and the spaces
        after that.
        @param position  a place in the text, in characters, clamped to the text
        @return          the end of that run */
    int WordBreakAfter(int position) const
    {
        int i = std::clamp(position, 0, Length());
        while (i < Length() && CategoryOf(text_[static_cast<std::size_t>(i)]) == 0)
        {
            ++i;
        }
        if (i < Length())
        {
            const int category = CategoryOf(text_[static_cast<std::size_t>(i)]);
            while (i < Length() && CategoryOf(text_[static_cast<std::size_t>(i)]) == category)
            {
                ++i;
            }
        }
        while (i < Length() && CategoryOf(text_[static_cast<std::size_t>(i)]) == 0)
        {
            ++i;
        }
        return i;
    }

    /** The character index a point in the field lands on: before the line's first word, past its last, or the
        nearer side of the character under it.
        @param point  the point, in the field's coordinates, in pixels
        @return       the character index, in characters from the start of the text */
    int IndexAt(gfx::PointF point) const
    {
        const gfx::PointI offset = TextOffset();
        const auto        x      = static_cast<float>(gfx::RoundToInt(point.x) - offset.x);
        const auto        y      = static_cast<float>(gfx::RoundToInt(point.y) - offset.y);
        if (!(y < style_.font.height))
        {
            return Length();
        }
        const Line line = LayOut();
        for (std::size_t i = 0; i < line.atoms.size(); ++i)
        {
            const Atom &atom  = line.atoms[i];
            const float right = line.x[i] + atom.width;
            if (x <= line.x[i])
            {
                return atom.start;
            }
            if (x < right)
            {
                for (int k = 0; k < atom.length; ++k)
                {
                    const float left = XWithin(line, i, atom.start + k);
                    const float next = XWithin(line, i, atom.start + k + 1);
                    if ((left + next) / 2.0f > x)
                    {
                        return atom.start + k;
                    }
                }
                return atom.start + atom.length;
            }
        }
        return Length();
    }

    /** The selection's fill: a whole-pixel rectangle the line's height under each selected word, merged into a list
        as the editor merges its own - a rectangle holding one already listed replaces it, and one overlapping keeps
        only the part the list does not yet cover.
        @param line  the line laid out
        @return      the rectangles, in pixels from the text's start */
    gfx::Path SelectionPath(const Line &line) const
    {
        std::vector<gfx::RectI> rects;
        for (std::size_t i = 0; i < line.atoms.size(); ++i)
        {
            const Atom &atom = line.atoms[i];
            if (!selection_.Intersects({atom.start, atom.start + atom.length}))
            {
                continue;
            }
            const float from = XWithin(line, i, selection_.start);
            const float to   = XWithin(line, i, selection_.end);
            gfx::RectI  rect = gfx::RectF{from, 0.0f, to - from, style_.font.height}.SmallestIntegerContainer();
            if (rect.IsEmpty())
            {
                continue;
            }
            std::erase_if(rects, [&](const gfx::RectI &listed) { return rect.Contains(listed); });
            for (const gfx::RectI &listed : rects)
            {
                // Every rectangle spans the line's one height, so what is left of an overlap is its end beyond it.
                if (rect.Intersects(listed))
                {
                    rect = listed.x() <= rect.x() ? rect.WithLeft(std::min(listed.right(), rect.right()))
                                                  : rect.WithRight(std::max(listed.x(), rect.x()));
                }
            }
            if (!rect.IsEmpty())
            {
                rects.push_back(rect);
            }
        }
        gfx::Path path;
        for (const gfx::RectI &rect : rects)
        {
            path.AddRect(rect.ToFloat());
        }
        return path;
    }

    /** The text holder's drawing, from the indent: the selection's fill, then each word in its colour.
        @param c  the canvas, the text holder's top left at its origin */
    void DrawContent(gfx::Canvas &c)
    {
        c.AddTransform(
            gfx::Affine::Translation(static_cast<float>(style_.left_indent), static_cast<float>(style_.top_indent)));
        const float y_offset = YOffset();
        if (y_offset > 0.0f)
        {
            c.AddTransform(gfx::Affine::Translation(0.0f, y_offset));
        }
        const Line line = LayOut();
        if (!selection_.empty())
        {
            c.SetColor(style_.highlight.WithMultipliedAlpha(ShowsFocus() ? 1.0f : 0.5f));
            c.FillPath(SelectionPath(line));
        }
        // The line is drawn while it meets the clip, as the editor draws only the lines that do.
        const gfx::RectI clip = c.ClipBounds();
        if (!(0.0f < static_cast<float>(clip.bottom())) || !(style_.font.height >= static_cast<float>(clip.y())))
        {
            return;
        }
        const float baseline = static_cast<float>(gfx::RoundToInt(0.0f + style_.font.height - c.Descent(style_.font)));
        const float top      = baseline - c.Ascent(style_.font);
        for (std::size_t i = 0; i < line.atoms.size(); ++i)
        {
            const Atom &atom = line.atoms[i];
            if (atom.space)
            {
                continue;
            }
            const Range span{atom.start, atom.start + atom.length};
            if (!selection_.Intersects(span))
            {
                DrawRun(c, atom.start, atom.length, line.x[i], top, style_.text);
                continue;
            }
            // The selected characters in the highlighted colour, any before or after them in the text's.
            const int first = std::max(span.start, selection_.start);
            const int last  = std::min(span.end, selection_.end);
            if (last < span.end)
            {
                DrawRun(c, last, span.end - last, XWithin(line, i, last), top, style_.text);
            }
            if (first > span.start)
            {
                DrawRun(c, span.start, first - span.start, line.x[i], top, style_.text);
            }
            DrawRun(c, first, last - first, XWithin(line, i, first), top, style_.highlighted_text);
        }
    }

    /** `count` characters from `start`, their first glyph at `x` and their tops at `top`: the line of text a glyph
        run draws, placed as a top-left box places it.
        @param c       the canvas
        @param start   the first character, in characters from the start of the text
        @param count   how many characters; none is drawn for zero or less
        @param x       where the first glyph starts, in pixels from the text's start
        @param top     where the characters' tops are, in pixels
        @param colour  the colour of the run */
    void DrawRun(gfx::Canvas &c, int start, int count, float x, float top, gfx::Color colour)
    {
        if (count <= 0)
        {
            return;
        }
        const std::string run =
            Utf8Of(std::u32string_view(text_).substr(static_cast<std::size_t>(start), static_cast<std::size_t>(count)));
        c.SetColor(colour);
        c.SetFont(style_.font);
        const float room = 2.0f * c.TextWidth(style_.font, run) + 16.0f; // wide enough that nothing is curtailed
        c.DrawText(run, gfx::RectF{x, top, room, style_.font.height}, gfx::Justify::kTopLeft, false);
    }

    /** Send the text to `on_commit`, if one is set. */
    void Commit()
    {
        if (on_commit)
        {
            const auto commit = on_commit; // a copy: committing may close the field
            commit(Utf8Of(text_));
        }
    }

    Style          style_;             ///< the field's dress
    std::u32string text_;              ///< the text, in code points
    int            caret_{0};          ///< the caret's place, in characters from the start of the text
    Range          selection_{};       ///< the selected characters; empty when the caret stands alone
    Drag           drag_{Drag::kNone}; ///< which end of the selection a selecting move extends
    int            holder_width_{0};   ///< the viewport inside the border: its width, in pixels
    int            holder_height_{0};  ///< the viewport's height, in pixels
    int            text_width_{0};     ///< the text holder: the text and its indents, at least the viewport, in pixels
    int            text_height_{0};    ///< the text holder's height, in pixels
    int            content_x_{0};      ///< the text holder's x in the viewport, scrolled: never above 0
    int            content_y_{0};      ///< the text holder's y in the viewport: never above 0
    bool           focused_{false};    ///< whether the router has given the field the keyboard
    bool           cancelled_{false};  ///< set by Escape until the field has the keyboard again
};
} // namespace reckless::view
