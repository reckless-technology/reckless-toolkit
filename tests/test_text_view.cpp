// Many lines of text (view/TextView.h): the wrap - between words, the spaces hanging, a word wider than the line
// broken between characters and never inside one; line breaks of every kind made line feeds and broken UTF-8 repaired;
// the scroll bar's width taken only when the lines need it; a paint drawing the lines that show and no others; the
// caret and the selection by keys and by the pointer, a character at a time over characters of several bytes, a word,
// a line, the column kept up and down; the copy; and editing - typing, line feeds, deletions, a cut and a paste.
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TextView.h"
#include "TextViewTestSupport.h"

namespace
{
using reckless::gfx::PointF;
using reckless::gfx::RectI;
using reckless::test::TextHost;
using reckless::test::TextRecordingCanvas;
namespace view = reckless::view;
using view::Key;
using view::TextLine;
using view::TextView;

/** Every code point 7 px wide, as the suites' host and canvas measure. */
float Pitch7(std::string_view run)
{
    return 7.0f * static_cast<float>(TextRecordingCanvas::CodePoints(run));
}

/** A text view in a window of its own: 200 x 100, 4 px of padding, 7 px a character and 18 px a line, so 192 px of
    room (27 characters) and five lines show whole. */
struct TextScene
{
    TextHost                 host;
    view::View               root;
    view::MouseRouter        router{root, &host};
    TextView                 text{TextView::Style{}};
    std::vector<std::string> copied;
    std::vector<std::string> changes;

    TextScene()
    {
        root.SetBounds({200, 100});
        root.SetHost(&host);
        host.router = &router;
        text.SetBounds({0, 0, 200, 100});
        root.AddChild(text);
        text.on_copy   = [this](std::string s) { copied.push_back(std::move(s)); };
        text.on_change = [this](const std::string &s) { changes.push_back(s); };
    }

    bool Press(Key key, view::Modifiers modifiers = {})
    {
        return router.Key({key, 0, modifiers});
    }

    bool Type(char32_t c, view::Modifiers modifiers = {})
    {
        return router.Key({Key::kCharacter, c, modifiers});
    }

    void Focus()
    {
        router.Focus(&text);
    }
};
} // namespace

TEST(WrapLines, BreaksBetweenWordsTheSpacesHangingAtTheLinesEnd)
{
    const std::vector<TextLine> lines = view::WrapLines("the quick brown fox", 70.0f, Pitch7);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], (TextLine{0, 10, true})) << "\"the quick \", its space hanging";
    EXPECT_EQ(lines[1], (TextLine{10, 19, false}));
    EXPECT_EQ(view::WrapLines("the quick brown fox", 0.0f, Pitch7).size(), 1u) << "no width, no wrap";
    EXPECT_EQ(view::WrapLines("the quick brown fox", 133.0f, Pitch7).size(), 1u) << "19 characters fit 133 px";
    EXPECT_EQ(view::WrapLines("the quick brown fox", 132.0f, Pitch7).size(), 2u);
}

TEST(WrapLines, LineFeedsBreakEveryParagraphEmptyOnesTooAndNothingIsOneLine)
{
    const std::vector<TextLine> lines = view::WrapLines("a\n\nb\n", 70.0f, Pitch7);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0], (TextLine{0, 1, false}));
    EXPECT_EQ(lines[1], (TextLine{2, 2, false})) << "the empty paragraph";
    EXPECT_EQ(lines[2], (TextLine{3, 4, false}));
    EXPECT_EQ(lines[3], (TextLine{5, 5, false})) << "the line after the last line feed";
    ASSERT_EQ(view::WrapLines("", 70.0f, Pitch7).size(), 1u);
    EXPECT_EQ(view::WrapLines("", 70.0f, Pitch7)[0], (TextLine{0, 0, false}));
}

TEST(WrapLines, AWordWiderThanTheLineBreaksBetweenCharactersNeverInsideOne)
{
    const std::vector<TextLine> lines = view::WrapLines("abcdefghijklmnop", 35.0f, Pitch7);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0], (TextLine{0, 5, true}));
    EXPECT_EQ(lines[3], (TextLine{15, 16, false}));

    // Five two-byte characters, three to a line: the break falls between characters, at byte 6.
    const std::string           accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    const std::vector<TextLine> wide    = view::WrapLines(accents, 21.0f, Pitch7);
    ASSERT_EQ(wide.size(), 2u);
    EXPECT_EQ(wide[0], (TextLine{0, 6, true}));
    EXPECT_EQ(wide[1], (TextLine{6, 10, false}));

    // A line narrower than one character still takes one.
    const std::vector<TextLine> narrow = view::WrapLines("\xE2\x99\x94\xE2\x99\x95", 3.0f, Pitch7);
    ASSERT_EQ(narrow.size(), 2u);
    EXPECT_EQ(narrow[0], (TextLine{0, 3, true})) << "one chess king, three bytes";
}

TEST(TextView, TextIsRepairedAndItsLineBreaksMadeLineFeeds)
{
    EXPECT_EQ(view::NormalisedText("a\r\nb\rc\nd"), "a\nb\nc\nd");
    EXPECT_EQ(view::NormalisedText("x\xFFy"), "x\xEF\xBF\xBDy") << "a broken byte one U+FFFD";
    TextScene s;
    s.text.SetText("one\r\ntwo");
    EXPECT_EQ(s.text.text(), "one\ntwo");
    EXPECT_EQ(view::NextCodePointStart("\xC3\xA9x", 0), 2u);
    EXPECT_EQ(view::PreviousCodePointStart("\xC3\xA9x", 2), 0u);
    EXPECT_EQ(view::PreviousCodePointStart("\xF0\x9F\x98\x80", 4), 0u) << "back over a four-byte character";
}

TEST(TextView, ItWrapsAtItsWidthAndTakesTheBarsWidthOnlyWhenTheLinesNeedIt)
{
    TextScene s;
    EXPECT_EQ(s.text.LineHeight(), 18);
    s.text.SetText("1. e4 c5 2. Nf3 d6 3. d4 cxd4 4. Nxd4 Nf6");
    EXPECT_FALSE(s.text.scroll_bar().visible());
    EXPECT_EQ(s.text.TextArea(), (RectI{4, 4, 192, 92}));
    ASSERT_EQ(s.text.Lines().size(), 2u) << "27 characters to a line";
    EXPECT_EQ(s.text.Lines()[0], (TextLine{0, 25, true})) << "\"1. e4 c5 2. Nf3 d6 3. d4 \"";

    std::string many;
    for (int i = 0; i < 40; ++i)
    {
        many += "line " + std::to_string(i) + " of forty, short\n";
    }
    s.text.SetText(many);
    EXPECT_TRUE(s.text.scroll_bar().visible()) << "forty-one lines do not fit five";
    EXPECT_EQ(s.text.TextArea(), (RectI{4, 4, 182, 92})) << "the bar's width taken";
    EXPECT_EQ(s.text.scroll_bar().bounds(), (RectI{190, 0, 10, 100}));
    EXPECT_EQ(s.text.Lines().size(), 41u) << "the 22-character lines still fit 26 to a line";
    EXPECT_EQ(s.text.MaxScroll(), 41 * 18 - 92);

    s.text.SetWordWrap(false);
    s.text.SetText(std::string(100, 'x'));
    EXPECT_EQ(s.text.Lines().size(), 1u) << "unwrapped, a paragraph a line";
}

TEST(TextView, APaintDrawsTheLinesThatShowAndNoOthers)
{
    TextScene   s;
    std::string many;
    for (int i = 0; i < 10000; ++i)
    {
        many += "line " + std::to_string(i) + "\n";
    }
    s.text.SetText(many);
    s.text.SetScroll(5000 * 18 + 9);
    TextRecordingCanvas c;
    view::PaintTree(s.root, c);
    std::vector<std::string> drawn;
    for (const TextRecordingCanvas::Drawn &d : c.texts)
    {
        drawn.push_back(d.text);
    }
    EXPECT_EQ(s.text.VisibleLines(), (std::pair<std::size_t, std::size_t>{5000, 5006}));
    ASSERT_EQ(drawn.size(), 6u) << "half a line, four whole and the one cut at the bottom";
    EXPECT_EQ(drawn.front(), "line 5000");
    EXPECT_EQ(drawn.back(), "line 5005");
    EXPECT_FLOAT_EQ(c.texts.front().area.y(), 4.0f - 9.0f) << "the first line half above the area";
}

TEST(TextView, TheKeysMoveTheCaretACharacterAWordAndALineShiftSelecting)
{
    TextScene s;
    s.text.SetText("caf\xC3\xA9 au lait\nno sugar");
    s.text.Select(0, 0); // a fresh view puts the caret at the end of the text it is given
    s.Focus();
    EXPECT_EQ(s.text.caret(), 0u);
    s.Press(Key::kRight);
    s.Press(Key::kRight);
    s.Press(Key::kRight);
    EXPECT_EQ(s.text.caret(), 3u);
    s.Press(Key::kRight);
    EXPECT_EQ(s.text.caret(), 5u) << "over the two bytes of the e with its accent";
    s.Press(Key::kLeft, {.shift = true});
    EXPECT_EQ(s.text.selection(), (std::pair<std::size_t, std::size_t>{3, 5}));
    EXPECT_EQ(s.text.SelectedText(), "\xC3\xA9");
    s.Press(Key::kLeft);
    EXPECT_EQ(s.text.caret(), 3u) << "Left on a selection goes to its start";
    s.Press(Key::kRight, {.alt = true});
    EXPECT_EQ(s.text.caret(), 5u) << "the word's end";
    s.Press(Key::kRight, {.alt = true});
    EXPECT_EQ(s.text.caret(), 8u) << "past the space to the next word's end";
    s.Press(Key::kLeft, {.alt = true});
    EXPECT_EQ(s.text.caret(), 6u);
    s.Press(Key::kEnd);
    EXPECT_EQ(s.text.caret(), 13u) << "the line's end, before its line feed";
    s.Press(Key::kDown);
    EXPECT_EQ(s.text.caret(), 22u) << "no sugar is shorter: its end";
    s.Press(Key::kHome, {.shift = true});
    EXPECT_EQ(s.text.SelectedText(), "no sugar");
    s.Press(Key::kEnd, {.command = true});
    EXPECT_EQ(s.text.caret(), s.text.text().size());
    s.Press(Key::kHome, {.command = true});
    EXPECT_EQ(s.text.caret(), 0u);

    // Up and down keep the column the moves began in, across a shorter line.
    s.text.SetText("abcdefghij\nab\nabcdefghij");
    s.text.Select(8, 8);
    s.Press(Key::kDown);
    EXPECT_EQ(s.text.caret(), 13u) << "the short line's end";
    s.Press(Key::kDown);
    EXPECT_EQ(s.text.caret(), 14u + 8u) << "back at column 8";
    s.Press(Key::kDown);
    EXPECT_EQ(s.text.caret(), s.text.text().size()) << "past the last line, the end";
    s.Press(Key::kUp, {.command = true});
    EXPECT_EQ(s.text.caret(), 0u);

    // Read-only: no typing, no line feed, no deletion.
    EXPECT_FALSE(s.Type(U'x'));
    EXPECT_FALSE(s.Press(Key::kReturn));
    EXPECT_FALSE(s.Press(Key::kBackspace));
    EXPECT_TRUE(s.changes.empty());

    s.Type(U'a', {.command = true});
    s.Type(U'c', {.command = true});
    ASSERT_EQ(s.copied.size(), 1u);
    EXPECT_EQ(s.copied[0], s.text.text()) << "Command-A, Command-C";
}

TEST(TextView, AWrappedLinesEndIsTheNextLinesStart)
{
    TextScene s;
    s.text.SetText("aaaa bbbb cccc dddd eeee ffff gggg");
    s.text.Select(0, 0);
    ASSERT_EQ(s.text.Lines()[0], (TextLine{0, 25, true}));
    EXPECT_EQ(s.text.LineOf(25), 1u) << "the break belongs to the line after it";
    EXPECT_EQ(s.text.PointOf(25).x, 4.0f);
    s.Focus();
    s.Press(Key::kEnd);
    EXPECT_EQ(s.text.caret(), 24u) << "End on a wrapped line stops before its hanging space";
    EXPECT_EQ(s.text.LineOf(s.text.caret()), 0u);
}

TEST(TextView, ThePointerPlacesTheCaretExtendsAndSelectsAWord)
{
    TextScene s;
    s.text.SetText("Nf3 d5\ng3 Nf6");
    // Line 0's top is at 4; characters 7 px from x = 4.
    s.router.Down(PointF{4.0f + 7.0f * 2 + 2.0f, 10.0f}, 1, false, {});
    s.router.Up(PointF{4.0f + 7.0f * 2 + 2.0f, 10.0f}, {});
    EXPECT_EQ(s.router.focused(), &s.text);
    EXPECT_EQ(s.text.caret(), 2u) << "the nearer side of the 3";
    s.router.Down(PointF{4.0f + 7.0f * 2 + 5.0f, 10.0f}, 1, false, {});
    s.router.Up(PointF{4.0f + 7.0f * 2 + 5.0f, 10.0f}, {});
    EXPECT_EQ(s.text.caret(), 3u) << "past its middle, after it";
    s.router.Down(PointF{4.0f + 7.0f * 1, 4.0f + 18.0f + 5.0f}, 1, false, {.shift = true});
    s.router.Up(PointF{4.0f + 7.0f * 1, 4.0f + 18.0f + 5.0f}, {});
    EXPECT_EQ(s.text.SelectedText(), " d5\ng") << "Shift extends to the second line";
    s.router.Down(PointF{4.0f, 10.0f}, 1, false, {});
    s.router.Drag(PointF{4.0f + 7.0f * 6, 10.0f}, {});
    s.router.Up(PointF{4.0f + 7.0f * 6, 10.0f}, {});
    EXPECT_EQ(s.text.SelectedText(), "Nf3 d5") << "a drag";
    s.router.Down(PointF{190.0f, 99.0f}, 1, false, {});
    s.router.Up(PointF{190.0f, 99.0f}, {});
    EXPECT_EQ(s.text.caret(), s.text.text().size()) << "below the last line: the end";

    // A double click on a word selects it; just past its end, too.
    s.router.Down(PointF{4.0f + 7.0f * 5, 4.0f + 18.0f + 5.0f}, 1, false, {});
    s.router.Up(PointF{4.0f + 7.0f * 5, 4.0f + 18.0f + 5.0f}, {});
    s.router.Down(PointF{4.0f + 7.0f * 5, 4.0f + 18.0f + 5.0f}, 2, false, {});
    s.router.Up(PointF{4.0f + 7.0f * 5, 4.0f + 18.0f + 5.0f}, {});
    EXPECT_EQ(s.text.SelectedText(), "Nf6");
    view::MouseEvent past_the_end;
    past_the_end.position = PointF{4.0f + 7.0f * 6 + 1.0f, 10.0f};
    s.text.MouseDoubleClick(past_the_end);
    EXPECT_EQ(s.text.SelectedText(), "d5") << "past the line's end, the word before it";
}

TEST(TextView, TheWheelAndTheCaretScrollTheLines)
{
    TextScene   s;
    std::string many;
    for (int i = 0; i < 50; ++i)
    {
        many += std::to_string(i) + "\n";
    }
    s.text.SetText(many);
    EXPECT_TRUE(s.router.Wheel(PointF{50.0f, 50.0f}, 0.0f, -20.0f, {}));
    EXPECT_EQ(s.text.scroll(), 54) << "three 18-pixel lines a notch";
    s.Focus();
    s.Press(Key::kEnd, {.command = true});
    EXPECT_EQ(s.text.scroll(), s.text.MaxScroll()) << "the caret at the end, the end shown";
    s.Press(Key::kPageUp);
    EXPECT_EQ(s.text.LineOf(s.text.caret()), 50u - 4u) << "a page is the whole lines that show less one";
    s.Press(Key::kHome, {.command = true});
    EXPECT_EQ(s.text.scroll(), 0);
    TextScene few;
    few.text.SetText("short");
    view::MouseEvent notch;
    notch.wheel_y = -20.0f;
    EXPECT_FALSE(few.text.MouseWheel(notch)) << "nothing hidden: the wheel is the parent's";
}

TEST(TextView, EditableItTakesTypingLineFeedsDeletionsCutsAndPastes)
{
    TextScene s;
    s.text.SetEditable(true);
    s.text.SetText("1. e4");
    s.Focus();
    s.Press(Key::kEnd);
    s.Type(U' ');
    s.Type(U'c');
    s.Type(U'5');
    EXPECT_EQ(s.text.text(), "1. e4 c5");
    EXPECT_EQ(s.changes.size(), 3u);
    EXPECT_EQ(s.changes.back(), "1. e4 c5");
    s.Press(Key::kReturn);
    s.Type(char32_t{0xBD}); // one half: two bytes
    EXPECT_EQ(s.text.text(), "1. e4 c5\n\xC2\xBD");
    EXPECT_EQ(s.text.caret(), s.text.text().size());
    s.Press(Key::kBackspace);
    EXPECT_EQ(s.text.text(), "1. e4 c5\n") << "Backspace takes the whole character";
    s.Press(Key::kBackspace);
    EXPECT_EQ(s.text.text(), "1. e4 c5");
    s.Press(Key::kBackspace, {.alt = true});
    EXPECT_EQ(s.text.text(), "1. e4 ") << "Alt-Backspace takes the word";
    s.Press(Key::kHome);
    s.Press(Key::kDelete);
    EXPECT_EQ(s.text.text(), ". e4 ");
    s.Press(Key::kDelete, {.alt = true});
    EXPECT_EQ(s.text.text(), " e4 ") << "Alt-Delete to the word's end";

    // The selection replaced by typing; cut; paste with its line breaks made line feeds.
    s.text.Select(1, 3);
    s.Type(U'd');
    EXPECT_EQ(s.text.text(), " d ");
    s.Type(U'a', {.command = true});
    s.Type(U'x', {.command = true});
    EXPECT_EQ(s.text.text(), "");
    ASSERT_FALSE(s.copied.empty());
    EXPECT_EQ(s.copied.back(), " d ");
    s.host.clipboard = "[Event \"Club\"]\r\n[Site \"Home\"]";
    s.Type(U'v', {.command = true});
    EXPECT_EQ(s.text.text(), "[Event \"Club\"]\n[Site \"Home\"]");
    EXPECT_EQ(s.text.caret(), s.text.text().size());
    s.text.InsertText("\xFF");
    EXPECT_EQ(s.text.text(), "[Event \"Club\"]\n[Site \"Home\"]\xEF\xBF\xBD") << "a broken byte typed is repaired";

    // A control character, Tab and Escape are the window's.
    EXPECT_FALSE(s.Type(U'\x01'));
    EXPECT_FALSE(s.Press(Key::kTab));
    EXPECT_FALSE(s.Press(Key::kEscape));

    // Back to read-only: the edits stop.
    s.text.SetEditable(false);
    const std::string kept = s.text.text();
    s.text.InsertText("x");
    s.Type(U'v', {.command = true});
    EXPECT_EQ(s.text.text(), kept);
}

TEST(TextView, TheCaretShowsWhileEditableAndFocusedAndTheSelectionInItsColours)
{
    TextScene s;
    s.text.SetText("e4 e5");
    const TextView::Style &style     = s.text.style();
    const auto             has_caret = [&] {
        TextRecordingCanvas c;
        view::PaintTree(s.root, c);
        for (const TextRecordingCanvas::Fill &f : c.fills)
        {
            if (f.colour == style.caret && f.area.width() == 2)
            {
                return true;
            }
        }
        return false;
    };
    s.Focus();
    EXPECT_FALSE(has_caret()) << "read-only: no caret";
    s.text.SetEditable(true);
    EXPECT_TRUE(has_caret());
    s.host.window_focused = false;
    EXPECT_FALSE(has_caret()) << "not while the window lacks the keyboard";
    s.host.window_focused = true;

    s.text.Select(3, 5);
    TextRecordingCanvas c;
    view::PaintTree(s.root, c);
    bool lit = false;
    for (const TextRecordingCanvas::Fill &f : c.fills)
    {
        lit = lit || (f.colour == style.highlight && f.area == RectI{4 + 21, 4, 14, 18});
    }
    EXPECT_TRUE(lit) << "the selection's fill behind e5";
    int highlighted = 0;
    for (const TextRecordingCanvas::Drawn &d : c.texts)
    {
        highlighted += d.colour == style.highlighted_text ? 1 : 0;
    }
    EXPECT_EQ(highlighted, 1) << "the line drawn again in the highlighted colour, clipped to the selection";
}
