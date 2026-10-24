// The series' look with no toolkit round it (look): each piece drawn onto a canvas that records what it is asked
// to draw, and the recording read for what is plain to check - where a plate, a lamp, a lens or a tube lands and in
// what colours, that the header's rule lies on the plate's seam, which of the knob's frames is drawn and at what size,
// how the cached pictures are made through an image backend that keeps no pixels, only how each image was made, and
// how many of them are kept - a zoom's sizes alone, and one knob strip shared by every strip made from one file -
// and how the indicators move in seconds: the needle, the lamp's filament and the glow it shows, the peak reading - and
// the oscilloscope: its trigger, armed and locked to the trace it showed last, its frame - a snapshot only when the
// ring has published, a torn one dropped - its range, up at once, down over a second and floored, its clear, and its
// screen and trace as painted.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "LookBallistics.h"
#include "LookDrawing.h"
#include "LookGeometry.h"
#include "LookImages.h"
#include "LookMeters.h"
#include "LookMetrics.h"
#include "LookPalette.h"
#include "LookScope.h"
#include "LookTubes.h"

#include <gtest/gtest.h>

namespace
{
namespace gfx  = reckless::gfx;
namespace look = reckless::look;

// ---- A canvas that records ------------------------------------------------------------------------------------

/** One drawing call a canvas was asked to make, with the brush, the font and the transform it was made under. */
struct Call
{
    std::string             op;
    gfx::RectF              rect{};
    float                   value{}; ///< a corner radius, a thickness
    gfx::Color              color{}; ///< the brush, when it is a colour
    bool                    is_gradient{false};
    gfx::Gradient           gradient{};
    gfx::Font               font{};
    std::string             text;
    gfx::Path               path;
    gfx::Affine             path_transform{};
    gfx::Affine             transform{}; ///< the canvas's own, in force
    const gfx::Image::Data *image{nullptr};
};

/** A gfx::Canvas that draws nothing and records every call. Its text is measured by rule - each character half the
    font's height wide, the ascent 0.8 of it and the descent 0.25 - so a placement that depends on a measure can be
    worked out by hand. The clip is a rectangle in the canvas's own coordinates, whatever the transform. */
class RecordingCanvas final : public gfx::Canvas
{
  public:
    explicit RecordingCanvas(float physical_scale = 1.0f, gfx::RectI clip = {0, 0, 10000, 10000})
        : physical_scale_(physical_scale)
    {
        state_.clip = clip;
    }

    std::vector<Call> calls;

    /** The calls of one kind, in order. */
    std::vector<Call> Of(std::string_view op) const
    {
        std::vector<Call> found;
        for (const auto &call : calls)
        {
            if (call.op == op)
            {
                found.push_back(call);
            }
        }
        return found;
    }

    void Save() override
    {
        stack_.push_back(state_);
    }

    void Restore() override
    {
        if (!stack_.empty())
        {
            state_ = stack_.back();
            stack_.pop_back();
        }
    }

    void AddTransform(const gfx::Affine &t) override
    {
        state_.transform = t.FollowedBy(state_.transform);
    }

    bool ClipToRect(gfx::RectI r) override
    {
        state_.clip = state_.clip.Intersection(r);
        return !state_.clip.IsEmpty();
    }

    void ClipToPath(const gfx::Path &path, const gfx::Affine &t) override
    {
        Record("ClipToPath").path   = path;
        calls.back().path_transform = t;
    }

    gfx::RectI ClipBounds() const override
    {
        return state_.clip;
    }

    float PhysicalPixelScale() const override
    {
        return physical_scale_;
    }

    void SetColor(gfx::Color c) override
    {
        state_.color       = c;
        state_.is_gradient = false;
    }

    void SetGradient(const gfx::Gradient &g) override
    {
        state_.gradient    = g;
        state_.is_gradient = true;
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const gfx::Font &font) override
    {
        state_.font = font;
    }

    void SetResampling(gfx::Resampling) override
    {
    }

    const gfx::Font &CurrentFont() const override
    {
        return state_.font;
    }

    void FillAll() override
    {
        Record("FillAll");
    }

    void FillAll(gfx::Color c) override
    {
        Record("FillAll").color = c;
    }

    void FillRect(gfx::RectI r) override
    {
        Record("FillRect").rect = r.ToFloat();
    }

    void FillRect(gfx::RectF r) override
    {
        Record("FillRect").rect = r;
    }

    void DrawRect(gfx::RectF r, float thickness) override
    {
        Shape("DrawRect", r, thickness);
    }

    void FillRoundedRect(gfx::RectF r, float corner_radius) override
    {
        Shape("FillRoundedRect", r, corner_radius);
    }

    void DrawRoundedRect(gfx::RectF r, float corner_radius, float thickness) override
    {
        Shape("DrawRoundedRect", r, corner_radius);
        stroke_widths.push_back(thickness);
    }

    void FillEllipse(gfx::RectF r) override
    {
        Shape("FillEllipse", r, 0.0f);
    }

    void DrawEllipse(gfx::RectF r, float thickness) override
    {
        Shape("DrawEllipse", r, thickness);
    }

    void DrawLine(gfx::PointF from, gfx::PointF to, float thickness) override
    {
        Shape("DrawLine", gfx::RectF::LeftTopRightBottom(from.x, from.y, to.x, to.y), thickness);
    }

    void DrawHorizontalLine(int y, float left, float right) override
    {
        Shape("DrawHorizontalLine",
              gfx::RectF::LeftTopRightBottom(left, static_cast<float>(y), right, static_cast<float>(y)), 0.0f);
    }

    void DrawVerticalLine(int x, float top, float bottom) override
    {
        Shape("DrawVerticalLine",
              gfx::RectF::LeftTopRightBottom(static_cast<float>(x), top, static_cast<float>(x), bottom), 0.0f);
    }

    void FillPath(const gfx::Path &path, const gfx::Affine &t) override
    {
        auto &call          = Record("FillPath");
        call.path           = path;
        call.path_transform = t;
    }

    void StrokePath(const gfx::Path &path, const gfx::Stroke &stroke, const gfx::Affine &t) override
    {
        auto &call          = Record("StrokePath");
        call.path           = path;
        call.path_transform = t;
        call.value          = stroke.thickness;
    }

    void DrawText(std::string_view text, gfx::RectF area, gfx::Justify, bool) override
    {
        auto &call = Record("DrawText");
        call.text  = std::string(text);
        call.rect  = area;
    }

    void DrawFittedText(std::string_view text, gfx::RectI area, gfx::Justify, int, float) override
    {
        auto &call = Record("DrawFittedText");
        call.text  = std::string(text);
        call.rect  = area.ToFloat();
    }

    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        return 0.5f * font.height * static_cast<float>(text.size());
    }

    float Ascent(const gfx::Font &font) const override
    {
        return 0.8f * font.height;
    }

    float Descent(const gfx::Font &font) const override
    {
        return 0.25f * font.height;
    }

    void DrawImage(const gfx::Image &image, gfx::RectF dest, bool) override
    {
        auto &call = Record("DrawImage");
        call.rect  = dest;
        call.image = image.data();
    }

    void DrawImageTransformed(const gfx::Image &image, const gfx::Affine &t) override
    {
        auto &call          = Record("DrawImageTransformed");
        call.image          = image.data();
        call.path_transform = t;
    }

    void BeginTransparencyLayer(float) override
    {
        Record("BeginTransparencyLayer");
    }

    void EndTransparencyLayer() override
    {
        Record("EndTransparencyLayer");
    }

    std::vector<float> stroke_widths; ///< every DrawRoundedRect's thickness, in order

  private:
    struct State
    {
        gfx::Affine   transform{};
        gfx::RectI    clip{};
        gfx::Color    color{};
        bool          is_gradient{false};
        gfx::Gradient gradient{};
        gfx::Font     font{};
    };

    Call &Record(std::string op)
    {
        Call call;
        call.op          = std::move(op);
        call.color       = state_.color;
        call.is_gradient = state_.is_gradient;
        call.gradient    = state_.gradient;
        call.font        = state_.font;
        call.transform   = state_.transform;
        calls.push_back(std::move(call));
        return calls.back();
    }

    void Shape(std::string op, gfx::RectF r, float value)
    {
        auto &call = Record(std::move(op));
        call.rect  = r;
        call.value = value;
    }

    float              physical_scale_;
    State              state_;
    std::vector<State> stack_;
};

// ---- An image backend that keeps no pixels ----------------------------------------------------------------------

/** A picture's size and how it was made: "512x512" decoded, "512x512[0,512]" a part of another at that corner, each
    resample after it ">256x256". How many are alive is counted, for what a cache holds. */
class FakeImage final : public gfx::Image::Data
{
  public:
    static inline int live = 0;

    FakeImage(int width, int height, std::string made) : width_(width), height_(height), made_(std::move(made))
    {
        ++live;
    }

    FakeImage(const FakeImage &)            = delete;
    FakeImage &operator=(const FakeImage &) = delete;

    ~FakeImage() override
    {
        --live;
    }

    int width() const noexcept override
    {
        return width_;
    }

    int height() const noexcept override
    {
        return height_;
    }

    const std::string &made() const noexcept
    {
        return made_;
    }

  private:
    int         width_;
    int         height_;
    std::string made_;
};

std::string Size(int width, int height)
{
    return std::to_string(width) + "x" + std::to_string(height);
}

const std::string &MadeOf(const gfx::Image &image)
{
    static const std::string kNothing = "(none)";
    const auto              *fake     = dynamic_cast<const FakeImage *>(image.data());
    return fake != nullptr ? fake->made() : kNothing;
}

/** The bytes a FakeImages backend decodes: no format, just the picture's size. */
struct FakeFile
{
    int width;
    int height;
};

class FakeImages final : public look::ImageBackend
{
  public:
    gfx::Image Decode(const void *data, std::size_t size) const override
    {
        ++decodes;
        if (size != sizeof(FakeFile))
        {
            return {};
        }
        const auto *file = static_cast<const FakeFile *>(data);
        return gfx::Image(std::make_shared<FakeImage>(file->width, file->height, Size(file->width, file->height)));
    }

    gfx::Image Clipped(const gfx::Image &image, gfx::RectI area) const override
    {
        ++clips;
        if (!image.IsValid())
        {
            return {};
        }
        const auto part = area.Intersection(image.Bounds());
        return gfx::Image(std::make_shared<FakeImage>(part.width(), part.height(),
                                                      MadeOf(image) + "[" + std::to_string(part.x()) + "," +
                                                          std::to_string(part.y()) + "]"));
    }

    gfx::Image Rescaled(const gfx::Image &image, int width, int height, gfx::Resampling quality) const override
    {
        ++rescales;
        are_all_high = are_all_high && quality == gfx::Resampling::kHigh;
        if (!image.IsValid())
        {
            return {};
        }
        return gfx::Image(std::make_shared<FakeImage>(width, height, MadeOf(image) + ">" + Size(width, height)));
    }

    gfx::Image Picture(int width, int height) const
    {
        const FakeFile file{width, height};
        return Decode(&file, sizeof(file));
    }

    mutable int  decodes{0};
    mutable int  clips{0};
    mutable int  rescales{0};
    mutable bool are_all_high{true};
};

bool Near(gfx::RectF a, gfx::RectF b, float tolerance = 1.0e-4f)
{
    return std::abs(a.x() - b.x()) <= tolerance && std::abs(a.y() - b.y()) <= tolerance &&
           std::abs(a.width() - b.width()) <= tolerance && std::abs(a.height() - b.height()) <= tolerance;
}

std::string Str(gfx::RectF r)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%.3f %.3f %.3f %.3f", static_cast<double>(r.x()), static_cast<double>(r.y()),
                  static_cast<double>(r.width()), static_cast<double>(r.height()));
    return text;
}

constexpr gfx::Color kPanel{0xff24262a};
constexpr gfx::Color kPlate{0xff141519};

// ---- The numbers ------------------------------------------------------------------------------------------------

TEST(LookMetrics, TheSeriesTableIsReadableWithNoJuce)
{
    // gui.md's table, one number each, in reckless::look.
    EXPECT_EQ(look::kKnobSize, 106);
    EXPECT_EQ(look::kKnobBlock, look::kKnobSize + 16) << "the knob and its value box";
    EXPECT_EQ(look::kKnobWidth, look::kKnobSize + 14);
    EXPECT_EQ(look::kLabelHeight, 18);
    EXPECT_EQ(look::kPlateHeight, 28);
    EXPECT_EQ(look::kSwitchStrip, look::kLabelHeight + look::kPlateHeight) << "a caption row and a row of plates";
    EXPECT_EQ(look::kHeaderHeight, 58);
    EXPECT_EQ(look::kMarginX, 32) << "clear of the corner screws, whose heads reach 28.5 px in";
    EXPECT_EQ(look::kMarginY, 10);
    EXPECT_EQ(look::kLampStrip, 94);
    EXPECT_EQ(look::kValueBoxWidth, 62);
    EXPECT_EQ(look::kValueBoxHeight, 17);
    EXPECT_EQ(look::kPresetReserve, look::kPresetPlateWidth + look::kResetGap);
    EXPECT_FLOAT_EQ(look::kCaptionFontSize, 12.1f);
    EXPECT_FLOAT_EQ(look::kSmallKnobSide, 60.0f);
    EXPECT_FLOAT_EQ(look::kTextScale, 1.1f);
    EXPECT_FLOAT_EQ(look::kTitleFontSize, 27.95f);
    EXPECT_EQ(look::kHeaderInset + look::kLogoWidth + look::kLogoGap, 97) << "where every title starts";
    EXPECT_EQ(look::colors::kInk.argb(), 0xffe8dcc8u) << "the cream";
    EXPECT_EQ(look::colors::kAmber.argb(), 0xffe0a038u) << "the amber";
    EXPECT_EQ(look::colors::kLensRim.argb(), 0xff7d7a75u) << "the chrome rim";
}

TEST(LookMetrics, AValueBoxIsEditedInTheDarkSchemesColoursAndADimmedControlIsFadedToTwoFifths)
{
    // A knob's value box being typed into, as the releases built on JUCE drew it - the dark scheme of JUCE's fourth
    // look, which the series' LookAndFeel never recolours there: white text on the box's transparent background made
    // opaque, so black; the selection in the scheme's blue at 40 % under white; the caret in the blue; the focused
    // outline in the scheme's grey. And a control a setting ignores faded to 0.4 with its caption.
    EXPECT_EQ(look::colors::kEditorText.argb(), 0xffffffffu);
    EXPECT_EQ(look::colors::kEditorBackground.argb(), 0xff000000u);
    EXPECT_EQ(look::colors::kEditorHighlight.argb(), 0x6642a2c8u) << "the blue at 40 %";
    EXPECT_EQ(look::colors::kEditorHighlightedText.argb(), 0xffffffffu);
    EXPECT_EQ(look::colors::kEditorCaret.argb(), 0xff42a2c8u);
    EXPECT_EQ(look::colors::kEditorFocusedOutline.argb(), 0xff8e989bu);
    EXPECT_FLOAT_EQ(look::kDimmedAlpha, 0.4f);
}

TEST(LookPalette, TheSeriesPaletteIsCreamInkAmberAccentAndAHalfStrengthAmberEdge)
{
    const auto palette = look::Palette::Series(kPanel, kPlate);
    EXPECT_EQ(palette.panel, kPanel);
    EXPECT_EQ(palette.plate, kPlate);
    EXPECT_EQ(palette.ink, look::colors::kInk);
    EXPECT_EQ(palette.accent, look::colors::kAmber);
    EXPECT_EQ(palette.plate_edge.WithAlpha(static_cast<std::uint8_t>(0xff)), look::colors::kAmber);
    EXPECT_EQ(palette.plate_edge.alpha(), 128) << "half strength: 127.5 rounds half to even";

    const gfx::Color umber{0xff2a1d10};
    EXPECT_EQ(look::Palette::Series(kPanel, kPlate, umber).ink, umber) << "a light plate prints in its own ink";
}

// ---- The plates ------------------------------------------------------------------------------------------------

TEST(LookPlates, TheLegendPlateStandsOnItsShadowWithTheArrowAtItsRight)
{
    const gfx::Color plate{0xff101010}, pressed{0xff080808}, edge{0x80e0a038}, arrow{0xcce8dcc8};
    RecordingCanvas  c;
    look::DrawLegendPlate(c, 100, 28, false, plate, pressed, edge, arrow);

    // The shadow first: eight ramps round a solid core, all under the plate.
    const auto fills = c.Of("FillRect");
    ASSERT_EQ(fills.size(), 9u);
    for (std::size_t i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(fills[i].is_gradient) << "piece " << i;
        EXPECT_EQ(fills[i].gradient.stops.size(), 10u) << "i^2 of the alpha at ten shares of the way out";
    }
    EXPECT_FALSE(fills[8].is_gradient);
    EXPECT_EQ(fills[8].color, gfx::colors::kBlack.WithAlpha(0.45f)) << "the core, solid";
    EXPECT_TRUE(Near(fills[8].rect, {6.0f, 7.0f, 92.0f, 20.0f}))
        << "the plate less half the radius, moved by (2, 3): " << Str(fills[8].rect);

    // The plate and its edge, a pixel in from the box; the arrow kArrowInset in from the plate's right edge.
    const auto plates = c.Of("FillRoundedRect");
    ASSERT_EQ(plates.size(), 1u);
    EXPECT_TRUE(Near(plates[0].rect, {1.0f, 1.0f, 98.0f, 26.0f}));
    EXPECT_FLOAT_EQ(plates[0].value, 4.0f) << "the corner";
    EXPECT_EQ(plates[0].color, plate);
    const auto edges = c.Of("DrawRoundedRect");
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].color, edge);
    EXPECT_FLOAT_EQ(c.stroke_widths[0], 1.2f);
    const auto arrows = c.Of("FillPath");
    ASSERT_EQ(arrows.size(), 1u);
    EXPECT_EQ(arrows[0].color, arrow);
    const auto tip = arrows[0].path.Bounds();
    EXPECT_TRUE(Near(tip, gfx::RectF::LeftTopRightBottom(83.0f, 11.5f, 91.0f, 17.0f), 1.0e-3f)) << Str(tip);
    EXPECT_FLOAT_EQ(100.0f - tip.x(), static_cast<float>(look::kArrowInset + look::kArrowHalfWidth + 1))
        << "the arrow reaches kArrowInset + kArrowHalfWidth + 1 in from the box's edge";

    RecordingCanvas held;
    look::DrawLegendPlate(held, 100, 28, true, plate, pressed, edge, arrow);
    EXPECT_EQ(held.Of("FillRoundedRect")[0].color, pressed) << "darker while pressed";
}

TEST(LookPlates, TheSwitchPlateIsTheLegendPlateInThePalettesColours)
{
    const auto      palette = look::Palette::Series(kPanel, kPlate);
    RecordingCanvas c;
    look::DrawSwitchPlate(c, palette, 120, 28, false);
    EXPECT_EQ(c.Of("FillRoundedRect")[0].color, palette.plate);
    EXPECT_EQ(c.Of("DrawRoundedRect")[0].color, palette.plate_edge);
    EXPECT_EQ(c.Of("FillPath")[0].color, palette.ink.WithAlpha(0.8f)) << "the arrow in the ink";

    RecordingCanvas held;
    look::DrawSwitchPlate(held, palette, 120, 28, true);
    EXPECT_EQ(held.Of("FillRoundedRect")[0].color, palette.plate.Darker(0.4f));
}

TEST(LookPlates, AButtonLightsUnderTheMouseAndMoreWhileHeldAndItsEdgeComesUpWhenOn)
{
    const auto palette = look::Palette::Series(kPanel, kPlate);

    struct Case
    {
        bool       on, over, down;
        gfx::Color fill;
        float      edge_alpha;
    };

    const Case cases[] = {
        {false, false, false, palette.plate, 1.0f},
        {false, true, false, palette.plate.Brighter(look::kPlateLitLift), 1.6f},
        {false, true, true, palette.plate.Brighter(look::kPlatePressedLift), 1.6f},
        {true, false, false, palette.plate.Brighter(look::kPlateLitLift), 1.8f},
    };
    for (const auto &k : cases)
    {
        RecordingCanvas c;
        look::DrawButtonPlate(c, palette, {10, 10, 96, 28}, k.on, k.over, k.down);
        ASSERT_EQ(c.calls.size(), 2u) << "a plate and its edge, no shadow";
        EXPECT_TRUE(Near(c.calls[0].rect, {11.0f, 11.0f, 94.0f, 26.0f}));
        EXPECT_EQ(c.calls[0].color, k.fill);
        EXPECT_EQ(c.calls[1].color, palette.plate_edge.WithMultipliedAlpha(k.edge_alpha));
    }
}

TEST(LookPlates, AMenuPlatesArrowIsTheLegendPlatesAndAToggleLampMirrorsIt)
{
    // The arrow a button that opens a menu carries is the legend plate's own: the same triangle, kArrowInset in from
    // the right of the plate a pixel inside the bounds.
    RecordingCanvas legend;
    look::DrawLegendPlate(legend, 96, 28, false, kPlate, kPlate, kPlate, look::colors::kInk);
    RecordingCanvas arrow;
    look::DrawPlateArrow(arrow, {10, 10, 96, 28}, look::colors::kInk);
    ASSERT_EQ(arrow.Of("FillPath").size(), 1u);
    EXPECT_TRUE(
        Near(arrow.Of("FillPath")[0].path.Bounds(), legend.Of("FillPath")[0].path.Bounds().Translated(10, 10), 1.0e-4f))
        << "the legend plate's arrow, where the bounds put it";
    EXPECT_EQ(arrow.Of("FillPath")[0].color, look::colors::kInk);

    // The lamp: kPlateLampDiameter across, its centre kArrowInset in from the plate's left edge on its centre line -
    // the arrow's place mirrored - in a chrome rim; dark, the lens alone; lit, two rings of glow round an amber lens.
    RecordingCanvas dark;
    look::DrawPlateLamp(dark, {10, 10, 96, 28}, false);
    const auto lens = dark.Of("FillEllipse");
    ASSERT_EQ(lens.size(), 1u) << "dark: the lens alone, no glow";
    EXPECT_TRUE(lens[0].is_gradient);
    EXPECT_TRUE(Near(lens[0].rect, gfx::RectF(look::kPlateLampDiameter, look::kPlateLampDiameter)
                                       .WithCentre({11.0f + static_cast<float>(look::kArrowInset), 24.0f})))
        << Str(lens[0].rect);
    ASSERT_EQ(dark.Of("DrawEllipse").size(), 1u);
    EXPECT_EQ(dark.Of("DrawEllipse")[0].color, look::colors::kLensRim) << "the chrome rim";

    RecordingCanvas lit;
    look::DrawPlateLamp(lit, {10, 10, 96, 28}, true);
    const auto glow = lit.Of("FillEllipse");
    ASSERT_EQ(glow.size(), 3u) << "lit: two rings of glow, then the lens";
    EXPECT_GT(glow[0].rect.width(), glow[1].rect.width()) << "the outer ring first";
    EXPECT_GT(glow[1].rect.width(), glow[2].rect.width());
    EXPECT_EQ(glow[0].color.WithAlpha(1.0f), look::colors::kAmber) << "the glow amber";
    EXPECT_TRUE(Near(glow[2].rect, lens[0].rect)) << "the lens where it sits dark";
    EXPECT_TRUE(glow[2].is_gradient);
    EXPECT_EQ(glow[2].gradient.end_color, look::colors::kAmber) << "amber to its rim";
}

// ---- The header ------------------------------------------------------------------------------------------------

TEST(LookHeader, TheRuleLiesOnTheSeamAndTheTitleFollowsTheMark)
{
    const FakeImages        images;
    const look::CompanyMark mark(images, images.Picture(512, 512));
    const gfx::Color        rule{0xff3a7bd5};
    const gfx::RectI        header{0, 0, 600, look::kHeaderHeight};
    RecordingCanvas         c;
    look::DrawHeader(c, mark, header, "RECKLESS GATE", "v0.1.3", look::colors::kInk, rule);

    // The rule is the last thing drawn, on the row under the header - the plate's seam - across its whole width.
    ASSERT_FALSE(c.calls.empty());
    const auto &last = c.calls.back();
    EXPECT_EQ(last.op, "DrawHorizontalLine");
    EXPECT_FLOAT_EQ(last.rect.y(), static_cast<float>(header.bottom())) << "the seam, row 58";
    EXPECT_FLOAT_EQ(last.rect.x(), 0.0f);
    EXPECT_FLOAT_EQ(last.rect.right(), 600.0f);
    EXPECT_EQ(last.color, rule);

    // The mark kHeaderInset in, fitted to its cell held kLogoInset off the edges: 48 px of a 52 px cell, centred.
    const auto drawn = c.Of("DrawImage");
    ASSERT_EQ(drawn.size(), 1u);
    EXPECT_TRUE(Near(drawn[0].rect, {42.0f, 5.0f, 48.0f, 48.0f})) << Str(drawn[0].rect);

    // The title on a row as tall as its font, centred on the header, after the mark and its gap; the stamp after it.
    const auto texts = c.Of("DrawText");
    ASSERT_EQ(texts.size(), 2u);
    std::printf("  header 600 x %d: mark %s, title %s, stamp %s, rule on row %.0f\n", look::kHeaderHeight,
                Str(drawn[0].rect).c_str(), Str(texts[0].rect).c_str(), Str(texts[1].rect).c_str(),
                static_cast<double>(last.rect.y()));
    const float title_height = look::kTitleFontSize * look::kTextScale;
    EXPECT_EQ(texts[0].text, "RECKLESS GATE");
    EXPECT_EQ(texts[0].color, look::colors::kInk);
    EXPECT_TRUE(texts[0].font.bold);
    EXPECT_FLOAT_EQ(texts[0].font.height, title_height);
    const int rows = static_cast<int>(std::lrint(title_height));
    EXPECT_TRUE(Near(texts[0].rect, gfx::RectF(97.0f, static_cast<float>((look::kHeaderHeight - rows) / 2), 463.0f,
                                               static_cast<float>(rows))))
        << Str(texts[0].rect);
    EXPECT_EQ(texts[1].text, "v0.1.3");
    EXPECT_EQ(texts[1].color, look::colors::kInk.WithAlpha(0.35f)) << "the stamp at 35 % of the ink";
    EXPECT_FLOAT_EQ(texts[1].font.height, look::kVersionFontSize * look::kTextScale);
    const int title_width = static_cast<int>(std::ceil(0.5f * title_height * 13.0f)); // the rule's measure
    EXPECT_FLOAT_EQ(texts[1].rect.x(), texts[0].rect.x() + static_cast<float>(title_width + look::kVersionGap));
    const float stamp_height = texts[1].font.height; // the shift is half the difference of (ascent - descent)
    const float shift =
        0.5f * ((0.8f * title_height - 0.25f * title_height) - (0.8f * stamp_height - 0.25f * stamp_height));
    EXPECT_FLOAT_EQ(texts[1].rect.y(), texts[0].rect.y() + static_cast<float>(std::lrint(shift)))
        << "the stamp's baseline on the title's";
}

TEST(LookHeader, ALampStripRepaintDrawsOnlyTheRule)
{
    // A lamp repainting its strip clips the paint to it; the content stops right_reserve short of the edge, so
    // nothing of it is inside the clip and neither the mark nor the title is laid out.
    const FakeImages        images;
    const look::CompanyMark mark(images, images.Picture(512, 512));
    const gfx::RectI        header{0, 0, 600, look::kHeaderHeight};
    RecordingCanvas         c(1.0f, look::LampStrip(header));
    look::DrawHeader(c, mark, header, "RECKLESS VIBE", "v0.1.3", look::colors::kInk, look::colors::kAmber,
                     look::kLampStrip);
    ASSERT_EQ(c.calls.size(), 1u);
    EXPECT_EQ(c.calls[0].op, "DrawHorizontalLine");
    EXPECT_EQ(images.rescales, 0) << "no mark was made for a paint that cannot show it";
}

TEST(LookHeader, WithNoMarkTheTitleStartsWhereTheMarkWouldHaveBeen)
{
    const FakeImages        images;
    const look::CompanyMark none(images, gfx::Image());
    RecordingCanvas         c;
    look::DrawHeader(c, none, {0, 0, 600, look::kHeaderHeight}, "RECKLESS GATE", {}, look::colors::kInk,
                     look::colors::kAmber);
    EXPECT_TRUE(c.Of("DrawImage").empty());
    const auto texts = c.Of("DrawText");
    ASSERT_EQ(texts.size(), 1u) << "no version, no stamp";
    EXPECT_FLOAT_EQ(texts[0].rect.x(), static_cast<float>(look::kHeaderInset));
}

TEST(LookHeader, TheMarkIsShrunkByHalvingToThePhysicalPixelsItCoversAndDrawnOnWholeOnes)
{
    // 48 design pixels at 1x is a tenth of the 512 px asset: halved while at least twice the size, then one resample;
    // at 2x it is 96 physical pixels and the halving stops sooner. Either way it lands on whole physical pixels at the
    // mark's own size, so the draw is a copy, and a size once made is not made again.
    struct Case
    {
        float       scale;
        std::string made;
        gfx::RectF  where;
    };

    const Case cases[] = {
        {1.0f, "512x512>256x256>128x128>64x64>48x48", {42.0f, 5.0f, 48.0f, 48.0f}},
        {2.0f, "512x512>256x256>128x128>96x96", {42.0f, 5.0f, 48.0f, 48.0f}},
        {1.5f, "512x512>256x256>128x128>72x72", {42.0f, 8.0f / 1.5f, 48.0f, 48.0f}},
    };
    for (const auto &k : cases)
    {
        const FakeImages        images;
        const look::CompanyMark mark(images, images.Picture(512, 512));
        RecordingCanvas         c(k.scale);
        mark.Draw(c, gfx::RectF(40.0f, 0.0f, 52.0f, 58.0f).Reduced(0.0f, 5.0f));
        const auto drawn = c.Of("DrawImage");
        ASSERT_EQ(drawn.size(), 1u);
        const auto *fake = dynamic_cast<const FakeImage *>(drawn[0].image);
        ASSERT_NE(fake, nullptr);
        std::printf("  mark at %.1fx: %s, drawn at %s\n", static_cast<double>(k.scale), fake->made().c_str(),
                    Str(drawn[0].rect).c_str());
        EXPECT_EQ(fake->made(), k.made) << "at " << k.scale << "x";
        EXPECT_TRUE(Near(drawn[0].rect, k.where)) << Str(drawn[0].rect) << " at " << k.scale << "x";
        EXPECT_TRUE(images.are_all_high) << "every resample at the best quality";

        const int made = images.rescales;
        mark.Draw(c, gfx::RectF(40.0f, 0.0f, 52.0f, 58.0f).Reduced(0.0f, 5.0f));
        EXPECT_EQ(images.rescales, made) << "the second draw at that size reuses the first's mark";
    }
}

TEST(LookHeader, TheMarkKeepsTheLastSizeAloneSoADraggedWindowHoldsOneMark)
{
    const FakeImages        images;
    const look::CompanyMark mark(images, images.Picture(512, 512));
    const int               asset = FakeImage::live;
    for (int step = 0; step <= 100; ++step)
    {
        RecordingCanvas c(1.0f + 0.01f * static_cast<float>(step));
        mark.Draw(c, {42.0f, 5.0f, 48.0f, 48.0f});
    }
    EXPECT_EQ(FakeImage::live - asset, 1) << "the 96 px mark of the last zoom, and none of the sizes before it";
    EXPECT_EQ(mark.AtSize(96, 96).width(), 96);
}

// ---- The lamp and the indicators ---------------------------------------------------------------------------------

TEST(LookLamp, TheJewelSitsInTheStripClearOfTheScrewAndGlowsWithItsArgument)
{
    const gfx::RectI header{0, 0, 400, look::kHeaderHeight};
    EXPECT_EQ(look::LampStrip(header), (gfx::RectI{306, 0, 94, look::kHeaderHeight}));

    const gfx::Color rim{0xff7d7a75};
    const gfx::RectF jewel{324.0f, 16.0f, 30.0f, 26.0f}; // the strip less 28 at the right, less 18 x 16 all round
    for (const double glow : {0.0, 0.5, 1.0, 2.0})
    {
        const float     lit = static_cast<float>(std::min(glow, 1.0));
        RecordingCanvas c;
        look::DrawLamp(c, header, glow, look::colors::kAmber, rim);
        ASSERT_EQ(c.calls.size(), 3u);
        EXPECT_TRUE(Near(c.calls[0].rect, jewel.Expanded(9.0f * lit + 3.0f))) << "the halo grows with the glow";
        EXPECT_EQ(c.calls[0].color, look::colors::kAmber.WithAlpha(0.10f + 0.35f * lit));
        EXPECT_TRUE(Near(c.calls[1].rect, jewel));
        EXPECT_EQ(c.calls[1].color, look::colors::kAmber.WithAlpha(0.25f + 0.75f * lit));
        EXPECT_EQ(c.calls[2].op, "DrawEllipse");
        EXPECT_EQ(c.calls[2].color, rim);
    }
}

TEST(LookIndicators, TheLensSitsOnTheFloorWithItsCaptionUnderIt)
{
    const gfx::RectI cell{0, 0, look::kLedCell, 58};
    const float      floor = look::IndicatorFloor(cell);
    EXPECT_FLOAT_EQ(floor, 3.0f + static_cast<float>(look::kTubeHeight));

    RecordingCanvas dark;
    look::DrawLedCell(dark, cell, "GATE", gfx::Color{0xff40c060}, 0.0);
    const auto lens  = gfx::RectF(look::kLensSize, look::kLensSize).WithCentre({30.0f, floor - 1.0f - 7.5f});
    const auto fills = dark.Of("FillEllipse");
    ASSERT_EQ(fills.size(), 1u) << "unlit, no halo";
    EXPECT_TRUE(Near(fills[0].rect, lens)) << Str(fills[0].rect);
    EXPECT_TRUE(fills[0].is_gradient && fills[0].gradient.radial) << "a shaded lens";
    EXPECT_EQ(fills[0].gradient.start_color, gfx::Color{0xff2a2825}.Brighter(0.6f)) << "the dark glass";
    const auto rims = dark.Of("DrawEllipse");
    ASSERT_EQ(rims.size(), 1u);
    EXPECT_EQ(rims[0].color, look::colors::kLensRim);
    const auto captions = dark.Of("DrawText");
    ASSERT_EQ(captions.size(), 1u);
    EXPECT_EQ(captions[0].text, "GATE");
    EXPECT_TRUE(Near(captions[0].rect,
                     {0.0f, floor + static_cast<float>(look::kIndicatorCaptionGap), static_cast<float>(look::kLedCell),
                      static_cast<float>(look::kIndicatorCaptionHeight)}));
    EXPECT_EQ(captions[0].color, look::colors::kInk.WithAlpha(0.45f)) << "the fine print in the cream at 45 %";
    EXPECT_FLOAT_EQ(captions[0].font.height, look::kFinePrintFontSize * look::kTextScale);

    RecordingCanvas lit;
    look::DrawLedCell(lit, cell, "GATE", gfx::Color{0xff40c060}, 1.0);
    ASSERT_EQ(lit.Of("FillEllipse").size(), 2u) << "lit, a halo behind the lens";
    EXPECT_TRUE(Near(lit.Of("FillEllipse")[0].rect, lens.Expanded(6.0f)));
}

TEST(LookIndicators, ATubeStandsOnTheFloorTheEl34FullHeightThe12ax7Smaller)
{
    const gfx::RectI cell{0, 0, 96, 140};
    const float      floor = look::IndicatorFloor(cell, look::kBayTubeHeight);
    for (const auto kind : {look::TubeGlyph::Kind::kEL34, look::TubeGlyph::Kind::k12AX7})
    {
        const auto &glyph  = look::TubeGlyph::Of(kind);
        const float height = (kind == look::TubeGlyph::Kind::kEL34 ? 1.0f : look::kSmallTubeShare) *
                             static_cast<float>(look::kBayTubeHeight);
        const float width  = height * glyph.bounds().width() / glyph.bounds().height();
        const auto  area   = gfx::RectF(width, height).WithCentre({48.0f, floor - 0.5f * height});
        const auto  fit    = glyph.FitTo(area);

        RecordingCanvas cold;
        look::DrawTubeCell(cold, cell, "OUTPUT", kind, look::colors::kAmber, 0.0, look::kBayTubeHeight);
        EXPECT_TRUE(cold.Of("FillEllipse").empty()) << "cold, no halo";
        // The glass's tint is the first path filled: through the tube's fit onto its area, standing on the floor.
        const auto paths = cold.Of("FillPath");
        ASSERT_FALSE(paths.empty());
        const auto bottom = paths[0].path_transform.Apply({0.0f, glyph.bounds().bottom()});
        EXPECT_NEAR(bottom.y, floor, 1.0e-3f) << "the pins' ends on the floor";
        EXPECT_NEAR(bottom.x, 48.0f, 1.0e-3f) << "centred in the cell";
        EXPECT_NEAR(paths[0].path_transform.m00, fit.scale, 1.0e-6f) << "the part's millimetres to the area's pixels";
        const auto captions = cold.Of("DrawText");
        ASSERT_EQ(captions.size(), 1u);
        EXPECT_FLOAT_EQ(captions[0].rect.y(), floor + static_cast<float>(look::kIndicatorCaptionGap));

        RecordingCanvas lit;
        look::DrawTubeCell(lit, cell, "OUTPUT", kind, look::colors::kAmber, 1.0, look::kBayTubeHeight);
        const auto halo = lit.Of("FillEllipse");
        ASSERT_EQ(halo.size(), 1u) << "lit, an amber halo behind the envelope";
        EXPECT_TRUE(halo[0].is_gradient && halo[0].gradient.radial);
        EXPECT_NEAR(halo[0].rect.CentreX(), fit.heater.x, 1.0e-3f) << "lit from the heater";
        EXPECT_NEAR(halo[0].rect.CentreY(), fit.heater.y, 1.0e-3f);
        EXPECT_FLOAT_EQ(halo[0].rect.width(), 2.0f * 0.8f * static_cast<float>(look::kBayTubeHeight));
    }
}

TEST(LookTubes, ATubeIsTallAndThinItsGlassClearColdAndLitFromItsHeater)
{
    for (const auto kind : {look::TubeGlyph::Kind::k12AX7, look::TubeGlyph::Kind::kEL34})
    {
        const auto &tube = look::TubeGlyph::Of(kind);
        const auto  mm   = tube.bounds();
        EXPECT_GT(mm.height(), 2.0f * mm.width()) << "a tube is tall and thin";
        EXPECT_TRUE(mm.Contains(tube.heater()) && mm.Contains(tube.glass_sample()) && mm.Contains(tube.plate_sample()));

        // Fitted into an area twice as wide as it needs, the tube is centred and keeps its proportions.
        const gfx::RectF area{10.0f, 20.0f, 2.0f * mm.width() * 4.0f, mm.height() * 4.0f};
        const auto       fit = tube.FitTo(area);
        EXPECT_FLOAT_EQ(fit.scale, 4.0f) << "the height decides";
        const auto top_left     = fit.to_pixels.Apply(mm.TopLeft());
        const auto bottom_right = fit.to_pixels.Apply(mm.BottomRight());
        EXPECT_NEAR(top_left.y, area.y(), 1.0e-3f);
        EXPECT_NEAR(bottom_right.y, area.bottom(), 1.0e-3f);
        EXPECT_NEAR(0.5f * (top_left.x + bottom_right.x), area.CentreX(), 1.0e-3f) << "letterboxed at the sides";

        // Cold: no light inside the glass, nothing clipped to it but the getter's silvering. Lit: the heater's glow
        // clipped to the envelope first, a warm ramp and the yellow-white core, and the plates run red over themselves.
        RecordingCanvas cold;
        tube.Draw(cold, fit, look::colors::kInk, look::colors::kAmber, 0.0);
        EXPECT_EQ(cold.Of("ClipToPath").size(), 1u);
        EXPECT_TRUE(cold.Of("FillRect").empty());
        RecordingCanvas lit;
        tube.Draw(lit, fit, look::colors::kInk, look::colors::kAmber, 1.0);
        EXPECT_EQ(lit.calls[0].op, "ClipToPath") << "the glow is the tube's, inside its glass";
        const auto glow = lit.Of("FillRect");
        ASSERT_EQ(glow.size(), 2u);
        EXPECT_TRUE(glow[0].is_gradient && glow[1].is_gradient);
        EXPECT_NEAR(glow[0].gradient.start.x, fit.heater.x, 1.0e-4f);
        EXPECT_EQ(lit.Of("FillPath").size(), cold.Of("FillPath").size() + 2) << "the plates and fins reddened";
        // The outline last but one, in the panel's ink at 62 %; the reflection over it.
        const auto strokes = lit.Of("StrokePath");
        ASSERT_GE(strokes.size(), 2u);
        EXPECT_EQ(strokes[strokes.size() - 2].color, look::colors::kInk.WithAlpha(0.62f));
    }
}

TEST(LookFrames, EachFrameIsARoundedRectangleInTheInkAtSixtyPercent)
{
    const std::vector<gfx::RectF> frames{{26.6f, 64.6f, 136.8f, 158.8f}, {170.6f, 64.6f, 136.8f, 158.8f}};
    RecordingCanvas               c;
    look::DrawFrames(c, frames, look::colors::kInk);
    const auto drawn = c.Of("DrawRoundedRect");
    ASSERT_EQ(drawn.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        EXPECT_TRUE(Near(drawn[i].rect, frames[i]));
        EXPECT_FLOAT_EQ(drawn[i].value, 6.0f) << "6 px corners";
        EXPECT_FLOAT_EQ(c.stroke_widths[i], 1.2f) << "the plates' weight";
        EXPECT_EQ(drawn[i].color, look::colors::kInk.WithAlpha(0.6f));
    }
}

// ---- The chicken head ---------------------------------------------------------------------------------------------

TEST(LookKnob, TheFrameNearestTheAngleIsDrawnAtThePhysicalSizeItLandsOn)
{
    // A strip of 128 frames of 512 px: frame i at 2 pi i / 128 clockwise from twelve.
    const FakeImages images;
    look::KnobStrip  knob(images, images.Picture(512, 128 * 512));
    const gfx::RectI area{100, 50, 68, 68};
    constexpr float  kSide  = 60.0f;
    constexpr float  kTwoPi = static_cast<float>(2.0 * std::numbers::pi);

    struct Case
    {
        float       angle;
        std::size_t frame;
    };

    const Case cases[] = {{0.0f, 0},
                          {kTwoPi * 10.0f / 128.0f, 10},
                          {kTwoPi * 10.4f / 128.0f, 10},
                          {kTwoPi * 10.6f / 128.0f, 11},
                          {-kTwoPi / 512.0f, 0},
                          {kTwoPi * 127.6f / 128.0f, 0},
                          {kTwoPi * (1.0f + 64.0f / 128.0f), 64}};
    for (const auto &k : cases)
    {
        RecordingCanvas c;
        knob.Draw(c, area, k.angle, kSide);
        const auto drawn = c.Of("DrawImage");
        ASSERT_EQ(drawn.size(), 1u);
        EXPECT_EQ(drawn[0].image, knob.FramesAtSize(60)[k.frame].data()) << "angle " << k.angle;
        EXPECT_TRUE(Near(drawn[0].rect, gfx::RectF(kSide, kSide).WithCentre({134.0f, 84.0f})))
            << "centred in the knob area less 4 px a side: " << Str(drawn[0].rect);
    }

    // Each frame cut from the strip and halved while at least 4x the size, then resampled once: 512 to 60 by way of
    // 256 and 128; at 2x, 120 px, by way of 256 alone.
    const auto &at_1x = knob.FramesAtSize(60);
    ASSERT_EQ(at_1x.size(), 128u) << "the count read from the strip";
    std::printf("  a 60 px knob's frame 0 at 1x: %s; at 2x: %s\n", MadeOf(at_1x[0]).c_str(),
                MadeOf(knob.FramesAtSize(120)[0]).c_str());
    EXPECT_EQ(MadeOf(at_1x[0]), "512x65536[0,0]>256x256>128x128>60x60");
    EXPECT_EQ(MadeOf(at_1x[127]), "512x65536[0,65024]>256x256>128x128>60x60");
    RecordingCanvas twice(2.0f);
    knob.Draw(twice, area, 0.0f, kSide);
    EXPECT_EQ(MadeOf(knob.FramesAtSize(120)[0]), "512x65536[0,0]>256x256>120x120");
    EXPECT_EQ(twice.Of("DrawImage")[0].image, knob.FramesAtSize(120)[0].data()) << "the 2x frames at 2x";
    EXPECT_TRUE(images.are_all_high);
    const int clips = images.clips;
    knob.Draw(twice, area, 1.0f, kSide);
    EXPECT_EQ(images.clips, clips) << "a size once made is not made again";

    // The side an area gives a knob that has none of its own: 2 kKnobMargin x half the shorter side less 4 px a side.
    EXPECT_FLOAT_EQ(look::KnobStrip::SideFor(78, 50), 58.8f);
    EXPECT_FLOAT_EQ(look::KnobStrip::SideFor(30, 20), 16.8f);
    EXPECT_FLOAT_EQ(look::KnobStrip::SideFor(4, 4), 0.0f) << "never under nothing";
}

TEST(LookKnob, AZoomsSizesAreLetGoAtTheNextZoomSoADraggedWindowHoldsOneSetOfFrames)
{
    // A window's corner dragged from 100 % to 200 % draws its knobs at a new physical size nearly every frame. The
    // strip keeps the sizes it draws at the scale in force - a panel's knobs and its small ones, each made once - and
    // lets them go when it draws at another, so what it holds at the end is the last zoom's two sizes, a frame per
    // angle each, and not the hundred sizes it passed through.
    const FakeImages images;
    look::KnobStrip  knob(images, images.Picture(512, 128 * 512));
    const int        strip = FakeImage::live;
    for (int step = 0; step <= 100; ++step)
    {
        RecordingCanvas c(1.0f + 0.01f * static_cast<float>(step));
        knob.Draw(c, {100, 50, 68, 68}, 0.0f, 60.0f);
        knob.Draw(c, {0, 0, 40, 40}, 0.5f, 30.0f); // a small knob
        const int clips = images.clips;
        knob.Draw(c, {200, 50, 68, 68}, 1.0f, 60.0f); // another of the first size: its frames already made
        EXPECT_EQ(images.clips, clips) << "a size drawn twice at one zoom is made once, at step " << step;
    }
    std::printf("  a drag from 1x to 2x: %d frames held, the strip aside\n", FakeImage::live - strip);
    EXPECT_EQ(FakeImage::live - strip, 2 * 128) << "the 120 px and the 60 px frames of the last zoom, and no others";
    EXPECT_EQ(MadeOf(knob.FramesAtSize(120)[0]), "512x65536[0,0]>256x256>120x120");
}

TEST(LookKnob, StripsMadeFromOneAssetShareItsPictureAndTheirFramesAndLetThemGoTogether)
{
    // Every panel makes its knob strip from the module's one embedded file: the strips share one decoded picture and
    // the frames at a size more than one draws at, made once; once the last strip goes, nothing of them is kept, and
    // the next strip decodes the file afresh. A strip from other bytes is its own.
    const FakeImages      images;
    static const FakeFile kAsset{512, 128 * 512};
    static const FakeFile kOther{512, 128 * 512};
    const auto            bytes = [](const FakeFile &file) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(&file), sizeof(file));
    };
    const int none = FakeImage::live;
    {
        auto a = look::KnobStrip::Shared(images, bytes(kAsset));
        auto b = look::KnobStrip::Shared(images, bytes(kAsset));
        EXPECT_EQ(images.decodes, 1) << "decoded once";
        ASSERT_TRUE(a.strip().IsValid());
        EXPECT_EQ(a.strip().data(), b.strip().data()) << "one picture";

        RecordingCanvas c;
        a.Draw(c, {100, 50, 68, 68}, 0.0f, 60.0f);
        const int clips = images.clips;
        b.Draw(c, {100, 50, 68, 68}, 0.0f, 60.0f);
        EXPECT_EQ(images.clips, clips) << "the second strip draws the first one's frames";
        EXPECT_EQ(a.FramesAtSize(60)[7].data(), b.FramesAtSize(60)[7].data());
        EXPECT_EQ(FakeImage::live - none, 1 + 128) << "one picture, one set of frames";

        RecordingCanvas twice(2.0f);
        a.Draw(twice, {100, 50, 68, 68}, 0.0f, 60.0f);
        EXPECT_EQ(FakeImage::live - none, 1 + 2 * 128) << "b still holds the 1x frames, a the 2x";
        b.Draw(twice, {100, 50, 68, 68}, 0.0f, 60.0f);
        EXPECT_EQ(FakeImage::live - none, 1 + 128) << "neither holds the 1x frames now";

        const auto other = look::KnobStrip::Shared(images, bytes(kOther));
        EXPECT_EQ(images.decodes, 2);
        EXPECT_NE(other.strip().data(), a.strip().data()) << "other bytes, another picture";
    }
    EXPECT_EQ(FakeImage::live, none) << "the last strip gone, nothing kept";
    const auto again = look::KnobStrip::Shared(images, bytes(kAsset));
    EXPECT_EQ(images.decodes, 3) << "decoded afresh";
    const auto bare = look::KnobStrip::Shared(images, {});
    EXPECT_FALSE(bare.strip().IsValid()) << "no bytes, no picture";
    EXPECT_EQ(images.decodes, 3) << "and nothing decoded for it";
}

// ---- The images and where they land --------------------------------------------------------------------------------

TEST(LookImages, AShrunkImageIsHalvedWhileTwiceTheSizeThenResampledOnce)
{
    const FakeImages images;
    EXPECT_EQ(MadeOf(look::ShrunkImage(images, images.Picture(512, 512), 48, 48)),
              "512x512>256x256>128x128>64x64>48x48");
    EXPECT_EQ(MadeOf(look::ShrunkImage(images, images.Picture(100, 100), 60, 60)), "100x100>60x60")
        << "under twice the size: one resample";
    EXPECT_EQ(MadeOf(look::ShrunkImage(images, images.Picture(512, 256), 48, 48)), "512x256>256x128>128x64>48x48")
        << "halved while both sides are twice theirs";
    EXPECT_EQ(MadeOf(look::ShrunkImage(images, images.Picture(512, 512), 0, -3)),
              "512x512>256x256>128x128>64x64>32x32>16x16>8x8>4x4>2x2>1x1>1x1")
        << "a size under a pixel is a pixel, the last resample to the size it already has";
    EXPECT_FALSE(look::ShrunkImage(images, gfx::Image(), 48, 48).IsValid()) << "nothing shrinks to nothing";
    EXPECT_TRUE(images.are_all_high);
}

TEST(LookGeometry, APictureFitsCentredAndLandsOnWholePhysicalPixels)
{
    EXPECT_TRUE(
        Near(look::CentredFit({0.0f, 0.0f, 512.0f, 512.0f}, {40.0f, 5.0f, 52.0f, 48.0f}), {42.0f, 5.0f, 48.0f, 48.0f}))
        << "a square in a wide cell: letterboxed at the sides";
    EXPECT_TRUE(
        Near(look::CentredFit({0.0f, 0.0f, 80.0f, 51.0f}, {0.0f, 0.0f, 80.0f, 80.0f}), {0.0f, 14.5f, 80.0f, 51.0f}));
    EXPECT_TRUE(
        Near(look::CentredFit({3.0f, 4.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 80.0f, 80.0f}), {3.0f, 4.0f, 0.0f, 10.0f}))
        << "no area, no move";
    EXPECT_TRUE(Near(look::OnPhysicalPixels({42.3f, 5.6f, 47.0f, 47.0f}, 48, 48, 1.0f), {42.0f, 6.0f, 48.0f, 48.0f}));
    EXPECT_TRUE(Near(look::OnPhysicalPixels({42.3f, 5.6f, 47.0f, 47.0f}, 96, 96, 2.0f), {42.5f, 5.5f, 48.0f, 48.0f}));
    EXPECT_FLOAT_EQ(look::FitScale({0.0f, 0.0f, 20.0f, 50.0f}, {0.0f, 0.0f, 100.0f, 100.0f}), 2.0f);
    const auto onto = look::FitTransform({-10.0f, 0.0f, 20.0f, 50.0f}, {0.0f, 0.0f, 100.0f, 100.0f});
    EXPECT_NEAR(onto.Apply({0.0f, 25.0f}).x, 50.0f, 1.0e-4f) << "the source's centre on the target's";
    EXPECT_NEAR(onto.Apply({0.0f, 25.0f}).y, 50.0f, 1.0e-4f);
}

// ---- The meters' scales ------------------------------------------------------------------------------------------

TEST(LookMeters, TheScalesMeetTheArtworksReferencePointsAndPrintTheirFigures)
{
    // The gain-reduction meter's log(1 + GR / 4) arc and the level meter's half-power law, at the artwork's marks.
    using Gain  = look::GainReductionScale;
    using Level = look::LevelMeterScale;
    std::printf("  gain reduction  dB:      0      3      5     10     20     30\n"
                "                  deg: %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f\n",
                Gain::AngleDegrees(0.0), Gain::AngleDegrees(3.0), Gain::AngleDegrees(5.0), Gain::AngleDegrees(10.0),
                Gain::AngleDegrees(20.0), Gain::AngleDegrees(30.0));
    std::printf("  level (face)    dB:    -30    -10      0     +6\n"
                "                  deg: %6.1f %6.1f %6.1f %6.1f\n",
                Level::AngleDegrees(-30.0), Level::AngleDegrees(-10.0), Level::AngleDegrees(0.0),
                Level::AngleDegrees(6.0));
    EXPECT_DOUBLE_EQ(Gain::AngleDegrees(0.0), 34.0) << "at rest, on the right";
    EXPECT_NEAR(Gain::AngleDegrees(3.0), 16.2, 0.05);
    EXPECT_NEAR(Gain::AngleDegrees(5.0), 8.2, 0.05);
    EXPECT_NEAR(Gain::AngleDegrees(10.0), -5.8, 0.05);
    EXPECT_NEAR(Gain::AngleDegrees(20.0), -22.9, 0.05);
    EXPECT_DOUBLE_EQ(Gain::AngleDegrees(30.0), -34.0);
    EXPECT_DOUBLE_EQ(Gain::AngleDegrees(45.0), -34.0) << "held at full scale";
    EXPECT_DOUBLE_EQ(Level::AngleDegrees(-30.0), -34.0);
    EXPECT_NEAR(Level::AngleDegrees(0.0), 11.3, 0.05);
    EXPECT_DOUBLE_EQ(Level::AngleDegrees(6.0), 34.0);
    EXPECT_DOUBLE_EQ(Level::AngleDegrees(-130.0), -34.0) << "silence on the left stop";

    // The gain-reduction figures on their ticks' rays, turned with them about the pivot; then the legend.
    RecordingCanvas gain;
    Gain::Paint(gain);
    const auto figures = gain.Of("DrawText");
    ASSERT_EQ(figures.size(), 12u) << "ten figures, the legend and its unit";
    EXPECT_EQ(figures[0].text, "0");
    EXPECT_EQ(figures[9].text, "30");
    EXPECT_EQ(figures[10].text, "GAIN REDUCTION");
    EXPECT_EQ(figures[11].text, "dB");
    EXPECT_EQ(figures[0].color, look::meter_art::kFaceInk);
    const auto rest = figures[0].transform.Apply({look::meter_art::kPivot.x, look::meter_art::kPivot.y - 646.0f});
    const auto rad  = 34.0 * std::numbers::pi / 180.0;
    EXPECT_NEAR(rest.x, 500.0 + 646.0 * std::sin(rad), 0.01) << "the 0 turned 34 degrees clockwise";
    EXPECT_NEAR(rest.y, 880.0 - 646.0 * std::cos(rad), 0.01);
    EXPECT_TRUE(figures[10].transform.IsIdentity()) << "the legend upright";

    // The level meter's figures upright, the ones over 0 red; its name and PEAK under it.
    RecordingCanvas level;
    Level::Paint(level, "OUT");
    const auto marks = level.Of("DrawText");
    ASSERT_EQ(marks.size(), 9u);
    EXPECT_EQ(marks[0].text, "-30");
    EXPECT_EQ(marks[5].text, "+3");
    EXPECT_EQ(marks[5].color, look::meter_art::kRed);
    EXPECT_EQ(marks[4].text, "0");
    EXPECT_EQ(marks[4].color, look::meter_art::kFaceInk);
    EXPECT_EQ(marks[7].text, "OUT");
    EXPECT_EQ(marks[8].text, "PEAK");
    EXPECT_FLOAT_EQ(look::meter_art::kAspect, 920.0f / 720.0f);
}
} // namespace

// ---- How the indicators move (LookBallistics.h) ----------------------------------------------------------------

TEST(NeedleBallistics, IsRateIndependentOvershootsByTheDampingAndIsStableOverAClampedFrame)
{
    // The same mechanism stepped in 1/60 s and 1/144 s frames is in the same place after the same time: the
    // integration runs in 2 ms substeps whatever the frame. Rising to 1 with the Comp's constants (6 Hz up, 3 Hz
    // down, a damping ratio of 0.75) it runs 4 to 5 % past the mark, the return on the softer spring; a whole
    // tenth of a second in one frame - the clamp after a stall - lands where the small frames do.
    constexpr double kRise = 6.0, kFall = 3.0, kDamping = 0.75;
    auto             run = [&](double frame, double until) {
        look::NeedleBallistics needle;
        double                 peak = 0.0;
        for (double t = 0.0; t < until - 1.0e-9; t += frame)
        {
            peak = std::max(peak, needle.Step(1.0, frame, kRise, kFall, kDamping));
        }
        return std::pair{needle.position(), peak};
    };
    const auto [at_60, peak_60]   = run(1.0 / 60.0, 0.5);
    const auto [at_144, peak_144] = run(1.0 / 144.0, 0.5);
    const auto [at_10, peak_10]   = run(0.1, 0.5);
    std::printf("    at 0.5 s: 60 Hz %.5f (peak %.4f), 144 Hz %.5f (peak %.4f), 10 Hz %.5f (peak %.4f)\n", at_60,
                peak_60, at_144, peak_144, at_10, peak_10);
    EXPECT_NEAR(at_60, at_144, 1.0e-3);
    EXPECT_NEAR(at_60, 1.0, 0.01);
    EXPECT_GT(peak_60, 1.02);
    EXPECT_LT(peak_60, 1.08);
    EXPECT_NEAR(peak_60, peak_144, 0.01) << "the peak is seen at either rate (144 Hz samples it more finely)";
    EXPECT_NEAR(at_10, at_60, 0.01) << "a clamped frame is the same mechanism in substeps";

    // Critically damped and above, nothing overshoots.
    look::NeedleBallistics calm;
    double                 calm_peak = 0.0;
    for (double t = 0.0; t < 1.0; t += 1.0 / 60.0)
    {
        calm_peak = std::max(calm_peak, calm.Step(1.0, 1.0 / 60.0, kRise, kFall, 1.0));
    }
    EXPECT_LE(calm_peak, 1.0 + 1.0e-6);

    // Snap puts the needle somewhere at rest.
    calm.Snap(3.0);
    EXPECT_EQ(calm.position(), 3.0);
    EXPECT_EQ(calm.velocity(), 0.0);
}

TEST(LampGlow, IsAFilamentWithItsOwnRiseAndFallTimeConstantsAtAnyFrameRate)
{
    // A lamp fed a raw per-block peak lights with a 20 ms time constant and cools with an 80 ms one (the series'
    // constants: the Amp's tubes, the Drive's lamp). The lag is exact in seconds, so a tenth of a second is the
    // same brightness in five 20 ms frames, ten 10 ms frames or one clamped 100 ms frame: 1 - e^-5 lit, e^-1.25
    // cooled.
    constexpr double kRise = 0.02, kFall = 0.08;
    auto             after = [&](double frame, double until, double target, double start) {
        look::LampGlow lamp;
        lamp.Snap(start);
        for (double t = 0.0; t < until - 1.0e-9; t += frame)
        {
            lamp.Step(target, frame, kRise, kFall);
        }
        return lamp.glow();
    };
    const double lit_50   = after(0.02, 0.1, 1.0, 0.0);
    const double lit_100  = after(0.01, 0.1, 1.0, 0.0);
    const double lit_one  = after(0.1, 0.1, 1.0, 0.0);
    const double cool_50  = after(0.02, 0.1, 0.0, 1.0);
    const double cool_100 = after(0.01, 0.1, 0.0, 1.0);
    const double cool_one = after(0.1, 0.1, 0.0, 1.0);
    std::printf("    after 0.1 s: lit %.5f / %.5f / %.5f, cooled %.5f / %.5f / %.5f (50 Hz / 100 Hz / one frame)\n",
                lit_50, lit_100, lit_one, cool_50, cool_100, cool_one);
    EXPECT_NEAR(lit_one, 1.0 - std::exp(-5.0), 1.0e-12);
    EXPECT_NEAR(lit_50, lit_one, 1.0e-9);
    EXPECT_NEAR(lit_100, lit_one, 1.0e-9);
    EXPECT_NEAR(cool_one, std::exp(-1.25), 1.0e-12);
    EXPECT_NEAR(cool_50, cool_one, 1.0e-9);
    EXPECT_NEAR(cool_100, cool_one, 1.0e-9);
    // Quicker to light than to cool: after 20 ms a dark lamp is 63 % lit, a lit one has lost only 22 %.
    EXPECT_NEAR(after(0.02, 0.02, 1.0, 0.0), 1.0 - std::exp(-1.0), 1.0e-12);
    EXPECT_NEAR(after(0.02, 0.02, 0.0, 1.0), std::exp(-0.25), 1.0e-12);
}

TEST(LampLight, MovesOnlyPastItsThresholdAndBehindAFilamentFollowsIt)
{
    // A lamp that shows what it is fed takes a move past its threshold whole and reports the frame worth a repaint;
    // a smaller move changes nothing. Behind a filament, the glow is the filament's: a step toward 1 over one frame
    // lands where a LampGlow does, and the series' filament is the two look:: times.
    look::LampLight level(0.002, 0.5, std::nullopt);
    EXPECT_EQ(level.glow(), 0.5) << "what it shows before its first frame";
    EXPECT_FALSE(level.Step(0.501, 1.0 / 60.0)) << "under the threshold: no repaint";
    EXPECT_EQ(level.glow(), 0.5);
    EXPECT_TRUE(level.Step(0.7, 1.0 / 60.0));
    EXPECT_EQ(level.glow(), 0.7) << "a move past it, whole";

    const look::LampFilament series{};
    EXPECT_EQ(series.rise_seconds, look::kLampRiseSeconds);
    EXPECT_EQ(series.fall_seconds, look::kLampFallSeconds);
    look::LampLight warm(0.005, 0.0, series);
    look::LampGlow  reference;
    EXPECT_TRUE(warm.Step(1.0, 1.0 / 60.0));
    EXPECT_DOUBLE_EQ(warm.glow(), reference.Step(1.0, 1.0 / 60.0, look::kLampRiseSeconds, look::kLampFallSeconds));
    EXPECT_FALSE(warm.Step(1.0, 0.0)) << "no time, no light: the filament has not moved";
}

TEST(PeakDecay, StandsOnANewPeakAtOnceHoldsItThenDischargesWithOneTimeConstantAtAnyFrameRate)
{
    // The level meters' reading (the Comp's bargraph peak line, LevelBars: a second's hold, then a 250 ms time
    // constant; the needle LevelMeter runs it with no hold): a peak is taken whole in the frame it arrives, whatever
    // the frame, and held for the hold before the discharge begins; a peak that matches or tops the reading starts the
    // hold again, so a held tone stands still; a lower one during the hold changes nothing, and after it leaves the
    // discharge alone; once the hold has run out the reading keeps e^(-t / tau) of itself - exact in seconds, the frame
    // that crosses the hold's end discharging for its remainder alone, so a quarter of a second of hold and half a
    // second of discharge is e^-1 (8.69 dB down) in forty-five 60 Hz frames, 108 at 144 Hz or three clamped quarters
    // alike; under kFloor it is exactly zero, not a denormal; and a peak that is not a number is no peak.
    constexpr double kHold = 0.25, kTau = 0.5;
    auto             after = [&](double frame, double until, double start) {
        look::PeakDecay reading;
        reading.Step(start, frame, kHold, kTau); // the peak lands and its hold begins
        for (double t = 0.0; t < until - 1.0e-9; t += frame)
        {
            reading.Step(0.0, frame, kHold, kTau);
        }
        return reading.level();
    };
    look::PeakDecay reading;
    EXPECT_EQ(reading.Step(0.7, 1.0 / 60.0, kHold, kTau), 0.7) << "the peak, whole, in its frame";
    EXPECT_EQ(reading.hold_remaining(), kHold);
    EXPECT_EQ(reading.Step(0.7, 1.0 / 60.0, kHold, kTau), 0.7) << "held: the same peak renews the hold";
    EXPECT_EQ(reading.hold_remaining(), kHold);
    EXPECT_EQ(reading.Step(0.2, 1.0 / 144.0, kHold, kTau), 0.7) << "a lower peak inside the hold: nothing moves";
    EXPECT_NEAR(reading.hold_remaining(), kHold - 1.0 / 144.0, 1.0e-15) << "but the hold runs on";
    const double after_nan = reading.Step(std::numeric_limits<double>::quiet_NaN(), 1.0 / 144.0, kHold, kTau);
    EXPECT_EQ(after_nan, 0.7) << "not a number is not a peak";
    reading.Snap(0.7); // the hold off: the discharge from here
    const double let_go = reading.Step(0.2, 1.0 / 144.0, kHold, kTau);
    EXPECT_NEAR(let_go, 0.7 * std::exp(-1.0 / 144.0 / kTau), 1.0e-15) << "a lower peak after the hold: the discharge";
    EXPECT_EQ(after(1.0 / 144.0, 0.2, 1.0), 1.0) << "a fifth of a second on, still held, untouched";
    EXPECT_NEAR(after(1.0 / 60.0, 0.25, 1.0), 1.0, 1.0e-12) << "the hold's whole length";
    const double at_60  = after(1.0 / 60.0, 0.75, 1.0);
    const double at_144 = after(1.0 / 144.0, 0.75, 1.0);
    const double at_4   = after(0.25, 0.75, 1.0);
    std::printf("    after 0.75 s of silence from 1 (a 0.25 s hold): %.6f / %.6f / %.6f (60 Hz / 144 Hz / clamped "
                "quarters), e^-1 = %.6f, %.3f dB\n",
                at_60, at_144, at_4, std::exp(-1.0), 20.0 * std::log10(at_60));
    EXPECT_NEAR(at_60, std::exp(-1.0), 1.0e-12);
    EXPECT_NEAR(at_144, std::exp(-1.0), 1.0e-12);
    EXPECT_NEAR(at_4, std::exp(-1.0), 1.0e-12);
    EXPECT_NEAR(20.0 * std::log10(at_60), -8.6859, 1.0e-3) << "one time constant is 8.69 dB";
    EXPECT_EQ(after(0.25, 20.0, 1.0), 0.0) << "forty time constants: e^-40 is under the floor, snapped to zero";
    reading.Snap(0.25);
    EXPECT_EQ(reading.level(), 0.25);
    EXPECT_EQ(reading.hold_remaining(), 0.0);
}

// ---- The oscilloscope's trigger ---------------------------------------------------------------------------------

namespace
{
constexpr double kScopeRate      = 48000.0;
constexpr int    kScopeWindow    = 480; ///< 10 ms at 48 kHz, the scope's window
constexpr int    kScopeLookBack  = 2;   ///< windows of look-back before it, as the series' ring keeps them
constexpr int    kScopeSpanCount = (1 + kScopeLookBack) * kScopeWindow;

std::vector<double> ScopeSine(double hz, double amplitude, int n, double phase = 0.0)
{
    std::vector<double> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        x[static_cast<std::size_t>(i)] =
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kScopeRate + phase);
    }
    return x;
}

bool RisesAt(const std::vector<double> &span, int i)
{
    return span[static_cast<std::size_t>(i)] >= 0.0 && span[static_cast<std::size_t>(i - 1)] < 0.0;
}
} // namespace

TEST(LookScope, TheTriggerIsTheLastRisingZeroCrossingThatLeavesAWholeWindow)
{
    // A 440 Hz sine over a span of three windows: the trigger lands at or before the newest window's start, within the
    // look-back of it, on a sample at or above zero whose predecessor is below - so the trace starts at the same phase
    // whatever the block boundaries did. The low E, 82 Hz and 12 ms a cycle, longer than the window, still finds a
    // crossing in the 20 ms of look-back. Silence falls back to the newest window; a span shorter than a window to 0.
    const int  latest = kScopeSpanCount - kScopeWindow;
    const auto span   = ScopeSine(440.0, 0.5, kScopeSpanCount, 0.3);
    const int  at     = look::Scope::TriggerIndex(span, kScopeWindow, kScopeLookBack);
    std::printf("    trigger at %d of %d (the newest window starts at %d)\n", at, kScopeSpanCount, latest);
    EXPECT_LE(at, latest);
    EXPECT_GE(at, latest - kScopeLookBack * kScopeWindow);
    ASSERT_GT(at, 0);
    EXPECT_TRUE(RisesAt(span, at));
    for (int i = at + 1; i <= latest; ++i)
    {
        EXPECT_FALSE(RisesAt(span, i)) << "a later crossing at " << i;
    }
    const auto low_e    = ScopeSine(82.4, 0.5, kScopeSpanCount, 1.1);
    const int  low_e_at = look::Scope::TriggerIndex(low_e, kScopeWindow, kScopeLookBack);
    ASSERT_GT(low_e_at, 0);
    EXPECT_LT(low_e_at, latest) << "a real crossing, not the fallback";
    EXPECT_TRUE(RisesAt(low_e, low_e_at));

    const std::vector<double> silence(static_cast<std::size_t>(kScopeSpanCount), 0.0);
    EXPECT_EQ(look::Scope::TriggerIndex(silence, kScopeWindow, kScopeLookBack), latest) << "silence: the newest window";
    const std::vector<double> dc(static_cast<std::size_t>(kScopeSpanCount), 0.3);
    EXPECT_EQ(look::Scope::TriggerIndex(dc, kScopeWindow, kScopeLookBack), latest) << "no crossing: the newest window";
    const auto slow    = ScopeSine(10.0, 0.5, kScopeSpanCount); // a 100 ms period: no whole cycle in the look-back
    const int  slow_at = look::Scope::TriggerIndex(slow, kScopeWindow, kScopeLookBack);
    EXPECT_TRUE(slow_at == latest || RisesAt(slow, slow_at));
    EXPECT_EQ(look::Scope::TriggerIndex(ScopeSine(440.0, 0.5, kScopeWindow / 2), kScopeWindow, kScopeLookBack), 0)
        << "a short span";
}

TEST(LookScope, TheTriggerIsArmedAndLocksToTheTraceItShowedLastFrame)
{
    // A note with a strong second harmonic - 110 Hz at 0.5 with its octave at 0.3 - crosses zero upward twice a
    // cycle. Frames of 800 samples (60 Hz at 48 kHz) slide a three-window span along two seconds of it; the trigger
    // must land on the same phase of the fundamental every frame, given the trace it showed last time, so the screen
    // holds one picture rather than flickering between the two segments a last-crossing trigger would alternate
    // between. Every trigger is an armed crossing: the wave dipped below kArmFraction of the span's peak since the
    // crossing before it. And a ripple about zero - a 3 kHz wiggle at a twentieth of the wave - adds crossings the
    // arming passes over, so the trigger stays within a few samples of the clean wave's.
    const double f0     = 110.0;
    const double period = kScopeRate / f0;
    const int    total  = 2 * static_cast<int>(kScopeRate);
    auto         note   = ScopeSine(f0, 0.5, total, 0.4);
    const auto   octave = ScopeSine(2.0 * f0, 0.3, total, 0.7);
    for (int i = 0; i < total; ++i)
    {
        note[static_cast<std::size_t>(i)] += octave[static_cast<std::size_t>(i)];
    }
    double peak = 0.0;
    for (const double v : note)
    {
        peak = std::max(peak, std::fabs(v));
    }

    std::vector<double> previous;
    double              phase0 = -1.0;
    int                 frames = 0;
    for (int start = 0; start + kScopeSpanCount <= total; start += 800)
    {
        const std::vector<double> span(note.begin() + start, note.begin() + start + kScopeSpanCount);
        const int                 at = look::Scope::TriggerIndex(span, kScopeWindow, kScopeLookBack, previous);
        ASSERT_GT(at, 0);
        ASSERT_LE(at, kScopeSpanCount - kScopeWindow);
        EXPECT_TRUE(RisesAt(span, at));
        // Armed: between the crossing before and this one the wave went below kArmFraction of the peak.
        int    before = at - 1;
        double lowest = 0.0;
        while (before > 0 && !RisesAt(span, before))
        {
            lowest = std::min(lowest, span[static_cast<std::size_t>(before)]);
            --before;
        }
        EXPECT_LE(lowest, -look::Scope::kArmFraction * peak) << "frame " << frames << ": an unarmed crossing";
        const double phase = std::fmod(static_cast<double>(start + at), period);
        if (phase0 < 0.0)
        {
            phase0 = phase;
        }
        else
        {
            const double d = std::fmod(phase - phase0 + 1.5 * period, period) - 0.5 * period;
            EXPECT_NEAR(d, 0.0, 1.5) << "frame " << frames << ": the trigger moved to another phase of the cycle";
        }
        previous.assign(span.begin() + at, span.begin() + at + kScopeWindow);
        ++frames;
    }
    std::printf("    %d frames of the two-crossing note: the trigger stayed at phase %.1f of %.1f samples\n", frames,
                phase0, period);
    EXPECT_GT(frames, 100);

    const auto clean  = ScopeSine(f0, 0.5, kScopeSpanCount, 0.4);
    auto       ripple = clean;
    const auto wiggle = ScopeSine(3000.0, 0.025, kScopeSpanCount);
    for (int i = 0; i < kScopeSpanCount; ++i)
    {
        ripple[static_cast<std::size_t>(i)] += wiggle[static_cast<std::size_t>(i)];
    }
    const int clean_at  = look::Scope::TriggerIndex(clean, kScopeWindow, kScopeLookBack);
    const int ripple_at = look::Scope::TriggerIndex(ripple, kScopeWindow, kScopeLookBack);
    std::printf("    clean trigger %d, with a 3 kHz ripple about zero %d\n", clean_at, ripple_at);
    EXPECT_NEAR(ripple_at, clean_at, 3) << "the ripple's extra crossings do not arm";
}

// ---- The oscilloscope's frame: the snapshot, the range, the clear and the painting --------------------------------

namespace
{
/** What a Scope reads, run by hand in the shape of the series' ring: the samples written
    so far, their count, and a snapshot of the newest three windows, oldest first - torn when the suite says so - each
    snapshot counted. */
class ScopeSource
{
  public:
    static constexpr int kLookBackWindows = kScopeLookBack;

    void Write(const std::vector<double> &x)
    {
        samples_.insert(samples_.end(), x.begin(), x.end());
    }

    std::uint64_t count() const noexcept
    {
        return samples_.size();
    }

    int WindowSamples() const noexcept
    {
        return kScopeWindow;
    }

    bool Snapshot(std::vector<double> &out) const
    {
        ++snapshots;
        const std::size_t n = std::min(samples_.size(), static_cast<std::size_t>(kScopeSpanCount));
        out.assign(samples_.end() - static_cast<std::ptrdiff_t>(n), samples_.end());
        return !torn;
    }

    std::vector<double> Newest(int n) const
    {
        return {samples_.end() - n, samples_.end()};
    }

    bool        torn{false};
    mutable int snapshots{0};

  private:
    std::vector<double> samples_;
};

/** The peak of the window the scope shows, as it took it. */
double ShownPeak(const look::Scope &scope)
{
    const auto &span = scope.span();
    const int   end  = std::min(static_cast<int>(span.size()), scope.trigger() + kScopeWindow);
    double      peak = 0.0;
    for (int i = scope.trigger(); i < end; ++i)
    {
        peak = std::max(peak, std::fabs(span[static_cast<std::size_t>(i)]));
    }
    return peak;
}
} // namespace

TEST(LookScope, ItTakesASnapshotOnlyWhenTheSourceHasPublishedAndKeepsItsTraceOverATornOne)
{
    // A frame takes a snapshot only when the source's count has moved since the last - most frames at a display's rate
    // find nothing new - and a frame with nothing new says the trace stands. A snapshot is the newest three windows,
    // the trace starting at the trigger found in it; a torn one is dropped, the last trace kept, and the next frame
    // tries again.
    ScopeSource source;
    look::Scope scope;
    EXPECT_TRUE(scope.span().empty());
    EXPECT_EQ(scope.trigger(), 0);
    EXPECT_EQ(scope.scale(), look::Scope::kScaleFloor) << "the range starts on its floor";

    EXPECT_TRUE(scope.Advance(source, 1.0 / 60.0)) << "the first frame takes what there is";
    EXPECT_EQ(source.snapshots, 1);
    EXPECT_TRUE(scope.span().empty()) << "nothing written yet";
    EXPECT_FALSE(scope.Advance(source, 1.0 / 60.0)) << "nothing new: the trace stands";
    EXPECT_EQ(source.snapshots, 1) << "and no snapshot is taken";

    source.Write(ScopeSine(440.0, 0.5, 4 * kScopeWindow, 0.3));
    EXPECT_TRUE(scope.Advance(source, 1.0 / 60.0));
    EXPECT_EQ(source.snapshots, 2);
    EXPECT_EQ(scope.span(), source.Newest(kScopeSpanCount)) << "the newest three windows, oldest first";
    EXPECT_EQ(scope.trigger(), look::Scope::TriggerIndex(scope.span(), kScopeWindow, kScopeLookBack));
    EXPECT_DOUBLE_EQ(scope.scale(), ShownPeak(scope)) << "the range up to the shown window's peak at once";
    EXPECT_FALSE(scope.Advance(source, 1.0 / 60.0));
    EXPECT_EQ(source.snapshots, 2);

    const auto shown = scope.span();
    source.Write(ScopeSine(220.0, 0.3, 800));
    source.torn = true;
    EXPECT_FALSE(scope.Advance(source, 1.0 / 60.0)) << "a torn copy is no new trace";
    EXPECT_EQ(source.snapshots, 3);
    EXPECT_EQ(scope.span(), shown) << "the last trace kept";
    source.torn = false;
    EXPECT_TRUE(scope.Advance(source, 1.0 / 60.0)) << "the next frame tries again";
    EXPECT_EQ(source.snapshots, 4);
    EXPECT_EQ(scope.span(), source.Newest(kScopeSpanCount));
}

TEST(LookScope, TheRangeRisesToAPeakAtOnceFallsOverASecondAndStopsAtItsFloor)
{
    // The range is the shown window's peak through a filament that rises at once and falls with a time constant of
    // kScaleFallSeconds, never under kScaleFloor: a loud tone sets it at once; a quieter one after it is followed
    // down frame by frame - after a second about a third of the way from the quiet peak to the loud one - and a louder
    // peak takes it straight back up; silence brings it down to the floor and no further.
    ScopeSource source;
    look::Scope scope;
    source.Write(ScopeSine(220.0, 0.5, 4 * kScopeWindow));
    scope.Advance(source, 1.0 / 60.0);
    const double loud = scope.scale();
    EXPECT_NEAR(loud, 0.5, 0.01) << "the loud tone's peak";

    look::LampGlow mirror;
    mirror.Snap(loud);
    double expected = loud;
    for (int frame = 0; frame < 60; ++frame)
    {
        const auto quiet = ScopeSine(330.0, 0.05, 800, 0.1 * frame);
        source.Write(quiet);
        ASSERT_TRUE(scope.Advance(source, 1.0 / 60.0));
        const double target = std::max(ShownPeak(scope), look::Scope::kScaleFloor);
        if (target >= expected)
        {
            mirror.Snap(target);
            expected = target;
        }
        else
        {
            expected = mirror.Step(target, 1.0 / 60.0, look::Scope::kScaleFallSeconds, look::Scope::kScaleFallSeconds);
        }
        ASSERT_DOUBLE_EQ(scope.scale(), expected) << "frame " << frame;
    }
    std::printf("    a second after the loud tone: the range %.4f, between the quiet peak 0.05 and %.4f\n",
                scope.scale(), loud);
    EXPECT_NEAR(scope.scale(), 0.05 + (loud - 0.05) * std::exp(-1.0), 0.01) << "one time constant down";

    source.Write(ScopeSine(220.0, 0.8, 4 * kScopeWindow));
    scope.Advance(source, 1.0 / 60.0);
    EXPECT_DOUBLE_EQ(scope.scale(), ShownPeak(scope)) << "a louder peak at once";
    EXPECT_GT(scope.scale(), 0.75);

    for (int frame = 0; frame < 1200; ++frame)
    {
        source.Write(std::vector<double>(800, 0.0));
        scope.Advance(source, 1.0 / 60.0);
        ASSERT_GE(scope.scale(), look::Scope::kScaleFloor) << "frame " << frame << ": never under the floor";
    }
    EXPECT_NEAR(scope.scale(), look::Scope::kScaleFloor, 1.0e-6) << "twenty seconds of silence: on the floor";
}

TEST(LookScope, ClearDropsTheTraceAndPutsTheRangeOnItsFloorUntilTheNextAdvance)
{
    // Clear is for a screen that has gone from view: the trace dropped, the trigger and the range back where a new
    // scope starts, and true only when there was something to drop; the next frame takes a snapshot afresh, though
    // the source has published nothing since.
    ScopeSource source;
    look::Scope scope;
    EXPECT_FALSE(scope.Clear()) << "nothing to drop";
    source.Write(ScopeSine(440.0, 0.5, 4 * kScopeWindow, 0.3));
    scope.Advance(source, 1.0 / 60.0);
    ASSERT_FALSE(scope.span().empty());
    ASSERT_GT(scope.scale(), look::Scope::kScaleFloor);
    EXPECT_TRUE(scope.Clear());
    EXPECT_TRUE(scope.span().empty());
    EXPECT_EQ(scope.trigger(), 0);
    EXPECT_EQ(scope.scale(), look::Scope::kScaleFloor);
    EXPECT_FALSE(scope.Clear()) << "cleared already";
    EXPECT_TRUE(scope.Advance(source, 1.0 / 60.0)) << "the next frame takes the source afresh";
    EXPECT_EQ(scope.span(), source.Newest(kScopeSpanCount));
    EXPECT_DOUBLE_EQ(scope.scale(), ShownPeak(scope));
}

TEST(LookScope, ItPaintsItsScreenAndATraceInTheAccentFittedToThePlot)
{
    // The screen fills its bounds in the screen colour; the plot is the bounds less kInset. With no trace a flat line
    // in the accent crosses the plot's middle; with one, the trace is a stroke in the accent from the plot's left to
    // its right, normalised to the range so a tone's peaks reach the plot's top and bottom and no further.
    const gfx::RectF bounds{10.0f, 20.0f, 558.0f, 200.0f};
    const gfx::Color ink    = look::colors::kInk;
    const gfx::Color accent = look::colors::kAmber;
    const gfx::Color screen{0xff070707};
    const auto       plot = look::Scope::PlotBounds(bounds);
    EXPECT_TRUE(plot == bounds.Reduced(static_cast<float>(look::Scope::kInset)));

    look::Scope     scope;
    RecordingCanvas empty;
    scope.Paint(empty, bounds, kScopeWindow, ink, accent, screen);
    const auto fills = empty.Of("FillRoundedRect");
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_TRUE(fills[0].rect == bounds);
    EXPECT_EQ(fills[0].color, screen) << "the screen in its colour";
    EXPECT_TRUE(empty.Of("StrokePath").empty()) << "no trace";
    bool flat = false;
    for (const auto &line : empty.Of("DrawHorizontalLine"))
    {
        flat = flat || (line.color == accent && std::fabs(line.rect.y() - plot.CentreY()) <= 0.5f);
    }
    EXPECT_TRUE(flat) << "a flat line in the accent across the middle";

    ScopeSource source;
    source.Write(ScopeSine(440.0, 0.5, 4 * kScopeWindow, 0.3));
    scope.Advance(source, 1.0 / 60.0);
    RecordingCanvas traced;
    scope.Paint(traced, bounds, kScopeWindow, ink, accent, screen);
    const auto strokes = traced.Of("StrokePath");
    ASSERT_EQ(strokes.size(), 1u);
    EXPECT_EQ(strokes[0].color, accent) << "the trace in the accent";
    const auto drawn = strokes[0].path.Bounds();
    std::printf("    the trace spans x %.1f to %.1f and y %.1f to %.1f in a plot of %.1f to %.1f by %.1f to %.1f\n",
                drawn.x(), drawn.right(), drawn.y(), drawn.bottom(), plot.x(), plot.right(), plot.y(), plot.bottom());
    EXPECT_NEAR(drawn.x(), plot.x(), 0.01f);
    EXPECT_NEAR(drawn.right(), plot.right(), 0.01f);
    EXPECT_GE(drawn.y(), plot.y() - 0.01f) << "no higher than the plot";
    EXPECT_LE(drawn.bottom(), plot.bottom() + 0.01f) << "no lower";
    EXPECT_GT(drawn.height(), 0.95f * plot.height()) << "the peaks fitted to the plot";
}
