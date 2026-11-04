// A window's full screen (app/AppPlatform.h's AppWindow::SetFullScreen) as an app holds it, on the suites' own platform
// (tests/FakeAppPlatform.h): only a window opened with it allowed goes; full screen is the display's size, told as a
// resize and then as the change, and the window comes back to its own size and place; RunApp passes the app's choice
// to the window, tells the app each change, and opens full screen where the app kept it, though never in a smoke run;
// and the menu's item (app/AppFullScreen.h) as each platform writes it - its text, its keys, its tick - choosing the
// toggle, with a Mac's Control a modifier of its own that only a shortcut written with it matches.
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AppFullScreen.h"
#include "AppRun.h"
#include "FakeAppPlatform.h"
#include "MenuBar.h"

namespace
{
using reckless::app::FullScreenMenuEntry;
using reckless::app::MenuPlatform;
using reckless::app::Shortcut;
using reckless::app::ShortcutKey;
using reckless::test::FakeAppPlatform;
using reckless::test::FakeAppWindow;

/** A child window of the suite's own: its scales counted, its frame callback kept. */
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

    std::function<void()> frame;  ///< the frame callback RunApp set
    std::vector<float>    scales; ///< every scale set
};

/** What a window that may go full screen is opened with: 800 x 600 at (120, 80), its resizes and its changes of full
    screen heard.
    @param resizes  every resize told, in order
    @param changes  every change of full screen told, in order
    @return         the options */
reckless::app::WindowOptions FullScreenOptions(std::vector<std::pair<int, int>> &resizes, std::vector<bool> &changes)
{
    reckless::app::WindowOptions options;
    options.width               = 800;
    options.height              = 600;
    options.position            = std::pair<int, int>{120, 80};
    options.may_go_full_screen  = true;
    options.resized             = [&resizes](int width, int height) { resizes.emplace_back(width, height); };
    options.full_screen_changed = [&changes, &resizes](bool is_full_screen) {
        EXPECT_FALSE(resizes.empty()) << "the change told after the resize it brings";
        changes.push_back(is_full_screen);
    };
    return options;
}
} // namespace

TEST(FullScreen, OnlyAWindowOpenedWithItAllowedGoesAndTheDefaultHasNone)
{
    FakeAppPlatform                                 platform;
    reckless::app::WindowOptions                    options; // every window before this option, a dialog among them
    const std::unique_ptr<reckless::app::AppWindow> window = platform.OpenWindow(options);
    EXPECT_FALSE(window->CanGoFullScreen());
    EXPECT_FALSE(window->EnterFullScreen()) << "nothing done for a window that did not ask";
    EXPECT_FALSE(window->IsFullScreen());
    EXPECT_FALSE(window->ToggleFullScreen());
    options.may_go_full_screen = true;
    options.dialog             = true;
    EXPECT_FALSE(platform.OpenWindow(options)->CanGoFullScreen()) << "a dialog never goes, whatever it asks";

    // The seam's defaults, for a platform of a product's own that has no full screen.
    class Plain final : public reckless::app::AppWindow
    {
      public:
        void *Handle() const override
        {
            return nullptr;
        }

        float Scale() const override
        {
            return 1.0f;
        }

        std::pair<int, int> Position() const override
        {
            return {0, 0};
        }

        void BringToFront() override
        {
        }

        void SetContentSize(int, int) override
        {
        }
    } plain;

    EXPECT_FALSE(plain.CanGoFullScreen());
    EXPECT_FALSE(plain.SetFullScreen(true));
    EXPECT_FALSE(plain.IsFullScreen());
}

TEST(FullScreen, AWindowGoesToTheDisplaysSizeAndComesBackToItsOwnSizeAndPlace)
{
    FakeAppPlatform                  platform;
    std::vector<std::pair<int, int>> resizes;
    std::vector<bool>                changes;
    const auto                       window = platform.OpenWindow(FullScreenOptions(resizes, changes));
    FakeAppWindow                   &fake   = *platform.windows().front();
    fake.display                            = reckless::gfx::RectI{0, 0, 2560, 1440};
    ASSERT_TRUE(window->CanGoFullScreen());

    ASSERT_TRUE(window->EnterFullScreen());
    EXPECT_TRUE(window->IsFullScreen());
    EXPECT_EQ(resizes, (std::vector<std::pair<int, int>>{{2560, 1440}})) << "the whole display, told as a resize";
    EXPECT_EQ(changes, (std::vector<bool>{true}));
    EXPECT_EQ(window->Position(), (std::pair<int, int>{120, 80})) << "where it stood, to come back to";
    EXPECT_FALSE(window->EnterFullScreen()) << "full screen already: nothing done";

    ASSERT_TRUE(window->ToggleFullScreen());
    EXPECT_FALSE(window->IsFullScreen());
    EXPECT_EQ(resizes.back(), (std::pair<int, int>{800, 600})) << "its own size again";
    EXPECT_EQ(changes, (std::vector<bool>{true, false}));
    EXPECT_FALSE(window->LeaveFullScreen()) << "out of full screen already";
    EXPECT_EQ(fake.full_screen_changes(), 2);

    // A size of the app's own, asked for while full screen, leaves full screen first.
    ASSERT_TRUE(window->EnterFullScreen());
    window->SetContentSizeOnScreen(1000, 700);
    EXPECT_FALSE(window->IsFullScreen());
    EXPECT_EQ(changes.back(), false);
    EXPECT_EQ(resizes.back(), (std::pair<int, int>{1000, 700}));
}

TEST(FullScreen, AChangeTheSuiteHoldsIsAskedAtOnceAndToldWhenItIsFinished)
{
    FakeAppPlatform                  platform;
    std::vector<std::pair<int, int>> resizes;
    std::vector<bool>                changes;
    const auto                       window = platform.OpenWindow(FullScreenOptions(resizes, changes));
    FakeAppWindow                   &fake   = *platform.windows().front();
    fake.defers_full_screen                 = true; // as a Mac's animation takes its time

    ASSERT_TRUE(window->EnterFullScreen());
    EXPECT_TRUE(window->IsFullScreen()) << "the state asked for, while the change is under way";
    EXPECT_TRUE(resizes.empty());
    EXPECT_TRUE(changes.empty()) << "told when it is made, not when it is asked";
    ASSERT_TRUE(fake.FinishFullScreen());
    EXPECT_EQ(resizes, (std::vector<std::pair<int, int>>{{1920, 1080}})) << "no display or work area: 1920 x 1080";
    EXPECT_EQ(changes, (std::vector<bool>{true}));
    EXPECT_FALSE(fake.FinishFullScreen()) << "nothing waiting";
}

TEST(FullScreen, RunAppHandsTheWindowTheAppsChoiceAndTellsItEachChangeAtTheNewSize)
{
    FakeAppPlatform      platform;
    reckless::view::View root;
    root.SetBounds({800, 600});
    SuiteChild                *child  = nullptr;
    reckless::app::AppWindow  *window = nullptr;
    std::vector<bool>          heard;
    reckless::app::AppRunHooks hooks;
    hooks.may_go_full_screen = true;
    hooks.on_full_screen     = [&](bool is_full_screen) {
        heard.push_back(is_full_screen);
        EXPECT_EQ(root.width(), is_full_screen ? 1600 : 800) << "the root at the new size by the time it is told";
    };
    hooks.on_open         = [&](reckless::app::AppWindow &opened) { window = &opened; };
    const auto make_child = [&](void *, reckless::view::View &, int, int, float,
                                std::unique_ptr<reckless::window::RunLoop>) {
        auto made = std::make_unique<SuiteChild>();
        child     = made.get();
        platform.CallSoon([&] {
            FakeAppWindow &fake = *platform.windows().front();
            EXPECT_TRUE(fake.options().may_go_full_screen);
            fake.display = reckless::gfx::RectI{0, 0, 1600, 900};
            ASSERT_TRUE(window->EnterFullScreen());
            EXPECT_EQ(root.width(), 1600) << "the root follows the window to the display's size";
            EXPECT_EQ(root.height(), 900);
            EXPECT_EQ(child->scales.size(), 1u) << "and the child window with it, at the window's scale";
            ASSERT_TRUE(window->LeaveFullScreen());
            EXPECT_EQ(root.width(), 800);
            EXPECT_EQ(root.height(), 600);
            fake.Close();
        });
        return made;
    };
    EXPECT_EQ(reckless::app::RunApp(platform, root, {320, 240}, {}, "Suite App", "0.0.0", make_child, hooks), 0);
    EXPECT_EQ(heard, (std::vector<bool>{true, false}));

    // An app that does not ask has a window that never goes.
    FakeAppPlatform      other;
    reckless::view::View other_root;
    other_root.SetBounds({400, 300});
    const auto plain_child = [&](void *, reckless::view::View &, int, int, float,
                                 std::unique_ptr<reckless::window::RunLoop>) {
        other.CallSoon([&] {
            EXPECT_FALSE(other.windows().front()->CanGoFullScreen());
            other.windows().front()->Close();
        });
        return std::make_unique<SuiteChild>();
    };
    EXPECT_EQ(reckless::app::RunApp(other, other_root, {320, 240}, {}, "Suite App", "0.0.0", plain_child), 0);
}

TEST(FullScreen, AWindowKeptFullScreenOpensFullScreenOnceTheLoopRunsButNotInASmokeRun)
{
    for (const bool is_smoke : {false, true})
    {
        FakeAppPlatform      platform;
        reckless::view::View root;
        root.SetBounds({800, 600});
        SuiteChild                *child = nullptr;
        std::optional<bool>        opened_full_screen;
        reckless::app::AppRunHooks hooks;
        hooks.may_go_full_screen = true;
        hooks.opens_full_screen  = true;
        hooks.on_open            = [&](reckless::app::AppWindow &opened) {
            opened_full_screen = opened.IsFullScreen(); // not yet: once the loop runs
        };
        reckless::app::AppOptions options;
        options.is_smoke      = is_smoke;
        options.smoke_frames  = 1;
        const auto make_child = [&](void *, reckless::view::View &, int, int, float,
                                    std::unique_ptr<reckless::window::RunLoop>) {
            auto made = std::make_unique<SuiteChild>();
            child     = made.get();
            platform.CallSoon([&] {
                // After the call RunApp queued: the window is full screen now, unless this is a smoke run.
                platform.CallSoon([&] {
                    EXPECT_EQ(platform.windows().front()->IsFullScreen(), !is_smoke) << is_smoke;
                    EXPECT_EQ(root.width(), is_smoke ? 800 : 1920);
                    if (is_smoke)
                    {
                        child->frame(); // the one frame a smoke run of one draws, which ends it
                    }
                    else
                    {
                        platform.windows().front()->Close();
                    }
                });
            });
            return made;
        };
        EXPECT_EQ(reckless::app::RunApp(platform, root, {320, 240}, options, "Suite App", "0.0.0", make_child, hooks),
                  0);
        ASSERT_TRUE(opened_full_screen.has_value());
        EXPECT_FALSE(*opened_full_screen) << "the window opens at its own size and place, and goes full screen after";
    }
}

TEST(FullScreenMenu, EachPlatformHasItsOwnKeyTextAndTick)
{
    using reckless::app::FullScreenShortcut;
    using reckless::app::FullScreenTitle;
    using reckless::app::ShortcutText;
    EXPECT_EQ(ShortcutText(FullScreenShortcut(MenuPlatform::kWindows), false), "F11");
    EXPECT_EQ(ShortcutText(FullScreenShortcut(MenuPlatform::kLinux), false), "F11");
    EXPECT_EQ(ShortcutText(FullScreenShortcut(MenuPlatform::kMac), true), "\xe2\x8c\x83\xe2\x8c\x98"
                                                                          "F")
        << "Control, Command, F: AppKit's own for Enter Full Screen";
    ASSERT_EQ(reckless::app::FullScreenOtherShortcuts(MenuPlatform::kWindows).size(), 1u);
    EXPECT_TRUE(reckless::app::FullScreenOtherShortcuts(MenuPlatform::kWindows)
                    .front()
                    .Matches(Shortcut::Plain(ShortcutKey::kReturn, false, true)))
        << "Alt+Enter on Windows, as games have it";
    EXPECT_TRUE(reckless::app::FullScreenOtherShortcuts(MenuPlatform::kMac).empty());
    EXPECT_TRUE(reckless::app::FullScreenOtherShortcuts(MenuPlatform::kLinux).empty());
    EXPECT_EQ(FullScreenTitle(false, MenuPlatform::kMac), "Enter Full Screen");
    EXPECT_EQ(FullScreenTitle(true, MenuPlatform::kMac), "Exit Full Screen");
    EXPECT_EQ(FullScreenTitle(false, MenuPlatform::kWindows), "Full screen");
    EXPECT_EQ(FullScreenTitle(true, MenuPlatform::kLinux), "Full screen");
#if defined(__APPLE__)
    EXPECT_EQ(reckless::app::kThisMenuPlatform, MenuPlatform::kMac);
#elif defined(_WIN32)
    EXPECT_EQ(reckless::app::kThisMenuPlatform, MenuPlatform::kWindows);
#else
    EXPECT_EQ(reckless::app::kThisMenuPlatform, MenuPlatform::kLinux);
#endif
}

TEST(FullScreenMenu, TheItemTogglesTheWindowIsTickedWhileFullScreenAndDimWhereItMayNotGo)
{
    FakeAppPlatform                  platform;
    std::vector<std::pair<int, int>> resizes;
    std::vector<bool>                changes;
    const auto                       window = platform.OpenWindow(FullScreenOptions(resizes, changes));

    for (const MenuPlatform menu_platform : {MenuPlatform::kWindows, MenuPlatform::kMac, MenuPlatform::kLinux})
    {
        std::vector<int>       chosen;
        reckless::app::MenuBar bar;
        bar.menus.push_back(
            reckless::app::MenuEntry::Menu("View", {FullScreenMenuEntry(window.get(), 77, menu_platform)}));
        platform.SetMenuBar(bar, [&chosen](int id) { chosen.push_back(id); });
        const reckless::app::MenuEntry &item = platform.menu_bar().bar().menus[0].submenu[0];
        EXPECT_TRUE(item.enabled);
        EXPECT_FALSE(item.checked);

        // Its key, from anywhere in the app, as the platform hands the key on.
        const Shortcut key = reckless::app::FullScreenShortcut(menu_platform);
        EXPECT_EQ(platform.PressKey(key), FakeAppPlatform::Took::kShortcut);
        EXPECT_TRUE(window->IsFullScreen());
        EXPECT_EQ(chosen, (std::vector<int>{77})) << "and its id told, as any item's";

        // Set again, as the app sets the bar on hearing the change: ticked, or a Mac's other words.
        bar.menus[0].submenu[0] = FullScreenMenuEntry(window.get(), 77, menu_platform);
        EXPECT_EQ(bar.menus[0].submenu[0].checked, menu_platform != MenuPlatform::kMac);
        EXPECT_EQ(bar.menus[0].submenu[0].title,
                  menu_platform == MenuPlatform::kMac ? "Exit Full Screen" : "Full screen");
        platform.SetMenuBar(bar, [&chosen](int id) { chosen.push_back(id); });
        ASSERT_TRUE(platform.ChooseCommand(platform.menu_bar().CommandOfId(77))) << "its click";
        EXPECT_FALSE(window->IsFullScreen()) << "and back";
    }
    EXPECT_EQ(changes, (std::vector<bool>{true, false, true, false, true, false}));

    // Alt+Enter, Windows' other key, does the same.
    reckless::app::MenuBar bar;
    bar.menus.push_back(
        reckless::app::MenuEntry::Menu("View", {FullScreenMenuEntry(window.get(), 0, MenuPlatform::kWindows)}));
    platform.SetMenuBar(bar, {});
    EXPECT_EQ(platform.PressKey(Shortcut::Plain(ShortcutKey::kReturn, false, true)), FakeAppPlatform::Took::kShortcut);
    EXPECT_TRUE(window->IsFullScreen());
    EXPECT_EQ(platform.PressKey(Shortcut::Plain(ShortcutKey::kF11)), FakeAppPlatform::Took::kShortcut);
    EXPECT_FALSE(window->IsFullScreen());

    // Dim, and choosing nothing, for a window that may not go and for none at all.
    const auto plain = platform.OpenWindow(reckless::app::WindowOptions{});
    EXPECT_FALSE(FullScreenMenuEntry(plain.get()).enabled);
    EXPECT_FALSE(FullScreenMenuEntry(nullptr).enabled);
    FullScreenMenuEntry(nullptr).action(); // does nothing, and does not fail
}

TEST(MenuBar, AMacsControlIsAModifierOfItsOwnThatOnlyAShortcutWrittenWithItMatches)
{
    Shortcut full_screen = Shortcut::Primary('f');
    full_screen.control  = true;
    Shortcut pressed     = Shortcut::Primary('F');
    EXPECT_FALSE(full_screen.Matches(pressed)) << "Command-F alone is Find's, not full screen's";
    pressed.control = true;
    EXPECT_TRUE(full_screen.Matches(pressed));
    EXPECT_FALSE(Shortcut::Primary('f').Matches(pressed)) << "a shortcut without Control is not chosen with it held";
    EXPECT_EQ(reckless::app::ShortcutText(full_screen, false), "Ctrl+F") << "written as Ctrl where Ctrl is primary";
    Shortcut control_alone = Shortcut::Plain('k');
    control_alone.control  = true;
    EXPECT_EQ(reckless::app::ShortcutText(control_alone, true), "\xe2\x8c\x83K");
    EXPECT_EQ(reckless::app::ShortcutText(control_alone, false), "Ctrl+K");
}
