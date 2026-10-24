// An app's shell (app/AppRun.h, AppRoot.h, AppTheme.h, AppWidgets.h, AppUpdates.h) on the suites' own platform
// (tests/FakeAppPlatform.h) with a child window of the suite's own: the command line read, the window opened round the
// root, titled and sized, its frames and its smoke run, its resizes followed; the root's zoom and its tooltip; the
// widgets as a person drives them; the theme's palette; the update check switched off by the environment.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AppRoot.h"
#include "AppRun.h"
#include "AppTheme.h"
#include "AppUpdates.h"
#include "AppWidgets.h"
#include "FakeAppPlatform.h"
#include "MenuBar.h"
#include "ScratchPath.h"

namespace
{
using reckless::app::AppOptions;
using reckless::test::FakeAppPlatform;

/** A theme of the suite's own, every colour different. */
reckless::app::Theme SuiteTheme()
{
    using reckless::gfx::Color;
    return {Color(0xff101010u), Color(0xff202020u), Color(0xff303030u), Color(0xff404040u),
            Color(0xffe0e0e0u), Color(0xffa0a0a0u), Color(0xff606060u), Color(0xffe0a040u),
            Color(0xff806020u), Color(0xff40c040u), Color(0xffc04040u)};
}

/** A child window of the suite's own: its frame callback kept for the suite to call, its scales counted. */
class SuiteChild final : public reckless::window::ChildWindow
{
  public:
    void SetScale(float scale) override
    {
        scales.push_back(scale);
    }

    void SetFrameCallback(std::function<void()> callback) override
    {
        frame = std::move(callback);
    }

    void HandOnKeysTo(void *top_level) override
    {
        handed_to.push_back(top_level);
    }

    std::function<void()> frame;     ///< the frame callback RunApp set
    std::vector<float>    scales;    ///< every scale set
    std::vector<void *>   handed_to; ///< every window the keys its views do not take were handed on to
};

/** A view of fixed content, with a tooltip. */
class Tipped final : public reckless::view::View
{
  public:
    Tipped()
    {
        tooltip = "A tip";
    }
};
} // namespace

TEST(AppOptions, SmokeTakesAnOptionalCountAndEverythingElseIsKeptInOrder)
{
    const AppOptions plain = reckless::app::ParseAppOptions({});
    EXPECT_FALSE(plain.is_smoke);
    EXPECT_EQ(plain.smoke_frames, 30);

    const AppOptions counted = reckless::app::ParseAppOptions({"--smoke", "12", "game.pgn"});
    EXPECT_TRUE(counted.is_smoke);
    EXPECT_EQ(counted.smoke_frames, 12);
    EXPECT_EQ(counted.unknown, std::vector<std::string>({"game.pgn"}));

    const AppOptions uncounted = reckless::app::ParseAppOptions({"--simulate", "--smoke", "-5", "x"});
    EXPECT_TRUE(uncounted.is_smoke);
    EXPECT_EQ(uncounted.smoke_frames, 30) << "a count under one is no count";
    EXPECT_EQ(uncounted.unknown, std::vector<std::string>({"--simulate", "-5", "x"}));

    EXPECT_EQ(reckless::app::ParseAppOptions({"--smoke", "7x"}).smoke_frames, 30);
    EXPECT_EQ(reckless::app::ParseAppOptions({"--smoke", ""}).smoke_frames, 30);
}

TEST(RunApp, ASmokeRunOpensTheWindowRoundTheRootDrawsItsFramesAndEnds)
{
    FakeAppPlatform      platform;
    reckless::view::View root;
    root.SetBounds({800, 600});
    SuiteChild                        *child  = nullptr;
    int                                frames = 0;
    std::optional<std::pair<int, int>> ended;
    std::string                        opened_title;

    reckless::app::AppRunHooks hooks;
    hooks.on_frame = [&](double seconds) {
        ++frames;
        EXPECT_GE(seconds, 0.0);
        EXPECT_LE(seconds, 0.1);
    };
    hooks.on_open = [&](reckless::app::AppWindow &window) {
        ASSERT_EQ(platform.windows().size(), 1u);
        opened_title = platform.windows().front()->options().title;
        EXPECT_EQ(&window, platform.windows().front());
    };
    hooks.on_end = [&](std::optional<std::pair<int, int>> position) { ended = position; };

    const auto make_child = [&](void *parent, reckless::view::View &shown, int width, int height, float scale,
                                std::unique_ptr<reckless::window::RunLoop>) {
        EXPECT_EQ(parent, platform.windows().front()->Handle());
        EXPECT_EQ(&shown, &root);
        EXPECT_EQ(width, 800);
        EXPECT_EQ(height, 600);
        EXPECT_EQ(scale, 1.0f);
        auto made = std::make_unique<SuiteChild>();
        child     = made.get();
        // The display's frames, once the loop runs: more than the smoke run asks for, which stops at its count.
        platform.CallSoon([&] {
            for (int i = 0; i < 40 && child->frame; ++i)
            {
                child->frame();
            }
        });
        return made;
    };

    AppOptions options   = reckless::app::ParseAppOptions({"--smoke", "25"});
    const int  exit_code = reckless::app::RunApp(platform, root, {400, 300}, options, "Suite App", "1.2.3", make_child,
                                                 hooks, std::pair<int, int>{30, 40});
    EXPECT_EQ(exit_code, 0);
    EXPECT_EQ(opened_title, "Suite App  v1.2.3");
    EXPECT_GE(frames, 25);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(*ended, (std::pair<int, int>{30, 40})) << "where the window stood";
    EXPECT_TRUE(platform.windows().empty()) << "the window closes as RunApp returns";
}

TEST(RunApp, AnAppWithAMenuBarHasItsWindowCarryItAndItsChildHandOnTheKeys)
{
    for (const bool has_menu_bar : {false, true})
    {
        FakeAppPlatform      platform;
        reckless::view::View root;
        root.SetBounds({300, 200});
        SuiteChild                *child       = nullptr;
        bool                       carries_bar = false;
        void                      *handle      = nullptr;
        std::vector<void *>        handed_to;
        reckless::app::AppRunHooks hooks;
        hooks.has_menu_bar = has_menu_bar;
        hooks.on_open      = [&](reckless::app::AppWindow &window) {
            carries_bar = platform.windows().front()->options().menu_bar;
            handle      = window.Handle();
        };
        hooks.on_end          = [&](std::optional<std::pair<int, int>>) { handed_to = child->handed_to; };
        const auto make_child = [&](void *, reckless::view::View &, int, int, float,
                                    std::unique_ptr<reckless::window::RunLoop>) {
            auto made = std::make_unique<SuiteChild>();
            child     = made.get();
            platform.CallSoon([&] {
                for (int i = 0; i < 5 && child->frame; ++i)
                {
                    child->frame();
                }
            });
            return made;
        };
        EXPECT_EQ(reckless::app::RunApp(platform, root, {100, 50}, reckless::app::ParseAppOptions({"--smoke", "3"}),
                                        "Suite App", "1.0.0", make_child, hooks),
                  0);
        EXPECT_EQ(carries_bar, has_menu_bar);
        EXPECT_EQ(handed_to, has_menu_bar ? std::vector<void *>{handle} : std::vector<void *>{})
            << "the keys no view takes handed on to the window only where it has a bar's shortcuts";
    }
}

TEST(RunApp, ASmokeRunThatDrewTooFewFramesFailsAndNoWindowIsOne)
{
    FakeAppPlatform      platform;
    reckless::view::View root;
    root.SetBounds({200, 100});
    const auto make_child = [&](void *, reckless::view::View &, int, int, float,
                                std::unique_ptr<reckless::window::RunLoop>) {
        auto  made     = std::make_unique<SuiteChild>();
        auto *frame_of = made.get();
        platform.CallSoon([&platform, frame_of] {
            frame_of->frame();
            platform.Quit(); // the person closes it after one frame
        });
        return made;
    };
    EXPECT_EQ(reckless::app::RunApp(platform, root, {100, 50}, reckless::app::ParseAppOptions({"--smoke", "5"}),
                                    "Suite App", "0.0.0", make_child),
              3);

    platform.refuse_windows = true;
    EXPECT_EQ(reckless::app::RunApp(platform, root, {100, 50}, {}, "Suite App", "0.0.0", make_child), 1);

    FakeAppPlatform no_child_platform;
    EXPECT_EQ(reckless::app::RunApp(no_child_platform, root, {100, 50}, {}, "Suite App", "0.0.0",
                                    [](void *, reckless::view::View &, int, int, float,
                                       std::unique_ptr<reckless::window::RunLoop>) { return nullptr; }),
              1)
        << "a platform that opens a window Skia cannot draw in";
}

TEST(RunApp, TheRootFollowsTheWindowAsThePersonResizesItAndClosingItEndsTheLoop)
{
    FakeAppPlatform      platform;
    reckless::view::View root;
    root.SetBounds({640, 480});
    SuiteChild *child      = nullptr;
    const auto  make_child = [&](void *, reckless::view::View &, int, int, float,
                                 std::unique_ptr<reckless::window::RunLoop>) {
        auto made = std::make_unique<SuiteChild>();
        child     = made.get();
        platform.CallSoon([&] {
            const auto &options = platform.windows().front()->options();
            EXPECT_EQ(options.constrain(reckless::gfx::PointF{10.0f, 1000.0f}).x, 320.0f) << "never under the least";
            EXPECT_EQ(options.constrain(reckless::gfx::PointF{10.0f, 1000.0f}).y, 1000.0f);
            platform.windows().front()->Resize(900, 700);
            EXPECT_EQ(root.width(), 900);
            EXPECT_EQ(root.height(), 700);
            options.rescaled(1.5f);
            platform.windows().front()->Close();
        });
        return made;
    };
    EXPECT_EQ(reckless::app::RunApp(platform, root, {320, 240}, {}, "Suite App", "0.0.0", make_child), 0);
    EXPECT_EQ(root.width(), 900);
}

TEST(AppRoot, TheContentIsLaidOutAtTheWindowsSizeAndZoomedDownBelowItsLeast)
{
    reckless::view::View content;
    content.SetBounds({1000, 800});
    reckless::app::AppRoot root(content, SuiteTheme().Tooltip(), [] { return 0.0; }, {1000, 800});
    EXPECT_EQ(root.width(), 1000);

    root.SetBounds({1200, 900});
    EXPECT_EQ(content.width(), 1200) << "larger than its least: laid out at the window's size";
    EXPECT_EQ(content.height(), 900);
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 1.0f);

    root.SetBounds({500, 600});
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 0.5f) << "half the least's width: zoomed to a half";
    EXPECT_EQ(content.width(), 1000);
    EXPECT_EQ(content.height(), 1200);

    reckless::view::View   free_content;
    reckless::app::AppRoot free_root(free_content, SuiteTheme().Tooltip());
    free_root.SetBounds({300, 200});
    EXPECT_EQ(free_content.width(), 300) << "with no least, always the window's size";
    EXPECT_FLOAT_EQ(free_root.zoom().zoom(), 1.0f);
}

TEST(AppRoot, AFixedZoomHoldsTheContentAtItsDesignSizeUntilTheRootIsSizedToAnythingElse)
{
    reckless::view::View content;
    content.SetBounds({1000, 800});
    reckless::app::AppRoot root(content, SuiteTheme().Tooltip(), [] { return 0.0; }, {900, 700});
    root.SetBounds({1000, 800});
    EXPECT_FALSE(root.fixed_zoom().has_value()) << "none until the app sets one";

    const reckless::gfx::PointI size = root.SetFixedZoom({1000, 800}, 1.25f);
    EXPECT_EQ(size, (reckless::gfx::PointI{1250, 1000})) << "the design times the zoom";
    EXPECT_FALSE(root.fixed_zoom().has_value()) << "the root is not that size yet: the window sizes it";
    EXPECT_EQ(root.width(), 1000) << "the root's own size is not set";
    root.SetBounds({1250, 1000});
    ASSERT_TRUE(root.fixed_zoom().has_value());
    EXPECT_FLOAT_EQ(*root.fixed_zoom(), 1.25f);
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 1.25f) << "drawn at exactly the zoom";
    EXPECT_EQ(content.width(), 1000) << "laid out at its design size, not the window's";
    EXPECT_EQ(content.height(), 800);

    EXPECT_EQ(root.SetFixedZoom({1000, 800}, 0.5f), (reckless::gfx::PointI{500, 400}));
    root.SetBounds({500, 400});
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 0.5f) << "below the layout's least too: the design size, halved";
    EXPECT_EQ(content.width(), 1000);
    EXPECT_EQ(content.height(), 800);

    root.SetBounds({1200, 900}); // the person drags the frame
    EXPECT_FALSE(root.fixed_zoom().has_value()) << "another size lets the fixed zoom go";
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 1.0f);
    EXPECT_EQ(content.width(), 1200) << "the layout's own again: at the window's size";
    root.SetBounds({500, 400});
    EXPECT_FALSE(root.fixed_zoom().has_value()) << "and coming back to that size does not bring it back";
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 500.0f / 900.0f) << "the least zoomed down to fit";

    root.SetBounds({1000, 800});
    EXPECT_EQ(root.SetFixedZoom({1000, 800}, 1.0f), (reckless::gfx::PointI{1000, 800}));
    ASSERT_TRUE(root.fixed_zoom().has_value()) << "the root that size already: held at once, no resize to wait for";
    EXPECT_FLOAT_EQ(root.zoom().zoom(), 1.0f);
    EXPECT_EQ(root.SetFixedZoom({1380, 1120}, 0.75f), (reckless::gfx::PointI{1035, 840}));
}

TEST(RunApp, AnAppsOwnSizeTakesTheWindowOntoItsScreenAndTheRootAndTheChildFollow)
{
    FakeAppPlatform      platform;
    reckless::view::View content;
    content.SetBounds({800, 600});
    reckless::app::AppRoot     root(content, SuiteTheme().Tooltip(), [] { return 0.0; }, {800, 600});
    SuiteChild                *child  = nullptr;
    reckless::app::AppWindow  *window = nullptr;
    reckless::app::AppRunHooks hooks;
    hooks.on_open         = [&](reckless::app::AppWindow &opened) { window = &opened; };
    const auto make_child = [&](void *, reckless::view::View &, int, int, float,
                                std::unique_ptr<reckless::window::RunLoop>) {
        auto made = std::make_unique<SuiteChild>();
        child     = made.get();
        platform.CallSoon([&] {
            reckless::test::FakeAppWindow &fake = *platform.windows().front();
            fake.work_area                      = reckless::gfx::RectI{0, 0, 1600, 1000};
            EXPECT_EQ(window->LargestContent(), (reckless::gfx::PointI{1600, 1000}));
            const reckless::gfx::PointI size = root.SetFixedZoom({800, 600}, 1.5f);
            window->SetContentSizeOnScreen(size.x, size.y);
            EXPECT_EQ(root.width(), 1200) << "the root follows the window's size";
            EXPECT_EQ(root.height(), 900);
            ASSERT_TRUE(root.fixed_zoom().has_value());
            EXPECT_FLOAT_EQ(*root.fixed_zoom(), 1.5f);
            EXPECT_EQ(child->scales.size(), 1u) << "and the child window with it";
            EXPECT_EQ(window->Position(), (std::pair<int, int>{400, 100})) << "moved in from the edges it crossed";
            fake.Resize(1000, 700); // the person drags the frame
            EXPECT_FALSE(root.fixed_zoom().has_value());
            fake.Close();
        });
        return made;
    };
    EXPECT_EQ(reckless::app::RunApp(platform, root, {320, 240}, {}, "Suite App", "0.0.0", make_child, hooks,
                                    std::pair<int, int>{600, 300}),
              0);
}

TEST(AppRoot, ATipShowsWhenThePointerRestsOnAViewAndGoesWithAPress)
{
    reckless::view::View content;
    content.SetBounds({400, 300});
    Tipped tipped;
    tipped.SetBounds({100, 100, 80, 40});
    content.AddChild(tipped);
    double                      now = 10.0;
    reckless::app::AppRoot      root(content, SuiteTheme().Tooltip(), [&now] { return now; });
    reckless::view::MouseRouter router(root);

    router.Move({120.0f, 110.0f}, {});
    for (int i = 0; i < 10 && !root.tooltip().visible(); ++i)
    {
        now += 0.2;
        root.Frame();
    }
    EXPECT_TRUE(root.tooltip().visible()) << "after the delay";
    router.Down({120.0f, 110.0f}, 1, false, {});
    router.Up({120.0f, 110.0f}, {});
    now += 0.2;
    root.Frame();
    EXPECT_FALSE(root.tooltip().visible()) << "a press hides it";
}

TEST(AppWidgets, TheSliderFollowsThePointerAndTheWheelAndTellsEachNewValue)
{
    reckless::app::Slider slider(SuiteTheme(), 0, 10, 4);
    slider.SetBounds({0, 0, 116, 20});
    std::vector<int> told;
    slider.on_change = [&](int value) { told.push_back(value); };

    reckless::view::MouseEvent press;
    press.position = {116.0f, 10.0f};
    slider.MouseDown(press);
    EXPECT_EQ(slider.value(), 10);
    press.position = {8.0f, 10.0f};
    slider.MouseDrag(press);
    EXPECT_EQ(slider.value(), 0);
    reckless::view::MouseEvent wheel;
    wheel.wheel_y = 3.0f;
    EXPECT_TRUE(slider.MouseWheel(wheel));
    EXPECT_EQ(slider.value(), 1);
    slider.SetValue(99);
    EXPECT_EQ(slider.value(), 10) << "held to the range, and not told";
    EXPECT_EQ(told, (std::vector<int>{10, 0, 1}));
}

TEST(AppWidgets, TextSectionsPlatesAndButtonsKeepWhatTheyAreGiven)
{
    const reckless::app::Theme theme = SuiteTheme();
    reckless::app::TextLine    line(theme.ink_dim);
    line.SetText("Ready");
    EXPECT_EQ(line.text(), "Ready");
    line.SetColour(theme.bad);
    EXPECT_EQ(line.colour().argb(), theme.bad.argb());
    EXPECT_FALSE(line.intercepts_self()) << "a line of text takes no mouse";

    reckless::app::Section section(theme, "Moves");
    section.SetBounds({0, 0, 200, 100});
    EXPECT_EQ(section.title(), "Moves");
    EXPECT_TRUE(section.LocalBounds().Contains(section.ContentArea()));
    EXPECT_GT(section.ContentArea().y(), reckless::app::Section::kPadding);

    reckless::app::ChoicePlate plate(theme, "Side");
    std::vector<int>           chosen;
    plate.on_choose = [&](int index) { chosen.push_back(index); };
    plate.SetChoices({"White", "Black"}, 0);
    EXPECT_EQ(plate.current(), 0);
    EXPECT_EQ(plate.plate().legend().text(), "White");
    plate.Choose(1);
    EXPECT_EQ(plate.current(), 1);
    plate.SetCurrent(0);
    EXPECT_EQ(chosen, std::vector<int>{1}) << "SetCurrent tells nobody";
    EXPECT_EQ(plate.caption().text(), "Side");

    int  clicks = 0;
    auto button = reckless::app::MakeButton(theme, "Flip", [&] { ++clicks; }, true);
    EXPECT_EQ(button->text(), "Flip");
    EXPECT_FALSE(button->on());
}

TEST(AppTheme, ThePaletteIsTheThemesColoursWithTheAccentAtHalfRoundThePlates)
{
    const reckless::app::Theme    theme   = SuiteTheme();
    const reckless::look::Palette palette = theme.Palette();
    EXPECT_EQ(palette.panel.argb(), theme.panel.argb());
    EXPECT_EQ(palette.ink.argb(), theme.ink.argb());
    EXPECT_EQ(palette.accent.argb(), theme.accent.argb());
    EXPECT_EQ(palette.plate.argb(), theme.plate.argb());
    EXPECT_EQ(palette.plate_edge.argb(), theme.accent.WithAlpha(0.5f).argb());
    EXPECT_EQ(theme.Tooltip().text.argb(), theme.ink.argb());
}

TEST(AppUpdates, TheEnvironmentSwitchesTheCheckOffAndTheFileIsTheAppsOwn)
{
#if defined(_WIN32)
    _putenv_s("RECKLESS_UPDATE_CHECK", "off");
#else
    setenv("RECKLESS_UPDATE_CHECK", "off", 1);
#endif
    EXPECT_EQ(reckless::app::EnvironmentVariable("RECKLESS_UPDATE_CHECK"), std::optional<std::string>("off"));
    EXPECT_EQ(
        reckless::app::MakeUpdateChecker({"X", "1.2.3", "https://example.invalid/updates/x.json", "x-updates.json"}),
        nullptr);
    EXPECT_FALSE(reckless::app::EnvironmentVariable("RECKLESS_NO_SUCH_VARIABLE_AT_ALL").has_value());
    const std::filesystem::path file = reckless::update::StateFile("x-updates.json");
    if (!file.empty())
    {
        EXPECT_EQ(file.filename(), "x-updates.json");
        EXPECT_EQ(file.parent_path().filename(), "reckless-technology");
    }
}

TEST(AppUpdates, TheUpdatesMenuOfAMenuBarIsTheToolkitsRowsAndItsItemsAct)
{
    // A checker that never reaches a network: no transport, a clock of the test's own, a file of its own.
    const std::filesystem::path folder = reckless::test::ScratchPath("reckless-app-updates-menu-bar");
    std::filesystem::remove_all(folder);
    double                    now = 1.0e9;
    reckless::update::Checker checker(
        folder / "updates.json", reckless::update::Version{1, 2, 3}, "https://recklesstechnology.com/updates/x.json",
        [] { return std::unique_ptr<reckless::net::Transport>(); }, [&now] { return now; });
    std::vector<std::string>                     opened;
    std::vector<reckless::update::UpdatesChoice> heard;
    const reckless::app::MenuEntry               menu = reckless::app::UpdatesMenuEntry(
        checker, [&opened](const std::string &url) { opened.push_back(url); },
        [&heard](reckless::update::UpdatesChoice choice) { heard.push_back(choice); });
    EXPECT_EQ(menu.title, "Updates");
    ASSERT_EQ(menu.submenu.size(), 4u);
    EXPECT_EQ(menu.submenu[0].title, "Version 1.2.3, not checked yet");
    EXPECT_FALSE(menu.submenu[0].enabled) << "the status is a line to read";
    EXPECT_FALSE(menu.submenu[0].action) << "and chooses nothing";
    EXPECT_TRUE(menu.submenu[1].separator);
    EXPECT_EQ(menu.submenu[2].title, "Check for updates now");
    EXPECT_EQ(menu.submenu[3].title, "Check automatically");
    EXPECT_TRUE(menu.submenu[3].checked);
    for (const reckless::app::MenuEntry &entry : menu.submenu)
    {
        EXPECT_EQ(entry.id, 0) << "the items act through their actions, so no id of the app's is taken";
    }

    // Chosen through the bar as a click or a shortcut chooses it: the action turns checks by the clock off.
    reckless::app::MenuBarController bar;
    reckless::app::MenuBar           shown;
    shown.menus.push_back(menu);
    bar.Set(shown, [](int) {});
    ASSERT_TRUE(bar.Choose(bar.CommandOf(bar.bar().menus[0].submenu[3])));
    EXPECT_FALSE(checker.automatic());
    EXPECT_EQ(heard,
              (std::vector<reckless::update::UpdatesChoice>{reckless::update::UpdatesChoice::kAutomaticChanged}));
    EXPECT_FALSE(reckless::app::UpdatesMenuEntry(checker, {}).submenu[3].checked) << "made again, it says so";
    EXPECT_TRUE(opened.empty());
    std::filesystem::remove_all(folder);
}
