// The suite's drawing types with no toolkit round them (gfx): the rectangle's layout arithmetic - strips taken
// off its sides held to what there is, sizes never under zero, an int rectangle's centre rounding down, whole pixels by
// rounding half to even - the colour's shades, alphas and blends to the byte, the affine transform's composition, and
// the path's flattening, bounds and hit test - and text that is not UTF-8 repaired for a shaper, each ill-formed part
// one U+FFFD.
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "Path.h"

#include <gtest/gtest.h>

namespace
{
using reckless::gfx::Affine;
using reckless::gfx::Color;
using reckless::gfx::Path;
using reckless::gfx::PointF;
using reckless::gfx::PointI;
using reckless::gfx::RectF;
using reckless::gfx::RectI;

TEST(GfxRect, StripsComeOffTheSidesHeldToWhatThereIs)
{
    RectI area{10, 20, 300, 100};
    EXPECT_EQ(area.RemoveFromTop(30), (RectI{10, 20, 300, 30}));
    EXPECT_EQ(area, (RectI{10, 50, 300, 70}));
    EXPECT_EQ(area.RemoveFromLeft(40), (RectI{10, 50, 40, 70}));
    EXPECT_EQ(area.RemoveFromRight(60), (RectI{250, 50, 60, 70}));
    EXPECT_EQ(area.RemoveFromBottom(500), (RectI{50, 50, 200, 70})) << "a strip held to the height there is";
    EXPECT_EQ(area, (RectI{50, 50, 200, 0})) << "a strip off the bottom leaves the top where it was";
}

TEST(GfxRect, SizesNeverGoUnderZeroAndCentresHalveInTheirType)
{
    const RectI r{0, 0, 7, 9};
    EXPECT_EQ(r.Reduced(5), (RectI{5, 5, 0, 0})) << "reduced past nothing is nothing, not negative";
    EXPECT_EQ(r.Reduced(1, 2), (RectI{1, 2, 5, 5}));
    EXPECT_EQ(r.Expanded(2), (RectI{-2, -2, 11, 13}));
    EXPECT_EQ(r.Centre(), (PointI{3, 4})) << "an int rectangle's centre rounds down";
    EXPECT_EQ(r.WithSizeKeepingCentre(4, 4), (RectI{1, 2, 4, 4}));
    EXPECT_EQ(r.WithCentre({10, 10}), (RectI{7, 6, 7, 9}));
    EXPECT_EQ(RectF(0, 0, 7, 9).Centre(), (PointF{3.5f, 4.5f}));
    EXPECT_EQ(r.WithWidth(-3), (RectI{0, 0, 0, 9}));
    EXPECT_EQ(r.WithTrimmedTop(4), (RectI{0, 4, 7, 5}));
    EXPECT_EQ(r.WithTrimmedRight(10), (RectI{0, 0, 0, 9}));
    EXPECT_EQ(r.WithBottom(-4), (RectI{0, -4, 7, 0}));
}

TEST(GfxRect, ContainmentOverlapAndRounding)
{
    const RectI r{0, 0, 10, 10};
    EXPECT_TRUE(r.Contains(PointI{0, 0})) << "the left and top edges in";
    EXPECT_FALSE(r.Contains(PointI{10, 5})) << "the right edge out";
    EXPECT_TRUE(r.Intersects({5, 5, 10, 10}));
    EXPECT_FALSE(r.Intersects({10, 0, 5, 5})) << "touching is not overlapping";
    EXPECT_EQ(r.Intersection({5, -5, 10, 10}), (RectI{5, 0, 5, 5}));
    EXPECT_EQ(r.Intersection({20, 20, 5, 5}), RectI{}) << "no overlap, nothing at the origin";
    EXPECT_EQ(r.Union({20, 20, 5, 5}), (RectI{0, 0, 25, 25}));
    EXPECT_EQ(r.Union({}), r) << "an empty rectangle counts for nothing";
    EXPECT_EQ(RectF(0.5f, 1.5f, 2.5f, 3.5f).ToNearestInt(), (RectI{0, 2, 2, 4})) << "half to even";
    EXPECT_EQ(RectF(0.2f, 0.7f, 2.2f, 2.0f).SmallestIntegerContainer(), (RectI{0, 0, 3, 3}));
}

TEST(GfxRect, EachSideMovesAsJucesRectangleMovesIt)
{
    // The rest of the layout arithmetic the views lay panels out with, each as JUCE's Rectangle does it: a size never
    // under zero with the moved side where it was asked, a centred size's offset halved toward zero in the type.
    const RectI r{10, 20, 30, 40}; // right 40, bottom 60
    EXPECT_EQ(r.Expanded(3, 5), (RectI{7, 15, 36, 50}));
    EXPECT_EQ(r.Expanded(-20, -1), (RectI{30, 21, 0, 38})) << "never under zero: the corner moves on";
    EXPECT_EQ(r.Reduced(20, 1), r.Expanded(-20, -1));
    EXPECT_EQ(r.WithLeft(25), (RectI{25, 20, 15, 40})) << "the right edge stays";
    EXPECT_EQ(r.WithLeft(50), (RectI{50, 20, 0, 40})) << "past the right edge: nothing, at the new left";
    EXPECT_EQ(r.WithTop(0), (RectI{10, 0, 30, 60})) << "the bottom stays";
    EXPECT_EQ(r.WithTop(70), (RectI{10, 70, 30, 0}));
    EXPECT_EQ(r.WithRight(35), (RectI{10, 20, 25, 40})) << "the left stays";
    EXPECT_EQ(r.WithRight(5), (RectI{5, 20, 0, 40})) << "left of the left edge: nothing, there";
    EXPECT_EQ(r.WithHeight(12), (RectI{10, 20, 30, 12}));
    EXPECT_EQ(r.WithHeight(-3), (RectI{10, 20, 30, 0}));
    EXPECT_EQ(r.Translated(-4, 7), (RectI{6, 27, 30, 40}));
    EXPECT_EQ(r.WithSizeKeepingCentre(11, 41), (RectI{19, 20, 11, 41})) << "(30 - 11) / 2 = 9, (40 - 41) / 2 = 0";
    EXPECT_EQ(r.WithSizeKeepingCentre(33, 50), (RectI{9, 15, 33, 50})) << "(30 - 33) / 2 = -1, toward zero";
    EXPECT_TRUE(r.Contains(r)) << "itself";
    EXPECT_TRUE(r.Contains(RectI{15, 25, 0, 0})) << "an empty rectangle inside";
    EXPECT_TRUE(r.Contains(RectI{40, 60, 0, 0})) << "an empty one on the far corner: the far edges count here";
    EXPECT_FALSE(r.Contains(RectI{15, 25, 26, 5})) << "one past the right edge";
    EXPECT_FALSE(r.Contains(RectI{9, 25, 5, 5})) << "one before the left";

    // A float rectangle: the same rules in floats, a strip held to what there is.
    const RectF f{1.5f, 2.5f, 10.0f, 6.0f};
    EXPECT_EQ(f.Reduced(1.25f, 0.5f), (RectF{2.75f, 3.0f, 7.5f, 5.0f}));
    EXPECT_EQ(f.Reduced(6.0f, 0.0f), (RectF{7.5f, 2.5f, 0.0f, 6.0f})) << "never under zero";
    EXPECT_EQ(f.WithSizeKeepingCentre(3.0f, 5.0f), (RectF{5.0f, 3.0f, 3.0f, 5.0f})) << "halved exactly";
    RectF g = f;
    EXPECT_EQ(g.RemoveFromTop(1.5f), (RectF{1.5f, 2.5f, 10.0f, 1.5f}));
    EXPECT_EQ(g, (RectF{1.5f, 4.0f, 10.0f, 4.5f}));
    EXPECT_EQ(g.RemoveFromLeft(2.25f), (RectF{1.5f, 4.0f, 2.25f, 4.5f}));
    EXPECT_EQ(g.RemoveFromRight(20.0f), (RectF{3.75f, 4.0f, 7.75f, 4.5f})) << "held to the width there is";
    EXPECT_EQ(g, (RectF{3.75f, 4.0f, 0.0f, 4.5f}));
    EXPECT_EQ(g.RemoveFromBottom(1.0f), (RectF{3.75f, 7.5f, 0.0f, 1.0f}));
    EXPECT_EQ(g, (RectF{3.75f, 4.0f, 0.0f, 3.5f}));
}

TEST(GfxAffine, AScaleAboutAPivotLeavesItWhereItIsAndATranslationAddsToTheLastColumn)
{
    // As JUCE's AffineTransform::scale about a pivot and translated: the pivot fixed, the offset the pivot times one
    // less the factor; a translation moving the result and leaving the matrix as it was.
    const Affine s = Affine::Scale(2.0f, 0.5f, 10.0f, 20.0f);
    EXPECT_EQ(s.Apply({10.0f, 20.0f}), (PointF{10.0f, 20.0f})) << "the pivot stays";
    EXPECT_EQ(s.Apply({12.0f, 24.0f}), (PointF{14.0f, 22.0f}));
    EXPECT_EQ(s.m02, -10.0f);
    EXPECT_EQ(s.m12, 10.0f);
    const Affine spin  = Affine::Rotation(0.5f);
    const Affine moved = spin.Translated(3.0f, -4.0f);
    EXPECT_EQ(moved.m00, spin.m00);
    EXPECT_EQ(moved.m01, spin.m01);
    EXPECT_EQ(moved.m10, spin.m10);
    EXPECT_EQ(moved.m11, spin.m11);
    EXPECT_EQ(moved.m02, 3.0f);
    EXPECT_EQ(moved.m12, -4.0f);
    EXPECT_EQ(Affine::Scale(2.0f).Translated(3.0f, 4.0f).Apply({1.0f, 1.0f}), (PointF{5.0f, 6.0f}))
        << "scaled, then moved";
}

TEST(GfxColor, ShadesAlphasAndBlendsToTheByte)
{
    const Color c = Color::FromRgb(200, 100, 50);
    // A distance from white scaled by 1 / 1.4, truncated: 255 - 55 / 1.4 = 215.7, 255 - 155 / 1.4 = 144.3.
    EXPECT_EQ(c.Brighter(), Color::FromRgb(215, 144, 108));
    EXPECT_EQ(c.Darker(), Color::FromRgb(142, 71, 35));
    EXPECT_EQ(c.WithAlpha(0.5f).alpha(), 128) << "127.5 rounds half to even";
    EXPECT_EQ(c.WithAlpha(0.3f).alpha(), 76);
    EXPECT_EQ(c.WithAlpha(-1.0f).alpha(), 0);
    EXPECT_EQ(c.WithAlpha(0.5f).WithMultipliedAlpha(0.5f).alpha(), 64);
    EXPECT_EQ(c.InterpolatedWith(reckless::gfx::colors::kBlack, 0.0f), c);
    EXPECT_EQ(c.InterpolatedWith(reckless::gfx::colors::kBlack, 1.0f), reckless::gfx::colors::kBlack);
    // Half way at full alpha: each channel moves (target - source) * 128 >> 8, with unsigned wrap for a fall.
    EXPECT_EQ(Color::FromRgb(0, 0, 0).InterpolatedWith(Color::FromRgb(200, 100, 50), 0.5f),
              Color::FromRgb(100, 50, 25));
    const Color faded = Color::FromArgb(128, 255, 0, 0).InterpolatedWith(Color::FromArgb(0, 0, 0, 0), 0.5f);
    // Premultiplied, the red is (255 x 128 + 127) >> 8 = 127; half way to nothing it is 63 at an alpha of 64, which
    // unpremultiplies to 63 x 255 / 64 = 251: the premultiplied rounding shows.
    EXPECT_EQ(faded.alpha(), 64) << "the blend runs on premultiplied channels";
    EXPECT_EQ(faded.red(), 251) << "and comes back unpremultiplied";
}

TEST(GfxAffine, ComposesAndRotatesAboutItsPivot)
{
    const Affine spin  = Affine::Rotation(0.5f, 10.0f, 20.0f);
    const PointF pivot = spin.Apply({10.0f, 20.0f});
    EXPECT_NEAR(pivot.x, 10.0f, 1.0e-5f);
    EXPECT_NEAR(pivot.y, 20.0f, 1.0e-5f);
    const Affine t = Affine::Scale(2.0f).FollowedBy(Affine::Translation(3.0f, 4.0f));
    EXPECT_EQ(t.Apply({1.0f, 1.0f}), (PointF{5.0f, 6.0f})) << "scaled first, then moved";
    const PointF quarter = Affine::Rotation(std::numbers::pi_v<float> / 2.0f).Apply({1.0f, 0.0f});
    EXPECT_NEAR(quarter.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(quarter.y, 1.0f, 1.0e-6f) << "a positive angle turns clockwise on a screen whose y runs down";
    EXPECT_TRUE(Affine::Identity().IsIdentity());
}

TEST(GfxPath, ShapesFlattenIntoTheirOutlines)
{
    Path rounded;
    rounded.AddRoundedRect({10.0f, 10.0f, 100.0f, 50.0f}, 8.0f);
    const RectF bounds = rounded.Bounds();
    EXPECT_NEAR(bounds.x(), 10.0f, 1.0e-3f);
    EXPECT_NEAR(bounds.right(), 110.0f, 1.0e-3f);
    EXPECT_NEAR(bounds.bottom(), 60.0f, 1.0e-3f);
    EXPECT_TRUE(rounded.Contains({60.0f, 35.0f}));
    EXPECT_FALSE(rounded.Contains({10.5f, 10.5f})) << "the corner is rounded off";

    Path ellipse;
    ellipse.AddEllipse({0.0f, 0.0f, 20.0f, 10.0f});
    EXPECT_TRUE(ellipse.Contains({10.0f, 5.0f}));
    EXPECT_FALSE(ellipse.Contains({1.0f, 1.0f}));

    Path line;
    line.MoveTo(0.0f, 0.0f);
    EXPECT_TRUE(line.IsEmpty()) << "a sub-path with nothing drawn";
    line.LineTo(10.0f, 0.0f);
    line.LineTo(10.0f, 10.0f);
    line.Close();
    EXPECT_FALSE(line.IsEmpty());
    EXPECT_TRUE(line.Contains({8.0f, 2.0f}));
    line.ApplyTransform(Affine::Translation(100.0f, 0.0f));
    EXPECT_TRUE(line.Contains({108.0f, 2.0f})) << "a transform moves everything before it";
    EXPECT_FALSE(line.Contains({8.0f, 2.0f}));
}

TEST(GfxText, TextThatIsNotUtf8IsRepairedOneReplacementAPart)
{
    using reckless::gfx::IsValidUtf8;
    using reckless::gfx::RepairedUtf8;
    const std::string replacement = "\xEF\xBF\xBD";

    // Well-formed text of every length a character takes stands as it is.
    for (const char *text : {"GAIN", "Name\xE2\x80\x99s Earphones", "\xC3\xA9t\xC3\xA9", "\xF0\x9F\x8E\xB8", ""})
    {
        EXPECT_TRUE(IsValidUtf8(text)) << text;
        EXPECT_EQ(RepairedUtf8(text), std::string(text));
    }

    // A code page's bytes: the system's message in the ANSI code page, a device's name in Mac OS Roman.
    EXPECT_FALSE(IsValidUtf8("Acc\xE8s refus\xE9."));
    EXPECT_EQ(RepairedUtf8("Acc\xE8s refus\xE9."), "Acc" + replacement + "s refus" + replacement + ".");
    EXPECT_EQ(RepairedUtf8("Name\xD5s Earphones"), "Name" + replacement + "s Earphones");
    EXPECT_EQ(RepairedUtf8("\xE8"), replacement) << "a lone lead byte";
    EXPECT_EQ(RepairedUtf8("ab\xE2\x80"), "ab" + replacement) << "a character cut short at the end: one for both";

    // The standard's own example of replacing the longest broken starts: F1 80 80, E1 80 and C2 each one replacement,
    // a stray continuation byte one each.
    EXPECT_EQ(RepairedUtf8("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64"),
              "a" + replacement + replacement + replacement + "b" + replacement + "c" + replacement + replacement +
                  "d");

    // What no well-formed sequence holds: a surrogate, an overlong form, past U+10FFFF - a replacement each byte.
    EXPECT_EQ(RepairedUtf8("\xED\xA0\x80"), replacement + replacement + replacement);
    EXPECT_EQ(RepairedUtf8("\xC0\xAF"), replacement + replacement);
    EXPECT_EQ(RepairedUtf8("\xF4\x90\x80\x80"), replacement + replacement + replacement + replacement);
    EXPECT_TRUE(IsValidUtf8(RepairedUtf8("\xF4\x90\x80\x80 \xC0 \xE8\x41")));
}

TEST(GfxText, TextReadACodePointAtATimeIsTheRepairedTextsCharacters)
{
    // A character a code point, each of a broken part's longest starts one U+FFFD - the good bytes after it read as
    // themselves, never swallowed as a lead byte's continuation - exactly the characters RepairedUtf8 leaves.
    using reckless::gfx::NextCodePoint;
    const auto points = [](std::string_view text) {
        std::u32string out;
        for (std::size_t at = 0; at < text.size();)
        {
            out.push_back(NextCodePoint(text, at));
        }
        return out;
    };
    EXPECT_EQ(points("A\xC3\xA9\xE2\x86\x92\xF0\x9F\x8E\xB8"), std::u32string(U"A\u00E9\u2192\U0001F3B8"));
    EXPECT_EQ(points("\xC3"
                     "A"),
              std::u32string(U"\uFFFDA"))
        << "a lead byte with no continuation: the A kept";
    EXPECT_EQ(points("\xC0\x80"), std::u32string(U"\uFFFD\uFFFD")) << "an overlong form";
    EXPECT_EQ(points("\xED\xA0\x80"), std::u32string(U"\uFFFD\uFFFD\uFFFD")) << "a surrogate";
    EXPECT_EQ(points("\xF5\x80"), std::u32string(U"\uFFFD\uFFFD")) << "past U+10FFFF";
    EXPECT_EQ(points("ab\xE2\x80"), std::u32string(U"ab\uFFFD")) << "cut short at the end: one for both";
    constexpr std::string_view kText = "\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64";
    EXPECT_EQ(points(kText), points(reckless::gfx::RepairedUtf8(kText)));
}
} // namespace
