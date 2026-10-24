// A drawing's SVG (gfx/SvgImage.h) and the monospaced family drawn on Skia (gfx/skia): the two chess pieces kept as
// fixtures, and every piece of a directory RECKLESS_SVG_PIECES names, drawn into a transparent picture at two sizes -
// opaque in the body in the piece's colour, dark on the outline, transparent outside, covering the same share of the
// square at both - and, with RECKLESS_SCREENSHOT_DIR set too, all of them on light and dark squares as a PNG for review
// (svg-pieces.png); Monaspace Xenon, opt in, a serif's text measured and drawn exactly as it was before and after the
// family's faces are set, every printable ASCII character one advance in the regular face and in the bold, and text
// drawn in it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "Canvas.h"
#include "SkiaCanvas.h"
#include "SvgImage.h"

#include <gtest/gtest.h>

namespace
{
using namespace reckless::gfx;

/** An environment variable, or empty: MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for. */
std::string Env(const char *name)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return value != nullptr ? std::string(value) : std::string();
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void LoadSerif()
{
    static const bool loaded = [] {
        SetSerifFaces(ReadBytes(RECKLESS_ASSETS_DIR "/fonts/NotoSerif-Regular.ttf"),
                      ReadBytes(RECKLESS_ASSETS_DIR "/fonts/NotoSerif-Bold.ttf"));
        return true;
    }();
    (void)loaded;
}

bool &MonospaceLoaded()
{
    static bool loaded = false;
    return loaded;
}

void LoadMonospace()
{
    LoadSerif();
    if (!MonospaceLoaded())
    {
        SetMonospaceFaces(ReadBytes(RECKLESS_ASSETS_DIR "/fonts/monaspace/MonaspaceXenon-Regular.otf"),
                          ReadBytes(RECKLESS_ASSETS_DIR "/fonts/monaspace/MonaspaceXenon-Bold.otf"));
        MonospaceLoaded() = true;
    }
}

/** A transparent raster of `size` pixels square and its pixels read back unpremultiplied. */
struct Raster
{
    sk_sp<SkSurface> surface;

    Raster(int w, int h) : surface(SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h)))
    {
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    }

    SkColor At(int x, int y) const
    {
        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32(1, 1, kUnpremul_SkAlphaType));
        surface->readPixels(bitmap, x, y);
        return bitmap.getColor(0, 0);
    }

    /** The mean alpha over the whole raster, 0 to 1. */
    double Coverage() const
    {
        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32Premul(surface->width(), surface->height()));
        surface->readPixels(bitmap, 0, 0);
        double sum = 0.0;
        for (int y = 0; y < surface->height(); ++y)
        {
            for (int x = 0; x < surface->width(); ++x)
            {
                sum += SkColorGetA(bitmap.getColor(x, y)) / 255.0;
            }
        }
        return sum / (surface->width() * surface->height());
    }
};

/** A piece drawn into a transparent square of `size` pixels. */
Raster Render(const SvgImage &image, int size)
{
    Raster     r(size, size);
    SkiaCanvas canvas(*r.surface->getCanvas());
    Canvas    &c = canvas;
    image.DrawIn(c, {0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size)});
    return r;
}

/** The pixel of a `size`-pixel rendering under a point of the piece's 100-unit square. */
SkColor Under(const Raster &r, int size, float x, float y)
{
    const float s = static_cast<float>(size) / 100.0f;
    return r.At(static_cast<int>(std::floor(x * s)), static_cast<int>(std::floor(y * s)));
}

/** The brightest of a colour's three channels. */
unsigned Brightest(SkColor c)
{
    return std::max({SkColorGetR(c), SkColorGetG(c), SkColorGetB(c)});
}

/** Every piece SVG in a directory, by file name. */
std::vector<std::filesystem::path> PiecesIn(const std::string &dir)
{
    std::vector<std::filesystem::path> out;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() == ".svg")
        {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** What every piece of the set shows at a size: an opaque body light for white or dark for black at the middle of
    its base, a dark outline along the base's bottom edge, and transparency in the corners. */
void ExpectAPiece(const SvgImage &image, int size, bool white, const std::string &name)
{
    const Raster  r    = Render(image, size);
    const SkColor body = Under(r, size, 50.0f, 84.5f);
    EXPECT_EQ(SkColorGetA(body), 255u) << name << " at " << size;
    if (white)
    {
        EXPECT_GT(Brightest(body), 200u) << name << " at " << size;
    }
    else
    {
        EXPECT_LT(Brightest(body), 100u) << name << " at " << size;
    }
    const SkColor outline = Under(r, size, 50.0f, 91.5f);
    EXPECT_EQ(SkColorGetA(outline), 255u) << name << " at " << size;
    EXPECT_LT(Brightest(outline), 110u) << name << " at " << size;
    for (const auto &[x, y] : {std::pair{2.0f, 2.0f}, std::pair{98.0f, 2.0f}, std::pair{2.0f, 98.0f}})
    {
        EXPECT_EQ(SkColorGetA(Under(r, size, x, y)), 0u) << name << " at " << size << ": outside, transparent";
    }
}
} // namespace

TEST(SvgImageOnSkia, TheKingDrawsItsBodyItsOutlineAndNothingOutside)
{
    const auto king = SvgImage::Parse(ReadText(RECKLESS_SVG_FIXTURES "/wK.svg"));
    ASSERT_TRUE(king.has_value());
    for (const int size : {64, 160})
    {
        ExpectAPiece(*king, size, true, "wK");
        const Raster  r    = Render(*king, size);
        const SkColor neck = Under(r, size, 50.0f, 60.0f);
        EXPECT_EQ(SkColorGetA(neck), 255u);
        EXPECT_GT(SkColorGetR(neck), 220u) << "the light gradient";
        EXPECT_GT(SkColorGetR(neck), SkColorGetB(neck)) << "warm: ivory, not grey";
        EXPECT_EQ(SkColorGetA(Under(r, size, 20.0f, 30.0f)), 0u) << "beside the crown";
    }
    EXPECT_NEAR(Render(*king, 64).Coverage(), Render(*king, 160).Coverage(), 0.01) << "the same share at both sizes";
}

TEST(SvgImageOnSkia, TheKnightDrawsDarkWithItsEyeInItsHighlight)
{
    const auto knight = SvgImage::Parse(ReadText(RECKLESS_SVG_FIXTURES "/bN.svg"));
    ASSERT_TRUE(knight.has_value());
    for (const int size : {64, 160})
    {
        ExpectAPiece(*knight, size, false, "bN");
        const Raster  r    = Render(*knight, size);
        const SkColor neck = Under(r, size, 50.0f, 60.0f);
        EXPECT_EQ(SkColorGetA(neck), 255u);
        EXPECT_LT(Brightest(neck), 90u) << "the dark gradient";
        const SkColor eye = Under(r, size, 36.5f, 30.5f);
        EXPECT_NEAR(static_cast<int>(SkColorGetR(eye)), 0xc9, 6) << size;
        EXPECT_NEAR(static_cast<int>(SkColorGetG(eye)), 0xbb, 6) << size;
        EXPECT_NEAR(static_cast<int>(SkColorGetB(eye)), 0xa0, 6) << size;
    }
    EXPECT_NEAR(Render(*knight, 64).Coverage(), Render(*knight, 160).Coverage(), 0.01);
}

TEST(SvgImageOnSkia, EveryPieceInTheDirectoryNamedDraws)
{
    const std::string dir = Env("RECKLESS_SVG_PIECES");
    if (dir.empty())
    {
        GTEST_SKIP() << "set RECKLESS_SVG_PIECES to a directory of piece SVGs to draw them all";
    }
    const auto pieces = PiecesIn(dir);
    EXPECT_FALSE(pieces.empty());
    for (const auto &path : pieces)
    {
        std::string error;
        const auto  piece = SvgImage::Parse(ReadText(path), &error);
        const auto  name  = path.stem().string();
        ASSERT_TRUE(piece.has_value()) << name << ": " << error;
        for (const int size : {64, 160})
        {
            ExpectAPiece(*piece, size, name.front() == 'w', name);
        }
        EXPECT_NEAR(Render(*piece, 64).Coverage(), Render(*piece, 160).Coverage(), 0.01) << name;
    }
}

TEST(SvgImageOnSkia, PiecesForReview)
{
    const std::string out = Env("RECKLESS_SCREENSHOT_DIR");
    if (out.empty())
    {
        GTEST_SKIP() << "set RECKLESS_SCREENSHOT_DIR to render the review image";
    }
    const std::string dir    = Env("RECKLESS_SVG_PIECES");
    auto              pieces = PiecesIn(dir.empty() ? std::string(RECKLESS_SVG_FIXTURES) : dir);
    // In a board's back-rank order of kinds - king, queen, rook, bishop, knight, pawn - by the letter after the colour.
    const auto rank = [](const std::filesystem::path &p) {
        const std::string stem = p.stem().string();
        return stem.size() > 1 ? std::string_view("KQRBNP").find(stem[1]) : std::string_view::npos;
    };
    std::stable_sort(pieces.begin(), pieces.end(), [&rank](const auto &a, const auto &b) { return rank(a) < rank(b); });
    // The white pieces, then the black, each over a light and a dark square at 96 pixels; under them every piece at
    // 48 pixels, as small as a board in a narrow window draws them.
    std::vector<SvgImage> white, black;
    for (const auto &path : pieces)
    {
        auto piece = SvgImage::Parse(ReadText(path));
        ASSERT_TRUE(piece.has_value()) << path;
        (path.stem().string().front() == 'w' ? white : black).push_back(std::move(*piece));
    }
    const int   square  = 96;
    const int   columns = static_cast<int>(std::max(white.size(), black.size()));
    const int   small   = 48;
    const int   width   = std::max(columns * square, static_cast<int>(pieces.size()) * small);
    const int   height  = 4 * square + 2 * small;
    const Color light   = Color::FromRgb(0xec, 0xdc, 0xbc);
    const Color dark    = Color::FromRgb(0x8a, 0x6a, 0x4a);
    Image       image   = MakeImage(width, height);
    auto        c       = MakeImageCanvas(image, 1.0f);
    c->FillAll(Color::FromRgb(0x30, 0x30, 0x34));
    const auto draw = [&](const std::vector<SvgImage> &set, int row, bool light_first) {
        for (std::size_t i = 0; i < set.size(); ++i)
        {
            const RectF cell{static_cast<float>(static_cast<int>(i) * square), static_cast<float>(row * square),
                             static_cast<float>(square), static_cast<float>(square)};
            c->SetColor(((i % 2 == 0) == light_first) ? light : dark);
            c->FillRect(cell);
            set[i].DrawIn(*c, cell);
        }
    };
    draw(white, 0, true);
    draw(white, 1, false);
    draw(black, 2, true);
    draw(black, 3, false);
    int i = 0;
    for (const auto *set : {&white, &black})
    {
        for (const SvgImage &piece : *set)
        {
            for (int row = 0; row < 2; ++row)
            {
                const RectF cell{static_cast<float>(i * small), static_cast<float>(4 * square + row * small),
                                 static_cast<float>(small), static_cast<float>(small)};
                c->SetColor(((i + row) % 2 == 0) ? light : dark);
                c->FillRect(cell);
                piece.DrawIn(*c, cell);
            }
            ++i;
        }
    }
    c.reset();
    const auto png = EncodePng(image);
    ASSERT_FALSE(png.empty());
    std::ofstream(out + "/svg-pieces.png", std::ios::binary)
        .write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
}

TEST(MonospacedFamily, IsOptInAndLeavesTheSerifAsItWas)
{
    LoadSerif();
    const char *texts[] = {"GAIN 20.0 dB", "Reckless Technology", "iiiWWW 0123", "e2e4 Nf3 O-O"};
    const Font  fonts[] = {Font(13.0f), Font(18.0f).Bold(), Font(11.0f).WithKerning(0.08f)};
    EXPECT_EQ(Font().family, FontFamily::kSerif) << "a font's family the serif unless it asks";
    std::vector<float> before;
    for (const Font &f : fonts)
    {
        for (const char *t : texts)
        {
            before.push_back(MeasureText(f, t));
            if (!MonospaceLoaded())
            {
                EXPECT_EQ(MeasureText(f.WithFamily(FontFamily::kMonospace), t), before.back())
                    << "the serif stands in until the monospaced faces are set";
            }
        }
    }
    const auto draw_serif = [&] {
        Raster     r(320, 90);
        SkiaCanvas canvas(*r.surface->getCanvas());
        Canvas    &c = canvas;
        c.SetColor(colors::kWhite);
        float y = 0.0f;
        for (const Font &f : fonts)
        {
            c.SetFont(f);
            c.DrawText(texts[0], {4.0f, y, 312.0f, f.height}, Justify::kCentredLeft, true);
            y += 28.0f;
        }
        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32Premul(320, 90));
        r.surface->readPixels(bitmap, 0, 0);
        return std::vector<std::uint8_t>(static_cast<const std::uint8_t *>(bitmap.getPixels()),
                                         static_cast<const std::uint8_t *>(bitmap.getPixels()) +
                                             bitmap.computeByteSize());
    };
    const auto pixels_before = draw_serif();
    LoadMonospace();
    std::size_t n = 0;
    for (const Font &f : fonts)
    {
        for (const char *t : texts)
        {
            EXPECT_EQ(MeasureText(f, t), before[n++]) << t << ": the serif measures as it did";
        }
    }
    EXPECT_TRUE(draw_serif() == pixels_before) << "and draws as it did, pixel for pixel";
    EXPECT_NE(MeasureText(Font(13.0f).WithFamily(FontFamily::kMonospace), texts[2]), MeasureText(Font(13.0f), texts[2]))
        << "the monospaced family its own faces once they are set";
}

TEST(MonospacedFamily, EveryPrintableAsciiCharacterHasOneAdvance)
{
    LoadMonospace();
    for (const Font &base : {Font(13.0f), Font(22.0f), Font(13.0f).Bold(), Font(22.0f).Bold()})
    {
        const Font  f       = base.WithFamily(FontFamily::kMonospace);
        const float advance = MeasureText(f, "M");
        EXPECT_GT(advance, 0.3f * f.height);
        float narrowest = advance, widest = advance;
        for (char ch = 0x20; ch < 0x7f; ++ch)
        {
            const float w = MeasureText(f, std::string(1, ch));
            narrowest     = std::min(narrowest, w);
            widest        = std::max(widest, w);
            EXPECT_FLOAT_EQ(w, advance) << "'" << ch << "' at " << f.height << (f.bold ? " bold" : "");
        }
        EXPECT_NEAR(MeasureText(f, "iiiiiiiiii"), 10.0f * advance, 1e-3f);
        EXPECT_NEAR(MeasureText(f, "WWWWWWWWWW"), 10.0f * advance, 1e-3f);
        std::printf("%5.1f px%s: advance %.4f px, every character from %.4f to %.4f\n", f.height,
                    f.bold ? " bold" : "     ", advance, narrowest, widest);
    }
    const Font regular = Font(13.0f).WithFamily(FontFamily::kMonospace);
    EXPECT_FLOAT_EQ(MeasureText(regular, "M"), MeasureText(regular.Bold(), "M")) << "the bold face on the same grid";
}

TEST(MonospacedFamily, TextDrawsInIt)
{
    LoadMonospace();
    const Font mono = Font(16.0f).WithFamily(FontFamily::kMonospace);
    Raster     r(400, 40);
    SkiaCanvas canvas(*r.surface->getCanvas());
    Canvas    &c = canvas;
    EXPECT_NEAR(c.Ascent(mono) + c.Descent(mono), 16.0f, 0.01f) << "sized as the serif is: its ascent and descent";
    c.FillAll(colors::kBlack);
    c.SetColor(colors::kWhite);
    c.SetFont(mono);
    c.DrawText("info depth 24 score cp 31 pv e2e4", {0.0f, 0.0f, 400.0f, 40.0f}, Justify::kCentredLeft, false);
    int lit = 0;
    for (int x = 0; x < 400; ++x)
    {
        lit += Brightest(r.At(x, 20)) > 128 ? 1 : 0;
    }
    EXPECT_GT(lit, 20) << "the glyphs drawn across the middle row";
    const float width = MeasureText(mono, "info depth 24 score cp 31 pv e2e4");
    EXPECT_EQ(Brightest(r.At(static_cast<int>(width) + 6, 20)), 0u) << "nothing past the text's measured end";
}
