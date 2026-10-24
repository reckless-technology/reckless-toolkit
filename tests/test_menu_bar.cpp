// The menu bar written once for every platform (app/MenuBar.h): a shortcut and how a key matches it, the text each
// platform writes for it, the bar's entries numbered as commands and chosen by a click or a shortcut - an entry's
// action then the bar's `chosen` - neither for a disabled entry or one under a disabled menu, a bar set anew from
// inside an action; the drawn bar (app/MenuBarView.h) Linux shows, opening its menus in the views' popups and choosing
// through the same controller; and keys on the suites' own platform as every platform now hands them on - the view with
// the keyboard first, Ctrl or Command with a letter reaching it as the letter with the primary modifier, so a text
// view's Ctrl+C copies and its Ctrl+V pastes, and only a key no view takes reaching the bar's shortcuts.
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FakeAppPlatform.h"
#include "MenuBar.h"
#include "MenuBarView.h"
#include "TextField.h"
#include "TextView.h"
#include "TextViewTestSupport.h"

using reckless::app::MenuBar;
using reckless::app::MenuBarController;
using reckless::app::MenuBarView;
using reckless::app::MenuEntry;
using reckless::app::Shortcut;
using reckless::app::ShortcutKey;
using reckless::app::ShortcutText;

namespace
{
/** A bar of two menus with an action, a disabled item, a checked one, a submenu and a disabled menu. */
MenuBar SampleBar(std::vector<std::string> &log)
{
    MenuBar bar;
    bar.menus.push_back(MenuEntry::Menu(
        "File", {MenuEntry::Item("New Game", 11, Shortcut::Primary('n'), [&log] { log.push_back("new"); }),
                 MenuEntry::Item("Open...", 12, Shortcut::Primary('o')), MenuEntry::Separator(),
                 MenuEntry::Menu("Recent", {MenuEntry::Item("game.pgn", 13)}),
                 MenuEntry::Item("Quit", 14, Shortcut::Primary('q'))}));
    MenuEntry flip = MenuEntry::Item("Flip Board", 21, Shortcut::Plain('f'), [&log] { log.push_back("flip"); });
    flip.checked   = true;
    MenuEntry gone = MenuEntry::Item("Analyse", 22, Shortcut::Primary('a', true));
    gone.enabled   = false;
    bar.menus.push_back(
        MenuEntry::Menu("View", {flip, gone, MenuEntry::Item("Engine Log", 23, Shortcut::Plain(ShortcutKey::kF5))}));
    MenuEntry off = MenuEntry::Menu("Engine", {MenuEntry::Item("Start", 31, Shortcut::Primary('e'))});
    off.enabled   = false;
    bar.menus.push_back(off);
    return bar;
}

/** A popup a host of the suite's own opens: its content a root of its own, the menu a drawn bar opened. */
class Popup final : public reckless::view::Popup, public reckless::view::Host
{
  public:
    Popup(reckless::view::View &content, std::vector<std::function<void()>> &calls, std::vector<Popup *> &open)
        : content_(content), calls_(calls), open_(open)
    {
        content_.SetHost(this);
        open_.push_back(this);
    }

    ~Popup() override
    {
        content_.SetHost(nullptr);
        open_.erase(std::find(open_.begin(), open_.end(), this));
    }

    bool Contains(reckless::gfx::PointI) const override
    {
        return false;
    }

    void Invalidate(reckless::gfx::RectI) override
    {
    }

    void CallSoon(std::function<void()> call) override
    {
        calls_.push_back(std::move(call));
    }

    std::unique_ptr<reckless::view::Popup> OpenPopup(reckless::view::View &content, reckless::gfx::PointI,
                                                     std::function<void(reckless::gfx::PointI)>, bool) override
    {
        return std::make_unique<Popup>(content, calls_, open_);
    }

    reckless::view::View &content() noexcept
    {
        return content_;
    }

  private:
    reckless::view::View               &content_;
    std::vector<std::function<void()>> &calls_;
    std::vector<Popup *>               &open_;
};

/** A window's host for the drawn bar: popups of the suite's own, and calls queued until the suite runs them, as a
    window's loop does. */
class BarHost final : public reckless::view::Host
{
  public:
    void Invalidate(reckless::gfx::RectI) override
    {
        ++invalidated;
    }

    void CallSoon(std::function<void()> call) override
    {
        calls.push_back(std::move(call));
    }

    std::unique_ptr<reckless::view::Popup> OpenPopup(reckless::view::View &content, reckless::gfx::PointI at,
                                                     std::function<void(reckless::gfx::PointI)>, bool) override
    {
        opened_at.push_back(at);
        return std::make_unique<Popup>(content, calls, open);
    }

    /** The queued calls, run in order, including any they queue. */
    void Drain()
    {
        while (!calls.empty())
        {
            auto call = std::move(calls.front());
            calls.erase(calls.begin());
            call();
        }
    }

    std::vector<std::function<void()>> calls;
    std::vector<Popup *>               open;
    std::vector<reckless::gfx::PointI> opened_at;
    int                                invalidated{0};
};

/** A press at a point, as the router hands one to a view. */
reckless::view::MouseEvent At(float x, float y)
{
    reckless::view::MouseEvent e;
    e.position = {x, y};
    e.down     = e.position;
    return e;
}
} // namespace

TEST(MenuBar, AShortcutIsTheKeyAndExactlyItsModifiersALetterInEitherCase)
{
    const Shortcut new_game = Shortcut::Primary('N');
    EXPECT_EQ(new_game.character, U'n') << "a letter kept in lower case: Shift is a modifier of its own";
    EXPECT_TRUE(new_game.Matches(Shortcut::Primary('n')));
    EXPECT_TRUE(new_game.Matches({ShortcutKey::kCharacter, U'N', true, false, false})) << "a press of either case";
    EXPECT_FALSE(new_game.Matches(Shortcut::Primary('n', true))) << "Shift held too is another shortcut";
    EXPECT_FALSE(new_game.Matches(Shortcut::Plain('n'))) << "nor the key alone";
    EXPECT_FALSE(new_game.Matches(Shortcut::Primary('n', false, true)));
    EXPECT_FALSE(new_game.Matches(Shortcut::Primary('m')));
    EXPECT_TRUE(Shortcut::Plain(ShortcutKey::kF5).Matches(Shortcut::Plain(ShortcutKey::kF5)));
    EXPECT_FALSE(Shortcut::Plain(ShortcutKey::kF5).Matches(Shortcut::Plain(ShortcutKey::kF6)));
    EXPECT_FALSE(Shortcut{}.Matches(Shortcut{})) << "no shortcut matches nothing";
    EXPECT_TRUE(Shortcut{}.empty());
    EXPECT_TRUE((Shortcut{ShortcutKey::kCharacter, 0, true, false, false}).empty()) << "a character key with none";
}

TEST(MenuBar, EachPlatformWritesAShortcutItsOwnWay)
{
    EXPECT_EQ(ShortcutText(Shortcut::Primary('n'), false), "Ctrl+N");
    EXPECT_EQ(ShortcutText(Shortcut::Primary('n', true), false), "Ctrl+Shift+N");
    EXPECT_EQ(ShortcutText(Shortcut::Primary('n', true, true), false), "Ctrl+Alt+Shift+N");
    EXPECT_EQ(ShortcutText(Shortcut::Plain(ShortcutKey::kF5), false), "F5");
    EXPECT_EQ(ShortcutText(Shortcut::Plain(ShortcutKey::kF12, true), false), "Shift+F12");
    EXPECT_EQ(ShortcutText(Shortcut::Primary(ShortcutKey::kLeft), false), "Ctrl+Left");
    EXPECT_EQ(ShortcutText(Shortcut::Plain('f'), false), "F");
    EXPECT_EQ(ShortcutText(Shortcut::Primary('='), false), "Ctrl+=");
    EXPECT_EQ(ShortcutText(Shortcut::Primary(static_cast<char32_t>(0xE9)), false), "Ctrl+\xc3\xa9")
        << "a character beyond ASCII whole";
    EXPECT_EQ(ShortcutText(Shortcut{}, false), "");
    // A Mac's: Option, Shift, Command, then the key.
    EXPECT_EQ(ShortcutText(Shortcut::Primary('n'), true), "\xe2\x8c\x98N");
    EXPECT_EQ(ShortcutText(Shortcut::Primary('n', true, true), true), "\xe2\x8c\xa5\xe2\x87\xa7\xe2\x8c\x98N");
    EXPECT_EQ(ShortcutText(Shortcut::Plain(ShortcutKey::kEscape), true), "\xe2\x8e\x8b");
    EXPECT_EQ(ShortcutText(Shortcut::Plain(ShortcutKey::kEscape), false), "Esc");
}

TEST(MenuBar, EntriesAreNumberedDepthFirstAndChosenByTheirActionThenTheirId)
{
    std::vector<std::string> log;
    std::vector<int>         chosen;
    MenuBarController        bar;
    bar.Set(SampleBar(log), [&](int id) {
        log.push_back("chosen " + std::to_string(id));
        chosen.push_back(id);
    });
    ASSERT_EQ(bar.command_count(), 8) << "every item; no separator, no menu";
    const std::vector<int> ids = [&] {
        std::vector<int> all;
        for (int c = 1; c <= bar.command_count(); ++c)
        {
            all.push_back(bar.EntryOf(c)->id);
        }
        return all;
    }();
    EXPECT_EQ(ids, (std::vector<int>{11, 12, 13, 14, 21, 22, 23, 31})) << "in order, a submenu's where it stands";
    EXPECT_EQ(bar.EntryOf(0), nullptr);
    EXPECT_EQ(bar.EntryOf(9), nullptr);
    EXPECT_EQ(bar.CommandOf(bar.bar().menus[0].submenu[0]), 1);
    EXPECT_EQ(bar.CommandOf(bar.bar().menus[0].submenu[2]), 0) << "a separator is no command";
    EXPECT_EQ(bar.CommandOf(bar.bar().menus[0]), 0) << "nor a menu";

    EXPECT_TRUE(bar.Choose(1));
    EXPECT_EQ(log, (std::vector<std::string>{"new", "chosen 11"})) << "its action, then its id";
    EXPECT_TRUE(bar.Choose(2)) << "an entry with no action reports its id";
    EXPECT_FALSE(bar.Choose(6)) << "a disabled entry is not chosen";
    EXPECT_FALSE(bar.Choose(8)) << "nor one under a disabled menu";
    EXPECT_FALSE(bar.Choose(42));
    EXPECT_EQ(chosen, (std::vector<int>{11, 12}));

    EXPECT_TRUE(bar.PressShortcut(Shortcut::Primary('o')));
    EXPECT_TRUE(bar.PressShortcut(Shortcut::Plain('F'))) << "the key in either case";
    EXPECT_TRUE(bar.PressShortcut(Shortcut::Plain(ShortcutKey::kF5)));
    EXPECT_FALSE(bar.PressShortcut(Shortcut::Primary('a', true))) << "a disabled entry's shortcut chooses nothing";
    EXPECT_FALSE(bar.PressShortcut(Shortcut::Primary('e'))) << "nor one under a disabled menu";
    EXPECT_FALSE(bar.PressShortcut(Shortcut::Primary('z'))) << "nor a key no entry has";
    EXPECT_EQ(chosen, (std::vector<int>{11, 12, 12, 21, 23}));
}

TEST(MenuBar, AnEntrysOtherShortcutsChooseItTooAndTheMenuShowsTheOneAlone)
{
    MenuBar   bar;
    MenuEntry larger        = MenuEntry::Item("Larger", 41, Shortcut::Primary('='));
    larger.other_shortcuts  = {Shortcut::Primary('=', true), Shortcut::Primary('+')};
    MenuEntry smaller       = MenuEntry::Item("Smaller", 42, Shortcut::Primary('-'));
    smaller.enabled         = false;
    smaller.other_shortcuts = {Shortcut::Primary('-', true)};
    bar.menus.push_back(MenuEntry::Menu("View", {larger, smaller}));
    MenuBarController controller;
    std::vector<int>  chosen;
    controller.Set(bar, [&](int id) { chosen.push_back(id); });

    EXPECT_TRUE(controller.PressShortcut(Shortcut::Primary('=')));
    EXPECT_TRUE(controller.PressShortcut(Shortcut::Primary('=', true))) << "the plus key: Shift with =";
    EXPECT_TRUE(controller.PressShortcut(Shortcut::Primary('+'))) << "the keypad's plus";
    EXPECT_FALSE(controller.PressShortcut(Shortcut::Plain('+'))) << "exactly its modifiers, as the one";
    EXPECT_FALSE(controller.PressShortcut(Shortcut::Primary('-', true))) << "a disabled entry's other keys do nothing";
    EXPECT_EQ(chosen, (std::vector<int>{41, 41, 41}));
    const std::vector<reckless::view::MenuItem> rows = controller.ItemsOf(controller.bar().menus[0].submenu);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].shortcut, "Ctrl+=") << "the menu shows the one shortcut";
}

TEST(MenuBar, AnEntryIsFoundByItsIdForTheAppMenusAbout)
{
    std::vector<std::string> log;
    MenuBarController        bar;
    MenuBar                  sample = SampleBar(log);
    EXPECT_EQ(sample.about_id, 0) << "the system's About panel unless the app names an entry";
    sample.about_id = 22;
    bar.Set(sample, {});
    EXPECT_EQ(bar.bar().about_id, 22);
    EXPECT_EQ(bar.CommandOfId(11), 1);
    EXPECT_EQ(bar.CommandOfId(22), 6);
    EXPECT_EQ(bar.EntryOf(bar.CommandOfId(31))->id, 31) << "a submenu's entry too";
    EXPECT_EQ(bar.CommandOfId(99), 0) << "an id no entry has";
    EXPECT_EQ(bar.CommandOfId(0), 0) << "nor 0, which reports nothing";
}

TEST(MenuBar, AnActionMaySetTheBarAnewAndWatchersHearEachSet)
{
    MenuBarController     bar;
    int                   heard   = 0;
    const int             token   = bar.Watch([&] { ++heard; });
    int                   chosen  = 0;
    int                   toggled = 0;
    std::function<void()> set;
    set = [&] {
        MenuBar   next;
        MenuEntry flip = MenuEntry::Item("Flip", 5, Shortcut::Plain('f'), [&] {
            ++toggled;
            set(); // the bar set anew from the entry's own action, its tick moved
        });
        flip.checked   = toggled % 2 == 1;
        next.menus.push_back(MenuEntry::Menu("View", {flip}));
        bar.Set(std::move(next), [&](int id) { chosen = id; });
    };
    set();
    EXPECT_EQ(heard, 1);
    const std::size_t before = bar.revision();
    EXPECT_TRUE(bar.PressShortcut(Shortcut::Plain('f')));
    EXPECT_EQ(toggled, 1);
    EXPECT_EQ(chosen, 5) << "the id reported after the action set the bar anew";
    EXPECT_TRUE(bar.bar().menus[0].submenu[0].checked);
    EXPECT_GT(bar.revision(), before);
    EXPECT_EQ(heard, 2);
    bar.Unwatch(token);
    set();
    EXPECT_EQ(heard, 2) << "an unwatched watcher hears nothing";
}

TEST(MenuBar, AMenusEntriesAsDrawnRowsCarryTheirCommandsTicksAndShortcuts)
{
    std::vector<std::string> log;
    MenuBarController        bar;
    bar.Set(SampleBar(log), nullptr);
    const std::vector<reckless::view::MenuItem> file = bar.ItemsOf(bar.bar().menus[0].submenu);
    ASSERT_EQ(file.size(), 5u);
    EXPECT_EQ(file[0].text, "New Game");
    EXPECT_EQ(file[0].id, 1) << "its command number, which the drawn menu reports";
    EXPECT_EQ(file[0].shortcut, "Ctrl+N");
    EXPECT_TRUE(file[2].separator);
    EXPECT_EQ(file[3].id, 0) << "a submenu's row chooses nothing itself";
    ASSERT_EQ(file[3].submenu.size(), 1u);
    EXPECT_EQ(file[3].submenu[0].id, 3);
    const std::vector<reckless::view::MenuItem> view = bar.ItemsOf(bar.bar().menus[1].submenu);
    EXPECT_TRUE(view[0].ticked);
    EXPECT_FALSE(view[1].enabled);
    EXPECT_EQ(view[1].shortcut, "Ctrl+Shift+A");
    EXPECT_EQ(view[2].shortcut, "F5");
}

TEST(MenuBarView, APressOnATitleOpensItsMenuUnderItAndAChoiceGoesThroughTheController)
{
    std::vector<std::string> log;
    auto                     controller = std::make_shared<MenuBarController>();
    std::vector<int>         chosen;
    controller->Set(SampleBar(log), [&](int id) { chosen.push_back(id); });
    BarHost     host;
    MenuBarView bar(controller);
    bar.SetBounds({400, bar.PreferredHeight()});
    bar.SetHost(&host);

    bar.MouseDown(At(5.0f, 5.0f));
    ASSERT_EQ(bar.titles().size(), 3u);
    EXPECT_EQ(bar.titles()[0].x(), 0);
    EXPECT_EQ(bar.titles()[1].x(), bar.titles()[0].right()) << "titles left to right";
    EXPECT_EQ(bar.open_menu(), 0);
    ASSERT_EQ(host.opened_at.size(), 1u);
    EXPECT_EQ(host.opened_at[0], (reckless::gfx::PointI{0, bar.PreferredHeight()})) << "under its title";
    ASSERT_NE(bar.session(), nullptr);
    reckless::view::Menu &menu = bar.session()->menu();
    ASSERT_EQ(menu.items().size(), 5u);
    EXPECT_EQ(menu.items()[0].shortcut, "Ctrl+N");

    // A release on the first row chooses it: the menu ends on the window's next turn, and the entry runs.
    const reckless::gfx::RectI row = menu.RowBounds(0);
    menu.MouseUp(At(static_cast<float>(row.x() + 4), static_cast<float>(row.y() + 4)));
    EXPECT_TRUE(log.empty()) << "nothing before the window's loop turns";
    host.Drain();
    EXPECT_EQ(log, (std::vector<std::string>{"new"}));
    EXPECT_EQ(chosen, (std::vector<int>{11}));
    EXPECT_EQ(bar.open_menu(), -1);
    EXPECT_EQ(bar.session(), nullptr);

    // A press on the open menu's title closes it; a disabled menu does not open.
    const reckless::gfx::RectI view = bar.titles()[1];
    bar.MouseDown(At(static_cast<float>(view.x() + 3), 5.0f));
    EXPECT_EQ(bar.open_menu(), 1);
    bar.MouseDown(At(static_cast<float>(view.x() + 3), 5.0f));
    EXPECT_EQ(bar.open_menu(), -1);
    EXPECT_TRUE(host.open.empty()) << "its popup closed";
    const reckless::gfx::RectI engine = bar.titles()[2];
    bar.MouseDown(At(static_cast<float>(engine.x() + 3), 5.0f));
    EXPECT_EQ(bar.open_menu(), -1) << "a disabled menu does not open";
    bar.MouseDown(At(399.0f, 5.0f));
    EXPECT_EQ(bar.open_menu(), -1) << "a press past the titles opens nothing";

    // The bar set anew while a menu is open: the menu closes, its rows stale, and the bar lays itself out again.
    ASSERT_TRUE(bar.Open(0));
    MenuBar shorter;
    shorter.menus.push_back(MenuEntry::Menu("Game", {MenuEntry::Item("Resign", 1)}));
    const int before = host.invalidated;
    controller->Set(std::move(shorter), nullptr);
    EXPECT_EQ(bar.open_menu(), -1);
    EXPECT_EQ(bar.titles().size(), 1u);
    EXPECT_GT(host.invalidated, before) << "painted again";
    bar.SetHost(nullptr);
}

TEST(MenuBarView, AMenuDismissedChoosesNothing)
{
    std::vector<std::string> log;
    auto                     controller = std::make_shared<MenuBarController>();
    int                      chosen     = 0;
    controller->Set(SampleBar(log), [&](int id) { chosen = id; });
    BarHost     host;
    MenuBarView bar(controller);
    bar.SetBounds({400, bar.PreferredHeight()});
    bar.SetHost(&host);
    ASSERT_TRUE(bar.Open(1));
    bar.session()->Finish(0); // a press outside it
    host.Drain();
    EXPECT_EQ(bar.open_menu(), -1);
    EXPECT_EQ(chosen, 0);
    EXPECT_TRUE(log.empty());
    bar.SetHost(nullptr);
}

TEST(FakeAppPlatform, TheViewWithTheKeyboardHasAKeyFirstAndTheBarsShortcutsTheRest)
{
    reckless::test::FakeAppPlatform platform;
    EXPECT_FALSE(platform.HasNativeMenuBar()) << "the fake draws its bar, as Linux's platform does";
    std::vector<std::string> log;
    MenuBar                  bar;
    bar.menus.push_back(MenuEntry::Menu(
        "Edit", {MenuEntry::Item("Copy FEN", 1, Shortcut::Primary('c'), [&] { log.push_back("menu copy"); }),
                 MenuEntry::Item("Paste FEN", 2, Shortcut::Primary('v'), [&] { log.push_back("menu paste"); }),
                 MenuEntry::Item("New Game", 3, Shortcut::Primary('n'), [&] { log.push_back("new"); })}));
    platform.SetMenuBar(bar, nullptr);
    const std::unique_ptr<MenuBarView> drawn = platform.MakeMenuBarView({});
    ASSERT_NE(drawn, nullptr);
    EXPECT_EQ(platform.menu_bar().command_count(), 3);
    EXPECT_TRUE(platform.ChooseCommand(3));
    EXPECT_EQ(log, (std::vector<std::string>{"new"}));
    log.clear();

    using Took = reckless::test::FakeAppPlatform::Took;
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('c')), Took::kShortcut) << "no view has the keyboard";
    EXPECT_EQ(log, (std::vector<std::string>{"menu copy"}));
    log.clear();

    // A text view with the keyboard: Ctrl+C is its copy, Ctrl+V its paste from the clipboard, Ctrl+N no view's.
    reckless::view::View        root;
    reckless::test::TextHost    host;
    reckless::view::MouseRouter router(root, &host);
    reckless::view::TextView    text{reckless::view::TextView::Style{}};
    host.router = &router;
    root.SetBounds({400, 300});
    root.SetHost(&host);
    text.SetBounds({0, 0, 400, 300});
    root.AddChild(text);
    text.SetEditable(true);
    text.SetText("1. e4 e5");
    std::vector<std::string> copied;
    text.on_copy = [&](std::string copy) {
        copied.push_back(copy);
        platform.SetClipboardText(copy);
    };
    router.Focus(&text);
    text.SelectAll();
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('c'), &router), Took::kView);
    EXPECT_EQ(copied, (std::vector<std::string>{"1. e4 e5"})) << "the view's copy, not the menu's";
    EXPECT_EQ(platform.ClipboardText(), "1. e4 e5");
    host.clipboard = " 2. Nf3";
    text.SelectAll();
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('v'), &router), Took::kView);
    EXPECT_EQ(text.text(), " 2. Nf3") << "the clipboard's text pasted over the selection";
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('n'), &router), Took::kShortcut) << "a key no view takes";
    EXPECT_EQ(log, (std::vector<std::string>{"new"}));
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('z'), &router), Took::kNothing);

    // A text field: Ctrl+A selects all, Ctrl+V pastes - each as on a Mac with Command.
    reckless::view::TextField field{reckless::view::TextField::Style{}};
    field.SetBounds({0, 0, 200, 24});
    root.AddChild(field);
    router.Focus(&field);
    host.clipboard = "a Black reply";
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('v'), &router), Took::kView);
    EXPECT_EQ(field.text(), "a Black reply");
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('a'), &router), Took::kView);
    EXPECT_EQ(platform.PressKey(Shortcut::Plain('x'), &router), Took::kView) << "typed over the whole selection";
    EXPECT_EQ(field.text(), "x");
    EXPECT_EQ(platform.PressKey(Shortcut::Primary('c'), &router), Took::kShortcut) << "a field copies nothing";
    root.RemoveChild(field);
    root.RemoveChild(text);
    root.SetHost(nullptr);
}
