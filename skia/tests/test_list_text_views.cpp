// The views that show many lines, drawn on Skia in the series' look (panel/ListStyles.h): a list's header on the
// panel's colour, its rows on the plate's, every other row striped, the selected row lit in the accent and its cells'
// text drawn in the serif; lines of text wrapped where Skia's own measure says they fit, the selection behind them in
// the accent and the caret in it; and, when RECKLESS_SCREENSHOT_DIR is set, the review image of each - a game's moves,
// an engine's analysis lines with their scores coloured, a crosstable and an engine's log kept at its bottom; a game's
// score as text, part of it selected, and a position's notation being edited.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "ListStyles.h"
#include "ListView.h"
#include "LookPalette.h"
#include "SkiaCanvas.h"
#include "TextView.h"
#include "View.h"

#include <gtest/gtest.h>

namespace
{
using namespace reckless;
using gfx::Color;
using gfx::RectI;

std::vector<std::uint8_t> ReadFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void LoadFaces()
{
    static const bool loaded = [] {
        gfx::SetSerifFaces(ReadFile(RECKLESS_ASSETS_DIR "/fonts/NotoSerif-Regular.ttf"),
                           ReadFile(RECKLESS_ASSETS_DIR "/fonts/NotoSerif-Bold.ttf"));
        return true;
    }();
    (void)loaded;
}

/** The directory the review images go to, or null when none is asked for. */
const char *ScreenshotDir()
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: a test has no use for _dupenv_s's copy
#endif
    return std::getenv("RECKLESS_SCREENSHOT_DIR");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

/** A chess program's palette: a dark panel, a darker screen, the series' cream and amber. */
const look::Palette kPalette = look::Palette::Series(Color(0xff2a2a2e), Color(0xff17171a));

/** A host that measures text as Skia draws it, as a window's does. */
class MeasuringHost final : public view::Host
{
  public:
    void Invalidate(RectI) override
    {
    }

    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        return gfx::MeasureText(font, text);
    }
};

/** Rows of text cells from a table, with a colour per cell where the function gives one. */
class TableModel final : public view::ListModel
{
  public:
    std::vector<std::vector<std::string>>                         rows;
    std::function<std::optional<Color>(std::size_t, std::size_t)> colour;
    std::function<std::optional<Color>(std::size_t, std::size_t)> fill;

    std::size_t RowCount() const override
    {
        return rows.size();
    }

    std::string CellText(std::size_t row, std::size_t column) const override
    {
        return column < rows[row].size() ? rows[row][column] : std::string();
    }

    std::optional<Color> CellColour(std::size_t row, std::size_t column) const override
    {
        return colour ? colour(row, column) : std::nullopt;
    }

    std::optional<Color> CellBackground(std::size_t row, std::size_t column) const override
    {
        return fill ? fill(row, column) : std::nullopt;
    }
};

/** A caption over a view, in the ink's bold. */
class Caption final : public view::View
{
  public:
    explicit Caption(std::string text) : text_(std::move(text))
    {
    }

    void Paint(gfx::Canvas &c) override
    {
        c.SetFont(gfx::Font(13.0f).Bold());
        c.SetColor(kPalette.ink);
        c.DrawText(text_, LocalBounds().ToFloat(), gfx::Justify::kCentredLeft, true);
    }

  private:
    std::string text_;
};

/** A surface of `w` x `h`, cleared to black, and a pixel reader. */
struct Surface
{
    sk_sp<SkSurface> surface;

    Surface(int w, int h) : surface(SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h)))
    {
        surface->getCanvas()->clear(SK_ColorBLACK);
    }

    SkColor At(int x, int y)
    {
        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32Premul(1, 1));
        surface->readPixels(bitmap, x, y);
        return bitmap.getColor(0, 0);
    }
};

SkColor Sk(Color c)
{
    return SkColorSetARGB(c.alpha(), c.red(), c.green(), c.blue());
}

/** `over` at its alpha on opaque `under`, as Skia blends it, to the nearest level. */
SkColor Blend(Color under, Color over)
{
    const float a   = over.AlphaFraction();
    const auto  mix = [a](int u, int o) {
        return static_cast<U8CPU>(static_cast<float>(u) + (static_cast<float>(o) - static_cast<float>(u)) * a + 0.5f);
    };
    return SkColorSetRGB(mix(under.red(), over.red()), mix(under.green(), over.green()),
                         mix(under.blue(), over.blue()));
}

bool Near(SkColor a, SkColor b, int tolerance = 2)
{
    return std::abs(static_cast<int>(SkColorGetR(a)) - static_cast<int>(SkColorGetR(b))) <= tolerance &&
           std::abs(static_cast<int>(SkColorGetG(a)) - static_cast<int>(SkColorGetG(b))) <= tolerance &&
           std::abs(static_cast<int>(SkColorGetB(a)) - static_cast<int>(SkColorGetB(b))) <= tolerance;
}

/** A game's first moves as a move list's rows: the move's number, White's and Black's. */
TableModel Moves()
{
    TableModel  m;
    const char *moves[][3] = {{"1.", "e4", "c5"},       {"2.", "Nf3", "d6"}, {"3.", "d4", "cxd4"},
                              {"4.", "Nxd4", "Nf6"},    {"5.", "Nc3", "a6"}, {"6.", "Be3", "e5"},
                              {"7.", "Nb3", "Be6"},     {"8.", "f3", "Be7"}, {"9.", "Qd2", "O-O"},
                              {"10.", "O-O-O", "Nbd7"}, {"11.", "g4", "b5"}, {"12.", "g5", "b4"}};
    for (const auto &row : moves)
    {
        m.rows.push_back({row[0], row[1], row[2]});
    }
    return m;
}
} // namespace

TEST(ListViewOnSkia, TheHeaderTheRowsAndTheSelectionLandInThePalette)
{
    LoadFaces();
    MeasuringHost  host;
    TableModel     model = Moves();
    view::ListView list(panel::ListViewStyle(kPalette, gfx::Font(14.0f)));
    list.SetHost(&host);
    list.SetBounds({0, 0, 240, 200});
    list.SetColumns({{"No.", 44}, {"White", 0, 1.0f}, {"Black", 0, 1.0f}});
    list.SetModel(&model);
    list.Select(2);
    const view::ListView::Style &style = list.style();
    ASSERT_EQ(style.row_height, 21);
    ASSERT_EQ(style.header_height, 23);

    Surface         s(240, 200);
    gfx::SkiaCanvas c(*s.surface->getCanvas());
    view::PaintTree(list, c);
    EXPECT_EQ(s.At(236, 4), Sk(kPalette.panel)) << "the header on the panel's colour";
    const int row0 = 23 + 10;
    const int row1 = 23 + 21 + 10;
    const int row2 = 23 + 2 * 21 + 10;
    ASSERT_TRUE(list.scroll_bar().visible()) << "twelve rows do not fit: the bar takes the right 10 px";
    EXPECT_TRUE(Near(s.At(215, row0), Sk(kPalette.plate))) << "a row on the plate's colour";
    EXPECT_TRUE(Near(s.At(215, row1), Blend(kPalette.plate, style.stripe))) << "every other row striped";
    EXPECT_TRUE(Near(s.At(215, row2), Blend(kPalette.plate, style.selection), 3)) << "the selected row in the accent";

    // The serif drew the cells' text: ink pixels in the second column of the first row, none past the text.
    bool inked = false;
    for (int x = 44; x < 100 && !inked; ++x)
    {
        for (int y = 23; y < 44 && !inked; ++y)
        {
            inked = SkColorGetR(s.At(x, y)) > 150;
        }
    }
    EXPECT_TRUE(inked) << "e4 drawn in the ink";
}

TEST(ListViewOnSkia, ReviewImage)
{
    const char *dir = ScreenshotDir();
    if (dir == nullptr)
    {
        GTEST_SKIP() << "set RECKLESS_SCREENSHOT_DIR to render the review image";
    }
    LoadFaces();
    MeasuringHost host;
    view::View    root;
    root.SetHost(&host);
    root.SetBounds({880, 560});

    const gfx::Font font(14.0f);

    // A game's moves, the move it stands at lit.
    TableModel moves = Moves();
    moves.fill       = [](std::size_t row, std::size_t column) -> std::optional<Color> {
        return row == 8 && column == 2 ? std::optional<Color>(kPalette.accent.WithAlpha(0.45f)) : std::nullopt;
    };
    Caption        moves_caption("MOVES");
    view::ListView moves_list(panel::ListViewStyle(kPalette, font));
    moves_list.SetSelectionMode(view::ListView::SelectionMode::kNone);
    moves_list.SetColumns({{"", 44, 1.0f, gfx::Justify::kCentredRight}, {"White", 0}, {"Black", 0}});

    // An engine's analysis lines, the scores coloured by who they favour.
    TableModel analysis;
    analysis.rows   = {{"24", "+0.42", "Be2 Nb6 Kb1 Rc8 h4 Nc4 Bxc4 Rxc4"},
                       {"24", "+0.31", "Kb1 Rc8 Nd5 Bxd5 exd5 Nb6 Bxb6 Qxb6"},
                       {"23", "+0.08", "h4 b4 Nd5 Nxd5 exd5 Bf5 Bd3 Bxd3"},
                       {"23", "-0.27", "Qe1 Rc8 Kb1 Nb6 Na5 Qc7 Nb3 Nc4"}};
    analysis.colour = [](std::size_t row, std::size_t column) -> std::optional<Color> {
        if (column != 1)
        {
            return std::nullopt;
        }
        return row == 3 ? Color(0xffe07060) : Color(0xff8cc0ae);
    };
    Caption        analysis_caption("ANALYSIS");
    view::ListView analysis_list(panel::ListViewStyle(kPalette, font));
    analysis_list.SetColumns(
        {{"Depth", 56, 1.0f, gfx::Justify::kCentred}, {"Score", 64, 1.0f, gfx::Justify::kCentredRight}, {"Line", 0}});

    // A tournament's crosstable.
    TableModel        table;
    const std::string half = "\xC2\xBD"; // one half, U+00BD
    table.rows             = {{"1", "Pawnstar 2.1", "*", "1", half, "1", "2" + half},
                              {"2", "Zenith 0.9", "0", "*", "1", "1", "2"},
                              {"3", "Stoopid 1.4", half, "0", "*", "1", "1" + half},
                              {"4", "Pawnstar-Go 0.3", "0", "0", "0", "*", "0"}};
    table.colour           = [](std::size_t row, std::size_t column) -> std::optional<Color> {
        return column == static_cast<std::size_t>(row + 2) ? std::optional<Color>(kPalette.ink.WithAlpha(0.35f))
                                                           : std::nullopt;
    };
    Caption        table_caption("CROSSTABLE");
    view::ListView table_list(panel::ListViewStyle(kPalette, font));
    table_list.SetColumns({{"#", 28, 1.0f, gfx::Justify::kCentred},
                           {"Engine", 0},
                           {"1", 34, 1.0f, gfx::Justify::kCentred},
                           {"2", 34, 1.0f, gfx::Justify::kCentred},
                           {"3", 34, 1.0f, gfx::Justify::kCentred},
                           {"4", 34, 1.0f, gfx::Justify::kCentred},
                           {"Score", 56, 1.0f, gfx::Justify::kCentredRight}});

    // An engine's log, a thousand lines, kept at its bottom.
    TableModel log;
    for (int i = 0; i < 1000; ++i)
    {
        const int depth = 1 + i % 30;
        log.rows.push_back({i % 7 == 0 ? "> go wtime 61000 btime 59000 winc 1000 binc 1000"
                                       : "< info depth " + std::to_string(depth) + " score cp " +
                                             std::to_string(20 + (i * 37) % 40) + " nodes " +
                                             std::to_string(1000 * (i + 1)) + " pv e2e4 c7c5 g1f3"});
    }
    log.colour = [&log](std::size_t row, std::size_t) -> std::optional<Color> {
        return log.rows[row][0][0] == '>' ? std::optional<Color>(kPalette.accent) : std::nullopt;
    };
    Caption        log_caption("ENGINE LOG");
    view::ListView log_list(panel::ListViewStyle(kPalette, gfx::Font(12.0f)));
    log_list.stick_to_bottom = true;
    log_list.SetSelectionMode(view::ListView::SelectionMode::kMultiple);

    struct Placed
    {
        Caption        &caption;
        view::ListView &list;
        RectI           bounds;
    };

    const Placed placed[] = {{moves_caption, moves_list, {24, 40, 240, 290}},
                             {analysis_caption, analysis_list, {288, 40, 568, 130}},
                             {table_caption, table_list, {288, 210, 568, 120}},
                             {log_caption, log_list, {24, 370, 832, 166}}};
    for (const Placed &p : placed)
    {
        p.caption.SetBounds({p.bounds.x(), p.bounds.y() - 24, p.bounds.width(), 20});
        p.list.SetBounds(p.bounds);
        root.AddChild(p.caption);
        root.AddChild(p.list);
    }
    moves_list.SetModel(&moves);
    analysis_list.SetModel(&analysis);
    analysis_list.Select(0);
    table_list.SetModel(&table);
    log_list.SetModel(&log);
    log_list.SelectRange(996, 997);
    EXPECT_TRUE(log_list.AtBottom());

    gfx::Image image = gfx::MakeImage(2 * root.width(), 2 * root.height());
    auto       c     = gfx::MakeImageCanvas(image, 2.0f);
    c->FillAll(kPalette.panel);
    view::PaintTree(root, *c);
    const auto png = gfx::EncodePng(image);
    std::ofstream(std::string(dir) + "/list-view.png", std::ios::binary)
        .write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
}

namespace
{
/** A game's score as a product would show it: its tag pairs, a blank line and its moves, one paragraph. */
const char *const kGame = "[Event \"Club championship\"]\n"
                          "[Site \"Home\"]\n"
                          "[White \"Pawnstar 2.1\"]\n"
                          "[Black \"Zenith 0.9\"]\n"
                          "[Result \"*\"]\n"
                          "\n"
                          "1. e4 c5 2. Nf3 d6 3. d4 cxd4 4. Nxd4 Nf6 5. Nc3 a6 6. Be3 e5 7. Nb3 Be6 8. f3 Be7 "
                          "9. Qd2 O-O 10. O-O-O Nbd7 11. g4 b5 12. g5 b4 13. Ne2 Ne8 14. f4 a5 15. f5 a4 "
                          "16. Nbd4 exd4 17. Nxd4 b3 18. Kb1 bxc2+ 19. Nxc2 Bb3 20. axb3 axb3 21. Na3 *";
} // namespace

TEST(TextViewOnSkia, LinesWrapWhereSkiaSaysTheyFitAndTheSelectionAndTheCaretAreLit)
{
    LoadFaces();
    MeasuringHost     host;
    view::View        root;
    view::MouseRouter router(root, nullptr);
    root.SetHost(&host);
    root.SetBounds({320, 200});
    view::TextView text(panel::TextViewStyle(kPalette, gfx::Font(14.0f)));
    text.SetBounds({0, 0, 320, 200});
    root.AddChild(text);
    text.SetText(kGame);

    // Every line, less the spaces hanging at its end, fits the area by Skia's own measure; and the next word would not.
    const gfx::RectI area  = text.TextArea();
    const auto      &lines = text.Lines();
    ASSERT_GT(lines.size(), 8u);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        std::string shown = text.text().substr(lines[i].start, lines[i].end - lines[i].start);
        while (!shown.empty() && shown.back() == ' ')
        {
            shown.pop_back();
        }
        EXPECT_LE(gfx::MeasureText(text.style().font, shown), static_cast<float>(area.width())) << shown;
        if (lines[i].soft && i + 1 < lines.size())
        {
            const std::string next = text.text().substr(lines[i + 1].start, lines[i + 1].end - lines[i + 1].start);
            const std::string with =
                text.text().substr(lines[i].start, lines[i].end - lines[i].start) + next.substr(0, next.find(' '));
            EXPECT_GT(gfx::MeasureText(text.style().font, with), static_cast<float>(area.width())) << with;
        }
    }

    // The word "Home" selected; the caret after it, editable and with the keyboard.
    const std::size_t home = text.text().find("Home");
    text.SetEditable(true);
    router.Focus(&text);
    text.Select(home, home + 4);
    Surface         s(320, 200);
    gfx::SkiaCanvas c(*s.surface->getCanvas());
    view::PaintTree(root, c);
    const gfx::PointF from = text.PointOf(home);
    const gfx::PointF to   = text.PointOf(home + 4);
    const int         top  = gfx::RoundToInt(from.y);
    EXPECT_TRUE(Near(s.At(gfx::RoundToInt(from.x) + 1, top + 1), Blend(kPalette.plate, text.style().highlight), 3))
        << "the selection behind the word, in the accent";
    EXPECT_TRUE(Near(s.At(gfx::RoundToInt(from.x) - 3, top + 1), Sk(kPalette.plate))) << "and nothing before it";
    EXPECT_TRUE(Near(s.At(gfx::RoundToInt(to.x), top + text.LineHeight() / 2), Sk(kPalette.accent), 3))
        << "the caret's bar at its end";
}

TEST(TextViewOnSkia, ReviewImage)
{
    const char *dir = ScreenshotDir();
    if (dir == nullptr)
    {
        GTEST_SKIP() << "set RECKLESS_SCREENSHOT_DIR to render the review image";
    }
    LoadFaces();
    MeasuringHost     host;
    view::View        root;
    view::MouseRouter router(root, nullptr);
    root.SetHost(&host);
    root.SetBounds({880, 400});

    Caption        game_caption("GAME");
    view::TextView game(panel::TextViewStyle(kPalette, gfx::Font(14.0f)));
    Caption        fen_caption("POSITION");
    view::TextView fen(panel::TextViewStyle(kPalette, gfx::Font(14.0f)));
    game_caption.SetBounds({24, 16, 520, 20});
    game.SetBounds({24, 40, 520, 336});
    fen_caption.SetBounds({568, 16, 288, 20});
    fen.SetBounds({568, 40, 288, 96});
    for (view::View *v : std::vector<view::View *>{&game_caption, &game, &fen_caption, &fen})
    {
        root.AddChild(*v);
    }
    game.SetText(kGame);
    const std::size_t line = game.text().find("13. Ne2");
    game.Select(line, game.text().find(" 16."));
    fen.SetEditable(true);
    fen.SetText("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    router.Focus(&fen);
    fen.Select(fen.text().size(), fen.text().size());

    gfx::Image image = gfx::MakeImage(2 * root.width(), 2 * root.height());
    auto       c     = gfx::MakeImageCanvas(image, 2.0f);
    c->FillAll(kPalette.panel);
    view::PaintTree(root, *c);
    const auto png = gfx::EncodePng(image);
    std::ofstream(std::string(dir) + "/text-view.png", std::ios::binary)
        .write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
}
