// The list of many rows (view/ListView.h): its columns laid out fixed and by share, the header, the scroll bar
// beside the rows only while some are hidden; a paint asking the model for the rows that show and no others, however
// many there are; the selection by keys and by the pointer, single and in ranges; the clicks; the wheel; a row
// scrolled into view; an engine's log kept at its bottom as rows arrive unless the person has scrolled up; the
// selection copied as text; and a cell's own colours.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ListView.h"
#include "TextViewTestSupport.h"

namespace
{
using reckless::gfx::Color;
using reckless::gfx::PointF;
using reckless::gfx::RectI;
using reckless::test::TextHost;
using reckless::test::TextRecordingCanvas;
namespace view = reckless::view;
using view::Key;
using view::ListView;

/** Rows of three cells, "r<row>c<column>", counting every cell asked for; a cell's colour and fill where a test sets
    them. */
class CountingModel final : public view::ListModel
{
  public:
    explicit CountingModel(std::size_t rows) : rows(rows)
    {
    }

    std::size_t                      rows;
    mutable std::vector<std::size_t> asked; ///< the rows asked for, once per cell
    std::optional<std::size_t>       red_row;
    std::optional<std::size_t>       painted_row; ///< a row the model paints itself

    std::size_t RowCount() const override
    {
        return rows;
    }

    std::string CellText(std::size_t row, std::size_t column) const override
    {
        asked.push_back(row);
        return "r" + std::to_string(row) + "c" + std::to_string(column);
    }

    std::optional<Color> CellColour(std::size_t row, std::size_t column) const override
    {
        if (red_row == row && column == 1)
        {
            return Color::FromRgb(255, 0, 0);
        }
        return std::nullopt;
    }

    std::optional<Color> CellBackground(std::size_t row, std::size_t column) const override
    {
        if (red_row == row && column == 2)
        {
            return Color::FromRgb(0, 0, 255);
        }
        return std::nullopt;
    }

    bool PaintRow(reckless::gfx::Canvas &c, std::size_t row, RectI bounds, bool) override
    {
        if (painted_row != row)
        {
            return false;
        }
        c.SetColor(Color::FromRgb(0, 255, 0));
        c.FillRect(bounds);
        return true;
    }
};

/** A list in a window of its own: 300 x 222, a 22 px header and 20 px rows, so ten rows show. */
struct ListScene
{
    TextHost          host;
    view::View        root;
    view::MouseRouter router{root, &host};
    CountingModel     model;
    ListView          list{ListView::Style{}};

    explicit ListScene(std::size_t rows, bool columns = true) : model(rows)
    {
        root.SetBounds({300, 222});
        root.SetHost(&host);
        host.router = &router;
        list.SetBounds({0, 0, 300, 222});
        root.AddChild(list);
        if (columns)
        {
            list.SetColumns({{"No.", 40}, {"White", 0, 1.0f}, {"Black", 0, 1.0f}});
        }
        list.SetModel(&model);
    }

    void Click(int x, int y, view::Modifiers modifiers = {})
    {
        router.Down(PointF{static_cast<float>(x), static_cast<float>(y)}, 1, false, modifiers);
        router.Up(PointF{static_cast<float>(x), static_cast<float>(y)}, modifiers);
    }

    void Press(Key key, view::Modifiers modifiers = {})
    {
        router.Key({key, 0, modifiers});
    }
};

std::vector<std::size_t> Distinct(std::vector<std::size_t> rows)
{
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}
} // namespace

TEST(RowSelection, RangesMergeSplitAndStaySorted)
{
    view::RowSelection s;
    EXPECT_TRUE(s.empty());
    s.Add(10, 20);
    s.Add(30, 40);
    s.Add(20, 25); // touches the first: merged
    ASSERT_EQ(s.ranges().size(), 2u);
    EXPECT_EQ(s.ranges()[0], (view::RowSelection::Range{10, 25}));
    s.Add(5, 35); // swallows both
    ASSERT_EQ(s.ranges().size(), 1u);
    EXPECT_EQ(s.ranges()[0], (view::RowSelection::Range{5, 40}));
    s.Remove(12, 14);
    ASSERT_EQ(s.ranges().size(), 2u);
    EXPECT_TRUE(s.Contains(11));
    EXPECT_FALSE(s.Contains(12));
    EXPECT_FALSE(s.Contains(13));
    EXPECT_TRUE(s.Contains(14));
    EXPECT_EQ(s.Count(), 33u);
    s.Toggle(12);
    EXPECT_TRUE(s.Contains(12));
    s.Toggle(12);
    EXPECT_FALSE(s.Contains(12));
    s.ClampTo(20);
    EXPECT_EQ(s.Count(), 13u) << "5..12 and 14..20";
    EXPECT_EQ(s.First(), 5u);
    s.Set(3, 4);
    EXPECT_EQ(s.Count(), 1u);
    s.Add(7, 7);
    EXPECT_EQ(s.Count(), 1u) << "an empty range adds nothing";
}

TEST(ListView, ColumnsAreFixedOrShareWhatIsLeftAndTheHeaderSitsOverThem)
{
    ListScene s(5);
    EXPECT_EQ(s.list.HeaderHeight(), 22);
    EXPECT_FALSE(s.list.scroll_bar().visible()) << "five rows fit: no bar";
    EXPECT_EQ(s.list.RowsArea(), (RectI{0, 22, 300, 200}));
    const std::vector<RectI> columns = s.list.ColumnBounds();
    ASSERT_EQ(columns.size(), 3u);
    EXPECT_EQ(columns[0], (RectI{0, 0, 40, 22}));
    EXPECT_EQ(columns[1], (RectI{40, 0, 130, 22}));
    EXPECT_EQ(columns[2], (RectI{170, 0, 130, 22}));
    EXPECT_EQ(s.list.ColumnAt(39), 0u);
    EXPECT_EQ(s.list.ColumnAt(40), 1u);
    EXPECT_EQ(s.list.ColumnAt(299), 2u);

    s.model.rows = 50;
    s.list.RowsChanged();
    EXPECT_TRUE(s.list.scroll_bar().visible()) << "fifty rows do not fit";
    EXPECT_EQ(s.list.RowsArea(), (RectI{0, 22, 290, 200})) << "the rows give the bar its width";
    EXPECT_EQ(s.list.scroll_bar().bounds(), (RectI{290, 22, 10, 200})) << "beside the rows, under the header";
    EXPECT_EQ(s.list.ColumnBounds()[2], (RectI{165, 0, 125, 22}));

    ListScene plain(3, false);
    EXPECT_EQ(plain.list.HeaderHeight(), 0) << "no columns, no header";
    EXPECT_EQ(plain.list.ColumnCount(), 1u);
    EXPECT_EQ(plain.list.ColumnBounds()[0], (RectI{0, 0, 300, 0}));
}

TEST(ListView, APaintAsksForTheRowsThatShowAndNoOthers)
{
    ListScene s(50000);
    {
        TextRecordingCanvas c;
        reckless::view::PaintTree(s.root, c);
        EXPECT_EQ(Distinct(s.model.asked), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}))
            << "ten rows of fifty thousand";
        EXPECT_EQ(s.model.asked.size(), 30u) << "three cells each";
    }
    s.model.asked.clear();
    s.list.SetScroll(25000 * 20 + 10); // half way, a row cut at the top and another at the bottom
    {
        TextRecordingCanvas c;
        reckless::view::PaintTree(s.root, c);
        const std::vector<std::size_t> rows = Distinct(s.model.asked);
        ASSERT_EQ(rows.size(), 11u) << "the row cut at the top, nine whole, the one cut at the bottom";
        EXPECT_EQ(rows.front(), 25000u);
        EXPECT_EQ(rows.back(), 25010u);
        EXPECT_EQ(s.list.VisibleRows(), (std::pair<std::size_t, std::size_t>{25000, 25011}));
        // The first row drawn starts half a row above the rows' area.
        ASSERT_FALSE(c.texts.empty());
        bool found = false;
        for (const TextRecordingCanvas::Drawn &d : c.texts)
        {
            if (d.text == "r25000c0")
            {
                found = true;
                EXPECT_FLOAT_EQ(d.area.y(), 12.0f) << "22 - 10";
            }
        }
        EXPECT_TRUE(found);
    }
    s.model.asked.clear();
    s.list.ScrollToBottom();
    {
        TextRecordingCanvas c;
        reckless::view::PaintTree(s.root, c);
        const std::vector<std::size_t> rows = Distinct(s.model.asked);
        ASSERT_EQ(rows.size(), 10u);
        EXPECT_EQ(rows.back(), 49999u);
    }
}

TEST(ListView, TheKeysMoveTheSelectionAndShiftExtendsIt)
{
    ListScene s(100);
    s.list.SetSelectionMode(ListView::SelectionMode::kMultiple);
    s.Click(100, 22 + 3 * 20 + 5); // row 3
    EXPECT_EQ(s.router.focused(), &s.list) << "a press gives a selecting list the keyboard";
    EXPECT_EQ(s.list.current_row(), 3u);
    s.Press(Key::kDown);
    EXPECT_EQ(s.list.current_row(), 4u);
    EXPECT_EQ(s.list.selection().Count(), 1u);
    s.Press(Key::kDown, {.shift = true});
    s.Press(Key::kDown, {.shift = true});
    EXPECT_EQ(s.list.current_row(), 6u);
    ASSERT_EQ(s.list.selection().ranges().size(), 1u);
    EXPECT_EQ(s.list.selection().ranges()[0], (view::RowSelection::Range{4, 7})) << "from the anchor to the current";
    s.Press(Key::kUp, {.shift = true});
    EXPECT_EQ(s.list.selection().ranges()[0], (view::RowSelection::Range{4, 6}));
    s.Press(Key::kPageDown);
    EXPECT_EQ(s.list.current_row(), 5u + 9u) << "a page is the rows that show less one";
    EXPECT_EQ(s.list.selection().Count(), 1u) << "no shift: one row";
    EXPECT_EQ(s.list.scroll(), 14 * 20 + 20 - 200) << "scrolled as little as shows it";
    s.Press(Key::kEnd);
    EXPECT_EQ(s.list.current_row(), 99u);
    EXPECT_TRUE(s.list.AtBottom());
    s.Press(Key::kHome, {.shift = true});
    EXPECT_EQ(s.list.selection().Count(), 100u) << "Shift-Home from the end: every row";
    EXPECT_EQ(s.list.scroll(), 0);
    s.Press(Key::kPageUp);
    EXPECT_EQ(s.list.current_row(), 0u);
    s.router.Key({Key::kCharacter, U'a', {.command = true}});
    EXPECT_EQ(s.list.selection().Count(), 100u) << "Command-A";

    // One row at a time: Shift extends nothing.
    s.list.SetSelectionMode(ListView::SelectionMode::kSingle);
    EXPECT_TRUE(s.list.selection().empty()) << "a change of mode clears the selection";
    s.Press(Key::kDown);
    EXPECT_EQ(s.list.current_row(), 0u) << "with no current row, the first";
    s.Press(Key::kDown, {.shift = true});
    EXPECT_EQ(s.list.selection().Count(), 1u);
    EXPECT_EQ(s.list.current_row(), 1u);
    std::optional<std::size_t> activated;
    s.list.on_activate = [&](std::size_t row) { activated = row; };
    s.Press(Key::kReturn);
    EXPECT_EQ(activated, 1u);

    // Selecting nothing: no keyboard, no keys.
    s.list.SetSelectionMode(ListView::SelectionMode::kNone);
    EXPECT_FALSE(s.list.WantsKeyboardFocus());
    EXPECT_FALSE(s.list.KeyPressed({Key::kDown, 0, {}}));
}

TEST(ListView, ThePointerSelectsExtendsTogglesAndDrags)
{
    ListScene s(100);
    s.list.SetSelectionMode(ListView::SelectionMode::kMultiple);
    int changes                 = 0;
    s.list.on_selection_changed = [&] { ++changes; };
    std::vector<std::pair<std::size_t, std::size_t>> clicks;
    s.list.on_click = [&](std::size_t row, std::size_t column) { clicks.emplace_back(row, column); };

    s.Click(10, 22 + 2 * 20 + 1);
    EXPECT_TRUE(s.list.IsSelected(2));
    ASSERT_EQ(clicks.size(), 1u);
    EXPECT_EQ(clicks[0], (std::pair<std::size_t, std::size_t>{2, 0}));
    s.Click(200, 22 + 5 * 20 + 1, {.shift = true});
    EXPECT_EQ(s.list.selection().ranges()[0], (view::RowSelection::Range{2, 6}));
    EXPECT_EQ(clicks.back(), (std::pair<std::size_t, std::size_t>{5, 2}));
    s.Click(200, 22 + 8 * 20 + 1, {.command = true});
    EXPECT_EQ(s.list.selection().Count(), 5u) << "Command adds a row";
    s.Click(200, 22 + 3 * 20 + 1, {.command = true});
    EXPECT_FALSE(s.list.IsSelected(3)) << "and takes one away";
    EXPECT_EQ(s.list.selection().Count(), 4u);
    EXPECT_GE(changes, 4);

    // A drag from row 1 down past the bottom selects to the last row it reaches and scrolls it into view.
    s.router.Down(PointF{100.0f, 22.0f + 20.0f + 5.0f}, 1, false, {});
    s.router.Drag(PointF{100.0f, 22.0f + 4 * 20.0f + 5.0f}, {});
    EXPECT_EQ(s.list.selection().ranges()[0], (view::RowSelection::Range{1, 5}));
    s.router.Drag(PointF{100.0f, 240.0f}, {});
    EXPECT_EQ(s.list.current_row(), 10u) << "the row under the pointer, below the area";
    EXPECT_EQ(s.list.scroll(), 20) << "scrolled to show it";
    s.router.Up(PointF{100.0f, 240.0f}, {});
    EXPECT_EQ(s.list.selection().ranges()[0], (view::RowSelection::Range{1, 11}));

    // A double click.
    std::optional<std::pair<std::size_t, std::size_t>> twice;
    s.list.on_double_click = [&](std::size_t row, std::size_t column) { twice.emplace(row, column); };
    s.router.Down(PointF{50.0f, 30.0f}, 1, false, {});
    s.router.Up(PointF{50.0f, 30.0f}, {});
    s.router.Down(PointF{50.0f, 30.0f}, 2, false, {});
    s.router.Up(PointF{50.0f, 30.0f}, {});
    ASSERT_TRUE(twice.has_value());
    EXPECT_EQ(*twice, (std::pair<std::size_t, std::size_t>{1, 1})) << "scrolled by a row: the second row shows first";

    // A right click on an unselected row selects it, then tells the owner where.
    std::optional<std::size_t> context;
    s.list.on_context_click = [&](std::optional<std::size_t> row, reckless::gfx::PointI) { context = row; };
    s.router.Down(PointF{50.0f, 22.0f + 6 * 20.0f + 1.0f}, 1, true, {});
    s.router.Up(PointF{50.0f, 22.0f + 6 * 20.0f + 1.0f}, {});
    EXPECT_EQ(context, 7u);
    EXPECT_EQ(s.list.selection().Count(), 1u);

    // The header.
    std::optional<std::size_t> header;
    s.list.on_header_click = [&](std::size_t column) { header = column; };
    s.Click(250, 10);
    EXPECT_EQ(header, 2u);
}

TEST(ListView, AListThatSelectsOneRowMovesItWithTheDrag)
{
    ListScene s(30);
    s.router.Down(PointF{10.0f, 30.0f}, 1, false, {});
    s.router.Drag(PointF{10.0f, 22.0f + 3 * 20.0f + 1.0f}, {.shift = true});
    s.router.Up(PointF{10.0f, 22.0f + 3 * 20.0f + 1.0f}, {});
    EXPECT_EQ(s.list.selection().Count(), 1u);
    EXPECT_TRUE(s.list.IsSelected(3));
}

TEST(ListView, TheWheelScrollsThreeRowsANotchAndIsHandedOnWhenAllShow)
{
    ListScene s(100);
    EXPECT_TRUE(s.router.Wheel(PointF{100.0f, 100.0f}, 0.0f, -20.0f, {}));
    EXPECT_EQ(s.list.scroll(), 60) << "a 20-pixel notch down: three 20-pixel rows";
    EXPECT_TRUE(s.router.Wheel(PointF{100.0f, 100.0f}, 0.0f, 20.0f, {}));
    EXPECT_EQ(s.list.scroll(), 0);
    ListScene        few(3);
    view::MouseEvent notch;
    notch.wheel_y = -20.0f;
    EXPECT_FALSE(few.list.MouseWheel(notch)) << "nothing hidden: the wheel is the parent's";
    s.list.ScrollToRow(50);
    EXPECT_EQ(s.list.scroll(), 50 * 20 + 20 - 200) << "a row below the view at the bottom";
    s.list.ScrollToRow(40);
    EXPECT_EQ(s.list.scroll(), 40 * 20) << "a row above it at the top";
    s.list.ScrollToRow(45);
    EXPECT_EQ(s.list.scroll(), 40 * 20) << "a row in view moves nothing";
    s.list.scroll_bar().on_move(500.0);
    EXPECT_EQ(s.list.scroll(), 500) << "the bar moves the rows";
}

TEST(ListView, ALogStaysAtItsBottomAsRowsArriveUnlessScrolledUp)
{
    ListScene s(5, false);
    s.list.stick_to_bottom = true;
    EXPECT_TRUE(s.list.AtBottom()) << "every row showing is at the bottom";
    s.model.rows = 40;
    s.list.RowsChanged();
    EXPECT_EQ(s.list.scroll(), 40 * 20 - 222) << "followed the new rows";
    s.model.rows = 41;
    s.list.RowsChanged();
    EXPECT_EQ(s.list.scroll(), 41 * 20 - 222);

    s.router.Wheel(PointF{100.0f, 100.0f}, 0.0f, 20.0f, {}); // the person scrolls up
    const int reading = s.list.scroll();
    EXPECT_FALSE(s.list.AtBottom());
    s.model.rows = 60;
    s.list.RowsChanged();
    EXPECT_EQ(s.list.scroll(), reading) << "reading back: left where it is";

    s.list.ScrollToBottom();
    s.list.SetBounds({0, 0, 300, 122});
    EXPECT_EQ(s.list.scroll(), 60 * 20 - 122) << "a resize keeps it at the bottom too";

    ListScene loose(5, false);
    loose.model.rows = 40;
    loose.list.RowsChanged();
    EXPECT_EQ(loose.list.scroll(), 0) << "without stick_to_bottom the top stays";

    // Shrinking holds the selection and the scroll to what is left.
    loose.list.SetSelectionMode(ListView::SelectionMode::kMultiple);
    loose.list.SelectRange(10, 30);
    loose.list.ScrollToBottom();
    loose.model.rows = 15;
    loose.list.RowsChanged();
    EXPECT_EQ(loose.list.selection().ranges()[0], (view::RowSelection::Range{10, 15}));
    EXPECT_EQ(loose.list.current_row(), 14u);
    EXPECT_EQ(loose.list.scroll(), 15 * 20 - 222);
}

TEST(ListView, TheSelectionCopiesAsTabbedLines)
{
    ListScene s(10);
    s.list.SetSelectionMode(ListView::SelectionMode::kMultiple);
    s.list.SelectRange(2, 3);
    EXPECT_EQ(s.list.SelectedText(), "r2c0\tr2c1\tr2c2\nr3c0\tr3c1\tr3c2");
    std::string copied;
    s.list.on_copy = [&](std::string text) { copied = std::move(text); };
    s.router.Focus(&s.list); // the keyboard, the range kept
    s.router.Key({Key::kCharacter, U'c', {.command = true}});
    EXPECT_EQ(copied, s.list.SelectedText());
    s.list.ClearSelection();
    copied = "kept";
    s.list.CopySelection();
    EXPECT_EQ(copied, "kept") << "nothing selected, nothing copied";
}

TEST(ListView, ACellCarriesItsOwnColoursAndAModelMayPaintARowWhole)
{
    ListScene s(10);
    s.list.SetSelectionMode(ListView::SelectionMode::kSingle);
    s.model.red_row     = 1;
    s.model.painted_row = 3;
    s.list.Select(2);
    TextRecordingCanvas c;
    reckless::view::PaintTree(s.root, c);
    const ListView::Style &style     = s.list.style();
    const auto             colour_of = [&](const std::string &text) -> std::optional<Color> {
        for (const TextRecordingCanvas::Drawn &d : c.texts)
        {
            if (d.text == text)
            {
                return d.colour;
            }
        }
        return std::nullopt;
    };
    EXPECT_EQ(colour_of("r0c1"), style.text);
    EXPECT_EQ(colour_of("r1c1"), Color::FromRgb(255, 0, 0)) << "the model's colour for the cell";
    EXPECT_EQ(colour_of("r1c0"), style.text) << "and only that cell";
    EXPECT_EQ(colour_of("r2c0"), style.selected_text) << "a selected row";
    EXPECT_FALSE(colour_of("r3c0").has_value()) << "the model painted row 3 itself";
    const auto filled = [&](RectI area, Color colour) {
        for (const TextRecordingCanvas::Fill &f : c.fills)
        {
            if (f.area == area && f.colour == colour)
            {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(filled({170, 42, 130, 20}, Color::FromRgb(0, 0, 255))) << "the cell's own fill";
    EXPECT_TRUE(filled({0, 62, 300, 20}, style.selection)) << "the selection behind row 2";
    EXPECT_TRUE(filled({0, 82, 300, 20}, Color::FromRgb(0, 255, 0))) << "the model's row";
    EXPECT_TRUE(filled({0, 0, 300, 22}, style.header_background));
    EXPECT_TRUE(colour_of("White") == style.header_text);
}
