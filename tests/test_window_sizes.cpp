// A window's sizes from a View menu (app/AppWindowSizes.h) on the suites' own platform (tests/FakeAppPlatform.h): each
// size the design times it, drawn at exactly that scale and in force until the person drags the frame; a launch opening
// at the size the app kept; a size the screen does not take dim and refused, a chosen one kept on the screen; the next
// size up and down from a size in force or a dragged one; nothing to size with no window.
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <utility>

#include "AppRoot.h"
#include "AppTheme.h"
#include "AppWindowSizes.h"
#include "FakeAppPlatform.h"

namespace
{
using reckless::app::kWindowSizes;
using reckless::app::WindowSizeMenu;
using reckless::app::WindowSizes;
using reckless::gfx::PointI;

/** The layout's design size, which 100 % is. */
constexpr PointI kDesign{1000, 800};

/** A tooltip's look; the suite shows none. */
reckless::view::TooltipStyle Tooltip()
{
    using reckless::gfx::Color;
    return reckless::app::Theme{Color(0xff101010u), Color(0xff202020u), Color(0xff303030u), Color(0xff404040u),
                                Color(0xffe0e0e0u), Color(0xffa0a0a0u), Color(0xff606060u), Color(0xffe0a040u),
                                Color(0xff806020u), Color(0xff40c040u), Color(0xffc04040u)}
        .Tooltip();
}

/** A window as an app holds it: the content in its root, the sizes over the root, opened at the size the app kept, the
    window opened round the root - its resizes followed by the root, as RunApp follows them - at (40, 30) on a screen
    of 3000 x 2000. */
struct WindowRig
{
    /** @param percent  the size the app kept */
    explicit WindowRig(int percent = 100)
    {
        sizes.Restore(percent);
        reckless::app::WindowOptions options;
        options.width    = root.width();
        options.height   = root.height();
        options.position = std::pair<int, int>{40, 30};
        options.resized  = [this](int width, int height) { root.SetBounds({width, height}); };
        window           = platform.OpenWindow(options);
        fake().work_area = reckless::gfx::RectI{0, 0, 3000, 2000};
        sizes.SetWindow(window.get());
    }

    /** @return  the window as the suites' platform holds it */
    reckless::test::FakeAppWindow &fake()
    {
        return *platform.windows().front();
    }

    /** @return  the window's content, as the platform holds it */
    PointI WindowSize()
    {
        return {fake().options().width, fake().options().height};
    }

    reckless::view::View                      content;
    reckless::app::AppRoot                    root{content, Tooltip(), [] { return 0.0; }, {900, 700}};
    WindowSizes                               sizes{root, kDesign};
    reckless::test::FakeAppPlatform           platform;
    std::unique_ptr<reckless::app::AppWindow> window;
};
} // namespace

TEST(WindowSizes, EachSizeMakesTheWindowTheDesignTimesItDrawnAtExactlyThatScale)
{
    WindowRig rig;
    EXPECT_EQ(rig.sizes.InForce(), 100) << "opened at the design size";
    EXPECT_EQ(rig.WindowSize(), kDesign);

    for (const int percent : kWindowSizes)
    {
        ASSERT_TRUE(rig.sizes.Choose(percent)) << percent;
        const PointI expected{kDesign.x * percent / 100, kDesign.y * percent / 100};
        EXPECT_EQ(rig.WindowSize(), expected) << percent;
        EXPECT_EQ(rig.root.width(), expected.x) << "the root follows the window";
        EXPECT_EQ(rig.root.height(), expected.y);
        EXPECT_FLOAT_EQ(rig.root.zoom().zoom(), static_cast<float>(percent) / 100.0f) << "drawn at exactly the size";
        EXPECT_EQ(rig.content.width(), kDesign.x) << "laid out at the design size, whatever the size";
        EXPECT_EQ(rig.content.height(), kDesign.y);
        EXPECT_EQ(rig.sizes.InForce(), percent);
        EXPECT_EQ(rig.sizes.Menu().in_force, percent) << "ticked";
    }
    EXPECT_FALSE(rig.sizes.Choose(80)) << "a size the menu does not offer";
    EXPECT_EQ(rig.sizes.InForce(), 150);

    rig.fake().Resize(900, 700); // the person drags the frame
    EXPECT_EQ(rig.sizes.InForce(), 0);
    const WindowSizeMenu dragged = rig.sizes.Menu();
    EXPECT_EQ(dragged.in_force, 0) << "none ticked";
    EXPECT_EQ(dragged.larger, 100) << "the next size past the window's 87.5 %";
    EXPECT_EQ(dragged.smaller, 75);
    EXPECT_FLOAT_EQ(rig.root.zoom().zoom(), 1.0f) << "the layout's own sizing again";
}

TEST(WindowSizes, ALaunchOpensAtTheSizeTheAppKeptAndAtTheDesignSizeForAnyOther)
{
    WindowRig kept(75);
    EXPECT_EQ(kept.sizes.InForce(), 75);
    EXPECT_EQ(kept.WindowSize(), (PointI{750, 600}));
    EXPECT_FLOAT_EQ(kept.root.zoom().zoom(), 0.75f);

    WindowRig odd(80);
    EXPECT_EQ(odd.sizes.InForce(), 100) << "a size the menu does not offer opens at 100 %";
    EXPECT_EQ(odd.WindowSize(), kDesign);
    EXPECT_TRUE(WindowSizes::IsOffered(125));
    EXPECT_FALSE(WindowSizes::IsOffered(80));
}

TEST(WindowSizes, ASizeTheScreenDoesNotTakeIsDimAndRefusedAndAChosenOneStaysOnTheScreen)
{
    WindowRig rig;
    rig.fake().work_area = reckless::gfx::RectI{0, 0, 1200, 1000};
    rig.sizes.Frame(WindowSizes::kRefreshSeconds / 2.0);
    EXPECT_EQ(rig.sizes.Menu().fits, (std::array<bool, 5>{true, true, true, true, true})) << "not asked again yet";
    rig.sizes.Frame(WindowSizes::kRefreshSeconds / 2.0); // the window moved to a smaller screen
    const WindowSizeMenu menu = rig.sizes.Menu();
    EXPECT_EQ(menu.fits, (std::array<bool, 5>{true, true, true, false, false})) << "125 % is 1250 wide";
    EXPECT_EQ(menu.larger, 0) << "from 100 %, nothing larger fits";
    EXPECT_EQ(menu.smaller, 75);
    EXPECT_FALSE(rig.sizes.Choose(125));
    EXPECT_EQ(rig.WindowSize(), kDesign) << "refused, not shrunk";
    EXPECT_EQ(rig.sizes.InForce(), 100);

    ASSERT_TRUE(rig.sizes.Choose(50));
    ASSERT_TRUE(rig.sizes.Choose(100));
    EXPECT_EQ(rig.window->Position(), (std::pair<int, int>{40, 30})) << "its top left kept where the window fits";
    rig.fake().work_area = reckless::gfx::RectI{0, 0, 1020, 1000};
    ASSERT_TRUE(rig.sizes.Choose(50));
    ASSERT_TRUE(rig.sizes.Choose(100));
    EXPECT_EQ(rig.window->Position(), (std::pair<int, int>{20, 30})) << "moved in from the right edge it crossed";
    EXPECT_EQ(rig.fake().on_screen_resizes(), 4);
    EXPECT_TRUE(rig.sizes.Choose(100)) << "the size it is at already";
    EXPECT_EQ(rig.fake().on_screen_resizes(), 4) << "taken, with no resize";
}

TEST(WindowSizes, WithNoWindowEverySizeIsDimAndNoneIsTaken)
{
    reckless::view::View   content;
    reckless::app::AppRoot root(content, Tooltip(), [] { return 0.0; }, {900, 700});
    WindowSizes            sizes(root, kDesign);
    sizes.Restore(125);
    EXPECT_EQ(sizes.InForce(), 125) << "the root held at the size kept";
    const WindowSizeMenu menu = sizes.Menu();
    EXPECT_FALSE(menu.has_window);
    EXPECT_EQ(menu.fits, (std::array<bool, 5>{}));
    EXPECT_EQ(menu.larger, 0);
    EXPECT_EQ(menu.smaller, 0);
    EXPECT_FALSE(sizes.Choose(100));

    WindowRig rig;
    rig.sizes.SetWindow(nullptr); // the window closing
    EXPECT_FALSE(rig.sizes.Menu().has_window);
    EXPECT_FALSE(rig.sizes.Choose(75));
}
