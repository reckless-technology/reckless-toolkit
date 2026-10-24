// The meters' artwork read with no toolkit (gfx/SvgArt.h): the four documents the meters draw read whole, and a
// re-export that adds what the reader does not draw refused; each thing outside the subset refused by name; and a
// document drawn onto a canvas that records what it is asked to draw, the recording read for JUCE's parser's way with
// it - the default fill, the inherited paint and clip, the opacities, the gradient's moved first stop and repeated
// last, a number in every form, the arc's centre, the rotation and the stroke width under it - and the meters' face and
// glass as JUCE's parser draws them.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "SvgArt.h"

#include <gtest/gtest.h>

namespace
{
using namespace reckless::gfx;

std::string ReadMeterFile(std::string_view name)
{
    std::ifstream      in(std::string(RECKLESS_METER_ASSETS) + "/" + std::string(name), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/** A shape's outline filled or stroked, with the brush, the stroke, the clips in force and the transform. */
struct Shape
{
    bool                    stroked{false};
    Path                    path;
    std::optional<Color>    color;
    std::optional<Gradient> gradient;
    Stroke                  stroke;
    std::vector<Path>       clips;
    Affine                  transform;
};

/** A canvas that draws nothing and records the outlines it is asked to fill and stroke. */
class RecordingCanvas final : public Canvas
{
  public:
    std::vector<Shape> shapes;
    int                depth{0}; ///< saves not yet restored

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

    void ClipToPath(const Path &path, const Affine &) override
    {
        state_.clips.push_back(path);
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
        shapes.push_back({false, path, state_.color, state_.gradient, {}, state_.clips, state_.transform});
    }

    void StrokePath(const Path &path, const Stroke &stroke, const Affine &) override
    {
        shapes.push_back({true, path, state_.color, state_.gradient, stroke, state_.clips, state_.transform});
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

    void BeginTransparencyLayer(float) override
    {
    }

    void EndTransparencyLayer() override
    {
    }

  private:
    struct State
    {
        Affine                  transform;
        std::optional<Color>    color;
        std::optional<Gradient> gradient;
        std::vector<Path>       clips;
    };

    State              state_;
    std::vector<State> stack_;
    Font               font_;
};

/** A document of 100 units square with `body` drawn and `defs` defined. */
std::string Doc(std::string_view body, std::string_view defs = {})
{
    return std::string(R"~(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">)~") +
           "<defs>" + std::string(defs) + "</defs>" + std::string(body) + "</svg>";
}

std::vector<Shape> DrawnShapes(const std::string &text, const Affine &t = {})
{
    std::string error;
    const auto  art = SvgArt::Parse(text, &error);
    EXPECT_TRUE(art.has_value()) << error;
    RecordingCanvas canvas;
    if (art)
    {
        art->Draw(canvas, t);
    }
    EXPECT_EQ(canvas.depth, 0) << "every save restored";
    return canvas.shapes;
}

/** The first element of `op` in `path`, or none. */
std::optional<Path::Element> FirstOf(const Path &path, Path::Op op)
{
    for (const auto &e : path.elements())
    {
        if (e.op == op)
        {
            return e;
        }
    }
    return std::nullopt;
}

constexpr Color Rgb(int r, int g, int b, int a = 255)
{
    return Color::FromArgb(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                           static_cast<std::uint8_t>(b));
}
} // namespace

TEST(SvgArt, TheMetersFourDocumentsReadWhole)
{
    struct File
    {
        const char *name;
        int         shapes;
        int         clipped;
    };

    // The face: the housing's four rectangles, then under the window's clip the backlight's three, the scale's arc or
    // arcs and its ticks; the glass: the sheen and the highlight under the clip, the window's rim and four screws.
    for (const File file : {File{"vu_meter_face.svg", 28, 24}, File{"vu_meter_glass.svg", 7, 2},
                            File{"level_meter_face.svg", 35, 31}, File{"level_meter_glass.svg", 7, 2}})
    {
        const std::string text = ReadMeterFile(file.name);
        ASSERT_FALSE(text.empty()) << file.name;
        std::string error;
        const auto  art = SvgArt::Parse(text, &error);
        ASSERT_TRUE(art.has_value()) << file.name << ": " << error;
        std::printf("  %-22s %2d shapes, %2d of them clipped\n", file.name, art->Shapes(), art->ClippedShapes());
        EXPECT_EQ(art->Shapes(), file.shapes) << file.name;
        EXPECT_EQ(art->ClippedShapes(), file.clipped) << file.name;
    }
}

TEST(SvgArt, AReExportThatAddsWhatTheReaderDoesNotDrawIsRefused)
{
    const std::string face = ReadMeterFile("vu_meter_face.svg");
    const auto        end  = face.rfind("</svg>");
    ASSERT_NE(end, std::string::npos);
    for (const char *added :
         {R"~(<ellipse cx="500" cy="500" rx="10" ry="20"/>)~", R"~(<text x="500" y="500">VU</text>)~",
          R"~(<rect x="0" y="0" width="10" height="10" style="fill:#fff"/>)~",
          R"~(<rect x="0" y="0" width="10" height="10" fill-opacity="0.5"/>)~",
          R"~(<path d="M 0 0 Q 5 5 10 0" stroke="#000"/>)~"})
    {
        std::string edited = face;
        edited.insert(end, added);
        std::string error;
        EXPECT_FALSE(SvgArt::Parse(edited, &error).has_value()) << added;
        std::printf("  %-64s refused: %s\n", added, error.c_str());
    }
}

TEST(SvgArt, WhatTheReaderDoesNotDrawIsRefusedByName)
{
    struct Case
    {
        std::string text;
        const char *named; ///< in the reason
    };

    const char             *rect  = R"~(<rect width="1" height="1")~";
    const std::vector<Case> cases = {
        {Doc(R"~(<ellipse cx="5" cy="5" rx="2" ry="3"/>)~"), "<ellipse>"},
        {Doc(R"~(<polygon points="0,0 1,1 2,0"/>)~"), "<polygon>"},
        {Doc(R"~(<use href="#a"/>)~"), "<use>"},
        {Doc(R"~(<image href="a.png" width="1" height="1"/>)~"), "<image>"},
        {Doc(R"~(<text x="1" y="1">0</text>)~"), "text"},
        {Doc(std::string(rect) + R"~( style="fill:#fff"/>)~"), "style"},
        {Doc(std::string(rect) + R"~( class="a"/>)~"), "class"},
        {Doc(std::string(rect) + R"~( fill-opacity="0.5"/>)~"), "fill-opacity"},
        {Doc(std::string(rect) + R"~( stroke="#000" stroke-dasharray="2 2"/>)~"), "stroke-dasharray"},
        {Doc(std::string(rect) + R"~( stroke="#000" stroke-linejoin="round"/>)~"), "stroke-linejoin"},
        {Doc(std::string(rect) + R"~( stroke="#000" stroke-linecap="bevel"/>)~"), "stroke-linecap"},
        {Doc(std::string(rect) + R"~( transform="rotate(5)"/>)~"), "transform"},
        {Doc(R"~(<g transform="translate(1 2)"><rect width="1" height="1"/></g>)~"), "rotate"},
        {Doc(R"~(<g transform="rotate(5) rotate(6)"><rect width="1" height="1"/></g>)~"), "rotate"},
        {Doc(R"~(<g opacity="0.5"><rect width="1" height="1"/></g>)~"), "opacity"},
        {Doc(R"~(<path d="m 1 1 l 2 2"/>)~"), "path data"},
        {Doc(R"~(<path d="M 1 1 C 2 2 3 3 4 4"/>)~"), "path data"},
        {Doc(R"~(<path d="M 1 1 H 5"/>)~"), "path data"},
        {Doc(R"~(<path d="M 1 1 2 2"/>)~"), "path data"},
        {Doc(R"~(<path d="L 1 1"/>)~"), "M"},
        {Doc(R"~(<rect width="50%" height="1"/>)~"), "plain number"},
        {Doc(R"~(<rect width="1px" height="1"/>)~"), "plain number"},
        {Doc(std::string(rect) + R"~( fill="red"/>)~"), "paint"},
        {Doc(std::string(rect) + R"~( fill="rgb(1,2,3)"/>)~"), "paint"},
        {Doc(std::string(rect) + R"~( fill="#ff00ff80"/>)~"), "paint"},
        {Doc(std::string(rect) + R"~( fill="url(#nothing)"/>)~"), "paint"},
        {Doc(std::string(rect) + R"~( fill="url(#c)"/>)~",
             R"~(<clipPath id="c"><rect width="1" height="1"/></clipPath>)~"),
         "paint"},
        {Doc("", R"~(<linearGradient id="a" gradientUnits="userSpaceOnUse"><stop/></linearGradient>)~"),
         "gradientUnits"},
        {Doc("", R"~(<radialGradient id="a" cx="1" cy="1" r="1"><stop/></radialGradient>)~"), "userSpaceOnUse"},
        {Doc("", R"~(<radialGradient id="a" cy="1" r="1" gradientUnits="userSpaceOnUse"><stop/></radialGradient>)~"),
         "cx"},
        {Doc("", R"~(<linearGradient id="a" xlink:href="#b"/>)~"), "xlink:href"},
        {Doc("", R"~(<linearGradient id="a" gradientTransform="rotate(5)"><stop/></linearGradient>)~"),
         "gradientTransform"},
        {Doc("", R"~(<linearGradient id="a"><stop offset="0" style="stop-color:#fff"/></linearGradient>)~"), "style"},
        {Doc("", R"~(<linearGradient id="a"></linearGradient>)~"), "no stops"},
        {Doc("", R"~(<filter id="f"><feOffset dx="1"/></filter>)~"), "<feOffset>"},
        {Doc("", R"~(<clipPath id="c"><circle cx="1" cy="1" r="1"/></clipPath>)~"), "<circle>"},
        {Doc("", R"~(<pattern id="p"/>)~"), "<pattern>"},
        {Doc(R"~(<linearGradient id="z"><stop/></linearGradient>)~"), "<linearGradient>"},
        {Doc(R"~(<g><defs/></g>)~"), "<defs>"},
        {Doc(R"~(<g>hello</g>)~"), "text"},
        {Doc(R"~(<g clip-path="url(#nothing)"><rect width="1" height="1"/></g>)~"), "clip-path"},
        {Doc(R"~(<rect id="a&amp;b" width="1" height="1"/>)~"), "entity"},
        {R"~(<!DOCTYPE svg><svg width="1" height="1"/>)~", "element"},
        {R"~(<svg width="1" height="1" preserveAspectRatio="xMidYMid"/>)~", "preserveAspectRatio"},
        {R"~(<svg width="1" height="1" transform="rotate(5)"/>)~", "transform"},
        {R"~(<html/>)~", "root"},
        {R"~(<svg width="1" height="1">)~", "never closed"},
    };
    for (const Case &c : cases)
    {
        std::string error;
        EXPECT_FALSE(SvgArt::Parse(c.text, &error).has_value()) << c.text;
        EXPECT_NE(error.find(c.named), std::string::npos) << c.text << "\n  refused for: " << error;
    }
}

TEST(SvgArt, ShapesAreFilledAndStrokedAsTheJuceBuildsParserDrawsThem)
{
    const auto shapes = DrawnShapes(Doc(R"~(
<rect x="10" y="10" width="20" height="10"/>
<path d="M 0 0 L 10 0"/>
<path d="M 0 0 L 10 0 L 10 10 L 0 0"/>
<g fill="#102030" stroke="#405060" stroke-width="2">
<circle cx="50" cy="50" r="5" opacity="0.5" stroke-opacity="0.5"/>
</g>
<g stroke="#000" stroke-width="3" stroke-linecap="round">
<g transform="rotate(90 50 50)"><line x1="50" y1="0" x2="50" y2="10"/></g>
</g>
)~"),
                                    Affine::Scale(0.5f));
    ASSERT_EQ(shapes.size(), 5u);

    // A closed outline with no fill is filled black; an open one with none is not drawn at all; one that ends where it
    // began is closed, and filled black.
    EXPECT_FALSE(shapes[0].stroked);
    EXPECT_EQ(shapes[0].color, colors::kBlack);
    EXPECT_FALSE(shapes[1].stroked);
    EXPECT_EQ(shapes[1].color, colors::kBlack) << "the triangle that returns to its start";
    EXPECT_TRUE(FirstOf(shapes[1].path, Path::Op::kClose).has_value());

    // The paint a group hands down, the fill's alpha times the opacity and the stroke's times both.
    EXPECT_EQ(shapes[2].color, Rgb(0x10, 0x20, 0x30, 128)) << "255 x 0.5, rounded half to even";
    EXPECT_TRUE(shapes[3].stroked);
    EXPECT_EQ(shapes[3].color, Rgb(0x40, 0x50, 0x60, 64)) << "255 x 0.25";
    EXPECT_FLOAT_EQ(shapes[3].stroke.thickness, 2.0f);

    // The line turned a quarter about (50, 50) into the document's units, its width through the turn, its cap the
    // outer group's, and no fill.
    const Shape &line = shapes[4];
    EXPECT_TRUE(line.stroked);
    EXPECT_NEAR(line.stroke.thickness, 3.0f, 1e-5f);
    EXPECT_EQ(line.stroke.cap, Stroke::Cap::kRounded);
    const auto points = line.path.Flatten().front().points;
    ASSERT_EQ(points.size(), 2u);
    EXPECT_NEAR(points[0].x, 100.0f, 1e-4f);
    EXPECT_NEAR(points[0].y, 50.0f, 1e-4f);
    EXPECT_NEAR(points[1].x, 90.0f, 1e-4f);
    EXPECT_NEAR(points[1].y, 50.0f, 1e-4f);

    for (const Shape &shape : shapes)
    {
        EXPECT_FLOAT_EQ(shape.transform.m00, 0.5f) << "drawn through the transform it was handed";
        EXPECT_TRUE(shape.clips.empty());
    }
}

TEST(SvgArt, AGradientsFirstStopMovesToTheStartAndItsLastRepeatsAtTheEnd)
{
    const auto shapes = DrawnShapes(Doc(R"~(
<rect x="0" y="20" width="10" height="40" fill="url(#lin)"/>
<rect x="0" y="0" width="100" height="100" fill="url(#rad)" opacity="0.5"/>
<rect x="5" y="5" width="0" height="0" fill="url(#lin)"/>
)~",
                                        R"~(
<linearGradient id="lin" x1="0" y1="0" x2="0" y2="1">
<stop offset="0.25" stop-color="#ff0000"/><stop offset="0.75" stop-color="#0000ff" stop-opacity="0.5"/>
</linearGradient>
<radialGradient id="rad" cx="30" cy="40" r="20" gradientUnits="userSpaceOnUse">
<stop offset="0" stop-color="#fff"/><stop offset="100%" stop-color="#000"/>
</radialGradient>
)~"));
    ASSERT_EQ(shapes.size(), 3u);

    // On the rectangle's bounds, top to bottom; the red at a quarter moved to the start, so red fades from the very
    // top, and the half-clear blue at three quarters repeated at the bottom.
    ASSERT_TRUE(shapes[0].gradient.has_value());
    const Gradient &lin = *shapes[0].gradient;
    EXPECT_FALSE(lin.radial);
    EXPECT_FLOAT_EQ(lin.start.x, 0.0f);
    EXPECT_FLOAT_EQ(lin.start.y, 20.0f);
    EXPECT_FLOAT_EQ(lin.end.x, 0.0f);
    EXPECT_FLOAT_EQ(lin.end.y, 60.0f);
    EXPECT_EQ(lin.start_color, Rgb(255, 0, 0));
    ASSERT_EQ(lin.stops.size(), 1u);
    EXPECT_FLOAT_EQ(lin.stops[0].first, 0.75f);
    EXPECT_EQ(lin.stops[0].second, Rgb(0, 0, 255, 128));
    EXPECT_EQ(lin.end_color, Rgb(0, 0, 255, 128));

    // In the document's units, out to the radius, every stop's alpha times the opacity.
    ASSERT_TRUE(shapes[1].gradient.has_value());
    const Gradient &rad = *shapes[1].gradient;
    EXPECT_TRUE(rad.radial);
    EXPECT_FLOAT_EQ(rad.start.x, 30.0f);
    EXPECT_FLOAT_EQ(rad.start.y, 40.0f);
    EXPECT_FLOAT_EQ(rad.end.x, 50.0f);
    EXPECT_FLOAT_EQ(rad.end.y, 40.0f);
    EXPECT_EQ(rad.start_color, Rgb(255, 255, 255, 128));
    EXPECT_EQ(rad.end_color, Rgb(0, 0, 0, 128));
    EXPECT_TRUE(rad.stops.empty());

    // A gradient across bounds with no extent has no direction: the last colour.
    EXPECT_FALSE(shapes[2].gradient.has_value());
    EXPECT_EQ(shapes[2].color, Rgb(0, 0, 255, 128));
}

TEST(SvgArt, EachShapeUnderAClippedGroupIsClippedThroughItsOwnTransforms)
{
    const auto shapes =
        DrawnShapes(Doc(R"~(
<g clip-path="url(#win)">
<rect width="100" height="100" fill="#fff"/>
<g stroke="#000"><g transform="rotate(90 50 50)"><line x1="50" y1="0" x2="50" y2="10"/></g></g>
</g>
<rect width="10" height="10" fill="#fff"/>
)~",
                        R"~(<clipPath id="win"><rect x="0" y="0" width="100" height="50" rx="5"/></clipPath>)~"));
    ASSERT_EQ(shapes.size(), 3u);
    ASSERT_EQ(shapes[0].clips.size(), 1u);
    const RectF window = shapes[0].clips[0].Bounds();
    EXPECT_FLOAT_EQ(window.x(), 0.0f);
    EXPECT_FLOAT_EQ(window.y(), 0.0f);
    EXPECT_FLOAT_EQ(window.width(), 100.0f);
    EXPECT_FLOAT_EQ(window.height(), 50.0f);
    EXPECT_TRUE(FirstOf(shapes[0].clips[0], Path::Op::kRoundedRect).has_value());

    // The line's clip is the window turned with it, as the parser reads a clip in the shape's own state.
    ASSERT_EQ(shapes[1].clips.size(), 1u);
    const RectF turned = shapes[1].clips[0].Bounds();
    EXPECT_NEAR(turned.x(), 50.0f, 1e-3f);
    EXPECT_NEAR(turned.y(), 0.0f, 1e-3f);
    EXPECT_NEAR(turned.width(), 50.0f, 1e-3f);
    EXPECT_NEAR(turned.height(), 100.0f, 1e-3f);

    EXPECT_TRUE(shapes[2].clips.empty()) << "and nothing outside the group clipped";
}

TEST(SvgArt, ANumberIsReadInEveryFormADecimalTakes)
{
    // A sign either way, no digit before the point or none after it, an exponent in either case - each the nearest
    // double, whatever the process's locale - and a number past a double's range refused.
    const auto shapes = DrawnShapes(Doc(R"~(<rect x="+1.5e1" y="-.5" width="2." height="1E1"/>)~"));
    ASSERT_EQ(shapes.size(), 1u);
    const RectF bounds = shapes[0].path.Bounds();
    EXPECT_FLOAT_EQ(bounds.x(), 15.0f);
    EXPECT_FLOAT_EQ(bounds.y(), -0.5f);
    EXPECT_FLOAT_EQ(bounds.width(), 2.0f);
    EXPECT_FLOAT_EQ(bounds.height(), 10.0f);
    std::string error;
    EXPECT_FALSE(SvgArt::Parse(Doc(R"~(<rect width="1e999" height="1"/>)~"), &error).has_value());
    EXPECT_NE(error.find("plain number"), std::string::npos) << error;
}

TEST(SvgArt, AnArcIsCentredByTheParsersArithmeticAndCarriedToItsEnd)
{
    const auto shapes = DrawnShapes(Doc(R"~(<path d="M 10 50 A 40 40 0 0 1 90 50" fill="none" stroke="#000"/>)~"));
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_TRUE(shapes[0].stroked) << "fill none: the stroke alone";
    const auto arc = FirstOf(shapes[0].path, Path::Op::kCentredArc);
    ASSERT_TRUE(arc.has_value());
    EXPECT_FLOAT_EQ(arc->v[0], 50.0f);
    EXPECT_FLOAT_EQ(arc->v[1], 50.0f);
    EXPECT_FLOAT_EQ(arc->v[2], 40.0f);
    EXPECT_FLOAT_EQ(arc->v[3], 40.0f);
    // From nine o'clock clockwise over twelve to three, angles clockwise from twelve: the parser starts it at 3 pi / 2.
    EXPECT_NEAR(arc->v[5], 1.5f * std::numbers::pi_v<float>, 1e-5f);
    EXPECT_NEAR(arc->v[6], 2.5f * std::numbers::pi_v<float>, 1e-5f);
    EXPECT_FALSE(arc->flag) << "joined to the M's point, not a sub-path of its own";
    const auto &elements = shapes[0].path.elements();
    EXPECT_EQ(elements.back().op, Path::Op::kLineTo) << "carried on to the arc's end with a line";
    EXPECT_FLOAT_EQ(elements.back().v[0], 90.0f);
    EXPECT_FLOAT_EQ(elements.back().v[1], 50.0f);
    EXPECT_FALSE(FirstOf(shapes[0].path, Path::Op::kClose).has_value()) << "ending away from its start, left open";

    // The sweep's turn held to the flag's direction: the large clockwise arc from twelve o'clock round to nine, and the
    // anticlockwise half from nine under six to three.
    const auto turns = DrawnShapes(Doc(R"~(
<path d="M 50 10 A 40 40 0 1 1 10 50" fill="none" stroke="#000"/>
<path d="M 10 50 A 40 40 0 0 0 90 50" fill="none" stroke="#000"/>
)~"));
    ASSERT_EQ(turns.size(), 2u);
    const auto large = FirstOf(turns[0].path, Path::Op::kCentredArc);
    const auto under = FirstOf(turns[1].path, Path::Op::kCentredArc);
    ASSERT_TRUE(large.has_value());
    ASSERT_TRUE(under.has_value());
    EXPECT_NEAR(large->v[0], 50.0f, 1e-4f);
    EXPECT_NEAR(large->v[1], 50.0f, 1e-4f);
    EXPECT_NEAR(large->v[5], 0.0f, 1e-5f);
    EXPECT_NEAR(large->v[6], 1.5f * std::numbers::pi_v<float>, 1e-5f);
    EXPECT_NEAR(under->v[5], 1.5f * std::numbers::pi_v<float>, 1e-5f);
    EXPECT_NEAR(under->v[6], 0.5f * std::numbers::pi_v<float>, 1e-5f);
}

TEST(SvgArt, TheMetersFaceAndGlassAsTheJuceBuildDrawsThem)
{
    const auto face = DrawnShapes(ReadMeterFile("vu_meter_face.svg"));
    ASSERT_EQ(face.size(), 28u);

    // The bezel's edge: top to bottom across the housing's bounds.
    ASSERT_TRUE(face[0].gradient.has_value());
    EXPECT_FLOAT_EQ(face[0].gradient->start.y, 140.0f);
    EXPECT_FLOAT_EQ(face[0].gradient->end.y, 860.0f);
    EXPECT_EQ(face[0].gradient->start_color, Rgb(0x6b, 0x6b, 0x6e));
    EXPECT_EQ(face[0].gradient->end_color, Rgb(0x2a, 0x2a, 0x2c));

    // The glow drawn sharp - the parser draws no filter - at 35 %, and the window's black over it.
    EXPECT_EQ(face[2].color, Rgb(0xff, 0x9a, 0x3a, 89));
    EXPECT_EQ(face[3].color, Rgb(5, 5, 5));

    // The vignette, which SVG keeps clear to 55 % of its radius, darkening from the centre: its first stop moved there.
    ASSERT_TRUE(face[5].gradient.has_value());
    const Gradient &vignette = *face[5].gradient;
    EXPECT_TRUE(vignette.radial);
    EXPECT_FLOAT_EQ(vignette.start.x, 500.0f);
    EXPECT_FLOAT_EQ(vignette.start.y, 480.0f);
    EXPECT_FLOAT_EQ(vignette.end.x, 1020.0f);
    EXPECT_EQ(vignette.start_color, Rgb(0, 0, 0, 0));
    EXPECT_EQ(vignette.end_color, Rgb(0, 0, 0, 140));
    EXPECT_TRUE(vignette.stops.empty());
    ASSERT_EQ(face[5].clips.size(), 1u);
    EXPECT_TRUE(FirstOf(face[5].clips[0], Path::Op::kRoundedRect).has_value()) << "under the window's rounded clip";

    // The scale's arc, stroked round-capped; then a major tick at its own width through its turn, under the window
    // turned with it.
    EXPECT_TRUE(face[7].stroked);
    EXPECT_FLOAT_EQ(face[7].stroke.thickness, 7.0f);
    EXPECT_EQ(face[7].stroke.cap, Stroke::Cap::kRounded);
    EXPECT_TRUE(FirstOf(face[7].path, Path::Op::kCentredArc).has_value());
    EXPECT_TRUE(face[8].stroked);
    EXPECT_NEAR(face[8].stroke.thickness, 6.0f, 1e-5f);
    EXPECT_EQ(face[8].stroke.cap, Stroke::Cap::kButt);
    EXPECT_EQ(face[8].color, Rgb(0x1a, 0x10, 0x05));
    ASSERT_EQ(face[8].clips.size(), 1u);
    EXPECT_TRUE(FirstOf(face[8].clips[0], Path::Op::kTransform).has_value());
    EXPECT_NEAR(face[18].stroke.thickness, 3.5f, 1e-5f) << "a minor tick";

    const auto glass = DrawnShapes(ReadMeterFile("vu_meter_glass.svg"));
    ASSERT_EQ(glass.size(), 11u) << "the sheen, the highlight, the rim stroked, and the screws filled and stroked";
    // The sheen: corner to corner of the window, its last stop short of the end repeated there.
    ASSERT_TRUE(glass[0].gradient.has_value());
    const Gradient &sheen = *glass[0].gradient;
    EXPECT_FLOAT_EQ(sheen.start.x, 92.0f);
    EXPECT_FLOAT_EQ(sheen.start.y, 192.0f);
    EXPECT_FLOAT_EQ(sheen.end.x, 908.0f);
    EXPECT_FLOAT_EQ(sheen.end.y, 748.0f);
    EXPECT_EQ(sheen.start_color, Rgb(255, 255, 255, 56));
    ASSERT_EQ(sheen.stops.size(), 2u);
    EXPECT_FLOAT_EQ(sheen.stops[0].first, 0.45f);
    EXPECT_EQ(sheen.stops[0].second, Rgb(255, 255, 255, 10));
    EXPECT_FLOAT_EQ(sheen.stops[1].first, 0.5f);
    EXPECT_EQ(sheen.end_color, Rgb(255, 255, 255, 0));
    EXPECT_EQ(glass[1].color, Rgb(255, 255, 255, 15)) << "the highlight at 6 %";
    // The rim: no fill, its stroke black at 60 %.
    EXPECT_TRUE(glass[2].stroked);
    EXPECT_EQ(glass[2].color, Rgb(0, 0, 0, 153));
    EXPECT_FLOAT_EQ(glass[2].stroke.thickness, 4.0f);
}
