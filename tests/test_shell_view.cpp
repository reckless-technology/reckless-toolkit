// The window round a panel, with no toolkit: the ZoomView (view/ZoomView.h) that shows a panel scaled as the
// panel's host - the mouse in the panel's pixels, the parts to paint again scaled, the keyboard, the pointer's shape,
// popups at the zoom, files; the scroll bar (ScrollBar.h) as JUCE's ScrollBar works and sizes its thumb; the tooltip
// (Tooltip.h) as JUCE's tooltip window times, lays out and places its tip; and the shell (window/ShellView.h) as
// the shell of the releases built on JUCE sizes, letterboxes, constrains, scrolls, remembers on the session's `ui`
// child and lists its View submenu - each held to that shell's own numbers.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Json.h"
#include "JsonFile.h"
#include "LicenceStore.h"
#include "Menu.h"
#include "PanelView.h"
#include "ScratchPath.h"
#include "ScrollBar.h"
#include "ShellView.h"
#include "Tooltip.h"
#include "UpdateCheck.h"
#include "View.h"
#include "ZoomView.h"

namespace
{
namespace gfx     = reckless::gfx;
namespace view    = reckless::view;
namespace window  = reckless::window;
namespace core    = reckless::core;
namespace licence = reckless::licence;
using gfx::PointF;
using gfx::PointI;
using gfx::RectI;

// ---- Views and a window that write down what happens --------------------------------------------------------------

/** A view that writes down the mouse and keys it takes, in its own coordinates. */
class Probe final : public view::View
{
  public:
    explicit Probe(bool wants_keys = false) : wants_keys_(wants_keys)
    {
    }

    std::vector<std::string> log;
    PointF                   last{};

    void MouseDown(const view::MouseEvent &e) override
    {
        last = e.position;
        log.push_back("down " + std::to_string(e.clicks));
    }

    void MouseDoubleClick(const view::MouseEvent &) override
    {
        log.push_back("double");
    }

    void MouseDrag(const view::MouseEvent &e) override
    {
        last = e.position;
        log.push_back("drag");
    }

    void MouseUp(const view::MouseEvent &e) override
    {
        last = e.position;
        log.push_back("up");
    }

    bool MouseWheel(const view::MouseEvent &e) override
    {
        last = e.position;
        log.push_back("wheel " + std::to_string(e.wheel_y));
        return true;
    }

    bool WantsKeyboardFocus() const override
    {
        return wants_keys_;
    }

    bool KeyPressed(const view::KeyEvent &key) override
    {
        log.push_back("key " + std::string(1, static_cast<char>(key.character)));
        return true;
    }

    void FocusGained() override
    {
        log.push_back("focus");
    }

    void FocusLost() override
    {
        log.push_back("lost");
    }

    int entered{0};
    int exited{0};
    int moved{0};

    void MouseMove(const view::MouseEvent &) override
    {
        ++moved;
    }

    void MouseEnter(const view::MouseEvent &) override
    {
        ++entered;
    }

    void MouseExit(const view::MouseEvent &) override
    {
        ++exited;
    }

    bool IsInterestedInFiles(const std::vector<std::string> &) const override
    {
        return wants_keys_;
    }

    void FilesDropped(const std::vector<std::string> &paths, PointF position) override
    {
        last = position;
        log.push_back("drop " + paths.front());
    }

    void FileDragEnter(const std::vector<std::string> &paths, PointF position) override
    {
        last = position;
        log.push_back("enter " + paths.front());
    }

    void FileDragMove(const std::vector<std::string> &paths, PointF position) override
    {
        last = position;
        log.push_back("move " + paths.front());
    }

    void FileDragExit() override
    {
        log.push_back("exit");
    }

  private:
    bool wants_keys_;
};

/** A window, as the platform's is to its root: the parts to paint again, the pointer's shape, the keyboard, popups
    and the loop's calls written down; its router handed the mouse by the test. */
class Window final : public view::Host
{
  public:
    struct Opened final : view::Popup
    {
        Window     *owner{nullptr};
        view::View *content{nullptr};

        ~Opened() override
        {
            owner->closed.push_back(content);
            owner->live.erase(std::remove(owner->live.begin(), owner->live.end(), this), owner->live.end());
        }

        bool Contains(PointI) const override
        {
            return false;
        }
    };

    explicit Window(view::View &root) : router(root, this), root_(root)
    {
        root_.SetHost(this);
    }

    ~Window() override
    {
        root_.SetHost(nullptr);
    }

    view::MouseRouter         router;
    std::vector<RectI>        invalid;
    view::Cursor              cursor{view::Cursor::kArrow};
    bool                      keyboard{false};
    int                       keyboard_changes{0}; ///< how often the router said whether a view has the keyboard
    std::vector<view::View *> popups;
    std::vector<PointI>       popup_at;
    std::vector<view::View *> closed;
    std::vector<Opened *>     live; ///< the popups open now
    float                     zoom{1.0f};

    void Invalidate(RectI area) override
    {
        invalid.push_back(area);
    }

    void SetCursor(view::Cursor shape) override
    {
        cursor = shape;
    }

    void RequestKeyboardFocus(view::View *v) override
    {
        router.Focus(v);
    }

    void KeyboardFocusChanged(bool has) override
    {
        keyboard = has;
        ++keyboard_changes;
    }

    void ViewLeaving(const view::View &v) override
    {
        router.Forget(v);
    }

    float Zoom() const override
    {
        return zoom;
    }

    std::string clipboard; ///< what a paste reads

    std::string ClipboardText() override
    {
        return clipboard;
    }

    bool SetClipboardText(const std::string &text) override
    {
        clipboard = text;
        return true;
    }

    std::unique_ptr<view::Popup> OpenPopup(view::View &content, PointI at, std::function<void(PointI)>, bool) override
    {
        popups.push_back(&content);
        popup_at.push_back(at);
        auto popup     = std::make_unique<Opened>();
        popup->owner   = this;
        popup->content = &content;
        live.push_back(popup.get());
        return popup;
    }

  private:
    view::View &root_;
};

// ---- The zoom ---------------------------------------------------------------------------------------------------

/** A panel of 200 by 100 with a probe at (100, 50, 20, 20), shown at a zoom of 2 at (10, 10) in a window's root. */
struct Zoomed
{
    Zoomed()
    {
        content.SetBounds({200, 100});
        probe.SetBounds({100, 50, 20, 20});
        content.AddChild(probe);
        root.SetBounds({500, 300});
        zoom.SetBounds({10, 10, 400, 200});
        root.AddChild(zoom);
    }

    view::View     root;
    Window         window{root};
    view::View     content;
    Probe          probe{true};
    view::ZoomView zoom{content, 2.0f};
};

TEST(ZoomView, HandsTheMouseToTheContentInItsOwnPixels)
{
    Zoomed z;
    // (10 + 2 * 105, 10 + 2 * 55): five pixels into the probe each way, in the panel's pixels.
    z.window.router.Down({220.0f, 120.0f}, 1, false, {});
    z.window.router.Drag({230.0f, 130.0f}, {});
    z.window.router.Up({230.0f, 130.0f}, {});
    EXPECT_EQ(z.probe.log, (std::vector<std::string>{"focus", "down 1", "drag", "up"}));
    EXPECT_FLOAT_EQ(z.probe.last.x, 10.0f) << "(230 - 10) / 2 - 100";
    EXPECT_FLOAT_EQ(z.probe.last.y, 10.0f);
    z.probe.log.clear();
    z.window.router.Down({220.0f, 120.0f}, 2, false, {});
    z.window.router.Up({220.0f, 120.0f}, {});
    EXPECT_EQ(z.probe.log, (std::vector<std::string>{"down 2", "up", "double"}))
        << "the content's router makes the double, after the release";
}

TEST(ZoomView, PassesTheWindowsClipboardToTheContentBothWays)
{
    Zoomed z;
    z.window.clipboard       = "from the window";
    view::Host *content_host = z.probe.host();
    ASSERT_NE(content_host, nullptr);
    EXPECT_EQ(content_host->ClipboardText(), "from the window") << "a paste into the content reads the window's";
    EXPECT_TRUE(content_host->SetClipboardText("from the content"));
    EXPECT_EQ(z.window.clipboard, "from the content") << "a copy from the content writes the window's";

    view::View     alone;
    view::ZoomView unhosted{alone, 1.0f};
    EXPECT_EQ(alone.host()->ClipboardText(), "") << "in no window, no clipboard";
    EXPECT_FALSE(alone.host()->SetClipboardText("lost"));
}

TEST(ZoomView, FindsTheContentsViewUnderAStillPointerAgainUnlessAPressHoldsOne)
{
    Zoomed z;
    // The zoom's point (205, 105) is on the probe at a zoom of 2, (102.5, 52.5) in the content, and off it at 1.5.
    z.window.router.Move({215.0f, 115.0f}, {});
    EXPECT_EQ(z.probe.entered, 1);
    z.zoom.SetZoom(1.5f);
    z.zoom.PointerStill({205.0f, 105.0f});
    EXPECT_EQ(z.probe.exited, 1);
    z.zoom.SetZoom(2.0f);
    z.zoom.PointerStill({205.0f, 105.0f});
    EXPECT_EQ(z.probe.entered, 2);
    z.zoom.PointerGone();
    EXPECT_EQ(z.probe.exited, 2) << "the zoom moved out from under the pointer";

    z.window.router.Down({215.0f, 115.0f}, 1, false, {});
    const int moved = z.probe.moved;
    z.zoom.SetZoom(1.5f);
    z.zoom.PointerStill({205.0f, 105.0f});
    EXPECT_EQ(z.probe.exited, 2) << "a press keeps its view...";
    EXPECT_EQ(z.probe.moved, moved) << "...which hears nothing, as JUCE fakes no move during a drag";
}

TEST(ZoomView, PaintsAgainThePartScaledAndRoundedOut)
{
    Zoomed z;
    z.window.invalid.clear();
    z.probe.Repaint({1, 2, 3, 4});
    ASSERT_EQ(z.window.invalid.size(), 1u);
    // (101, 52, 3, 4) in the panel: (202, 104, 6, 8) at the zoom, a pixel more each way, at the zoom's (10, 10).
    EXPECT_EQ(z.window.invalid.back(), (RectI{211, 113, 8, 10}));
}

TEST(ZoomView, TakesTheWindowsKeyboardWhileAViewOfTheContentHasIt)
{
    Zoomed     z;
    view::View other;
    other.SetBounds({0, 0, 50, 50});
    z.content.AddChild(other);
    EXPECT_FALSE(z.window.keyboard);
    z.window.router.Down({220.0f, 120.0f}, 1, false, {});
    EXPECT_TRUE(z.window.keyboard) << "the probe wants keys: the zoom takes the window's for it";
    EXPECT_EQ(z.window.router.focused(), &z.zoom);
    EXPECT_TRUE(z.window.router.Key({view::Key::kCharacter, U'a', {}}));
    EXPECT_EQ(z.probe.log.back(), "key a");
    z.window.router.Up({220.0f, 120.0f}, {});
    z.window.router.Down({30.0f, 30.0f}, 1, false, {}); // on the other view, which wants none
    EXPECT_EQ(z.probe.log.back(), "lost");
    EXPECT_FALSE(z.window.keyboard) << "no view of the content has it: the window's goes back";
    z.window.router.Up({30.0f, 30.0f}, {});

    z.window.router.Down({220.0f, 120.0f}, 1, false, {});
    z.window.router.Up({220.0f, 120.0f}, {});
    z.window.router.Focus(nullptr); // the window's keyboard went elsewhere
    EXPECT_EQ(z.probe.log.back(), "lost") << "and the content's with it";
}

TEST(ZoomView, AViewBesideTheZoomTakesTheKeyboardFromTheContentAndKeepsIt)
{
    // The window's keyboard goes from the zoom - a view of its content has it - to a view beside the zoom that wants
    // it: the content's view hears it lost it and the zoom gives up the window's keyboard as it goes, which asks the
    // window's router to release a keyboard the zoom no longer holds - nothing. The view beside it has the keyboard,
    // told it gained it and never that it lost it, and the window keeps its keyboard throughout.
    Zoomed z;
    Probe  beside{true};
    beside.SetBounds({420, 10, 60, 60});
    z.root.AddChild(beside);
    z.window.router.Down({220.0f, 120.0f}, 1, false, {}); // the content's probe, through the zoom
    z.window.router.Up({220.0f, 120.0f}, {});
    ASSERT_EQ(z.window.router.focused(), &z.zoom);
    const int changes = z.window.keyboard_changes;
    z.window.router.Down({440.0f, 30.0f}, 1, false, {});
    z.window.router.Up({440.0f, 30.0f}, {});
    EXPECT_EQ(z.window.router.focused(), &beside);
    EXPECT_EQ(beside.log, (std::vector<std::string>{"focus", "down 1", "up"}));
    EXPECT_EQ(z.probe.log.back(), "lost");
    EXPECT_TRUE(z.window.keyboard);
    EXPECT_EQ(z.window.keyboard_changes, changes) << "a view held the keyboard throughout: nothing to tell the window";
    EXPECT_TRUE(z.window.router.Key({view::Key::kCharacter, U'b', {}}));
    EXPECT_EQ(beside.log.back(), "key b");
}

TEST(ZoomView, AContentViewThatKeepsTheKeyboardHasItWhenTheWindowHasItAgain)
{
    // A box open in the panel keeps the keyboard when the window's goes elsewhere (a switch to another window): the
    // content's view still has it, and the window's next press - on a view that wants none - hands keys to it again.
    Zoomed          z;
    view::TextField field(view::TextField::Style{});
    field.SetBounds({10, 10, 60, 17});
    field.keeps_keyboard_focus = true;
    z.content.AddChild(field);
    z.window.router.Down({50.0f, 50.0f}, 1, false, {}); // (10 + 2 * 20, 10 + 2 * 20): in the field
    z.window.router.Up({50.0f, 50.0f}, {});
    ASSERT_TRUE(field.has_focus());
    z.window.router.Focus(nullptr); // the window's keyboard went elsewhere
    EXPECT_TRUE(field.has_focus()) << "the box's field keeps the content's keyboard";
    z.window.router.Down({390.0f, 190.0f}, 1, false, {}); // on the content's bare plate
    z.window.router.Up({390.0f, 190.0f}, {});
    EXPECT_EQ(z.window.router.focused(), &z.zoom) << "the window's keyboard to the zoom, for the field";
    EXPECT_TRUE(z.window.router.Key({view::Key::kCharacter, U'7', {}}));
    EXPECT_EQ(field.text(), "7");
}

TEST(ZoomView, ShowsTheContentsPointerShapeAsItsOwn)
{
    Zoomed z;
    z.probe.cursor = view::Cursor::kPointingHand;
    z.window.router.Move({220.0f, 120.0f}, {});
    EXPECT_EQ(z.window.cursor, view::Cursor::kPointingHand);
    EXPECT_EQ(z.zoom.cursor, view::Cursor::kPointingHand) << "what the window's router reads after each event";
    z.window.router.Move({15.0f, 15.0f}, {});
    EXPECT_EQ(z.window.cursor, view::Cursor::kArrow);
}

TEST(ZoomView, OpensTheContentsPopupsAtItsZoom)
{
    Zoomed     z;
    view::View menu;
    menu.SetBounds({100, 40});
    auto popup = z.probe.OpenPopup(menu, {5, 20}, nullptr);
    ASSERT_NE(popup, nullptr);
    ASSERT_EQ(z.window.popups.size(), 1u);
    auto *shown = dynamic_cast<view::ZoomView *>(z.window.popups.front());
    ASSERT_NE(shown, nullptr) << "the window shows a zoom of the popup's content";
    EXPECT_EQ(&shown->content(), &menu);
    EXPECT_FLOAT_EQ(shown->zoom(), 2.0f);
    EXPECT_EQ(shown->bounds(), (RectI{200, 80}));
    // The probe's (5, 20) is the panel's (105, 70): the zoom's (210, 140), the root's (220, 150).
    EXPECT_EQ(z.window.popup_at.front(), (PointI{220, 150}));
    EXPECT_NE(menu.host(), nullptr) << "the popup's content lives in the zoom";
    EXPECT_NE(menu.host(), &z.window);
    popup.reset();
    ASSERT_EQ(z.window.closed.size(), 1u) << "the window's popup closed with the handle";
    EXPECT_EQ(menu.host(), nullptr) << "and the content let go";
}

TEST(ZoomView, PassesOnTheWindowsWordThatItClosedTheContentsPopup)
{
    // The window, going, closes its own popup - the one holding the zoom of the content's - and says so on that one:
    // whoever opened the content's popup hears it on the handle it holds.
    Zoomed     z;
    view::View menu;
    menu.SetBounds({100, 40});
    auto popup = z.probe.OpenPopup(menu, {5, 20}, nullptr);
    ASSERT_NE(popup, nullptr);
    int heard               = 0;
    popup->closed_by_window = [&] { ++heard; };
    ASSERT_EQ(z.window.live.size(), 1u);
    const std::function<void()> windows_word = z.window.live.front()->closed_by_window;
    ASSERT_TRUE(windows_word) << "the zoom listens on the window's popup";
    windows_word();
    EXPECT_EQ(heard, 1) << "the content's opener told";
    popup.reset();
    windows_word();
    EXPECT_EQ(heard, 1) << "a word that comes after the handle has gone reaches nothing";
}

TEST(ZoomView, TellsTheContentsViewUnderThePointerOfFiles)
{
    Zoomed z;
    EXPECT_TRUE(z.window.router.WouldTakeFiles({220.0f, 120.0f}, {"/a.json"}));
    EXPECT_FALSE(z.window.router.WouldTakeFiles({30.0f, 30.0f}, {"/a.json"}));
    EXPECT_TRUE(z.window.router.DropFiles({222.0f, 124.0f}, {"/a.json"}));
    EXPECT_EQ(z.probe.log.back(), "drop /a.json");
    EXPECT_FLOAT_EQ(z.probe.last.x, 6.0f);
    EXPECT_FLOAT_EQ(z.probe.last.y, 7.0f);
}

TEST(ZoomView, FollowsADragOfFilesThroughToTheContentsViewUnderIt)
{
    // The window's router tells the zoom of a drag as it comes, moves and goes; the zoom's own router tells the
    // content's view under the files, in the content's pixels - the hover a drop target lights for (the NAM's screen,
    // the Space's).
    Zoomed                         z;
    const std::vector<std::string> json{"/a.json"};
    EXPECT_TRUE(z.window.router.DragFiles({222.0f, 124.0f}, json));
    EXPECT_EQ(z.probe.log, (std::vector<std::string>{"enter /a.json"}));
    EXPECT_FLOAT_EQ(z.probe.last.x, 6.0f) << "(222 - 10) / 2 - 100";
    EXPECT_FLOAT_EQ(z.probe.last.y, 7.0f);
    EXPECT_TRUE(z.window.router.DragFiles({226.0f, 128.0f}, json));
    EXPECT_EQ(z.probe.log, (std::vector<std::string>{"enter /a.json", "move /a.json"}));
    EXPECT_FLOAT_EQ(z.probe.last.x, 8.0f);
    EXPECT_FLOAT_EQ(z.probe.last.y, 9.0f);
    EXPECT_FALSE(z.window.router.DragFiles({30.0f, 30.0f}, json)) << "over nothing that takes them";
    EXPECT_EQ(z.probe.log, (std::vector<std::string>{"enter /a.json", "move /a.json", "exit"}));
    z.probe.log.clear();
    z.window.router.DragFiles({222.0f, 124.0f}, json);
    z.window.router.EndFileDrag();
    EXPECT_EQ(z.probe.log, (std::vector<std::string>{"enter /a.json", "exit"}))
        << "back over it, and out of the window";
}

TEST(ZoomView, TheContentsZoomIsTheWindowsTimesItsOwn)
{
    Zoomed z;
    z.window.zoom          = 1.5f;
    const view::Host *host = z.content.host();
    ASSERT_NE(host, nullptr);
    EXPECT_FLOAT_EQ(host->Zoom(), 3.0f);
}

// ---- The scroll bar ---------------------------------------------------------------------------------------------

/** A vertical bar 10 by 200 over a range of 1000 showing 200. */
struct Bar
{
    Bar()
    {
        bar.SetBounds({0, 0, 10, 200});
        bar.SetRangeLimits(0.0, 1000.0);
        bar.SetCurrentRange(0.0, 200.0);
        bar.on_move = [this](double start) { moves.push_back(start); };
    }

    view::MouseEvent At(float y)
    {
        view::MouseEvent e;
        e.position = {5.0f, y};
        return e;
    }

    view::ScrollBar     bar{true};
    std::vector<double> moves;
};

TEST(ScrollBar, ItsThumbIsTheShowingShareOfTheBar)
{
    Bar b;
    EXPECT_EQ(b.bar.thumb_size(), 40) << "200 of 1000 showing on a bar of 200";
    EXPECT_EQ(b.bar.thumb_start(), 0);
    b.bar.SetCurrentRange(400.0, 200.0);
    EXPECT_EQ(b.bar.thumb_start(), 80) << "400 / 800 of the 160 the thumb travels";
    b.bar.SetCurrentRange(0.0, 10.0);
    EXPECT_EQ(b.bar.thumb_size(), 20) << "at least twice the bar's thickness";
    EXPECT_TRUE(b.bar.visible());
    b.bar.SetCurrentRange(0.0, 1000.0);
    EXPECT_FALSE(b.bar.visible()) << "nothing hidden: no bar";
}

TEST(ScrollBar, APressBesideTheThumbMovesAPageAndRepeatsWhileHeld)
{
    Bar b;
    b.bar.Step(10.0);
    b.bar.MouseDown(b.At(150.0f));
    EXPECT_EQ(b.moves, (std::vector<double>{200.0})) << "a page: the showing size";
    b.bar.Step(10.3);
    EXPECT_EQ(b.moves.size(), 1u) << "nothing before 400 ms";
    b.bar.Step(10.45);
    EXPECT_EQ(b.moves.back(), 400.0) << "then a page";
    b.bar.Step(10.5);
    EXPECT_EQ(b.moves.back(), 600.0) << "and one every 40 ms";
    b.bar.MouseUp(b.At(150.0f));
    b.bar.Step(11.0);
    EXPECT_EQ(b.moves.size(), 3u) << "until the press ends";
}

TEST(ScrollBar, DraggingTheThumbMovesTheRangeWithIt)
{
    Bar b;
    b.bar.MouseDown(b.At(10.0f));
    EXPECT_TRUE(b.moves.empty()) << "a press on the thumb moves nothing";
    b.bar.MouseDrag(b.At(50.0f));
    EXPECT_EQ(b.moves, (std::vector<double>{200.0})) << "40 of the thumb's 160 is 200 of the hidden 800";
    b.bar.MouseDrag(b.At(500.0f));
    EXPECT_EQ(b.bar.start(), 800.0) << "held to the end";
}

TEST(ScrollBar, TheWheelMovesTenStepsANotch)
{
    Bar              b;
    view::MouseEvent e = b.At(100.0f);
    b.bar.SetCurrentRange(500.0, 200.0);
    e.wheel_y = -100.0f; // a notch toward the user, in the views' pixels
    b.bar.MouseWheel(e);
    EXPECT_EQ(b.bar.start(), 660.0) << "ten steps of a viewport's 16";
    e.wheel_y = 1.0f; // a hundredth of a notch
    b.bar.MouseWheel(e);
    EXPECT_EQ(b.bar.start(), 644.0) << "at least one step";
}

// ---- The tooltip ------------------------------------------------------------------------------------------------

/** Text measured at 6 pixels a character, as no canvas would but every line's sum can be checked by hand. */
float SixPerCharacter(std::string_view text)
{
    return 6.0f * static_cast<float>(text.size());
}

TEST(Tooltip, AShortTipIsOneLine)
{
    const auto layout = view::LayOutTooltip("Gain, in decibels.", {}, SixPerCharacter);
    ASSERT_EQ(layout.lines.size(), 1u);
    EXPECT_EQ(layout.lines.front(), "Gain, in decibels.");
    EXPECT_FLOAT_EQ(layout.width, 108.0f);
    EXPECT_FLOAT_EQ(layout.height, 13.0f) << "the bold 13-pixel serif's line";
}

TEST(Tooltip, ALongTipWrapsOnLinesBalancedAsJucesAre)
{
    // 106 characters at 6 pixels: 636, wrapped at 400 and then narrower until 340, where the lines of 318 and 312
    // come within a tenth of each other.
    const std::string text   = "Presets: the plugin's by name, save, save as, delete, load from a file, the folder, "
                               "and Reset to defaults.";
    const auto        layout = view::LayOutTooltip(text, {}, SixPerCharacter);
    ASSERT_EQ(layout.lines.size(), 2u);
    for (std::size_t i = 0; i < layout.lines.size(); ++i)
    {
        EXPECT_LE(layout.widths[i], 400.0f);
        EXPECT_FLOAT_EQ(layout.widths[i], SixPerCharacter(layout.lines[i]));
    }
    const float shortest = std::min(layout.widths[0], layout.widths[1]);
    const float longest  = std::max(layout.widths[0], layout.widths[1]);
    EXPECT_LT(longest / shortest, 1.1f) << layout.lines[0] << " / " << layout.lines[1];
    EXPECT_EQ(layout.lines[0] + " " + layout.lines[1], text) << "the words as they came, the space at the break gone";
    EXPECT_FLOAT_EQ(layout.height, 26.0f);
}

TEST(Tooltip, ItSitsBesideThePointerInsideTheWindow)
{
    view::TooltipLayout layout;
    layout.width       = 100.0f;
    layout.height      = 13.0f;
    const RectI window = {600, 300};
    // Width 114 and height 19; right of and below the pointer in the window's first quarter...
    EXPECT_EQ(view::TooltipBounds(layout, {100, 100}, window), (RectI{124, 106, 114, 19}));
    // ...left of and above it past the middle...
    EXPECT_EQ(view::TooltipBounds(layout, {500, 250}, window), (RectI{374, 225, 114, 19}));
    // ...and held inside the window.
    EXPECT_EQ(view::TooltipBounds(layout, {290, 290}, window).bottom(), 290 - 6);
    EXPECT_EQ(view::TooltipBounds(layout, {590, 5}, window), (RectI{464, 11, 114, 19}));
    EXPECT_EQ(view::TooltipBounds(layout, {100, 5}, {200, 300}).right(), 200) << "pushed in from the right edge";
}

/** A tracker ticked as JUCE's timer ticks, every 123 ms, the pointer over a view with a tip. */
struct Ticked
{
    view::TooltipTracker        tracker{0.5};
    double                      now{100.0};
    view::TooltipTracker::Under knob{&tracker, "The knob's tip."};

    void Ticks(int count, const view::TooltipTracker::Under &under)
    {
        for (int i = 0; i < count; ++i)
        {
            now += view::TooltipTracker::kTickSeconds;
            tracker.Tick(now, under);
        }
    }
};

TEST(TooltipTracker, ATipShowsOnceThePointerHasRestedForTheDelay)
{
    Ticked t;
    t.tracker.PointerAt({100.0f, 100.0f});
    t.Ticks(1, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "the tip changed at this tick";
    t.Ticks(4, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "492 ms since: not yet";
    t.Ticks(1, t.knob);
    EXPECT_TRUE(t.tracker.showing()) << "615 ms: past the 500";
    EXPECT_EQ(t.tracker.tip(), "The knob's tip.");
    EXPECT_EQ(t.tracker.shown_at(), (PointI{100, 100}));
}

TEST(TooltipTracker, APointerStillMovingStartsTheWaitAgain)
{
    Ticked t;
    t.tracker.PointerAt({100.0f, 100.0f});
    t.Ticks(1, t.knob);
    for (int i = 0; i < 8; ++i)
    {
        t.tracker.PointerAt({100.0f + 13.0f * static_cast<float>(i + 1), 100.0f}); // 13 pixels a tick
        t.Ticks(1, t.knob);
    }
    EXPECT_FALSE(t.tracker.showing()) << "a second of moving more than 12 pixels a tick";
    t.Ticks(5, t.knob);
    EXPECT_TRUE(t.tracker.showing()) << "then rested";
    EXPECT_EQ(t.tracker.shown_at(), (PointI{204, 100})) << "beside where it rests";
}

TEST(TooltipTracker, APressDismissesItAndNoneShowsWhereThePressLanded)
{
    Ticked t;
    t.tracker.PointerAt({100.0f, 100.0f});
    t.Ticks(6, t.knob);
    ASSERT_TRUE(t.tracker.showing());
    t.tracker.Pressed({100.0f, 100.0f});
    t.tracker.Released();
    t.Ticks(1, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "dismissed at the next tick";
    t.Ticks(20, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "never again at the press's very point";
    t.tracker.PointerAt({101.0f, 100.0f});
    t.Ticks(6, t.knob);
    EXPECT_TRUE(t.tracker.showing()) << "a pixel off it, it shows";
}

TEST(TooltipTracker, AnotherTipReplacesOneShowingAtTheNextTick)
{
    Ticked                            t;
    const view::TooltipTracker::Under plate{&t.tracker, "The plate's tip."};
    t.tracker.PointerAt({100.0f, 100.0f});
    t.Ticks(6, t.knob);
    ASSERT_TRUE(t.tracker.showing());
    t.tracker.PointerAt({105.0f, 100.0f});
    t.Ticks(1, plate);
    EXPECT_EQ(t.tracker.tip(), "The plate's tip.") << "no wait while one shows";
    t.Ticks(1, {&t.tracker, ""});
    EXPECT_FALSE(t.tracker.showing()) << "a view with no tip hides it";
    t.Ticks(1, t.knob);
    EXPECT_EQ(t.tracker.tip(), "The knob's tip.") << "within half a second of going: back at once";
    t.Ticks(1, {&t.tracker, ""});
    t.Ticks(5, {&t.tracker, ""});
    t.Ticks(1, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "past the half second: the delay again";
}

TEST(TooltipTracker, NoneShowsWhileAButtonIsHeldOrOffTheWindow)
{
    Ticked t;
    t.tracker.PointerAt({100.0f, 100.0f});
    t.tracker.Pressed({90.0f, 100.0f});
    t.Ticks(10, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "a button held";
    t.tracker.Released();
    t.tracker.PointerLeft();
    t.Ticks(10, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "off the window";
    t.tracker.PointerAt({100.0f, 100.0f});
    t.Ticks(6, t.knob);
    EXPECT_TRUE(t.tracker.showing());
    t.tracker.PointerLeft();
    t.Ticks(1, t.knob);
    EXPECT_FALSE(t.tracker.showing()) << "gone with the pointer";
}

// ---- The shell --------------------------------------------------------------------------------------------------

/** A panel of 400 by 200 with two knob-sized views carrying tips, at (100, 50, 40, 40) and (230, 110, 40, 40). */
class ToyPanel final : public view::Panel
{
  public:
    ToyPanel() : view::Panel(400, 200)
    {
        knob.tooltip = "The toy's knob.";
        knob.SetBounds({100, 50, 40, 40});
        AddChild(knob);
        other.tooltip = "The toy's other knob.";
        other.SetBounds({230, 110, 40, 40});
        AddChild(other);
    }

    void Frame(double) override
    {
        ++frames;
    }

    Probe knob;
    Probe other;
    int   frames{0};
};

/** The session's "ui" and the preference as the shell keeps them, written down: no "ui" until the shell writes one. */
class Store final : public window::ShellStore
{
  public:
    std::optional<core::json::Value> ui;
    bool                             scroll{false};
    int                              told{0};
    std::vector<bool>                written;

    core::json::Value Ui() const override
    {
        return ui ? *ui : core::json::Value::MakeObject();
    }

    void WriteUi(const std::function<void(core::json::Value &ui)> &write) override
    {
        write(Seed());
    }

    /** The "ui", made where there is none: what a test writes before the shell opens. */
    core::json::Value &Seed()
    {
        if (!ui)
        {
            ui = core::json::Value::MakeObject();
        }
        return *ui;
    }

    void UiChanged() override
    {
        ++told;
    }

    bool ScrollMode() const override
    {
        return scroll;
    }

    void SetScrollMode(bool s) override
    {
        written.push_back(s);
        scroll = s;
    }
};

/** The toy panel in a shell on a store, in a window, on a clock the test moves. */
struct Shell
{
    explicit Shell(std::function<void(Store &)> before = {})
    {
        if (before)
        {
            before(store);
        }
        shell = std::make_unique<window::ShellView>(panel, window::ShellStyle{}, store, [this] { return now; });
        shell->on_size = [this](int w, int h) { asked.push_back({w, h}); };
        window_        = std::make_unique<Window>(*shell);
    }

    ~Shell()
    {
        window_.reset();
        shell.reset();
    }

    /** Frames at 60 Hz for `seconds`. */
    void Frames(double seconds)
    {
        for (double t = 0.0; t < seconds; t += 1.0 / 60.0)
        {
            now += 1.0 / 60.0;
            shell->Frame(1.0 / 60.0);
        }
    }

    Window &window()
    {
        return *window_;
    }

    ToyPanel                           panel;
    Store                              store;
    double                             now{50.0};
    std::unique_ptr<window::ShellView> shell;
    std::vector<PointI>                asked;

  private:
    std::unique_ptr<Window> window_;
};

TEST(ShellView, OpensAtTheDesignSizeTimesTheRememberedScale)
{
    Shell fresh;
    EXPECT_EQ(fresh.shell->bounds(), (RectI{400, 200}));
    EXPECT_EQ(fresh.shell->zoom_view().bounds(), (RectI{400, 200}));
    EXPECT_FLOAT_EQ(fresh.shell->zoom_view().zoom(), 1.0f);
    EXPECT_FALSE(fresh.store.ui.has_value()) << "opening a window writes nothing";

    Shell remembered([](Store &s) { s.Seed().Set("scale", 1.5); });
    EXPECT_EQ(remembered.shell->WantedSize(), (PointI{600, 300}));
    EXPECT_EQ(remembered.shell->bounds(), (RectI{600, 300}));
    EXPECT_FLOAT_EQ(remembered.shell->zoom_view().zoom(), 1.5f);
    EXPECT_EQ(remembered.store.told, 0);
}

TEST(ShellView, AScaleThatIsNotAPositiveNumberOpensAtTheDesignSizeAndIsWrittenOver)
{
    // A damaged or hand-edited session's scale that is no positive number would open a window of no size that never
    // recovers: the design size instead, and the scale written over with it.
    for (const core::json::Value &scale :
         {core::json::Value("1.5"), core::json::Value(0.0), core::json::Value(-2.0), core::json::Value(true)})
    {
        Shell             s([&scale](Store &store) { store.Seed().Set("scale", scale); });
        const std::string what = core::json::Write(scale);
        EXPECT_EQ(s.shell->bounds(), (RectI{400, 200})) << what;
        EXPECT_FLOAT_EQ(s.shell->zoom_view().zoom(), 1.0f) << what;
        EXPECT_DOUBLE_EQ(s.shell->scale(), 1.0) << what;
        ASSERT_TRUE(s.store.ui.has_value());
        EXPECT_EQ((*s.store.ui)["scale"], core::json::Value(1.0)) << what << ": written over";
    }
}

TEST(ShellView, AScaleChosenIsHeldToTheLimitsAndAskedOfTheWindow)
{
    Shell s;
    s.shell->SetScale(1.25);
    EXPECT_EQ(s.asked, (std::vector<PointI>{{500, 250}}));
    EXPECT_EQ(s.shell->bounds(), (RectI{500, 250})) << "laid out at it already";
    s.shell->SetScale(3.0);
    EXPECT_EQ(s.asked.back(), (PointI{800, 400}));
    s.shell->SetScale(0.1);
    EXPECT_EQ(s.asked.back(), (PointI{200, 100}));
    EXPECT_DOUBLE_EQ(s.shell->scale(), 0.5);
}

TEST(ShellView, AHostsSizeIsReadAsAScaleAndAProposalHeldToTheAspect)
{
    Shell s;
    s.shell->SetBounds({800, 400});
    EXPECT_DOUBLE_EQ(s.shell->scale(), 2.0);
    EXPECT_EQ(s.shell->zoom_view().bounds(), (RectI{800, 400}));
    // JUCE's VST3 wrapper on the shell's constrainer: too wide for the aspect, the width from the height...
    EXPECT_EQ(s.shell->Constrain({700.0f, 300.0f}).x, 600.0f);
    EXPECT_EQ(s.shell->Constrain({700.0f, 300.0f}).y, 300.0f);
    // ...too tall, the height from the width...
    EXPECT_EQ(s.shell->Constrain({500.0f, 400.0f}).y, 250.0f);
    // ...and each side to 50 to 200 %.
    EXPECT_EQ(s.shell->Constrain({4000.0f, 2000.0f}).x, 800.0f);
    EXPECT_EQ(s.shell->Constrain({10.0f, 5.0f}).y, 100.0f);
}

TEST(ShellView, AnOffAspectSizeLetterboxesThePanelCentred)
{
    Shell s;
    s.shell->SetBounds({600, 400});
    EXPECT_DOUBLE_EQ(s.shell->scale(), 1.5);
    EXPECT_EQ(s.shell->zoom_view().bounds(), (RectI{0, 50, 600, 300}));
    s.shell->SetBounds({700, 300});
    EXPECT_EQ(s.shell->zoom_view().bounds(), (RectI{50, 0, 600, 300}));
}

/** A canvas that keeps the transform in force and writes down every fill - its colour, its rectangle and the
    transform it was drawn under - and draws nothing. */
class FillRecorder final : public gfx::Canvas
{
  public:
    struct Fill
    {
        gfx::Color  color;
        gfx::RectF  rect;
        gfx::Affine transform;
    };

    std::vector<Fill> fills;
    std::vector<Fill> rounded_fills; ///< the rounded rectangles, apart from the rest

    void Save() override
    {
        saved_.push_back({transform_, color_});
    }

    void Restore() override
    {
        if (!saved_.empty())
        {
            transform_ = saved_.back().first;
            color_     = saved_.back().second;
            saved_.pop_back();
        }
    }

    void AddTransform(const gfx::Affine &t) override
    {
        transform_ = t.FollowedBy(transform_);
    }

    bool ClipToRect(RectI) override
    {
        return true;
    }

    void ClipToPath(const gfx::Path &, const gfx::Affine &) override
    {
    }

    RectI ClipBounds() const override
    {
        return {-10000, -10000, 20000, 20000};
    }

    float PhysicalPixelScale() const override
    {
        return 1.0f;
    }

    void SetColor(gfx::Color c) override
    {
        color_ = c;
    }

    void SetGradient(const gfx::Gradient &) override
    {
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const gfx::Font &f) override
    {
        font_ = f;
    }

    void SetResampling(gfx::Resampling) override
    {
    }

    const gfx::Font &CurrentFont() const override
    {
        return font_;
    }

    void FillAll() override
    {
        FillAll(color_);
    }

    void FillAll(gfx::Color c) override
    {
        fills.push_back({c, {-1.0e6f, -1.0e6f, 2.0e6f, 2.0e6f}, transform_});
    }

    void FillRect(RectI r) override
    {
        FillRect(r.ToFloat());
    }

    void FillRect(gfx::RectF r) override
    {
        fills.push_back({color_, r, transform_});
    }

    void DrawRect(gfx::RectF, float) override
    {
    }

    void FillRoundedRect(gfx::RectF r, float) override
    {
        rounded_fills.push_back({color_, r, transform_});
    }

    void DrawRoundedRect(gfx::RectF, float, float) override
    {
    }

    void FillEllipse(gfx::RectF) override
    {
    }

    void DrawEllipse(gfx::RectF, float) override
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

    void FillPath(const gfx::Path &, const gfx::Affine &) override
    {
    }

    void StrokePath(const gfx::Path &, const gfx::Stroke &, const gfx::Affine &) override
    {
    }

    void DrawText(std::string_view, gfx::RectF, gfx::Justify, bool) override
    {
    }

    void DrawFittedText(std::string_view, RectI, gfx::Justify, int, float) override
    {
    }

    float TextWidth(const gfx::Font &f, std::string_view text) const override
    {
        return 0.5f * f.height * static_cast<float>(text.size());
    }

    float Ascent(const gfx::Font &f) const override
    {
        return 0.8f * f.height;
    }

    float Descent(const gfx::Font &f) const override
    {
        return 0.2f * f.height;
    }

    void DrawImage(const gfx::Image &, gfx::RectF, bool) override
    {
    }

    void DrawImageTransformed(const gfx::Image &, const gfx::Affine &) override
    {
    }

    void BeginTransparencyLayer(float) override
    {
    }

    void EndTransparencyLayer() override
    {
    }

  private:
    gfx::Affine                                     transform_{};
    gfx::Color                                      color_{};
    gfx::Font                                       font_{};
    std::vector<std::pair<gfx::Affine, gfx::Color>> saved_;
};

/** The toy panel filling itself in a colour of its own as it paints. */
class FilledPanel final : public view::Panel
{
  public:
    static constexpr gfx::Color kFace{0xff336699};

    FilledPanel() : view::Panel(400, 200)
    {
    }

    void Paint(gfx::Canvas &c) override
    {
        c.SetColor(kFace);
        c.FillRect(LocalBounds());
    }
};

TEST(ShellView, PaintsTheLetterboxInItsColourAndThePanelUnderItsZoomWhereItSits)
{
    // The shell paints its colour across the window first - the strip beside a letterboxed panel - and the panel over
    // it: at 100 % under no transform at all, so the shell adds nothing to the panel's pixels; at a zoom under that
    // scale, moved to where the letterbox centres it.
    FilledPanel        panel;
    Store              store;
    window::ShellStyle style;
    style.letterbox = gfx::Color{0xff445566};
    window::ShellView shell(panel, style, store, [] { return 0.0; });
    Window            host(shell);

    FillRecorder at_one;
    view::PaintTree(shell, at_one);
    ASSERT_EQ(at_one.fills.size(), 2u);
    EXPECT_EQ(at_one.fills[0].color, style.letterbox) << "the letterbox's colour first, across the window";
    EXPECT_TRUE(at_one.fills[0].transform.IsIdentity());
    EXPECT_EQ(at_one.fills[1].color, FilledPanel::kFace) << "then the panel over it";
    EXPECT_EQ(at_one.fills[1].rect, (gfx::RectF{0.0f, 0.0f, 400.0f, 200.0f}));
    EXPECT_TRUE(at_one.fills[1].transform.IsIdentity()) << "at 100 %, no transform";

    shell.SetBounds({600, 400}); // 150 %, letterboxed 50 px from the top
    FillRecorder zoomed;
    view::PaintTree(shell, zoomed);
    ASSERT_EQ(zoomed.fills.size(), 2u);
    EXPECT_EQ(zoomed.fills[0].color, style.letterbox);
    const gfx::Affine t = zoomed.fills[1].transform;
    EXPECT_EQ(t.Apply({0.0f, 0.0f}), (PointF{0.0f, 50.0f})) << "the panel's corner at the letterbox's offset";
    EXPECT_EQ(t.Apply({400.0f, 200.0f}), (PointF{600.0f, 350.0f})) << "and its far corner scaled by 1.5 from it";
}

TEST(ShellView, WithinAPixelOfTheChosenScaleTheScaleStands)
{
    Shell s;
    s.shell->SetScale(1.37);
    s.shell->SetBounds({549, 275}); // a host's rounding of 548 by 274: a fit of 1.3725, a pixel's worth off
    EXPECT_DOUBLE_EQ(s.shell->scale(), 1.37) << "a pixel's rounding moves no scale";
    s.shell->SetBounds({600, 300});
    EXPECT_DOUBLE_EQ(s.shell->scale(), 1.5) << "beyond it, the fit is the scale";
}

TEST(ShellView, TheSizeRidesOnTheUiChildAndTheHostHearsWhenItSettles)
{
    Shell s;
    s.shell->SetScale(1.25);
    ASSERT_TRUE(s.store.ui.has_value());
    EXPECT_DOUBLE_EQ(s.store.ui->NumberOr("scale", 0.0), 1.25);
    s.Frames(0.3);
    EXPECT_EQ(s.store.told, 0) << "not before 400 ms";
    s.shell->SetScale(1.5);
    s.Frames(0.3);
    EXPECT_EQ(s.store.told, 0) << "400 ms after the last change";
    s.Frames(0.2);
    EXPECT_EQ(s.store.told, 1);
    s.Frames(1.0);
    EXPECT_EQ(s.store.told, 1) << "once";
    EXPECT_GT(s.panel.frames, 0) << "the panel's frames ride on the shell's";
}

TEST(ScrollBar, ItsThumbIsTheFillUnlessItsOwnerGivesItAColour)
{
    Bar b;
    EXPECT_EQ(b.bar.thumb_colour(), view::ScrollBar::kThumb) << "a bar as made: the V4 scheme's fill";
    FillRecorder plain;
    b.bar.Paint(plain);
    ASSERT_EQ(plain.rounded_fills.size(), 1u);
    EXPECT_EQ(plain.rounded_fills[0].color, view::ScrollBar::kThumb);

    const gfx::Color amber = gfx::Color::FromRgb(0xe2, 0xa2, 0x3e);
    b.bar.SetThumbColour(amber);
    EXPECT_EQ(b.bar.thumb_colour(), amber);
    FillRecorder lit;
    b.bar.Paint(lit);
    ASSERT_EQ(lit.rounded_fills.size(), 1u);
    EXPECT_EQ(lit.rounded_fills[0].color, amber);
    EXPECT_EQ(lit.rounded_fills[0].rect, plain.rounded_fills[0].rect) << "the colour alone moves";

    b.bar.MouseEnter(b.At(10.0f));
    FillRecorder over;
    b.bar.Paint(over);
    ASSERT_EQ(over.rounded_fills.size(), 1u);
    EXPECT_EQ(over.rounded_fills[0].color, amber.Brighter(0.25f)) << "a quarter brighter under the pointer";
}

TEST(ShellView, ScrollModeShowsThePanelAtItsSizeBehindScrollBars)
{
    Shell s([](Store &store) {
        store.scroll = true;
        store.Seed().Set("width", 330);
        store.Seed().Set("height", 150);
    });
    EXPECT_EQ(s.shell->WantedSize(), (PointI{330, 200})) << "the height held to the panel's 200 at least";
    EXPECT_FLOAT_EQ(s.shell->zoom_view().zoom(), 1.0f);
    EXPECT_EQ(s.shell->zoom_view().parent(), &s.shell->viewport());
    EXPECT_TRUE(s.shell->horizontal_bar().visible()) << "400 wide in 330";
    EXPECT_TRUE(s.shell->vertical_bar().visible()) << "and the horizontal bar leaves 190 for 200";
    EXPECT_EQ(s.shell->viewport().bounds(), (RectI{320, 190}));
    EXPECT_EQ(s.shell->vertical_bar().bounds(), (RectI{320, 0, 10, 190}));
    EXPECT_EQ(s.shell->horizontal_bar().bounds(), (RectI{0, 190, 320, 10}));
    EXPECT_EQ(s.shell->Constrain({1000.0f, 1000.0f}).x, 400.0f) << "no larger than the panel";
    EXPECT_EQ(s.shell->Constrain({10.0f, 10.0f}).x, 320.0f);

    s.shell->SetBounds({400, 200});
    EXPECT_FALSE(s.shell->horizontal_bar().visible());
    EXPECT_FALSE(s.shell->vertical_bar().visible());
    ASSERT_TRUE(s.store.ui.has_value());
    EXPECT_EQ((*s.store.ui)["width"].AsInt(), 400) << "the scroll window's size remembered";
}

TEST(ShellView, ABarDraggedScrollsThePanelAndTheWheelOverThePanelIsThePanels)
{
    Shell s([](Store &store) { store.scroll = true; });
    s.shell->SetBounds({320, 200});
    view::ScrollBar &bar = s.shell->horizontal_bar();
    ASSERT_TRUE(bar.visible());
    s.window().router.Down({5.0f, 195.0f}, 1, false, {}); // on the thumb, at the bar's start
    s.window().router.Drag({105.0f, 195.0f}, {});
    s.window().router.Up({105.0f, 195.0f}, {});
    EXPECT_GT(s.shell->view_position().x, 0);
    EXPECT_EQ(s.shell->zoom_view().bounds().x(), -s.shell->view_position().x) << "the panel moved under the window";

    const int before = s.shell->view_position().x;
    s.window().router.Wheel({30.0f, 70.0f}, 0.0f, -100.0f, {}); // on the knob, scrolled 90 under the window
    EXPECT_EQ(s.shell->view_position().x, before) << "the panel's wheel, taken by the knob";
    EXPECT_EQ(s.panel.knob.log.back(), "wheel -100.000000");
}

TEST(ShellView, AWheelNoViewOfThePanelUsesScrollsTheViewportAsJucesViewportDoes)
{
    // In scroll mode a wheel no view of the panel takes scrolls the panel under the window - fourteen steps of 16
    // pixels a notch, as JUCE's Viewport scrolls under a wheel no component uses; over a view that takes it, the knob,
    // the viewport stays. In scale mode there is nothing to scroll.
    Shell s([](Store &store) { store.scroll = true; });
    s.shell->SetBounds({320, 140});
    ASSERT_TRUE(s.shell->vertical_bar().visible());
    const PointI start = s.shell->view_position();
    EXPECT_TRUE(s.window().router.Wheel({20.0f, 20.0f}, 0.0f, -100.0f, {})) << "over the panel's bare plate";
    const PointI moved = s.shell->view_position();
    EXPECT_GT(moved.y, start.y) << "a notch toward the person scrolls down";
    EXPECT_EQ(s.shell->zoom_view().bounds().y(), -moved.y) << "the panel moved under the window";

    const PointF over_knob{120.0f - static_cast<float>(moved.x), 70.0f - static_cast<float>(moved.y)};
    EXPECT_TRUE(s.window().router.Wheel(over_knob, 0.0f, 100.0f, {}));
    EXPECT_EQ(s.panel.knob.log.back(), "wheel 100.000000") << "the knob's";
    EXPECT_EQ(s.shell->view_position(), moved) << "and the viewport stays";

    Shell scaled;
    EXPECT_FALSE(scaled.window().router.Wheel({20.0f, 20.0f}, 0.0f, -100.0f, {})) << "scaled to fit: nothing scrolls";
}

TEST(ShellView, AModeChosenMovesThePanelWritesThePreferenceAndAsksForTheSize)
{
    Shell s;
    s.shell->Perform(window::ShellView::kModeScrollItem);
    EXPECT_EQ(s.store.written, (std::vector<bool>{true}));
    EXPECT_EQ(s.shell->mode(), window::ShellView::Mode::kScroll);
    EXPECT_EQ(s.shell->zoom_view().parent(), &s.shell->viewport());
    s.shell->SetScale(1.5);
    EXPECT_DOUBLE_EQ(s.shell->scale(), 1.0) << "no sizes in scroll mode";
    s.shell->SetBounds({330, 200});
    s.shell->Perform(window::ShellView::kModeScaleItem);
    EXPECT_EQ(s.store.written, (std::vector<bool>{true, false}));
    EXPECT_EQ(s.shell->zoom_view().parent(), s.shell.get());
    EXPECT_EQ(s.asked.back(), (PointI{400, 200})) << "the scale the mode remembers";
    s.shell->Perform(window::ShellView::kModeScaleItem);
    EXPECT_EQ(s.store.written.size(), 3u)
        << "the preference written even where the mode stands, as the shell of the releases built on JUCE does";
}

TEST(ShellView, AScaleChosenComesBackWhenScaleModeIsChosenAfterScrollMode)
{
    // 150 % chosen, then scroll bars - the panel at 1x, the window the size the person drags it to - then scale to fit
    // again: the window asked for 150 % of the panel, as the shell of the releases built on JUCE asked for its
    // remembered scale on a change of mode.
    Shell s;
    s.shell->SetScale(1.5);
    EXPECT_EQ(s.asked.back(), (PointI{600, 300}));
    s.shell->Perform(window::ShellView::kModeScrollItem);
    EXPECT_FLOAT_EQ(s.shell->zoom_view().zoom(), 1.0f) << "scroll bars: the panel at its own size";
    s.shell->SetBounds({330, 180});
    s.shell->Perform(window::ShellView::kModeScaleItem);
    EXPECT_EQ(s.asked.back(), (PointI{600, 300})) << "the scale the mode remembers";
    s.shell->SetBounds({600, 300});
    EXPECT_DOUBLE_EQ(s.shell->scale(), 1.5);
    EXPECT_FLOAT_EQ(s.shell->zoom_view().zoom(), 1.5f);
}

TEST(ShellView, TheViewSubmenuNamesTheModesAndTheSizes)
{
    Shell      s;
    const auto items = s.shell->MenuItems();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].text, "View");
    EXPECT_EQ(items[0].id, 0) << "a submenu, as JUCE's addSubMenu makes one";
    const auto &sub = items[0].submenu;
    ASSERT_EQ(sub.size(), 8u);
    EXPECT_EQ(sub[0].text, "Scale to fit");
    EXPECT_EQ(sub[0].id, 2001);
    EXPECT_TRUE(sub[0].ticked);
    EXPECT_EQ(sub[1].text, "Scroll bars");
    EXPECT_EQ(sub[1].id, 2002);
    EXPECT_FALSE(sub[1].ticked);
    EXPECT_TRUE(sub[2].separator);
    EXPECT_EQ(sub[3].text, "75 %");
    EXPECT_EQ(sub[3].id, 2011);
    EXPECT_EQ(sub[4].text, "100 %");
    EXPECT_TRUE(sub[4].ticked);
    EXPECT_EQ(sub[7].text, "200 %");
    EXPECT_EQ(sub[7].id, 2015);
    s.shell->Perform(2014);
    EXPECT_TRUE(s.shell->MenuItems()[0].submenu[6].ticked) << "150 %";
    s.shell->Perform(window::ShellView::kModeScrollItem);
    for (int i = 3; i < 8; ++i)
    {
        EXPECT_FALSE(s.shell->MenuItems()[0].submenu[static_cast<std::size_t>(i)].enabled) << "greyed in scroll mode";
    }
}

TEST(ShellView, ATipShowsBesideThePointerAtTheShellsScaleNotThePanels)
{
    Shell s;
    s.shell->SetScale(2.0);
    // The knob at (100, 50, 40, 40) in the panel is (200, 100, 80, 80) in the shell.
    s.window().router.Move({240.0f, 140.0f}, {});
    s.Frames(0.4);
    EXPECT_FALSE(s.shell->tooltip().visible());
    s.Frames(0.4);
    ASSERT_TRUE(s.shell->tooltip().visible());
    EXPECT_EQ(s.shell->tooltip().text(), "The toy's knob.");
    const auto &layout = s.shell->tooltip().layout();
    // 15 characters at the estimate of a host with no canvas (half the height a character): 97.5, unzoomed.
    EXPECT_FLOAT_EQ(layout.width, 97.5f);
    EXPECT_EQ(s.shell->tooltip().bounds(), view::TooltipBounds(layout, {240, 140}, s.shell->LocalBounds()));
    EXPECT_EQ(s.shell->tooltip().bounds().height(), 19) << "13 and 6 whatever the zoom";

    s.window().router.Down({240.0f, 140.0f}, 1, false, {});
    s.window().router.Up({240.0f, 140.0f}, {});
    s.Frames(0.2);
    EXPECT_FALSE(s.shell->tooltip().visible()) << "a press dismisses it";

    s.window().router.Move({20.0f, 20.0f}, {}); // the panel's plate, no tip
    s.Frames(1.0);
    EXPECT_FALSE(s.shell->tooltip().visible());
}

TEST(ShellView, AViewWithNoTipOfItsOwnShowsItsNearestParentsTip)
{
    // The tip under the pointer is the view's own or the nearest of its parents' (view::TipOf), as JUCE's tooltip
    // window asks a component and then its parents: a part inside the knob that carries none shows the knob's.
    view::View outer;
    view::View middle;
    view::View inner;
    outer.tooltip = "Outer.";
    outer.AddChild(middle);
    middle.AddChild(inner);
    EXPECT_EQ(view::TipOf(&inner), "Outer.") << "two parents up";
    middle.tooltip = "Middle.";
    EXPECT_EQ(view::TipOf(&inner), "Middle.") << "the nearest";
    inner.tooltip = "Inner.";
    EXPECT_EQ(view::TipOf(&inner), "Inner.") << "its own first";
    EXPECT_EQ(view::TipOf(nullptr), "") << "nothing under the pointer";

    Shell s;
    Probe part;
    part.SetBounds({10, 10, 20, 20}); // inside the knob at (100, 50)
    s.panel.knob.AddChild(part);
    s.window().router.Move({120.0f, 70.0f}, {});
    s.Frames(0.8);
    ASSERT_TRUE(s.shell->tooltip().visible());
    EXPECT_EQ(s.shell->tooltip().text(), "The toy's knob.") << "the part's parent's tip";
    s.panel.knob.RemoveChild(part);
}

/** A panel 400 by 200 that changes its own design size, as a drawer opens. */
class GrowingPanel final : public view::Panel
{
  public:
    GrowingPanel() : view::Panel(400, 200)
    {
    }

    void ResizeTo(int width, int height)
    {
        SetDesignSize(width, height);
    }
};

TEST(ShellView, APanelThatResizesItselfIsFollowedAsTheJuceShellFollowsOne)
{
    GrowingPanel        panel;
    Store               store;
    double              now = 50.0;
    window::ShellView   shell(panel, window::ShellStyle{}, store, [&now] { return now; });
    std::vector<PointI> asked;
    shell.on_size = [&asked](int w, int h) { asked.push_back({w, h}); };
    Window host(shell);
    shell.SetScale(1.5);
    asked.clear();

    panel.ResizeTo(400, 300);
    EXPECT_EQ(asked, (std::vector<PointI>{{600, 450}})) << "the new design size at the scale in force";
    EXPECT_EQ(shell.bounds(), (RectI{600, 450})) << "laid out at it already";
    EXPECT_EQ(shell.zoom_view().bounds(), (RectI{600, 450}));
    EXPECT_EQ(panel.bounds(), (RectI{400, 300})) << "the panel at its design size, in its own pixels";
    EXPECT_DOUBLE_EQ(shell.scale(), 1.5) << "the scale stands";
    EXPECT_EQ(shell.Constrain({700.0f, 300.0f}).x, 400.0f) << "a host's proposal held to the new aspect, 4:3";
    EXPECT_EQ(shell.Constrain({4000.0f, 4000.0f}).y, 600.0f) << "and to 200 % of the new size";
    ASSERT_TRUE(store.ui.has_value());
    EXPECT_DOUBLE_EQ(store.ui->NumberOr("scale", 0.0), 1.5) << "the size in the session's ui, as before";

    panel.ResizeTo(400, 200);
    EXPECT_EQ(asked.back(), (PointI{600, 300}));
    panel.ResizeTo(400, 200);
    EXPECT_EQ(asked.size(), 2u) << "nothing when the size stands";

    // Scroll mode: the window it had, held to the new panel's limits, as the shell of the releases built on JUCE holds
    // it.
    shell.Perform(window::ShellView::kModeScrollItem);
    shell.SetBounds({330, 200});
    asked.clear();
    panel.ResizeTo(400, 300);
    EXPECT_EQ(asked, (std::vector<PointI>{{330, 240}})) << "the height up to the minimum of the taller panel";
    EXPECT_TRUE(shell.vertical_bar().visible()) << "the rest behind a bar";
    panel.ResizeTo(300, 200);
    EXPECT_EQ(asked.back(), (PointI{300, 200})) << "never larger than the panel";
}

TEST(ShellView, ALayoutUnderAStillPointerFindsThePanelsViewUnderItAgain)
{
    Shell s;
    // The pointer at (250, 130) stays there: on the other knob at 100 %; on the knob, (200, 100, 80, 80) in the shell,
    // at 200 %; on the strip beside the panel in a window 1000 wide, which letterboxes it at 100 % from x = 300.
    s.window().router.Move({250.0f, 130.0f}, {});
    s.Frames(0.8);
    ASSERT_TRUE(s.shell->tooltip().visible());
    EXPECT_EQ(s.shell->tooltip().text(), "The toy's other knob.");

    s.shell->SetBounds({800, 400}); // the host's size
    EXPECT_EQ(s.panel.other.exited, 1) << "the view moved out from under the pointer told it left...";
    EXPECT_EQ(s.panel.knob.entered, 1) << "...and the one moved under it that it entered, as by JUCE's fake move";
    s.Frames(0.15);
    ASSERT_TRUE(s.shell->tooltip().visible());
    EXPECT_EQ(s.shell->tooltip().text(), "The toy's knob.") << "the tip showing replaced at the next tick";

    s.shell->SetBounds({1000, 200});
    EXPECT_EQ(s.panel.knob.exited, 1) << "the panel moved out from under the pointer";
    s.Frames(0.15);
    EXPECT_FALSE(s.shell->tooltip().visible()) << "no tip on the strip";

    // A button held on the strip while the window doubles, which brings the knob under the pointer.
    s.window().router.Down({250.0f, 130.0f}, 1, false, {});
    s.shell->SetBounds({800, 400});
    EXPECT_EQ(s.panel.knob.entered, 1) << "nothing while a button is held, as JUCE fakes no move during a drag";
    s.window().router.Up({250.0f, 130.0f}, {});
    EXPECT_EQ(s.panel.knob.entered, 2) << "the release finds the view under the pointer";
}

// ---- The licence ------------------------------------------------------------------------------------------------

/** The test vectors' first key and licences (licence/tests/vectors.json), and a store on them in a folder of
    the test's own. */
struct LicenceRig
{
    LicenceRig()
    {
        core::json::ParseResult parsed = core::json::Parse(core::ReadBytes(RECKLESS_LICENCE_VECTORS).value_or(""));
        EXPECT_TRUE(parsed.IsOk());
        const core::json::Value &key = parsed.value.Find("keys")->AsArray()->front();
        const std::string        hex = key.StringOr("public", "");
        for (std::size_t i = 0; i < 32; ++i)
        {
            keys[0].bytes[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
        }
        keys[0].id        = "test-1";
        good              = parsed.value.Find("licences")->AsArray()->front().StringOr("text", "");
        rules.keys        = keys;
        rules.suite_major = 1;
        folder            = reckless::test::ScratchPath("reckless-shell-licence");
        std::filesystem::remove_all(folder);
        store = std::make_unique<licence::Store>(folder / "Licence.json", folder / "machine.json", rules);
    }

    ~LicenceRig()
    {
        store.reset();
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }

    std::array<licence::PublicKey, 1> keys{};
    licence::Rules                    rules;
    std::string                       good;
    std::filesystem::path             folder;
    std::unique_ptr<licence::Store>   store;
};

/** An update check on a scripted transport that names 1.2.0, a clock of the rig's own and a file in a folder of its
    own, this build 1.1.5. */
struct UpdateRig
{
    /** A transport that counts its requests and answers the manifest. */
    class Transport final : public reckless::net::Transport
    {
      public:
        explicit Transport(std::atomic<int> &requests) : requests_(requests)
        {
        }

        reckless::net::Response Send(const reckless::net::Request &, reckless::net::Cancellation &) override
        {
            ++requests_;
            reckless::net::Response response;
            response.status = 200;
            response.body   = R"({"version": "1.2.0", "page": "https://recklesstechnology.com/plugins/download/"})";
            return response;
        }

      private:
        std::atomic<int> &requests_;
    };

    UpdateRig()
    {
        folder = reckless::test::ScratchPath(
            "reckless-shell-update-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);
        checker = std::make_unique<reckless::update::Checker>(
            folder / "Updates.json", reckless::update::Version{1, 1, 5},
            "https://recklesstechnology.com/updates/latest.json",
            [this] { return std::make_unique<Transport>(requests); }, [this] { return clock; });
    }

    ~UpdateRig()
    {
        checker.reset();
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }

    std::atomic<int>                           requests{0};
    double                                     clock{1.0e9};
    std::filesystem::path                      folder;
    std::unique_ptr<reckless::update::Checker> checker;
};

/** The texts of a menu's items, in order. */
std::vector<std::string> TextsOf(const std::vector<view::MenuItem> &items)
{
    std::vector<std::string> texts;
    for (const auto &item : items)
    {
        texts.push_back(item.separator ? "-" : item.text);
    }
    return texts;
}

TEST(ShellView, TheUpdatesSubmenuIsOfferedOnlyWithACheckToShow)
{
    Shell s;
    EXPECT_EQ(s.shell->MenuItems().size(), 1u) << "no check: the View submenu alone";
    UpdateRig rig;
    s.shell->SetUpdates(rig.checker.get(), {});
    const auto items = s.shell->MenuItems();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1].text, "Updates");
    EXPECT_EQ(TextsOf(items[1].submenu), (std::vector<std::string>{"Version 1.1.5, not checked yet", "-",
                                                                   "Check for updates now", "Check automatically"}));
    EXPECT_FALSE(items[1].submenu[0].enabled) << "the status is a line to read";
    EXPECT_TRUE(items[1].submenu[3].ticked) << "on by default";
}

TEST(ShellView, TheShellChecksOnlyWhileItsPanelShowsAndOffersTheNewerReleasesPage)
{
    Shell                    s;
    UpdateRig                rig;
    std::vector<std::string> opened;
    s.shell->SetUpdates(rig.checker.get(), [&](const std::string &url) { opened.push_back(url); });

    s.panel.SetShown(false);
    s.Frames(3.0);
    EXPECT_EQ(rig.requests, 0) << "no window on a screen: no check";

    s.panel.SetShown(true);
    for (int i = 0; i < 400 && rig.checker->state() != reckless::update::State::kAvailable; ++i)
    {
        s.Frames(1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(rig.requests, 1);
    const auto items = s.shell->MenuItems();
    EXPECT_EQ(TextsOf(items[1].submenu), (std::vector<std::string>{"Version 1.2.0 is out", "Get version 1.2.0...", "-",
                                                                   "Check for updates now", "Check automatically"}));
    s.shell->Perform(window::ShellView::kUpdatePageItem);
    EXPECT_EQ(opened, (std::vector<std::string>{"https://recklesstechnology.com/plugins/download/"}));

    s.shell->Perform(window::ShellView::kCheckDailyItem);
    EXPECT_FALSE(rig.checker->automatic()) << "Check automatically, off";
    EXPECT_FALSE(s.shell->MenuItems()[1].submenu[4].ticked);
}

TEST(ShellView, TheLicenceItemIsOfferedOnlyWithALicenceToShow)
{
    Shell s;
    EXPECT_EQ(s.shell->MenuItems().size(), 1u) << "no licence: the View submenu alone";
    LicenceRig rig;
    s.shell->SetLicence(rig.store.get(), {});
    const auto items = s.shell->MenuItems();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1].text, "Licence...");
    EXPECT_EQ(items[1].id, window::ShellView::kLicenceItem);
    EXPECT_EQ(window::ShellView::kLicenceItem, 2201);
}

TEST(ShellView, TheLicenceCardInstallsAPastedLicenceAndRefusesAnAlteredOne)
{
    Shell                    s;
    LicenceRig               rig;
    std::vector<std::string> opened;
    s.shell->SetLicence(rig.store.get(), [&](const std::string &url) { opened.push_back(url); });
    s.shell->Perform(window::ShellView::kLicenceItem);
    window::LicenceDialog *card = s.shell->licence_dialog();
    ASSERT_NE(card, nullptr);
    EXPECT_EQ(card->lines().front(), "Not licensed");
    EXPECT_FALSE(card->remove_button().visible());

    // An altered licence: refused in a person's words, nothing written.
    std::string       altered = rig.good; // a character of the signature: the payload still reads
    const std::size_t at      = altered.size() - 5;
    altered[at]               = altered[at] == 'A' ? 'B' : 'A';
    EXPECT_NE(card->InstallText(altered).verdict, licence::Verdict::kValid);
    EXPECT_EQ(card->note(), licence::VerdictText(licence::Verdict::kBadSignature));
    EXPECT_FALSE(std::filesystem::exists(rig.store->user_file()));

    // A licence pasted as an email wraps it: the field keeps what a licence is made of, and Return installs it.
    s.window().clipboard = rig.good.substr(0, 60) + "\r\n" + rig.good.substr(60) + "\n";
    card->field().GrabKeyboardFocus();
    view::KeyEvent paste;
    paste.key               = view::Key::kCharacter;
    paste.character         = U'v';
    paste.modifiers.command = true;
    EXPECT_TRUE(s.window().router.Key(paste));
    EXPECT_EQ(card->field().text(), rig.good);
    view::KeyEvent enter;
    enter.key = view::Key::kReturn;
    s.window().router.Key(enter);
    EXPECT_TRUE(rig.store->licensed());
    EXPECT_EQ(card->note(), "Installed. Thank you.");
    EXPECT_EQ(card->lines().front(), "Licensed to Jo Buyer");
    EXPECT_TRUE(card->remove_button().visible());

    // Remove takes it off the machine: a click on the button, the card over the whole shell.
    const RectI       remove = card->remove_button().bounds();
    const gfx::PointF centre{static_cast<float>(remove.x() + remove.width() / 2),
                             static_cast<float>(remove.y() + remove.height() / 2)};
    s.window().router.Down(centre, 1, false, {});
    s.window().router.Up(centre, {});
    EXPECT_FALSE(rig.store->licensed());
    EXPECT_FALSE(std::filesystem::exists(rig.store->user_file()));
}

TEST(ShellView, EscapeClosesTheLicenceCardAndItCanOpenAgain)
{
    Shell      s;
    LicenceRig rig;
    s.shell->SetLicence(rig.store.get(), {});
    s.shell->Perform(window::ShellView::kLicenceItem);
    ASSERT_NE(s.shell->licence_dialog(), nullptr);
    view::KeyEvent escape;
    escape.key = view::Key::kEscape;
    s.window().router.Key(escape);
    EXPECT_EQ(s.shell->licence_dialog(), nullptr) << "a host with no loop closes it at once";
    s.shell->Perform(window::ShellView::kLicenceItem);
    EXPECT_NE(s.shell->licence_dialog(), nullptr);
}
} // namespace
