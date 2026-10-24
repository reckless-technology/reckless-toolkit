// The suite's drawing on Skia (gfx/skia): what a panel draws lands where it asked - a fill's colour inside its
// shape and nothing outside, a border inside its rectangle, the state saved and restored with the brush, a clip,
// text in the series' serif at a width that grows with the text and with the bold, text that is not UTF-8 measured and
// drawn with U+FFFD for each ill-formed part rather than thrown out of the paint, text cut with an ellipsis inside its
// box however it is justified, an image decoded from a PNG and
// drawn and an image of any other format declined, an offscreen picture drawn into and back, an image shrunk by the
// look's halving to the average of every pixel it covers, a meter's artwork - its document, its shadow and the
// drawing over it (SkiaArtwork) - and the whole scene written as a PNG for review when RECKLESS_SCREENSHOT_DIR is set.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/codec/SkBmpDecoder.h"
#include "include/codec/SkCodec.h"
#include "include/codec/SkGifDecoder.h"
#include "include/codec/SkIcoDecoder.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkRawDecoder.h"
#include "include/codec/SkWbmpDecoder.h"
#include "include/codec/SkWebpDecoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"

#include "Artwork.h"
#include "Canvas.h"
#include "LookImages.h"
#include "Path.h"
#include "SkiaArtwork.h"
#include "SkiaCanvas.h"

#include <gtest/gtest.h>

namespace
{
using namespace reckless::gfx;

std::vector<std::uint8_t> ReadFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void LoadFaces()
{
    static const bool loaded = [] {
        SetSerifFaces(ReadFile(RECKLESS_ASSETS_DIR "/fonts/NotoSerif-Regular.ttf"),
                      ReadFile(RECKLESS_ASSETS_DIR "/fonts/NotoSerif-Bold.ttf"));
        return true;
    }();
    (void)loaded;
}

/** A surface of `w` x `h`, its canvas, and a pixel reader. */
struct Surface
{
    sk_sp<SkSurface> surface;

    Surface(int w, int h) : surface(SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h)))
    {
        surface->getCanvas()->clear(SK_ColorBLACK);
    }

    SkCanvas &canvas()
    {
        return *surface->getCanvas();
    }

    SkColor At(int x, int y)
    {
        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32Premul(1, 1));
        surface->readPixels(bitmap, x, y);
        return bitmap.getColor(0, 0);
    }
};

TEST(SkiaCanvas, AFillLandsInsideItsShape)
{
    Surface    s(100, 100);
    SkiaCanvas c(s.canvas());
    c.SetColor(Color::FromRgb(224, 160, 56));
    c.FillRoundedRect({10.0f, 10.0f, 60.0f, 40.0f}, 8.0f);
    EXPECT_EQ(s.At(40, 30), SkColorSetRGB(224, 160, 56)) << "inside";
    EXPECT_EQ(s.At(11, 11), SK_ColorBLACK) << "the corner rounded off";
    EXPECT_EQ(s.At(80, 30), SK_ColorBLACK) << "outside";
    c.SetColor(colors::kWhite);
    c.DrawRect({10.0f, 60.0f, 30.0f, 30.0f}, 2.0f);
    EXPECT_EQ(s.At(10, 70), SK_ColorWHITE) << "a border starts at the rectangle's edge";
    EXPECT_EQ(s.At(11, 70), SK_ColorWHITE) << "and runs inward its thickness";
    EXPECT_EQ(s.At(13, 70), SK_ColorBLACK);
    EXPECT_EQ(s.At(9, 70), SK_ColorBLACK) << "nothing outside it";
}

TEST(SkiaCanvas, StateAndClipsSaveAndRestore)
{
    Surface    s(100, 100);
    SkiaCanvas c(s.canvas());
    c.SetColor(colors::kWhite);
    {
        ScopedState state(c);
        c.SetColor(Color::FromRgb(255, 0, 0));
        c.ClipToRect({0, 0, 50, 100});
        c.FillRect(RectI{0, 0, 100, 10});
    }
    EXPECT_EQ(s.At(25, 5), SK_ColorRED);
    EXPECT_EQ(s.At(75, 5), SK_ColorBLACK) << "clipped";
    c.FillRect(RectI{0, 20, 100, 10});
    EXPECT_EQ(s.At(75, 25), SK_ColorWHITE) << "the colour and the clip both restored";
    c.AddTransform(Affine::Scale(2.0f));
    EXPECT_FLOAT_EQ(c.PhysicalPixelScale(), 2.0f);
    EXPECT_EQ(c.ClipBounds(), (RectI{0, 0, 50, 50})) << "the clip in the transformed units";
}

TEST(SkiaCanvas, TextIsTheSerifAtItsWidth)
{
    LoadFaces();
    Surface     s(300, 60);
    SkiaCanvas  c(s.canvas());
    const Font  regular(18.0f);
    const float short_width = c.TextWidth(regular, "GAIN");
    const float long_width  = c.TextWidth(regular, "GAIN STAGE ONE");
    EXPECT_GT(short_width, 20.0f);
    EXPECT_GT(long_width, 2.5f * short_width);
    EXPECT_GT(c.TextWidth(regular.Bold(), "GAIN"), short_width) << "the bold face, wider";
    EXPECT_NEAR(c.TextWidth(regular.WithKerning(0.1f), "GAIN"), short_width + 3 * 0.1f * 18.0f, 0.01f)
        << "kerning a share of the height after every glyph but the last";
    EXPECT_NEAR(c.Ascent(regular) + c.Descent(regular), 18.0f, 0.01f) << "a font's height its ascent and descent";

    c.SetColor(colors::kWhite);
    c.SetFont(regular);
    c.DrawText("GAIN", {0.0f, 0.0f, 300.0f, 60.0f}, Justify::kCentred, false);
    int lit = 0;
    for (int x = 0; x < 300; ++x)
    {
        lit += s.At(x, 30) != SK_ColorBLACK ? 1 : 0;
    }
    EXPECT_GT(lit, 4) << "the glyphs drawn across the middle row";
    EXPECT_EQ(s.At(5, 30), SK_ColorBLACK) << "centred, so nothing at the left edge";
}

TEST(SkiaCanvas, TextThatIsNotUtf8IsMeasuredAndDrawnWithAReplacementForEachBrokenPart)
{
    // A system's message in the ANSI code page ("Acces refuse." with each e accented as 0xE8 and 0xE9), a device's
    // name in Mac OS Roman (0xD5 for the apostrophe): Skia counts text that is not UTF-8 as SIZE_MAX glyphs, which no
    // vector holds. It measures as the text with U+FFFD in each broken part's place, and draws without throwing.
    LoadFaces();
    Surface    s(300, 60);
    SkiaCanvas c(s.canvas());
    const Font regular(18.0f);
    float      broken = 0.0f;
    EXPECT_NO_THROW(broken = c.TextWidth(regular, "Acc\xE8s refus\xE9."));
    EXPECT_FLOAT_EQ(broken, c.TextWidth(regular, "Acc\xEF\xBF\xBDs refus\xEF\xBF\xBD."));
    EXPECT_GT(broken, 0.0f);
    float lone = -1.0f;
    EXPECT_NO_THROW(lone = c.TextWidth(regular, "\xE8"));
    EXPECT_FLOAT_EQ(lone, c.TextWidth(regular, "\xEF\xBF\xBD")) << "a lone lead byte";

    c.SetColor(colors::kWhite);
    c.SetFont(regular);
    EXPECT_NO_THROW(c.DrawText("Name\xD5s Earphones", {0.0f, 0.0f, 300.0f, 60.0f}, Justify::kCentred, true));
    int lit = 0;
    for (int x = 0; x < 300; ++x)
    {
        lit += s.At(x, 30) != SK_ColorBLACK ? 1 : 0;
    }
    EXPECT_GT(lit, 4) << "the name drawn, its well-formed letters as they are";
}

TEST(SkiaCanvas, TextCutWithAnEllipsisStaysInsideItsAreaHoweverItIsJustified)
{
    // A name too long for its box is cut a glyph at a time until an ellipsis where the last cut glyph stood fits: the
    // dots end inside the box, so the text neither runs past its right edge nor, justified right or centred, starts
    // left of its left one; and it keeps as many glyphs as fit, so the dots end within a glyph of the right edge.
    LoadFaces();
    const RectF       area{100.0f, 0.0f, 150.0f, 40.0f};
    const std::string text = "GAIN STAGE ONE TWO THREE FOUR";
    for (const Justify justify : {Justify::kCentredLeft, Justify::kCentred, Justify::kCentredRight})
    {
        Surface    s(400, 40);
        SkiaCanvas c(s.canvas());
        const Font regular(18.0f);
        ASSERT_GT(c.TextWidth(regular, text), area.width() + 40.0f) << "long enough to be cut";
        c.SetColor(colors::kWhite);
        c.SetFont(regular);
        c.DrawText(text, area, justify, true);
        int left = 400, right = -1;
        for (int x = 0; x < 400; ++x)
        {
            for (int y = 0; y < 40; ++y)
            {
                if (s.At(x, y) != SK_ColorBLACK)
                {
                    left  = std::min(left, x);
                    right = std::max(right, x);
                }
            }
        }
        std::printf("  cut text justified %d: ink from %d to %d in an area from 100 to 250\n",
                    static_cast<int>(justify), left, right);
        ASSERT_GE(right, 0) << "something drawn";
        EXPECT_GE(left, 99) << "nothing left of the area";
        EXPECT_LE(right, 250) << "nothing right of the area";
        if (justify == Justify::kCentredLeft)
        {
            EXPECT_GE(right, 250 - 18) << "as much kept as fits: the dots within a glyph of the edge";
        }
    }
}

TEST(SkiaCanvas, AnImageDecodesAndDraws)
{
    const auto png = ReadFile(RECKLESS_ASSETS_DIR "/logos/RecklessRtLogo.png");
    ASSERT_FALSE(png.empty());
    const Image logo = DecodeImage(png);
    ASSERT_TRUE(logo.IsValid());
    Image target = MakeImage(64, 64);
    auto  canvas = MakeImageCanvas(target);
    ASSERT_NE(canvas, nullptr);
    canvas->SetResampling(Resampling::kHigh);
    canvas->DrawImage(logo, {0.0f, 0.0f, 64.0f, 64.0f});
    const auto encoded = EncodePng(target);
    ASSERT_GT(encoded.size(), 8u);
    EXPECT_EQ(encoded[1], 'P');
    EXPECT_EQ(encoded[2], 'N');
    EXPECT_EQ(encoded[3], 'G');
    EXPECT_TRUE(DecodeImage(encoded).IsValid()) << "and the PNG reads back";
}

// One pixel in each format Skia's codec registry names beside PNG, as Pillow wrote them and read them back (the WBMP by
// hand: type 0, fixed header 0, width 1, height 1, a row of one byte), and for the raw decoder a TIFF header with no
// image after it, on which the pack's raw decoder answers kInvalidInput. The pack's decoders, linked, decode each of
// the six others, through SkCodec's search and through the decoder's own entry point.
constexpr std::uint8_t kJpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x10, 0x0b, 0x0c, 0x0e, 0x0c, 0x0a, 0x10, 0x0e, 0x0d, 0x0e, 0x12,
    0x11, 0x10, 0x13, 0x18, 0x28, 0x1a, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23, 0x25, 0x1d, 0x28, 0x3a, 0x33, 0x3d,
    0x3c, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5c, 0x4e, 0x40, 0x44, 0x57, 0x45, 0x37, 0x38, 0x50, 0x6d, 0x51,
    0x57, 0x5f, 0x62, 0x67, 0x68, 0x67, 0x3e, 0x4d, 0x71, 0x79, 0x70, 0x64, 0x78, 0x5c, 0x65, 0x67, 0x63, 0xff,
    0xc0, 0x00, 0x0b, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0xff, 0xc4,
    0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0x2d, 0xff, 0xd9};
constexpr std::uint8_t kWebp[] = {0x52, 0x49, 0x46, 0x46, 0x1e, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
                                  0x50, 0x38, 0x4c, 0x11, 0x00, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x07,
                                  0x50, 0x8f, 0x22, 0x57, 0xab, 0xff, 0x81, 0x88, 0xe8, 0x7f, 0x00, 0x00};
constexpr std::uint8_t kGif[]  = {0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x01, 0x00, 0x01, 0x00, 0x81,
                                  0x00, 0x00, 0xcc, 0x33, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
                                  0x00, 0x00, 0x08, 0x04, 0x00, 0x01, 0x04, 0x04, 0x00, 0x3b};
constexpr std::uint8_t kBmp[]  = {0x42, 0x4d, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00,
                                  0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
                                  0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
                                  0x00, 0x00, 0xc4, 0x0e, 0x00, 0x00, 0xc4, 0x0e, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x1e, 0xc8, 0x00};
constexpr std::uint8_t kIco[]  = {
    0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x45, 0x00, 0x00, 0x00, 0x16,
    0x00, 0x00, 0x00, 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xde, 0x00, 0x00,
    0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x38, 0x21, 0x17, 0x05, 0x00, 0x02, 0xf2, 0x01, 0x41, 0xd7,
    0x2f, 0xd1, 0x66, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
constexpr std::uint8_t kWbmp[] = {0x00, 0x00, 0x01, 0x01, 0x00};
constexpr std::uint8_t kTiff[] = {0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00};

TEST(SkiaCanvas, OnlyAPngDecodes)
{
    // SkiaCanvas.cpp declines every decoder but PNG's, so that no module links libjpeg-turbo, libwebp, wuffs, the DNG
    // SDK or piex: a real image of each other format decodes to nothing - through DecodeImage, through SkCodec's own
    // search of its decoders and through the decoder's entry point, which answers kUnimplemented, as the real
    // decoders never do here. A decoder of the pack's linked again decodes its image and fails this.
    using DecodeFn =
        std::unique_ptr<SkCodec> (*)(std::unique_ptr<SkStream>, SkCodec::Result *, SkCodecs::DecodeContext);

    struct Declined
    {
        const char                   *format;
        std::span<const std::uint8_t> bytes;
        DecodeFn                      decode;
    };

    const Declined declined[] = {{"JPEG", kJpeg, &SkJpegDecoder::Decode}, {"WebP", kWebp, &SkWebpDecoder::Decode},
                                 {"GIF", kGif, &SkGifDecoder::Decode},    {"BMP", kBmp, &SkBmpDecoder::Decode},
                                 {"ICO", kIco, &SkIcoDecoder::Decode},    {"WBMP", kWbmp, &SkWbmpDecoder::Decode},
                                 {"raw", kTiff, &SkRawDecoder::Decode}};
    for (const auto &[format, bytes, decode] : declined)
    {
        const auto data = [&bytes] { return SkData::MakeWithCopy(bytes.data(), bytes.size()); };
        EXPECT_FALSE(DecodeImage(bytes).IsValid()) << format;
        EXPECT_FALSE(SkCodec::MakeFromData(data())) << format << ", through SkCodec's search";
        SkCodec::Result result = SkCodec::kSuccess;
        EXPECT_FALSE(decode(std::make_unique<SkMemoryStream>(data()), &result, nullptr)) << format;
        EXPECT_EQ(result, SkCodec::kUnimplemented) << format << ": the decoder declined, not one of the pack's";
    }
    const auto png = ReadFile(RECKLESS_ASSETS_DIR "/logos/RecklessRtLogo.png");
    EXPECT_TRUE(SkCodec::MakeFromData(SkData::MakeWithCopy(png.data(), png.size())))
        << "a PNG through SkCodec's search still finds PNG's decoder";
}

TEST(SkiaCanvas, AnOffscreenPictureIsSkiasOwnAndDrawsBack)
{
    // What a view keeps between paints (view/CachedPicture.h): an image of Skia's own, transparent, whose canvas draws
    // a unit a pixel until the view scales it - and which a canvas on screen then draws like any other image.
    Surface    s(100, 100);
    SkiaCanvas c(s.canvas());
    Offscreen  offscreen = c.MakeOffscreen(40, 20);
    ASSERT_NE(offscreen.canvas, nullptr);
    EXPECT_EQ(offscreen.image.width(), 40);
    EXPECT_EQ(offscreen.image.height(), 20);
    EXPECT_FLOAT_EQ(offscreen.canvas->PhysicalPixelScale(), 1.0f);
    offscreen.canvas->AddTransform(Affine::Scale(2.0f));
    EXPECT_FLOAT_EQ(offscreen.canvas->PhysicalPixelScale(), 2.0f) << "the scale it is drawn at, as a view reads it";
    offscreen.canvas->SetColor(colors::kWhite);
    offscreen.canvas->FillRect(RectI{0, 0, 10, 10}); // the left half, in the picture's scaled units
    offscreen.canvas.reset();
    c.DrawImage(offscreen.image, {0.0f, 0.0f, 40.0f, 20.0f}, false);
    EXPECT_EQ(s.At(10, 10), SK_ColorWHITE) << "what was drawn into it";
    EXPECT_EQ(s.At(30, 10), SK_ColorBLACK) << "and transparent where nothing was";
    EXPECT_EQ(c.MakeOffscreen(0, -3).image.width(), 1) << "never empty";
}

/** The look's image backend on Skia, as a module's panel context gives it (window/ModulePanel.h): PNGs
    decoded, parts cut and images resampled through the canvas's own functions. */
class SkiaImages final : public reckless::look::ImageBackend
{
  public:
    Image Decode(const void *data, std::size_t size) const override
    {
        return DecodeImage({static_cast<const std::uint8_t *>(data), size});
    }

    Image Clipped(const Image &image, RectI area) const override
    {
        return SubImage(image, area);
    }

    Image Rescaled(const Image &image, int width, int height, Resampling quality) const override
    {
        return RescaleImage(image, width, height, quality);
    }
};

TEST(SkiaCanvas, AShrunkImageIsTheAverageOfEveryPixelItCovers)
{
    // What the header's mark and the trade mark logos are drawn from (look::ShrunkImage): an image halved while it is
    // twice the size asked, then resampled once - on Skia's bilinear read at 2:1 a box filter. Columns one pixel wide,
    // black and white in turn, average to mid grey over every block, so the shrink comes out flat - where a single
    // resample from 512 to 48 reads a few of the hundred pixels each one covers and comes out barred.
    Image stripes = MakeImage(512, 512);
    {
        auto canvas = MakeImageCanvas(stripes);
        ASSERT_NE(canvas, nullptr);
        canvas->FillAll(colors::kBlack);
        canvas->SetColor(colors::kWhite);
        for (int x = 1; x < 512; x += 2)
        {
            canvas->FillRect(RectI{x, 0, 1, 512});
        }
    }
    const SkiaImages backend;
    const auto       red_range = [](const Image &image) {
        Surface    s(image.width(), image.height());
        SkiaCanvas c(s.canvas());
        c.DrawImage(image, {0.0f, 0.0f, static_cast<float>(image.width()), static_cast<float>(image.height())}, false);
        int low  = 255;
        int high = 0;
        for (int y = 2; y < image.height() - 2; ++y) // clear of the edge, which a resample blends with what is past it
        {
            for (int x = 2; x < image.width() - 2; ++x)
            {
                const int red = static_cast<int>(SkColorGetR(s.At(x, y)));
                low           = std::min(low, red);
                high          = std::max(high, red);
            }
        }
        return std::pair{low, high};
    };
    const Image shrunk               = reckless::look::ShrunkImage(backend, stripes, 48, 48);
    const auto [low, high]           = red_range(shrunk);
    const auto [once_low, once_high] = red_range(RescaleImage(stripes, 48, 48, Resampling::kHigh));
    std::printf("  512 -> 48 px of one-pixel stripes: halved %d..%d; one resample %d..%d\n", low, high, once_low,
                once_high);
    EXPECT_EQ(shrunk.width(), 48);
    EXPECT_EQ(shrunk.height(), 48);
    EXPECT_GE(low, 120) << "every pixel the average of what it covers";
    EXPECT_LE(high, 135) << "every pixel the average of what it covers";
}

std::span<const std::uint8_t> Bytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

TEST(SkiaArtwork, ADocumentItsShadowAndTheDrawingOverItLandWhereTheyAsked)
{
    // A document of 100 x 60 units drawn at twice its size: an amber square, and a white bar whose blur filter JUCE's
    // SVG parser ignores, so it is drawn sharp; under them the soft shadow of a rectangle, half-black, three pixels of
    // blur, falling two right and three down; over them a red square the panel draws in code.
    static constexpr std::string_view kSvg =
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="60" viewBox="0 0 100 60">
<defs><filter id="glow"><feGaussianBlur stdDeviation="6"/></filter></defs>
<rect x="60" y="10" width="20" height="20" fill="#e0a038"/>
<rect x="60" y="40" width="30" height="10" fill="#ffffff" filter="url(#glow)"/>
</svg>)svg";
    Path shape;
    shape.AddRect({10.0f, 10.0f, 40.0f, 60.0f});
    const ArtworkLayer layer{Bytes(kSvg), Affine::Scale(2.0f),
                             Shadow{shape, colors::kBlack.WithAlpha(0.5f), 3, {2, 3}}};

    // On white: the square, the red square, and the shadow - its middle half-black, its edge soft, out of the blur's
    // reach nothing, and further below the shape than above it.
    Surface s(200, 120);
    s.canvas().clear(SK_ColorWHITE);
    SkiaCanvas c(s.canvas());
    DrawArtwork(c, layer);
    c.SetColor(Color::FromRgb(255, 0, 0));
    c.FillRect(RectF{170.0f, 10.0f, 20.0f, 20.0f});
    EXPECT_EQ(s.At(140, 40), SkColorSetRGB(224, 160, 56)) << "the square at twice its size";
    EXPECT_EQ(s.At(118, 40), SK_ColorWHITE) << "and nothing beside it";
    EXPECT_EQ(s.At(180, 20), SkColorSetRGB(255, 0, 0)) << "the panel's own drawing over the rest";
    const auto grey = [&](int x, int y) { return static_cast<int>(SkColorGetR(s.At(x, y))); };
    EXPECT_NEAR(grey(32, 43), 128, 1) << "the shadow's middle half-black";
    EXPECT_EQ(grey(4, 43), 255) << "out of the blur's reach";
    EXPECT_GT(grey(12, 43), grey(14, 43)) << "the edge soft: lighter outward";
    EXPECT_GT(grey(12, 43), 128);
    EXPECT_LT(grey(12, 43), 255);
    // JUCE's blur of the edge: six passes of the three-tap box spread the mask's step at x = 12 to 27, 59, 103, 152,
    // 196 and 228 of 255 from x = 9 to 14, which over white at half-black is these greys.
    EXPECT_NEAR(grey(9, 43), 241, 2);
    EXPECT_NEAR(grey(10, 43), 225, 2);
    EXPECT_NEAR(grey(14, 43), 141, 2);
    EXPECT_GT(grey(32, 11), grey(32, 69)) << "it falls down: a pixel inside the shape's top lighter than its bottom";

    // On dark round the bar: its edge sharp, where the filter's glow would light the dark a few pixels out.
    c.SetColor(colors::kBlack);
    c.FillRect(RectF{100.0f, 70.0f, 100.0f, 40.0f});
    DrawArtwork(c, layer);
    EXPECT_EQ(s.At(150, 77), SK_ColorBLACK) << "nothing three pixels above the bar";
    EXPECT_EQ(s.At(150, 82), SK_ColorWHITE) << "the bar itself";

    // No document and no shadow draws nothing, and nor does a document the reader refuses.
    Surface blank(40, 40);
    blank.canvas().clear(SK_ColorWHITE);
    SkiaCanvas on_blank(blank.canvas());
    DrawArtwork(on_blank, ArtworkLayer{});
    DrawArtwork(on_blank, ArtworkLayer{Bytes(R"(<svg><ellipse cx="20" cy="20" rx="9" ry="5"/></svg>)"), {}, {}});
    EXPECT_EQ(blank.At(20, 20), SK_ColorWHITE);
}

TEST(SkiaArtwork, AGradientsLeadingStopStartsItAsTheJuceBuildDrawsIt)
{
    // A vignette whose first stop sits halfway out: transparent there in SVG, but JUCE's SVG parser starts the gradient
    // at that stop's colour, so it darkens from the centre - half-dark halfway out, where SVG has nothing - and the
    // meters' faces are drawn so.
    static constexpr std::string_view kSvg =
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
<defs><radialGradient id="v" cx="50" cy="50" r="50" gradientUnits="userSpaceOnUse">
<stop offset="0.5" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity="1"/>
</radialGradient></defs>
<rect x="0" y="0" width="100" height="100" fill="url(#v)"/>
</svg>)svg";
    Surface s(100, 100);
    s.canvas().clear(SK_ColorWHITE);
    SkiaCanvas c(s.canvas());
    DrawArtwork(c, ArtworkLayer{Bytes(kSvg), {}, {}});
    EXPECT_NEAR(static_cast<int>(SkColorGetR(s.At(50, 50))), 255, 6) << "clear at the centre";
    EXPECT_NEAR(static_cast<int>(SkColorGetR(s.At(75, 50))), 128, 4) << "half-dark halfway out";
    EXPECT_NEAR(static_cast<int>(SkColorGetR(s.At(62, 50))), 191, 4) << "a quarter of the way, a quarter dark";
}

TEST(SkiaArtwork, TheMetersFaceAndGlassDrawOnSkia)
{
    // The gain-reduction meter's face and glass at a fifth of their size: the window lit amber at its middle, the
    // bezel a dark grey, and the glass's sheen lightening the window's top left.
    const auto face  = ReadFile(RECKLESS_ASSETS_DIR "/meters/vu_meter_face.svg");
    const auto glass = ReadFile(RECKLESS_ASSETS_DIR "/meters/vu_meter_glass.svg");
    ASSERT_FALSE(face.empty());
    ASSERT_FALSE(glass.empty());
    Surface    s(200, 200);
    SkiaCanvas c(s.canvas());
    DrawArtwork(c, ArtworkLayer{face, Affine::Scale(0.2f), {}});
    const SkColor middle = s.At(100, 94);
    EXPECT_GT(SkColorGetR(middle), 200u) << "the backlight, warm";
    EXPECT_GT(SkColorGetR(middle), SkColorGetG(middle));
    EXPECT_GT(SkColorGetG(middle), SkColorGetB(middle));
    const SkColor bezel = s.At(14, 100);
    EXPECT_LT(SkColorGetR(bezel), 64u) << "the bezel, a dark grey";
    EXPECT_NEAR(static_cast<int>(SkColorGetR(bezel)), static_cast<int>(SkColorGetB(bezel)), 4);
    const SkColor before = s.At(24, 44);
    DrawArtwork(c, ArtworkLayer{glass, Affine::Scale(0.2f), {}});
    const SkColor after = s.At(24, 44);
    EXPECT_GT(SkColorGetR(after) + SkColorGetG(after) + SkColorGetB(after),
              SkColorGetR(before) + SkColorGetG(before) + SkColorGetB(before))
        << "the sheen lightening the window's top left";
}

TEST(SkiaCanvas, SceneForReview)
{
    LoadFaces();
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *dir = std::getenv("RECKLESS_SCREENSHOT_DIR");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (dir == nullptr)
    {
        GTEST_SKIP() << "set RECKLESS_SCREENSHOT_DIR to render the review image";
    }
    Image image = MakeImage(600, 248);
    auto  c     = MakeImageCanvas(image, 2.0f);
    c->FillAll(Color(0xff1b1330));
    c->SetColor(Color(0xffe0a038));
    c->FillRoundedRect({10.5f, 8.0f, 80.0f, 30.0f}, 6.0f);
    c->SetColor(Color(0xffe8dcc8).WithAlpha(0.7f));
    c->DrawRoundedRect({100.0f, 8.0f, 60.0f, 30.0f}, 4.0f, 1.5f);
    c->FillEllipse({170.0f, 6.0f, 24.0f, 24.0f});
    c->DrawEllipse({200.0f, 6.0f, 24.0f, 24.0f}, 2.0f);
    Path path;
    path.MoveTo(10.0f, 80.0f);
    path.LineTo(40.0f, 70.0f);
    path.CubicTo(60.0f, 60.0f, 80.0f, 100.0f, 100.0f, 80.0f);
    path.QuadTo(110.0f, 70.0f, 120.0f, 80.0f);
    path.AddCentredArc(150.0f, 85.0f, 12.0f, 8.0f, 0.3f, -2.0f, 2.0f, true);
    c->SetColor(Color(0xff8cc0ae));
    c->StrokePath(path, Stroke(2.2f, Stroke::Join::kCurved, Stroke::Cap::kRounded));
    Path ring;
    ring.AddEllipse({230.0f, 60.0f, 40.0f, 40.0f});
    Stroke dashed(1.4f);
    dashed.dashes = {4.0f, 3.0f};
    c->StrokePath(ring, dashed);
    c->SetGradient(Gradient{Color(0xffe0a038), {90.0f, 100.0f}, Color(0x00e0a038), {150.0f, 120.0f}, true});
    c->FillRect(RectF{90.0f, 100.0f, 60.0f, 20.0f});
    c->SetColor(colors::kWhite);
    c->SetFont(Font(14.0f).Bold());
    c->DrawText("GAIN 20.0 dB", {160.0f, 104.0f, 110.0f, 16.0f}, Justify::kCentred, true);
    c->SetFont(Font(11.0f).WithKerning(0.08f));
    c->DrawFittedText("a long caption that wraps", {230, 8, 60, 40}, Justify::kCentredLeft, 2, 0.0f);
    const auto png = EncodePng(image);
    std::ofstream(std::string(dir) + "/skia-scene.png", std::ios::binary)
        .write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
}
} // namespace
