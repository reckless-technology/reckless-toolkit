// A window's content in design pixels held to a work area in the display's pixels (app/WindowPlace.h), as the Windows
// and Linux platforms hold a window opened at a kept size: a size that fits comes back whole at every scale a display
// is set to - 885 x 615 at 1.25 is 1106 x 769 pixels, which is 885 x 615 again, not the 884 x 615 a truncated 884.8
// gave - and a size that does not is the most whose pixels fit, never a pixel past the area.
#include <gtest/gtest.h>

#include <array>

#include "WindowPlace.h"

namespace
{
using reckless::app::DesignWithinPixels;
using reckless::app::FittedDesignContent;
using reckless::app::PixelsOfDesign;
using reckless::gfx::PointI;
using reckless::gfx::RectI;

/** The scales a display is set to: 96, 120, 144, 168 and 192 dpi on Windows. */
constexpr std::array<float, 5> kScales{1.0f, 1.25f, 1.5f, 1.75f, 2.0f};

/** Content sizes the apps keep: Sudoku's 75 % and 125 %, Knect 4's 75 %, and odd and even sides beside them. */
constexpr std::array<PointI, 6> kSizes{{{885, 615}, {1475, 1025}, {825, 570}, {886, 616}, {1, 1}, {1179, 821}}};

/** A frame's title bar and borders, in the display's pixels: Windows' at 1.25 with a menu bar. */
constexpr PointI kDecoration{20, 79};
} // namespace

TEST(WindowPlace, ADesignSizeComesBackFromItsPixelsAtEveryScale)
{
    for (const float scale : kScales)
    {
        for (int design = 1; design <= 4000; ++design)
        {
            ASSERT_EQ(DesignWithinPixels(PixelsOfDesign(design, scale), scale), design) << "at " << scale;
        }
    }
}

TEST(WindowPlace, TheDesignWithinARoomIsTheMostWhosePixelsFit)
{
    for (const float scale : {0.75f, 1.0f, 1.25f, 1.333333f, 1.5f, 1.75f, 2.0f, 2.25f, 3.0f})
    {
        for (int pixels = 1; pixels <= 4000; ++pixels)
        {
            const int design = DesignWithinPixels(pixels, scale);
            ASSERT_LE(PixelsOfDesign(design, scale), pixels) << pixels << " pixels at " << scale << ": never past it";
            ASSERT_GT(PixelsOfDesign(design + 1, scale), pixels) << pixels << " pixels at " << scale << ": the most";
        }
    }
}

TEST(WindowPlace, AKeptSizeThatFitsOpensWholeAtEveryScale)
{
    const RectI roomy{0, 0, 8000, 6000};
    for (const float scale : kScales)
    {
        for (const PointI size : kSizes)
        {
            EXPECT_EQ(FittedDesignContent(size, kDecoration, roomy, scale), size)
                << size.x << " x " << size.y << " at " << scale;
            // A work area that takes the window to the pixel takes it whole too.
            const RectI exact{0, 0, PixelsOfDesign(size.x, scale) + kDecoration.x,
                              PixelsOfDesign(size.y, scale) + kDecoration.y};
            EXPECT_EQ(FittedDesignContent(size, kDecoration, exact, scale), size)
                << size.x << " x " << size.y << " at " << scale << " on an area it just fits";
        }
    }
}

TEST(WindowPlace, AKeptSizeThatDoesNotFitIsTheMostWhosePixelsStandOnTheArea)
{
    for (const float scale : kScales)
    {
        for (const PointI size : kSizes)
        {
            if (size.x == 1)
            {
                continue; // nothing smaller to give
            }
            // A pixel short of the window on each side.
            const RectI  area{100, 50, PixelsOfDesign(size.x, scale) + kDecoration.x - 1,
                              PixelsOfDesign(size.y, scale) + kDecoration.y - 1};
            const PointI fitted = FittedDesignContent(size, kDecoration, area, scale);
            EXPECT_LT(fitted.x, size.x) << size.x << " at " << scale;
            EXPECT_LT(fitted.y, size.y) << size.y << " at " << scale;
            EXPECT_LE(PixelsOfDesign(fitted.x, scale) + kDecoration.x, area.width()) << "never past the area";
            EXPECT_LE(PixelsOfDesign(fitted.y, scale) + kDecoration.y, area.height());
            EXPECT_GT(PixelsOfDesign(fitted.x + 1, scale) + kDecoration.x, area.width()) << "the most that fits";
            EXPECT_GT(PixelsOfDesign(fitted.y + 1, scale) + kDecoration.y, area.height());
        }
    }
}

TEST(WindowPlace, AFittedSideIsNeverLessThanOne)
{
    EXPECT_EQ(FittedDesignContent({885, 615}, kDecoration, RectI{0, 0, 10, 10}, 1.25f), (PointI{1, 1}));
}
