// A drawing's SVG read with no toolkit (gfx/SvgImage.h): the two chess pieces kept as fixtures (tests/svg) read whole,
// and every piece of a directory RECKLESS_SVG_PIECES names; each drawn onto a canvas that records what it is asked to
// draw, the recording read for the specification's way with it - the default fill, the inherited paint, a gradient in
// the shape's bounds slanted with them, the stops held in order, the opacities as layers, the stroke's joins, caps and
// dashes, the fill rule, every path command absolute and relative against its absolute spelling, the arc on its
// ellipse, the transforms, the viewBox fitted to an area every way preserveAspectRatio asks - and each thing outside
// the subset refused by name.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Canvas.h"
#include "SvgImage.h"

#include <gtest/gtest.h>

namespace
{
using namespace reckless::gfx;

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

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

std::string Fixture(std::string_view name)
{
    return ReadText(std::filesystem::path(RECKLESS_SVG_FIXTURES) / std::string(name));
}

/** What the drawing asked of the canvas: a fill or a stroke, or a layer's start or end. */
struct Call
{
    enum class Kind
    {
        kFill,
        kStroke,
        kBeginLayer,
        kEndLayer,
    };
    Kind                    kind{Kind::kFill};
    Path                    path;
    std::optional<Color>    color;
    std::optional<Gradient> gradient;
    Stroke                  stroke;
    Affine                  transform;
    float                   opacity{1.0f};
};

/** A canvas that draws nothing and records the outlines it is asked to fill and stroke, and the layers. */
class RecordingCanvas final : public Canvas
{
  public:
    std::vector<Call> calls;
    int               depth{0}; ///< saves and layers not yet restored

    void Save() override
    {
        stack_.push_back(state_);
        ++depth;
    }

    void Restore() override
    {
        state_ = stack_.back();
        stack_.pop_back();
        --depth;
    }

    void AddTransform(const Affine &t) override
    {
        state_.transform = t.FollowedBy(state_.transform);
    }

    bool ClipToRect(RectI) override
    {
        return true;
    }

    void ClipToPath(const Path &, const Affine &) override
    {
    }

    RectI ClipBounds() const override
    {
        return {0, 0, 1000, 1000};
    }

    float PhysicalPixelScale() const override
    {
        return 1.0f;
    }

    void SetColor(Color c) override
    {
        state_.color = c;
        state_.gradient.reset();
    }

    void SetGradient(const Gradient &g) override
    {
        state_.gradient = g;
        state_.color.reset();
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const Font &) override
    {
    }

    void SetResampling(Resampling) override
    {
    }

    const Font &CurrentFont() const override
    {
        return font_;
    }

    void FillAll() override
    {
    }

    void FillAll(Color) override
    {
    }

    void FillRect(RectI) override
    {
    }

    void FillRect(RectF) override
    {
    }

    void DrawRect(RectF, float) override
    {
    }

    void FillRoundedRect(RectF, float) override
    {
    }

    void DrawRoundedRect(RectF, float, float) override
    {
    }

    void FillEllipse(RectF) override
    {
    }

    void DrawEllipse(RectF, float) override
    {
    }

    void DrawLine(PointF, PointF, float) override
    {
    }

    void DrawHorizontalLine(int, float, float) override
    {
    }

    void DrawVerticalLine(int, float, float) override
    {
    }

    void FillPath(const Path &path, const Affine &) override
    {
        calls.push_back({Call::Kind::kFill, path, state_.color, state_.gradient, {}, state_.transform, 1.0f});
    }

    void StrokePath(const Path &path, const Stroke &stroke, const Affine &) override
    {
        calls.push_back({Call::Kind::kStroke, path, state_.color, state_.gradient, stroke, state_.transform, 1.0f});
    }

    void DrawText(std::string_view, RectF, Justify, bool) override
    {
    }

    void DrawFittedText(std::string_view, RectI, Justify, int, float) override
    {
    }

    float TextWidth(const Font &, std::string_view) const override
    {
        return 0.0f;
    }

    float Ascent(const Font &) const override
    {
        return 0.0f;
    }

    float Descent(const Font &) const override
    {
        return 0.0f;
    }

    void DrawImage(const Image &, RectF, bool) override
    {
    }

    void DrawImageTransformed(const Image &, const Affine &) override
    {
    }

    void BeginTransparencyLayer(float opacity) override
    {
        calls.push_back({Call::Kind::kBeginLayer, {}, {}, {}, {}, state_.transform, opacity});
        Save();
    }

    void EndTransparencyLayer() override
    {
        Restore();
        calls.push_back({Call::Kind::kEndLayer, {}, {}, {}, {}, state_.transform, 1.0f});
    }

  private:
    struct State
    {
        Affine                  transform;
        std::optional<Color>    color;
        std::optional<Gradient> gradient;
    };

    State              state_;
    std::vector<State> stack_;
    Font               font_;
};

/** A document of 100 units square with `body` drawn and `defs` defined. */
std::string Doc(std::string_view body, std::string_view defs = {})
{
    return std::string(R"~(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)~") + "<defs>" +
           std::string(defs) + "</defs>" + std::string(body) + "</svg>";
}

std::vector<Call> Drawn(const std::string &text, const Affine &t = {})
{
    std::string error;
    const auto  image = SvgImage::Parse(text, &error);
    EXPECT_TRUE(image.has_value()) << error << "\n" << text;
    RecordingCanvas canvas;
    if (image)
    {
        image->Draw(canvas, t);
    }
    EXPECT_EQ(canvas.depth, 0) << "every save restored";
    return canvas.calls;
}

std::string Refusal(const std::string &text)
{
    std::string error;
    EXPECT_FALSE(SvgImage::Parse(text, &error).has_value()) << text;
    return error;
}

/** The flattened points of `path`, every polygon's in order. */
std::vector<PointF> Points(const Path &path, float tolerance = 0.01f)
{
    std::vector<PointF> out;
    for (const auto &polygon : path.Flatten(tolerance))
    {
        out.insert(out.end(), polygon.points.begin(), polygon.points.end());
    }
    return out;
}

/** Two outlines flatten to the same points, to within `slack`. */
void ExpectSameOutline(const Path &a, const Path &b, float slack = 1e-4f)
{
    const auto pa = Points(a), pb = Points(b);
    ASSERT_EQ(pa.size(), pb.size());
    for (std::size_t i = 0; i < pa.size(); ++i)
    {
        EXPECT_NEAR(pa[i].x, pb[i].x, slack) << i;
        EXPECT_NEAR(pa[i].y, pb[i].y, slack) << i;
    }
}

Path FilledPath(const std::string &d)
{
    const auto calls = Drawn(Doc(R"~(<path d=")~" + d + R"~("/>)~"));
    EXPECT_EQ(calls.size(), 1u) << d;
    return calls.empty() ? Path{} : calls.front().path;
}

/** The proportion of the way along a linear gradient a point lies, as a renderer reads it. */
float Along(const Gradient &g, PointF p)
{
    const float dx = g.end.x - g.start.x, dy = g.end.y - g.start.y;
    return ((p.x - g.start.x) * dx + (p.y - g.start.y) * dy) / (dx * dx + dy * dy);
}
} // namespace

TEST(SvgImage, TheTwoPiecesReadWhole)
{
    for (const char *name : {"wK.svg", "bN.svg"})
    {
        std::string error;
        const auto  image = SvgImage::Parse(Fixture(name), &error);
        ASSERT_TRUE(image.has_value()) << name << ": " << error;
        EXPECT_EQ(image->ViewBox(), RectF(0.0f, 0.0f, 100.0f, 100.0f)) << name;
        RecordingCanvas canvas;
        image->Draw(canvas, {});
        EXPECT_EQ(canvas.depth, 0);
        std::printf("%s: %d shapes, %zu fills and strokes\n", name, image->Shapes(), canvas.calls.size());
    }
    EXPECT_EQ(SvgImage::Parse(Fixture("wK.svg"))->Shapes(), 8);
    EXPECT_EQ(SvgImage::Parse(Fixture("bN.svg"))->Shapes(), 8);
}

TEST(SvgImage, EveryPieceInTheDirectoryNamedReadsWhole)
{
    const std::string dir = Env("RECKLESS_SVG_PIECES");
    if (dir.empty())
    {
        GTEST_SKIP() << "set RECKLESS_SVG_PIECES to a directory of piece SVGs to read them all";
    }
    int count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".svg")
        {
            continue;
        }
        std::string error;
        const auto  image = SvgImage::Parse(ReadText(entry.path()), &error);
        EXPECT_TRUE(image.has_value()) << entry.path() << ": " << error;
        if (image)
        {
            EXPECT_GT(image->Shapes(), 0);
            std::printf("%s: %d shapes\n", entry.path().filename().string().c_str(), image->Shapes());
        }
        ++count;
    }
    EXPECT_GT(count, 0);
}

TEST(SvgImage, TheKingIsFilledWithItsGradientAndStrokedRoundInItsOutline)
{
    const auto calls = Drawn(Fixture("wK.svg"));
    ASSERT_GE(calls.size(), 2u);
    // The base: filled with the gradient in its own bounds, 15..85 across and 82..92 down, then stroked.
    const Call &base = calls[0];
    ASSERT_EQ(base.kind, Call::Kind::kFill);
    ASSERT_TRUE(base.gradient.has_value());
    const Gradient &g = *base.gradient;
    EXPECT_FALSE(g.radial);
    EXPECT_EQ(g.start_color, Color::FromRgb(0xff, 0xfd, 0xf6));
    EXPECT_EQ(g.end_color, Color::FromRgb(0xe3, 0xd6, 0xb8));
    EXPECT_NEAR(g.start.x, 15.0f + 70.0f * 0.2f, 0.01f);
    EXPECT_NEAR(g.start.y, 82.0f, 0.01f);
    // Linear in the box's units: a point's proportion is its offset along (0.6, 1) in those units.
    for (const PointF p : {PointF{30.0f, 85.0f}, PointF{70.0f, 90.0f}, PointF{50.0f, 82.5f}})
    {
        const float u = (p.x - 15.0f) / 70.0f, v = (p.y - 82.0f) / 10.0f;
        const float expected = ((u - 0.2f) * 0.6f + v * 1.0f) / (0.6f * 0.6f + 1.0f);
        EXPECT_NEAR(Along(g, p), expected, 1e-3f) << p.x << ", " << p.y;
    }
    const Call &outline = calls[1];
    ASSERT_EQ(outline.kind, Call::Kind::kStroke);
    EXPECT_EQ(outline.color, Color::FromRgb(0x1d, 0x1a, 0x16));
    EXPECT_FLOAT_EQ(outline.stroke.thickness, 2.4f);
    EXPECT_EQ(outline.stroke.join, Stroke::Join::kCurved);
    EXPECT_EQ(outline.stroke.cap, Stroke::Cap::kRounded);
    // The highlight line across the base: no fill, a stroke at 55 % of its colour.
    bool found = false;
    for (const Call &c : calls)
    {
        if (c.kind == Call::Kind::kStroke && c.color && c.color->alpha() < 255)
        {
            found = true;
            EXPECT_EQ(c.color->alpha(), static_cast<int>(std::lround(0.55 * 255.0)));
            EXPECT_FLOAT_EQ(c.stroke.thickness, 1.2f);
        }
    }
    EXPECT_TRUE(found);
}

TEST(SvgImage, TheKnightsEyeIsACircleFilledSolid)
{
    const auto calls = Drawn(Fixture("bN.svg"));
    int        eyes  = 0;
    for (const Call &c : calls)
    {
        if (c.kind == Call::Kind::kFill && c.color == Color::FromRgb(0xc9, 0xbb, 0xa0))
        {
            ++eyes;
            ASSERT_EQ(c.path.elements().size(), 1u);
            EXPECT_EQ(c.path.elements()[0].op, Path::Op::kEllipse);
        }
    }
    EXPECT_EQ(eyes, 2);
}

TEST(SvgImage, ShapesTakeTheSpecificationsDefaultsAndInheritFromTheirGroups)
{
    auto calls = Drawn(Doc(R"~(<rect x="10" y="10" width="20" height="20"/>)~"));
    ASSERT_EQ(calls.size(), 1u) << "filled black, not stroked";
    EXPECT_EQ(calls[0].color, colors::kBlack);
    calls = Drawn(Doc(R"~(<g fill="#f00" stroke="#00f" stroke-width="3" fill-opacity="0.5"><circle cx="50" cy="50"
        r="10"/><path d="M0 0 L10 10" stroke-linecap="square" stroke-dasharray="4 2"/></g>)~"));
    ASSERT_EQ(calls.size(), 4u);
    EXPECT_EQ(calls[0].color, Color::FromArgb(128, 255, 0, 0));
    EXPECT_EQ(calls[1].color, Color::FromRgb(0, 0, 255));
    EXPECT_FLOAT_EQ(calls[1].stroke.thickness, 3.0f);
    EXPECT_EQ(calls[3].stroke.cap, Stroke::Cap::kSquare);
    EXPECT_EQ(calls[3].stroke.dashes, (std::vector<float>{4.0f, 2.0f}));
    calls = Drawn(Doc(R"~(<path d="M0 0 L10 10" fill="none" stroke="white" stroke-dasharray="3"/>)~"));
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].stroke.dashes, (std::vector<float>{3.0f, 3.0f})) << "an odd list repeated";
    EXPECT_EQ(calls[0].color, colors::kWhite);
    EXPECT_TRUE(Drawn(Doc(R"~(<path d="M0 0 L10 10" fill="none" stroke="#000" stroke-width="0"/>)~")).empty())
        << "a stroke of no width draws nothing";
    calls = Drawn(Doc(R"~(<line x1="0" y1="0" x2="10" y2="0" stroke="#000" fill="#fff"/>)~"));
    ASSERT_EQ(calls.size(), 1u) << "a line covers no area to fill";
    EXPECT_EQ(calls[0].kind, Call::Kind::kStroke);
}

TEST(SvgImage, AnOpacityDrawsTheElementAsOneLayer)
{
    auto calls = Drawn(Doc(R"~(<rect width="10" height="10" opacity="0.25" stroke="#000"/>)~"));
    ASSERT_EQ(calls.size(), 4u);
    EXPECT_EQ(calls[0].kind, Call::Kind::kBeginLayer);
    EXPECT_FLOAT_EQ(calls[0].opacity, 0.25f);
    EXPECT_EQ(calls[1].color, colors::kBlack) << "the fill and the stroke at their own alpha, inside the layer";
    EXPECT_EQ(calls[3].kind, Call::Kind::kEndLayer);
    calls = Drawn(Doc(R"~(<g opacity="0.5"><rect width="10" height="10"/><rect x="20" width="10" height="10"/></g>)~"));
    ASSERT_EQ(calls.size(), 4u);
    EXPECT_EQ(calls[0].kind, Call::Kind::kBeginLayer);
    EXPECT_EQ(calls[3].kind, Call::Kind::kEndLayer);
    calls = Drawn(Doc(R"~(<g opacity="1"><rect width="10" height="10"/></g>)~"));
    EXPECT_EQ(calls.size(), 1u) << "no layer at full opacity";
}

TEST(SvgImage, AGradientsStopsAndUnitsFollowTheSpecification)
{
    const std::string defs =
        R"~(<linearGradient id="a" x1="0%" y1="0%" x2="100%" y2="0%"><stop offset="20%" stop-color="#f00"/>)~"
        R"~(<stop offset="0.1" stop-color="#0f0" stop-opacity="0.5"/><stop offset="0.8" stop-color="#00f"/>)~"
        R"~(</linearGradient><linearGradient id="u" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="0" y2="50">)~"
        R"~(<stop offset="0" stop-color="#000"/><stop offset="1" stop-color="#fff"/></linearGradient>)~"
        R"~(<linearGradient id="one"><stop offset="0.5" stop-color="#123456"/></linearGradient>)~"
        R"~(<linearGradient id="none"/>)~";
    auto calls = Drawn(Doc(R"~(<rect x="10" y="20" width="40" height="10" fill="url(#a)"/>)~", defs));
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_TRUE(calls[0].gradient);
    const Gradient &g = *calls[0].gradient;
    EXPECT_NEAR(g.start.x, 10.0f, 1e-3f);
    EXPECT_NEAR(g.end.x, 50.0f, 1e-3f) << "across the box: the box's own units, horizontal";
    EXPECT_NEAR(g.end.y, 20.0f, 1e-3f);
    ASSERT_EQ(g.stops.size(), 3u);
    EXPECT_FLOAT_EQ(g.stops[0].first, 0.2f);
    EXPECT_FLOAT_EQ(g.stops[1].first, 0.2f) << "an offset before the last held at it";
    EXPECT_EQ(g.stops[1].second, Color::FromArgb(128, 0, 255, 0));
    EXPECT_FLOAT_EQ(g.stops[2].first, 0.8f);
    EXPECT_EQ(g.start_color, Color::FromRgb(255, 0, 0)) << "the first stop's colour before it";
    EXPECT_EQ(g.end_color, Color::FromRgb(0, 0, 255)) << "the last stop's after it";

    calls = Drawn(Doc(R"~(<rect x="10" y="20" width="40" height="10" fill="url(#u)"/>)~", defs));
    ASSERT_TRUE(calls[0].gradient);
    EXPECT_EQ(calls[0].gradient->start, PointF(0.0f, 0.0f)) << "in the drawing's units";
    EXPECT_EQ(calls[0].gradient->end, PointF(0.0f, 50.0f));

    calls = Drawn(Doc(R"~(<rect width="10" height="10" fill="url(#one)"/>)~", defs));
    EXPECT_EQ(calls[0].color, Color::FromRgb(0x12, 0x34, 0x56)) << "one stop: its colour, solid";
    EXPECT_TRUE(Drawn(Doc(R"~(<rect width="10" height="10" fill="url(#none)"/>)~", defs)).empty())
        << "no stops: painted as none";
    EXPECT_TRUE(Drawn(Doc(R"~(<path d="M0 5 L50 5" fill="none" stroke="url(#a)"/>)~", defs)).empty())
        << "a box with no height: the gradient paints nothing";
    calls = Drawn(Doc(R"~(<path d="M0 5 L50 5" fill="none" stroke="url(#u)"/>)~", defs));
    EXPECT_EQ(calls.size(), 1u) << "the drawing's units need no box";
    calls = Drawn(Doc(R"~(<rect width="10" height="10" fill="url(#late)"/><defs><linearGradient id="late">)~"
                      R"~(<stop stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient></defs>)~"));
    EXPECT_TRUE(calls[0].gradient) << "a gradient defined after the shape that names it";
}

TEST(SvgImage, TheEvenOddRuleCutsANestedRingOut)
{
    const auto calls = Drawn(Doc(R"~(<path fill-rule="evenodd" d="M0 0 H100 V100 H0 Z M25 25 H75 V75 H25 Z"/>)~"));
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].path.fill_rule(), Path::FillRule::kEvenOdd);
    EXPECT_TRUE(calls[0].path.Contains({10.0f, 10.0f}));
    EXPECT_FALSE(calls[0].path.Contains({50.0f, 50.0f})) << "the hole";
    const auto nonzero = Drawn(Doc(R"~(<path d="M0 0 H100 V100 H0 Z M25 25 H75 V75 H25 Z"/>)~"));
    EXPECT_EQ(nonzero[0].path.fill_rule(), Path::FillRule::kNonZero);
    EXPECT_TRUE(nonzero[0].path.Contains({50.0f, 50.0f})) << "both wound the same way";
}

TEST(SvgImage, EveryPathCommandRelativeOrAbsoluteDrawsItsAbsoluteSpelling)
{
    struct Case
    {
        const char *relative;
        const char *absolute;
    };

    const Case cases[] = {
        {"M10 10 h20 v20 h-20 z", "M10,10 L30,10 L30,30 L10,30 Z"},
        {"M10 10 H30 V30 H10 Z", "M10,10 L30,10 L30,30 L10,30 Z"},
        {"M0 0 10 0 10 10", "M0 0 L10 0 L10 10"},
        {"m10 10 5 0 0 5", "M10 10 L15 10 L15 15"},
        {"M5 5 l10 0 l0 10", "M5 5 L15 5 L15 15"},
        {"M0 0 C10 0 20 10 20 20 S30 40 40 40", "M0 0 C10 0 20 10 20 20 C20 30 30 40 40 40"},
        {"M0 0 c10 0 20 10 20 20 s10 20 20 20", "M0 0 C10 0 20 10 20 20 C20 30 30 40 40 40"},
        {"M0 0 S10 10 20 0", "M0 0 C0 0 10 10 20 0"},
        {"M0 0 Q10 0 20 10 T40 20", "M0 0 Q10 0 20 10 Q30 20 40 20"},
        {"M0 0 q10 0 20 10 t20 10", "M0 0 Q10 0 20 10 Q30 20 40 20"},
        {"M0 0 T20 10", "M0 0 Q0 0 20 10"},
        {"M1.5.5l-1-2", "M1.5 0.5 L0.5 -1.5"},
        {"M1e1,1E1 L2e+1 -1e-1", "M10 10 L20 -0.1"},
        {"M10 10 L20 20 Z L30 10", "M10 10 L20 20 Z M10 10 L30 10"},
        {"M10 10 L20 20 z m5 0 l5 0", "M10 10 L20 20 Z M15 10 L20 10"},
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(c.relative);
        ExpectSameOutline(FilledPath(c.relative), FilledPath(c.absolute));
    }
}

TEST(SvgImage, AnArcLiesOnItsEllipseAndSweepsTheWayItsFlagsSay)
{
    // A half circle from the left to the right of (50, 50): the sweep flag 1 runs clockwise on the screen, over the
    // top; 0 under the bottom.
    for (const bool sweep : {true, false})
    {
        const Path path = FilledPath(std::string("M0 50 A50 50 0 0 ") + (sweep ? "1" : "0") + " 100 50");
        float      top = 1e9f, bottom = -1e9f;
        for (const PointF p : Points(path))
        {
            EXPECT_NEAR(std::hypot(p.x - 50.0f, p.y - 50.0f), 50.0f, 0.02f);
            top    = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        EXPECT_NEAR(sweep ? top : bottom, sweep ? 0.0f : 100.0f, 0.05f);
        EXPECT_NEAR(sweep ? bottom : top, 50.0f, 0.05f);
    }
    // The relative form, the flags run together, and radii too small for the endpoints scaled up to meet them.
    ExpectSameOutline(FilledPath("M0 50 a50 50 0 0 1 100 0"), FilledPath("M0 50 A50 50 0 0 1 100 50"));
    ExpectSameOutline(FilledPath("M0 50a50 50 0 01100 0"), FilledPath("M0 50 A50 50 0 0 1 100 50"));
    ExpectSameOutline(FilledPath("M0 50 A10 10 0 0 1 100 50"), FilledPath("M0 50 A50 50 0 0 1 100 50"), 1e-3f);
    // The large arc of a circle of radius 50 through two points 50 apart: three quarters and more of its turn.
    const Path large = FilledPath("M50 0 A50 50 0 1 1 0 50");
    int        below = 0;
    for (const PointF p : Points(large))
    {
        below += p.y > 60.0f ? 1 : 0;
    }
    EXPECT_GT(below, 0) << "the large arc goes round under the centre";
    // An ellipse turned 45 degrees, from one end of its major axis to the other about (50, 50): every point on
    // x'^2 / 40^2 + y'^2 / 20^2 = 1 in the ellipse's own axes.
    const float h       = 40.0f * std::sqrt(0.5f);
    const Path  rotated = FilledPath("M" + std::to_string(50.0f - h) + " " + std::to_string(50.0f - h) +
                                     " A40 20 45 0 1 " + std::to_string(50.0f + h) + " " + std::to_string(50.0f + h));
    for (const PointF p : Points(rotated))
    {
        const float dx = p.x - 50.0f, dy = p.y - 50.0f;
        const float u = (dx + dy) * std::sqrt(0.5f), v = (dy - dx) * std::sqrt(0.5f);
        EXPECT_NEAR(u * u / 1600.0f + v * v / 400.0f, 1.0f, 2e-3f);
    }
    // A zero radius draws a line; an arc to where it starts, nothing.
    ExpectSameOutline(FilledPath("M10 10 A0 5 0 0 1 30 10"), FilledPath("M10 10 L30 10"));
    ExpectSameOutline(FilledPath("M10 10 A5 5 0 0 1 10 10 L20 20"), FilledPath("M10 10 L20 20"));
}

TEST(SvgImage, TransformsComposeInTheOrderTheyAreWritten)
{
    const auto at = [](const char *transform, PointF p) {
        const auto calls =
            Drawn(Doc(std::string(R"~(<g transform=")~") + transform + R"~("><rect width="1" height="1"/></g>)~"));
        EXPECT_EQ(calls.size(), 1u);
        return calls.empty() ? PointF{} : calls[0].transform.Apply(p);
    };
    const auto near = [](PointF a, PointF b) { return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f; };
    EXPECT_TRUE(near(at("translate(10 20) scale(2)", {1.0f, 1.0f}), {12.0f, 22.0f}));
    EXPECT_TRUE(near(at("scale(2) translate(10 20)", {1.0f, 1.0f}), {22.0f, 42.0f}));
    EXPECT_TRUE(near(at("rotate(90 50 50)", {100.0f, 50.0f}), {50.0f, 100.0f}));
    EXPECT_TRUE(near(at("rotate(90)", {10.0f, 0.0f}), {0.0f, 10.0f}));
    EXPECT_TRUE(near(at("matrix(1 0 0 1 5 6)", {1.0f, 1.0f}), {6.0f, 7.0f}));
    EXPECT_TRUE(near(at("matrix(0,1,-1,0,0,0)", {1.0f, 0.0f}), {0.0f, 1.0f}));
    EXPECT_TRUE(near(at("skewX(45)", {0.0f, 10.0f}), {10.0f, 10.0f}));
    EXPECT_TRUE(near(at("skewY(45)", {10.0f, 0.0f}), {10.0f, 10.0f}));
    EXPECT_TRUE(near(at("translate(5)", {0.0f, 0.0f}), {5.0f, 0.0f}));
    EXPECT_TRUE(near(at("scale(3, 2)", {1.0f, 1.0f}), {3.0f, 2.0f}));
    const auto nested =
        Drawn(Doc(R"~(<g transform="translate(10 0)"><path transform="scale(2)" d="M0 0 L1 1"/></g>)~"));
    ASSERT_EQ(nested.size(), 1u);
    EXPECT_TRUE(near(nested[0].transform.Apply({1.0f, 1.0f}), {12.0f, 2.0f})) << "the shape's own, then its group's";
    // Through the transform the document is drawn by, too.
    const auto scaled = Drawn(Doc(R"~(<rect width="1" height="1"/>)~"), Affine::Scale(4.0f));
    EXPECT_TRUE(near(scaled[0].transform.Apply({1.0f, 1.0f}), {4.0f, 4.0f}));
}

TEST(SvgImage, ShapesHaveTheirOutlines)
{
    auto calls = Drawn(Doc(R"~(<ellipse cx="50" cy="40" rx="30" ry="10"/>)~"));
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].path.Contains({75.0f, 40.0f}));
    EXPECT_FALSE(calls[0].path.Contains({50.0f, 55.0f}));
    calls = Drawn(Doc(R"~(<rect x="10" y="10" width="40" height="20" rx="10"/>)~"));
    EXPECT_FALSE(calls[0].path.Contains({10.5f, 10.5f})) << "the corner rounded off";
    EXPECT_TRUE(calls[0].path.Contains({30.0f, 20.0f}));
    calls = Drawn(Doc(R"~(<rect x="10" y="10" width="40" height="20" rx="30" ry="2"/>)~"));
    EXPECT_TRUE(calls[0].path.Contains({30.0f, 10.5f})) << "rx held to half the width, ry its own";
    calls = Drawn(
        Doc(R"~(<polygon points="0,0 10,0 10,10"/><polyline points="0 20 10 20 10 30" fill="none" stroke="#000"/>)~"));
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_TRUE(calls[0].path.Contains({8.0f, 2.0f}));
    EXPECT_EQ(calls[1].kind, Call::Kind::kStroke);
    EXPECT_TRUE(Drawn(Doc(R"~(<circle cx="5" cy="5" r="0"/><rect width="0" height="5"/>)~")).empty())
        << "a shape of no size draws nothing";
    calls = Drawn(Doc(R"~(<rect width="10px" height="10px"/>)~"));
    EXPECT_EQ(calls.size(), 1u) << "a length in px";
}

TEST(SvgImage, TheViewBoxFitsAnAreaAsPreserveAspectRatioAsks)
{
    const auto fit = [](const char *svg_attributes, RectF area, PointF p) {
        const auto image = SvgImage::Parse(std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" ") + svg_attributes +
                                           "><rect width=\"1\" height=\"1\"/></svg>");
        EXPECT_TRUE(image.has_value()) << svg_attributes;
        return image ? image->FitTransform(area).Apply(p) : PointF{};
    };
    const RectF wide{10.0f, 20.0f, 200.0f, 100.0f};
    EXPECT_EQ(fit(R"~(viewBox="0 0 100 100")~", wide, {0.0f, 0.0f}), PointF(60.0f, 20.0f))
        << "centred, as large as fits";
    EXPECT_EQ(fit(R"~(viewBox="0 0 100 100")~", wide, {100.0f, 100.0f}), PointF(160.0f, 120.0f));
    EXPECT_EQ(fit(R"~(viewBox="50 50 100 100")~", wide, {50.0f, 50.0f}), PointF(60.0f, 20.0f)) << "the box's origin";
    EXPECT_EQ(fit(R"~(viewBox="0 0 100 100" preserveAspectRatio="xMinYMax")~", wide, {0.0f, 0.0f}),
              PointF(10.0f, 20.0f));
    EXPECT_EQ(fit(R"~(viewBox="0 0 100 100" preserveAspectRatio="xMaxYMid meet")~", wide, {0.0f, 0.0f}),
              PointF(110.0f, 20.0f));
    EXPECT_EQ(fit(R"~(viewBox="0 0 100 100" preserveAspectRatio="xMidYMin slice")~", wide, {100.0f, 0.0f}),
              PointF(210.0f, 20.0f))
        << "filling the area";
    EXPECT_EQ(fit(R"~(viewBox="0 0 100 100" preserveAspectRatio="none")~", wide, {100.0f, 100.0f}),
              PointF(210.0f, 120.0f))
        << "stretched";
    EXPECT_EQ(fit(R"~(width="50" height="25")~", wide, {50.0f, 25.0f}), PointF(210.0f, 120.0f))
        << "no viewBox: the width and height from the origin";
    const auto      image = SvgImage::Parse(Fixture("wK.svg"));
    RecordingCanvas canvas;
    image->DrawIn(canvas, {0.0f, 0.0f, 50.0f, 50.0f});
    ASSERT_FALSE(canvas.calls.empty());
    EXPECT_EQ(canvas.calls[0].transform.Apply({100.0f, 100.0f}), PointF(50.0f, 50.0f));
}

TEST(SvgImage, WhatTheReaderDoesNotDrawIsRefusedByName)
{
    struct Case
    {
        std::string text;
        const char *named;
    };

    const Case cases[] = {
        {"<html/>", "a root that is not <svg>"},
        {R"~(<svg xmlns="http://www.w3.org/2000/svg"><rect width="1" height="1"/></svg>)~", "neither a viewBox"},
        {R"~(<svg viewBox="0 0 100"/>)~", "viewBox"},
        {R"~(<svg viewBox="0 0 0 10"/>)~", "viewBox"},
        {R"~(<svg viewBox="0 0 10 10" preserveAspectRatio="xMidYMid squeeze"/>)~", "preserveAspectRatio"},
        {R"~(<svg viewBox="0 0 10 10" preserveAspectRatio="xLeftYMid"/>)~", "preserveAspectRatio"},
        {R"~(<svg viewBox="0 0 10 10" xmlns:xlink="http://www.w3.org/1999/xlink"/>)~", "xmlns:xlink"},
        {Doc("<text>Q</text>"), "text in <text>"},
        {Doc("<text/>"), "<text>, which the reader does not draw"},
        {Doc("<use/>"), "<use>"},
        {Doc("<image/>"), "<image>"},
        {Doc("", "<radialGradient id=\"r\"/>"), "<radialGradient>"},
        {Doc("", "<clipPath id=\"c\"/>"), "<clipPath>"},
        {Doc("", "<linearGradient/>"), "<linearGradient> with no id"},
        {Doc("", "<linearGradient id=\"g\" gradientTransform=\"rotate(9)\"/>"), "gradientTransform"},
        {Doc("", "<linearGradient id=\"g\" spreadMethod=\"reflect\"/>"), "spreadMethod"},
        {Doc("", "<linearGradient id=\"g\" gradientUnits=\"strokeBox\"/>"), "gradientUnits"},
        {Doc("", "<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x2=\"50%\"/>"), "x2"},
        {Doc("", "<linearGradient id=\"g\"><stop stop-color=\"red\"/></linearGradient>"), "stop-color"},
        {Doc("", "<linearGradient id=\"g\"><circle/></linearGradient>"), "<circle>"},
        {Doc(R"~(<path d="M0 0" style="fill:red"/>)~"), "<path> with style"},
        {Doc(R"~(<path d="M0 0" class="a"/>)~"), "<path> with class"},
        {Doc(R"~(<rect fill="red" width="1" height="1"/>)~"), "a paint that is not"},
        {Doc(R"~(<rect fill="url(#missing)" width="1" height="1"/>)~"), "url(#missing)"},
        {Doc(R"~(<rect fill-rule="inherit" width="1" height="1"/>)~"), "fill-rule"},
        {Doc(R"~(<rect stroke-linejoin="arcs" width="1" height="1"/>)~"), "stroke-linejoin"},
        {Doc(R"~(<rect stroke-linecap="pointy" width="1" height="1"/>)~"), "stroke-linecap"},
        {Doc(R"~(<rect stroke-miterlimit="10" width="1" height="1"/>)~"), "stroke-miterlimit"},
        {Doc(R"~(<rect stroke-dasharray="4 -2" width="1" height="1"/>)~"), "stroke-dasharray"},
        {Doc(R"~(<rect stroke-width="-1" width="1" height="1"/>)~"), "stroke-width"},
        {Doc(R"~(<rect stroke-width="1em" width="1" height="1"/>)~"), "stroke-width"},
        {Doc(R"~(<rect width="1in" height="1"/>)~"), "width"},
        {Doc(R"~(<rect width="-1" height="1"/>)~"), "negative"},
        {Doc(R"~(<circle r="-1"/>)~"), "negative"},
        {Doc(R"~(<rect width="1" height="1" opacity="half"/>)~"), "opacity"},
        {Doc(R"~(<g transform="perspective(2)"/>)~"), "transform"},
        {Doc(R"~(<g transform="rotate(1 2)"/>)~"), "transform"},
        {Doc(R"~(<g transform="translate(1"/>)~"), "transform"},
        {Doc(R"~(<path d="M0 0 B1 1"/>)~"), "'B'"},
        {Doc(R"~(<path d="L1 1"/>)~"), "no M at the start"},
        {Doc(R"~(<path d="M0 0 Z 1 1"/>)~"), "numbers with no command"},
        {Doc(R"~(<path d="M0 0 L1"/>)~"), "an L that is not a point"},
        {Doc(R"~(<path d="M0 0 A1 1 0 2 0 5 5"/>)~"), "a malformed A"},
        {Doc(R"~(<path d=""/>)~"), "draws nothing"},
        {Doc(R"~(<path/>)~"), "no d"},
        {Doc(R"~(<polygon points="0 0 1"/>)~"), "points"},
        {Doc(R"~(<path d="M0 0"><title/></path>)~"), "with children"},
    };
    for (const Case &c : cases)
    {
        const std::string error = Refusal(c.text);
        EXPECT_NE(error.find(c.named), std::string::npos) << c.text << "\n  refused for: " << error;
    }
}
