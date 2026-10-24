// The suite's views with no toolkit round them (view/View.h): the tree paints in order, the router hands a
// window's mouse, keys and dropped files to the right view and forgets a view that leaves, and the host hears the
// pointer's shape and whether a view has the keyboard; then the views a panel is built from - among them the line a
// value box is typed into (TextField.h), its keys and its pointer as the JUCE editor's: words, the ends, the selection
// by keys and by the pointer, Return and Escape.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "View.h"

namespace
{
using reckless::gfx::Affine;
using reckless::gfx::Canvas;
using reckless::gfx::Color;
using reckless::gfx::PointF;
using reckless::gfx::PointI;
using reckless::gfx::RectF;
using reckless::gfx::RectI;
namespace view = reckless::view;

/** A canvas that draws nothing and records the fills it is asked for, each as the rectangle in the root's
    coordinates - the transform's translation applied - so a test reads where each view painted; and the fitted text
    and the layers it is asked for. */
class RecordingCanvas final : public Canvas
{
  public:
    struct Text
    {
        std::string text;
        RectI       area;
        int         lines{0};
        Color       colour;
    };

    std::vector<RectI> fills;
    std::vector<Text>  texts;
    std::vector<float> layers;

    void Save() override
    {
        stack_.push_back(state_);
    }

    void Restore() override
    {
        state_ = stack_.back();
        stack_.pop_back();
    }

    void AddTransform(const Affine &t) override
    {
        state_.dx += static_cast<int>(t.m02);
        state_.dy += static_cast<int>(t.m12);
    }

    bool ClipToRect(RectI r) override
    {
        state_.clip = state_.clip.Intersection(r.Translated(state_.dx, state_.dy));
        return !state_.clip.IsEmpty();
    }

    void ClipToPath(const reckless::gfx::Path &, const Affine &) override
    {
    }

    /** The rectangles left out of the clip, where they land on the canvas; the clip itself kept whole. */
    std::vector<RectI> excluded;

    bool ExcludeClipRect(RectI r) override
    {
        excluded.push_back(r.Translated(state_.dx, state_.dy));
        return !state_.clip.IsEmpty();
    }

    RectI ClipBounds() const override
    {
        return state_.clip.Translated(-state_.dx, -state_.dy);
    }

    float PhysicalPixelScale() const override
    {
        return 1.0f;
    }

    void SetColor(Color c) override
    {
        colour_ = c;
    }

    void SetGradient(const reckless::gfx::Gradient &) override
    {
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const reckless::gfx::Font &f) override
    {
        font_ = f;
    }

    void SetResampling(reckless::gfx::Resampling) override
    {
    }

    const reckless::gfx::Font &CurrentFont() const override
    {
        return font_;
    }

    void FillAll() override
    {
        fills.push_back(state_.clip);
    }

    void FillAll(Color) override
    {
        FillAll();
    }

    void FillRect(RectI r) override
    {
        fills.push_back(r.Translated(state_.dx, state_.dy));
    }

    void FillRect(RectF r) override
    {
        FillRect(r.ToNearestInt());
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

    void FillPath(const reckless::gfx::Path &, const Affine &) override
    {
    }

    void StrokePath(const reckless::gfx::Path &, const reckless::gfx::Stroke &, const Affine &) override
    {
    }

    void DrawText(std::string_view, RectF, reckless::gfx::Justify, bool) override
    {
    }

    void DrawFittedText(std::string_view text, RectI area, reckless::gfx::Justify, int lines, float) override
    {
        texts.push_back({std::string(text), area, lines, colour_});
    }

    float TextWidth(const reckless::gfx::Font &, std::string_view text) const override
    {
        return static_cast<float>(text.size());
    }

    float Ascent(const reckless::gfx::Font &f) const override
    {
        return f.height * 0.8f;
    }

    float Descent(const reckless::gfx::Font &f) const override
    {
        return f.height * 0.2f;
    }

    void DrawImage(const reckless::gfx::Image &, RectF, bool) override
    {
    }

    void DrawImageTransformed(const reckless::gfx::Image &, const Affine &) override
    {
    }

    void BeginTransparencyLayer(float opacity) override
    {
        layers.push_back(opacity);
        Save();
    }

    void EndTransparencyLayer() override
    {
        Restore();
    }

  private:
    struct State
    {
        int   dx{0};
        int   dy{0};
        RectI clip{0, 0, 10000, 10000};
    };

    State               state_;
    std::vector<State>  stack_;
    reckless::gfx::Font font_;
    Color               colour_;
};

/** A view that fills itself and logs what reaches it. */
class Probe : public view::View
{
  public:
    explicit Probe(std::string name, bool wants_keys = false) : name_(std::move(name)), wants_keys_(wants_keys)
    {
    }

    std::vector<std::string> log;
    std::vector<std::string> files;

    void Paint(Canvas &c) override
    {
        c.FillRect(LocalBounds());
    }

    void MouseDown(const view::MouseEvent &e) override
    {
        log.push_back("down " + std::to_string(static_cast<int>(e.position.x)) + "," +
                      std::to_string(static_cast<int>(e.position.y)));
    }

    void MouseDrag(const view::MouseEvent &e) override
    {
        log.push_back("drag " + std::to_string(static_cast<int>(e.position.x)));
    }

    void MouseUp(const view::MouseEvent &) override
    {
        log.push_back("up");
    }

    void MouseEnter(const view::MouseEvent &) override
    {
        log.push_back("enter");
    }

    void MouseExit(const view::MouseEvent &) override
    {
        log.push_back("exit");
    }

    bool WantsKeyboardFocus() const override
    {
        return wants_keys_;
    }

    void FocusGained() override
    {
        log.push_back("focus");
    }

    void FocusLost() override
    {
        log.push_back("blur");
    }

    bool KeyPressed(const view::KeyEvent &key) override
    {
        log.push_back(key.key == view::Key::kCharacter ? std::string("key ") + static_cast<char>(key.character)
                                                       : std::string("key named"));
        return true;
    }

    bool IsInterestedInFiles(const std::vector<std::string> &paths) const override
    {
        return !paths.empty() && paths.front().ends_with(".wav");
    }

    void FilesDropped(const std::vector<std::string> &paths, PointF) override
    {
        files = paths;
    }

    void FileDragEnter(const std::vector<std::string> &, PointF position) override
    {
        log.push_back("drag in " + std::to_string(static_cast<int>(position.x)) + "," +
                      std::to_string(static_cast<int>(position.y)));
    }

    void FileDragMove(const std::vector<std::string> &, PointF position) override
    {
        log.push_back("drag over " + std::to_string(static_cast<int>(position.x)) + "," +
                      std::to_string(static_cast<int>(position.y)));
    }

    void FileDragExit() override
    {
        log.push_back("drag out");
    }

  private:
    std::string name_;
    bool        wants_keys_;
};

/** A host that records what the views ask of it, and forwards a leaving view to the router, as a window does. */
class RecordingHost : public view::Host
{
  public:
    view::MouseRouter *router{nullptr};
    std::vector<RectI> invalid;
    view::Cursor       cursor{view::Cursor::kArrow};
    int                focus_changes{0};
    bool               keyboard{false};

    void Invalidate(RectI area) override
    {
        invalid.push_back(area);
    }

    void SetCursor(view::Cursor c) override
    {
        cursor = c;
    }

    void RequestKeyboardFocus(view::View *v) override
    {
        if (router != nullptr)
        {
            router->Focus(v);
        }
    }

    void KeyboardFocusChanged(bool has) override
    {
        ++focus_changes;
        keyboard = has;
    }

    void ViewLeaving(const view::View &v) override
    {
        if (router != nullptr)
        {
            router->Forget(v);
        }
    }
};

struct Scene
{
    // The host and the router first, so they outlive the views, which tell the host as they go (a window clears the
    // root's host before it goes, as these never need to).
    RecordingHost     host;
    view::MouseRouter router{root, &host};
    Probe             root{"root"};
    Probe             left{"left"};
    Probe             right{"right", true};
    Probe             inner{"inner"};

    Scene()
    {
        root.SetBounds({200, 100});
        left.SetBounds({10, 10, 80, 80});
        right.SetBounds({110, 10, 80, 80});
        inner.SetBounds({10, 10, 20, 20});
        root.AddChild(left);
        root.AddChild(right);
        right.AddChild(inner);
        root.SetHost(&host);
        host.router = &router;
    }
};
} // namespace

TEST(View, TheTreePaintsBackToFrontEachChildAtItsOrigin)
{
    Scene           s;
    RecordingCanvas canvas;
    view::PaintTree(s.root, canvas);
    ASSERT_EQ(canvas.fills.size(), 4u);
    EXPECT_EQ(canvas.fills[0], (RectI{0, 0, 200, 100})) << "the root first";
    EXPECT_EQ(canvas.fills[1], (RectI{10, 10, 80, 80}));
    EXPECT_EQ(canvas.fills[2], (RectI{110, 10, 80, 80}));
    EXPECT_EQ(canvas.fills[3], (RectI{120, 20, 20, 20})) << "a grandchild at its parent's origin plus its own";

    s.left.SetVisible(false);
    RecordingCanvas again;
    view::PaintTree(s.root, again);
    EXPECT_EQ(again.fills.size(), 3u) << "a hidden view and its children are not painted";
}

TEST(View, AViewLessThanOpaquePaintsInALayerAtItsAlphaAndNoneAtAll)
{
    Scene s;
    s.left.SetAlpha(0.5f);
    EXPECT_FLOAT_EQ(s.left.alpha(), 128.0f / 255.0f) << "in 256 steps, half rounded to even";
    RecordingCanvas canvas;
    view::PaintTree(s.root, canvas);
    EXPECT_EQ(canvas.layers, (std::vector<float>{128.0f / 255.0f})) << "the one child, its own layer";
    EXPECT_EQ(canvas.fills.size(), 4u) << "and everything still painted";

    s.left.SetAlpha(0.0f);
    RecordingCanvas none;
    view::PaintTree(s.root, none);
    EXPECT_TRUE(none.layers.empty());
    EXPECT_EQ(none.fills.size(), 3u) << "a view with no opacity is not painted at all";
}

TEST(View, AnOpaqueViewIsCutOutOfWhatPaintsUnderIt)
{
    // As JUCE clips round an opaque component: with no view opaque nothing is cut out; an opaque grandchild is cut out
    // of the root's painting and its parent's, where it lands on the canvas; an opaque child out of the root's painting
    // and each earlier sibling's; a child drawn less than whole covers nothing of its parent, but still its earlier
    // siblings.
    Scene           s;
    RecordingCanvas none;
    view::PaintTree(s.root, none);
    EXPECT_TRUE(none.excluded.empty());

    s.inner.SetOpaque(true);
    RecordingCanvas grandchild;
    view::PaintTree(s.root, grandchild);
    EXPECT_EQ(grandchild.excluded, (std::vector<RectI>{{120, 20, 20, 20}, {120, 20, 20, 20}}))
        << "out of the root's painting and out of its parent's";
    EXPECT_EQ(grandchild.fills.size(), 4u) << "everything still painted";

    s.inner.SetOpaque(false);
    s.right.SetOpaque(true);
    RecordingCanvas child;
    view::PaintTree(s.root, child);
    EXPECT_EQ(child.excluded, (std::vector<RectI>{{110, 10, 80, 80}, {110, 10, 80, 80}}))
        << "out of the root's painting and out of the sibling under it";

    s.right.SetAlpha(0.5f);
    RecordingCanvas faded;
    view::PaintTree(s.root, faded);
    EXPECT_EQ(faded.excluded, (std::vector<RectI>{{110, 10, 80, 80}})) << "its earlier sibling's alone";
}

TEST(View, NothingTakesThePointerOutsideTheRootOrUnderAHiddenOne)
{
    Scene s;
    EXPECT_EQ(view::ViewAt(s.root, {30, 30}), &s.left);
    EXPECT_EQ(view::ViewAt(s.root, {500, 30}), nullptr) << "outside the root";
    s.root.SetVisible(false);
    EXPECT_EQ(view::ViewAt(s.root, {30, 30}), nullptr) << "a hidden root takes nothing";
}

TEST(View, AViewIsEnabledWhileItAndEveryParentAre)
{
    Scene s;
    EXPECT_TRUE(s.inner.IsEnabled());
    s.right.SetEnabled(false);
    EXPECT_FALSE(s.right.IsEnabled());
    EXPECT_FALSE(s.inner.IsEnabled()) << "under a disabled parent";
    EXPECT_TRUE(s.left.IsEnabled());
    s.right.SetEnabled(true);
    EXPECT_TRUE(s.inner.IsEnabled());
}

TEST(View, APressStaysWithItsViewWhereverThePointerGoes)
{
    Scene s;
    s.router.Down({20.0f, 30.0f}, 1, false, {});
    s.router.Drag({150.0f, 30.0f}, {});
    s.router.Up({150.0f, 30.0f}, {});
    const std::vector<std::string> expected{"enter", "down 10,20", "drag 140", "up", "exit"};
    EXPECT_EQ(s.left.log, expected) << "the drag reaches the pressed view in its own coordinates";
    EXPECT_EQ(s.right.log, (std::vector<std::string>{"enter"})) << "the release over another view enters it";
}

TEST(View, APressGivesTheKeyboardToTheFirstViewThatWantsItAndKeysGoThere)
{
    Scene s;
    s.router.Down({125.0f, 25.0f}, 1, false, {}); // on the inner view, inside the view that wants keys
    s.router.Up({125.0f, 25.0f}, {});
    EXPECT_EQ(s.router.focused(), &s.right) << "the inner view does not want keys; its parent does";
    EXPECT_TRUE(s.host.keyboard) << "the host takes the platform's keyboard";
    EXPECT_TRUE(s.router.Key({view::Key::kCharacter, U'a', {}}));
    EXPECT_EQ(s.right.log.back(), "key a");

    s.router.Down({20.0f, 20.0f}, 1, false, {}); // a view that does not want keys
    EXPECT_EQ(s.router.focused(), nullptr) << "a press elsewhere takes the keyboard away";
    EXPECT_FALSE(s.host.keyboard);
    EXPECT_EQ(s.right.log.back(), "blur");
    EXPECT_FALSE(s.router.Key({view::Key::kEscape, 0, {}})) << "with no view holding it, a key is not taken";
}

TEST(View, AViewCanTakeTheKeyboardItself)
{
    Scene s;
    s.right.GrabKeyboardFocus();
    EXPECT_EQ(s.router.focused(), &s.right);
    EXPECT_TRUE(s.host.keyboard);
    EXPECT_EQ(s.right.log.back(), "focus");
}

/** A view that takes the wheel or hands it on, writing down each wheel it is offered. */
class WheelProbe final : public view::View
{
  public:
    explicit WheelProbe(bool uses) : uses_(uses)
    {
    }

    std::vector<float> offered;

    bool MouseWheel(const view::MouseEvent &e) override
    {
        offered.push_back(e.wheel_y);
        return uses_;
    }

  private:
    bool uses_;
};

TEST(View, TheWheelGoesUpFromTheViewUnderItUntilOneUsesIt)
{
    // As a JUCE component hands a wheel it has no use for to its parent: the view under the pointer offered it first,
    // then its parents in turn, the first that uses it the last; the router says whether any did, for the window to
    // hand an unused one on - a viewport round the panel scrolls under it.
    WheelProbe root(false);
    WheelProbe outer(true);
    WheelProbe inner(false);
    root.SetBounds({100, 100});
    outer.SetBounds({10, 10, 60, 60});
    inner.SetBounds({10, 10, 20, 20});
    root.AddChild(outer);
    outer.AddChild(inner);
    view::MouseRouter router(root);
    EXPECT_TRUE(router.Wheel({25.0f, 25.0f}, 0.0f, 100.0f, {}));
    EXPECT_EQ(inner.offered, (std::vector<float>{100.0f})) << "the view under the pointer first";
    EXPECT_EQ(outer.offered, (std::vector<float>{100.0f})) << "then its parent, which uses it";
    EXPECT_TRUE(root.offered.empty()) << "and no further";
    EXPECT_FALSE(router.Wheel({90.0f, 90.0f}, 0.0f, -100.0f, {})) << "over the root alone, which hands it on";
    EXPECT_EQ(root.offered, (std::vector<float>{-100.0f}));
}

TEST(View, ARightPressIsRightToItsReleaseWhateverComesBetween)
{
    // A right press's drags and its release say right: a wheel notch or a move during the press is an event of its
    // own, which says nothing of the press - so a right click on a knob's box that rolls the wheel on the way is still
    // a right click, and opens no editor.
    struct Pressed final : view::View
    {
        std::vector<std::string> log;

        void MouseDown(const view::MouseEvent &e) override
        {
            log.push_back(e.right ? "down right" : "down left");
        }

        void MouseDrag(const view::MouseEvent &e) override
        {
            log.push_back(e.right ? "drag right" : "drag left");
        }

        void MouseUp(const view::MouseEvent &e) override
        {
            log.push_back(e.right ? "up right" : "up left");
        }

        bool MouseWheel(const view::MouseEvent &) override
        {
            log.push_back("wheel");
            return false; // handed up, as a box hands its knob the wheel
        }
    };

    view::View root;
    Pressed    pressed;
    root.SetBounds({100, 100});
    pressed.SetBounds({10, 10, 50, 50});
    root.AddChild(pressed);
    view::MouseRouter router(root);
    router.Down({20.0f, 20.0f}, 1, true, {});
    router.Wheel({20.0f, 20.0f}, 0.0f, 100.0f, {});
    router.Move({22.0f, 20.0f}, {});
    router.Drag({25.0f, 20.0f}, {});
    router.Up({25.0f, 20.0f}, {});
    EXPECT_EQ(pressed.log, (std::vector<std::string>{"down right", "wheel", "drag right", "up right"}));
    router.Down({20.0f, 20.0f}, 1, false, {});
    router.Up({20.0f, 20.0f}, {});
    EXPECT_EQ(pressed.log.back(), "up left") << "and the next press is its own";
}

TEST(View, TheRouterForgetsAViewThatLeavesOrGoes)
{
    Scene s;
    s.router.Move({125.0f, 25.0f}, {});
    ASSERT_EQ(s.router.hovered(), &s.inner);
    s.right.GrabKeyboardFocus();
    s.root.RemoveChild(s.right); // the hovered view's parent, and the focused view, leave
    EXPECT_EQ(s.router.hovered(), nullptr) << "a view under the one leaving is forgotten too";
    EXPECT_EQ(s.router.focused(), nullptr);
    EXPECT_FALSE(s.host.keyboard) << "the host gives the platform's keyboard back";

    auto going = std::make_unique<Probe>("going");
    going->SetBounds({0, 0, 50, 50});
    s.root.AddChild(*going);
    s.router.Down({5.0f, 5.0f}, 1, false, {});
    ASSERT_EQ(s.router.pressed(), going.get());
    going.reset(); // destroyed mid-press
    EXPECT_EQ(s.router.pressed(), nullptr);
    s.router.Drag({6.0f, 6.0f}, {}); // nothing left to reach: no use of the destroyed view
    s.router.Up({6.0f, 6.0f}, {});
}

TEST(View, ThePointerTakesTheShapeOfTheViewUnderIt)
{
    Scene s;
    s.left.cursor  = view::Cursor::kUpDown;
    s.right.cursor = view::Cursor::kPointingHand;
    s.router.Move({20.0f, 20.0f}, {});
    EXPECT_EQ(s.host.cursor, view::Cursor::kUpDown);
    s.router.Move({150.0f, 50.0f}, {});
    EXPECT_EQ(s.host.cursor, view::Cursor::kPointingHand);

    s.router.Down({20.0f, 20.0f}, 1, false, {});
    s.router.Move({150.0f, 50.0f}, {}); // a move during a press: the pressed view keeps the shape
    EXPECT_EQ(s.host.cursor, view::Cursor::kUpDown);
}

TEST(View, DroppedFilesGoToTheViewUnderThemThatTakesThem)
{
    Scene                          s;
    const std::vector<std::string> wav{"/tmp/room.wav"};
    const std::vector<std::string> text{"/tmp/notes.txt"};
    EXPECT_TRUE(s.router.WouldTakeFiles({20.0f, 20.0f}, wav));
    EXPECT_FALSE(s.router.WouldTakeFiles({20.0f, 20.0f}, text));
    EXPECT_TRUE(s.router.DropFiles({125.0f, 25.0f}, wav)) << "the inner view takes them";
    EXPECT_EQ(s.inner.files, wav);
    EXPECT_TRUE(s.right.files.empty());
}

TEST(View, FilesDraggedOverTheWindowTellTheViewTheyAreOverAsTheDragComesAndGoes)
{
    // Each move of a drag reaches the view under it that takes the files, at the point in its own coordinates: told
    // the drag came in, then each move over it, and told it left as the drag goes on to another view, leaves the
    // window or ends with nothing dropped; a drop ends the drag with no word of leaving, as JUCE's drop target hears
    // none.
    Scene                          s;
    const std::vector<std::string> wav{"/tmp/room.wav"};
    const std::vector<std::string> text{"/tmp/notes.txt"};
    EXPECT_TRUE(view::AnyViewTakesFiles(s.root, wav));
    EXPECT_FALSE(view::AnyViewTakesFiles(s.root, text));

    EXPECT_TRUE(s.router.DragFiles({20.0f, 20.0f}, wav));
    EXPECT_EQ(s.left.log.back(), "drag in 10,10");
    EXPECT_EQ(s.router.dragged_over(), &s.left);
    const std::size_t once = s.left.log.size();
    EXPECT_TRUE(s.router.DragFiles({30.0f, 25.0f}, wav));
    EXPECT_EQ(s.left.log.size(), once + 1) << "a move within the view is a move, not a second coming";
    EXPECT_EQ(s.left.log.back(), "drag over 20,15");
    EXPECT_TRUE(s.router.DragFiles({125.0f, 25.0f}, wav));
    EXPECT_EQ(s.left.log.back(), "drag out");
    EXPECT_EQ(s.inner.log.back(), "drag in 5,5") << "the deepest view that takes them";
    EXPECT_FALSE(s.router.DragFiles({125.0f, 25.0f}, text)) << "files none takes";
    EXPECT_EQ(s.inner.log.back(), "drag out");
    EXPECT_EQ(s.router.dragged_over(), nullptr);

    s.router.DragFiles({125.0f, 25.0f}, wav);
    s.router.EndFileDrag();
    EXPECT_EQ(s.inner.log.back(), "drag out") << "the drag left the window";
    s.router.EndFileDrag();
    EXPECT_EQ(std::count(s.inner.log.begin(), s.inner.log.end(), "drag out"), 2) << "told once";

    s.router.DragFiles({20.0f, 20.0f}, wav);
    EXPECT_TRUE(s.router.DropFiles({20.0f, 20.0f}, wav));
    EXPECT_EQ(s.left.files, wav);
    EXPECT_EQ(s.left.log.back(), "drag in 10,10") << "a drop: no word of leaving";
    EXPECT_EQ(s.router.dragged_over(), nullptr);
    s.router.EndFileDrag();
    EXPECT_EQ(s.left.log.back(), "drag in 10,10");

    s.router.DragFiles({125.0f, 25.0f}, wav);
    s.root.RemoveChild(s.right); // the view the drag is over leaves the tree
    EXPECT_EQ(s.router.dragged_over(), nullptr) << "forgotten with the view";
}

// ---- Menus (view/Menu.h) ---------------------------------------------------------------------------------

#include "Menu.h"

namespace
{
/** A popup that is a host in turn, as a real popup's window is: its content's events are routed to it by the test,
    and it opens nested popups at screen points offset from its own. CallSoon queues, as a window's loop does, until
    the test drains it. */
class FakePopup;

class PopupHost : public view::Host
{
  public:
    explicit PopupHost(PointI screen_origin) : origin(screen_origin)
    {
    }

    PointI                              origin;
    std::vector<FakePopup *>           *opened{nullptr};
    std::vector<std::function<void()>> *queue{nullptr};

    void Invalidate(RectI) override
    {
    }

    std::unique_ptr<view::Popup> OpenPopup(view::View &content, PointI at, std::function<void(PointI)> pressed_outside,
                                           bool takes_mouse) override;

    void CallSoon(std::function<void()> call) override
    {
        queue->push_back(std::move(call));
    }
};

class FakePopup final : public view::Popup, public PopupHost
{
  public:
    FakePopup(view::View &content, PointI screen, std::function<void(PointI)> outside,
              std::vector<FakePopup *> &opened_list, std::vector<std::function<void()>> &calls)
        : PopupHost(screen), pressed_outside(std::move(outside)), content_(content), list_(opened_list),
          router(content, this)
    {
        opened = &opened_list;
        queue  = &calls;
        content.SetHost(this);
        list_.push_back(this);
    }

    ~FakePopup() override
    {
        content_.SetHost(nullptr);
        list_.erase(std::find(list_.begin(), list_.end(), this));
    }

    bool Contains(PointI screen) const override
    {
        return RectI{origin.x, origin.y, content_.width(), content_.height()}.Contains(screen);
    }

    view::View &content() noexcept
    {
        return content_;
    }

    void ViewLeaving(const view::View &v) override
    {
        router.Forget(v);
    }

    std::function<void(PointI)> pressed_outside;

  private:
    view::View               &content_;
    std::vector<FakePopup *> &list_;

  public:
    view::MouseRouter router;
};

std::unique_ptr<view::Popup> PopupHost::OpenPopup(view::View &content, PointI at,
                                                  std::function<void(PointI)> pressed_outside, bool)
{
    return std::make_unique<FakePopup>(content, PointI{origin.x + at.x, origin.y + at.y}, std::move(pressed_outside),
                                       *opened, *queue);
}

/** A press outside every popup, as the window layer reports it to each open popup. */
void PressOutside(std::vector<FakePopup *> &opened, PointI screen)
{
    const std::vector<FakePopup *> now = opened;
    for (FakePopup *p : now)
    {
        if (!p->Contains(screen))
        {
            p->pressed_outside(screen);
        }
    }
}

void Drain(std::vector<std::function<void()>> &queue)
{
    while (!queue.empty())
    {
        auto call = std::move(queue.front());
        queue.erase(queue.begin());
        call();
    }
}

/** The window the menus hang from going, as the window layer has it go: every popup closed, a submenu's before the
    menu's it opened from, then each popup's closed_by_window run - copies, since an owner may drop the popup in one. */
void WindowGoes(std::vector<FakePopup *> &opened)
{
    std::vector<std::function<void()>> heard;
    for (auto popup = opened.rbegin(); popup != opened.rend(); ++popup) // a submenu opens after its menu
    {
        if ((*popup)->closed_by_window)
        {
            heard.push_back((*popup)->closed_by_window);
        }
    }
    for (const auto &closed : heard)
    {
        closed();
    }
}

struct MenuScene
{
    std::vector<FakePopup *>           opened;
    std::vector<std::function<void()>> queue;
    PopupHost                          window{{100, 100}};
    Probe                              root{"root"};
    std::unique_ptr<view::MenuSession> session;
    std::vector<int>                   chosen;
    bool                               keeps_session{false}; ///< an owner that does not drop the menu as it hears

    MenuScene()
    {
        window.opened = &opened;
        window.queue  = &queue;
        root.SetBounds({200, 100});
        root.SetHost(&window);
    }

    ~MenuScene()
    {
        session.reset();
        root.SetHost(nullptr);
    }

    void Open(std::vector<view::MenuItem> items)
    {
        session = view::MenuSession::Open(root, std::move(items), view::MenuStyle{}, {10, 20}, [this](int id) {
            chosen.push_back(id);
            if (!keeps_session)
            {
                session.reset(); // the owner closes the menu where the choice arrives, as a plate does
            }
        });
    }
};

view::MenuItem Item(std::string text, int id, bool enabled = true, bool ticked = false)
{
    view::MenuItem item;
    item.text    = std::move(text);
    item.id      = id;
    item.enabled = enabled;
    item.ticked  = ticked;
    return item;
}

view::MenuItem Rule()
{
    view::MenuItem item;
    item.separator = true;
    return item;
}

view::MenuItem Submenu(std::string text, std::vector<view::MenuItem> items)
{
    view::MenuItem item;
    item.text    = std::move(text);
    item.submenu = std::move(items);
    return item;
}

std::vector<view::MenuItem> ThreeItems()
{
    return {Item("Rational", 1), Item("Triode", 2, true, true), Rule(), Item("Off", 3, false)};
}
} // namespace

TEST(Menu, OpensSizedToItsItemsWhereItWasAsked)
{
    MenuScene s;
    s.Open(ThreeItems());
    ASSERT_NE(s.session, nullptr);
    ASSERT_EQ(s.opened.size(), 1u);
    const view::MenuStyle style;
    EXPECT_EQ(s.session->menu().height(), 3 * style.row_height + style.separator_height + 2 * style.border_width);
    EXPECT_GE(s.session->menu().width(), style.min_width);
    EXPECT_TRUE(s.opened[0]->Contains({110, 120})) << "the popup's top left at the anchor's point on the screen";
}

TEST(Menu, AReleaseOnAnItemChoosesItAfterTheEventAndTheOwnerClosesIt)
{
    MenuScene s;
    s.Open(ThreeItems());
    view::MouseRouter &menu   = s.opened[0]->router;
    const RectI        second = s.session->menu().RowBounds(1);
    const PointF       at{static_cast<float>(second.x() + 5), static_cast<float>(second.y() + 5)};
    menu.Move(at, {});
    EXPECT_EQ(s.session->menu().hovered_row(), 1);
    menu.Down(at, 1, false, {});
    menu.Up(at, {});
    EXPECT_TRUE(s.chosen.empty()) << "not inside the event: the menu is still handling it";
    Drain(s.queue);
    EXPECT_EQ(s.chosen, (std::vector<int>{2}));
    EXPECT_EQ(s.session, nullptr);
    EXPECT_TRUE(s.opened.empty()) << "the popup closed with the session";
}

TEST(Menu, ADisabledItemOrARuleIsNotChosen)
{
    MenuScene s;
    s.Open(ThreeItems());
    view::MouseRouter &menu = s.opened[0]->router;
    for (const std::size_t row : {std::size_t{2}, std::size_t{3}})
    {
        const RectI  r = s.session->menu().RowBounds(row);
        const PointF at{static_cast<float>(r.x() + 5), static_cast<float>(r.y() + r.height() / 2)};
        menu.Down(at, 1, false, {});
        menu.Up(at, {});
    }
    Drain(s.queue);
    EXPECT_TRUE(s.chosen.empty());
    EXPECT_NE(s.session, nullptr);
}

TEST(Menu, APressOutsideDismissesItWithNoChoice)
{
    MenuScene s;
    s.Open(ThreeItems());
    PressOutside(s.opened, {5, 5});
    Drain(s.queue);
    EXPECT_EQ(s.chosen, (std::vector<int>{0}));
    EXPECT_EQ(s.session, nullptr);
}

TEST(Menu, ASubmenuOpensBesideItsRowAndItsChoiceIsTheMenus)
{
    MenuScene s;
    s.Open({Submenu("Presets", {Item("Warm", 11), Item("Bright", 12)}), Item("Reset to defaults", 1)});
    view::MouseRouter &menu = s.opened[0]->router;
    const RectI        row  = s.session->menu().RowBounds(0);
    menu.Move({static_cast<float>(row.x() + 5), static_cast<float>(row.y() + 5)}, {});
    ASSERT_EQ(s.opened.size(), 2u) << "the pointer on the row opens its submenu";
    FakePopup *sub = s.opened[1];
    EXPECT_EQ(sub->origin.x, s.opened[0]->origin.x + row.right()) << "beside the row";

    // A press in the submenu is outside the menu's own window, but inside its chain: no dismissal.
    PressOutside(s.opened, {sub->origin.x + 5, sub->origin.y + 5});
    Drain(s.queue);
    EXPECT_TRUE(s.chosen.empty());

    sub->router.Down({5.0f, 30.0f}, 1, false, {}); // the second row of the submenu
    sub->router.Up({5.0f, 30.0f}, {});
    Drain(s.queue);
    EXPECT_EQ(s.chosen, (std::vector<int>{12}));
    EXPECT_TRUE(s.opened.empty()) << "the submenu closed with its menu";
}

TEST(Menu, AMenuWhoseWindowGoesEndsDismissedThereAndThenItsSubmenuWithIt)
{
    MenuScene s;
    s.Open({Submenu("Presets", {Item("Warm", 11), Item("Bright", 12)}), Item("Reset to defaults", 1)});
    const RectI row = s.session->menu().RowBounds(0);
    s.opened[0]->router.Move({static_cast<float>(row.x() + 5), static_cast<float>(row.y() + 5)}, {});
    ASSERT_EQ(s.opened.size(), 2u) << "the submenu open beside its row";
    WindowGoes(s.opened);
    EXPECT_EQ(s.chosen, (std::vector<int>{0})) << "dismissed at once: no event of the menu's own to finish first";
    EXPECT_EQ(s.session, nullptr) << "its owner dropped it there";
    EXPECT_TRUE(s.opened.empty()) << "the submenu with it";
    Drain(s.queue);
    EXPECT_EQ(s.chosen, (std::vector<int>{0})) << "once";
}

TEST(Menu, AChoiceOnItsWayAsTheWindowGoesGoesWithItsDeferredCalls)
{
    // The release that chose an item deferred its choice; the window goes before its loop comes round, taking the
    // call with it, and the menu ends dismissed - its owner hearing once, whichever came first.
    MenuScene s;
    s.keeps_session = true;
    s.Open(ThreeItems());
    view::MouseRouter &menu   = s.opened[0]->router;
    const RectI        second = s.session->menu().RowBounds(1);
    const PointF       at{static_cast<float>(second.x() + 5), static_cast<float>(second.y() + 5)};
    menu.Down(at, 1, false, {});
    menu.Up(at, {});
    ASSERT_EQ(s.queue.size(), 1u) << "the choice deferred";
    WindowGoes(s.opened);
    EXPECT_EQ(s.chosen, (std::vector<int>{0}));
    Drain(s.queue); // were the call to come round after all, the menu has ended
    EXPECT_EQ(s.chosen, (std::vector<int>{0})) << "heard once";

    MenuScene t;
    t.keeps_session = true;
    t.Open(ThreeItems());
    t.opened[0]->router.Down(at, 1, false, {});
    t.opened[0]->router.Up(at, {});
    Drain(t.queue);
    EXPECT_EQ(t.chosen, (std::vector<int>{2})) << "the choice heard before the window went";
    WindowGoes(t.opened);
    EXPECT_EQ(t.chosen, (std::vector<int>{2})) << "and the window's going adds nothing";
}

// ---- Plates (view/Switch.h) ------------------------------------------------------------------------------

#include "Switch.h"

namespace
{
/** A choice parameter of three, recording its gestures. */
class FakeChoice final : public view::ParameterControl
{
  public:
    double                   position{0.5};
    std::vector<std::string> log;

    double Normalized() const override
    {
        return position;
    }

    double DefaultNormalized() const override
    {
        return 0.0;
    }

    std::string Text() const override
    {
        return ChoiceText(ChoiceIndex());
    }

    void BeginGesture() override
    {
        log.push_back("begin");
    }

    void SetNormalized(double value) override
    {
        position = value;
        log.push_back("set " + std::to_string(value));
    }

    void EndGesture() override
    {
        log.push_back("end");
    }

    int ChoiceCount() const override
    {
        return 3;
    }

    std::string ChoiceText(int index) const override
    {
        static const char *const kNames[] = {"Rational", "Triode", "Diode"};
        return kNames[index];
    }
};
} // namespace

TEST(Switch, APressOpensEveryChoiceUnderThePlateTheCurrentOneTicked)
{
    MenuScene         s;
    FakeChoice        law;
    std::vector<bool> painted;
    view::Switch      plate(
        law, "CLIP", [&](Canvas &, RectI, bool held) { painted.push_back(held); }, view::Switch::Style{});
    plate.SetBounds({20, 40, 100, 28});
    s.root.AddChild(plate);
    EXPECT_EQ(plate.legend().text(), "Triode") << "the legend reads the current choice";
    EXPECT_EQ(plate.legend().bounds(), (RectI{18, 0, 64, 28})) << "inset clear of the arrow at either side";
    view::MouseRouter router(s.root, &s.window);

    router.Down({30.0f, 50.0f}, 1, false, {});
    RecordingCanvas canvas;
    plate.Paint(canvas);
    EXPECT_EQ(painted, (std::vector<bool>{true})) << "held while the press lasts";
    router.Up({30.0f, 50.0f}, {});
    ASSERT_EQ(s.opened.size(), 1u);
    EXPECT_TRUE(plate.menu_open());
    EXPECT_EQ(s.opened[0]->origin.x, s.window.origin.x + 20) << "under the plate's left edge";
    EXPECT_EQ(s.opened[0]->origin.y, s.window.origin.y + 40 + 28);

    auto &menu = dynamic_cast<view::Menu &>(s.opened[0]->content());
    ASSERT_EQ(menu.items().size(), 3u);
    EXPECT_FALSE(menu.items()[0].ticked);
    EXPECT_TRUE(menu.items()[1].ticked) << "the current choice";
    EXPECT_GE(menu.width(), 100) << "at least as wide as the plate";

    const RectI  third = menu.RowBounds(2);
    const PointF at{static_cast<float>(third.x() + 5), static_cast<float>(third.y() + 5)};
    s.opened[0]->router.Down(at, 1, false, {});
    s.opened[0]->router.Up(at, {});
    Drain(s.queue);
    EXPECT_EQ(law.log, (std::vector<std::string>{"begin", "set 1.000000", "end"})) << "one gesture";
    EXPECT_FALSE(plate.menu_open());
    EXPECT_TRUE(s.opened.empty());
    EXPECT_EQ(plate.legend().text(), "Diode") << "the plate shows the new choice";

    painted.clear();
    plate.Paint(canvas);
    EXPECT_EQ(painted, (std::vector<bool>{false})) << "released";
}

TEST(Switch, ADimmedPlateShowsItsChoiceAndOpensNothing)
{
    MenuScene    s;
    FakeChoice   law;
    view::Switch plate(law, "CLIP", [](Canvas &, RectI, bool) {}, view::Switch::Style{});
    plate.SetBounds({20, 40, 100, 28});
    s.root.AddChild(plate);
    plate.SetDimmed(true);
    view::MouseRouter router(s.root, &s.window);
    router.Down({30.0f, 50.0f}, 1, false, {});
    EXPECT_TRUE(s.opened.empty());
    EXPECT_EQ(plate.cursor, view::Cursor::kArrow);
    EXPECT_FALSE(plate.legend().IsEnabled()) << "its legend draws as disabled";
    EXPECT_FLOAT_EQ(plate.alpha(), 0.4f);
    EXPECT_FLOAT_EQ(plate.caption().alpha(), 0.4f) << "and its caption fades with it";
    EXPECT_EQ(plate.caption().text(), "CLIP");
    EXPECT_TRUE(plate.dimmed());
    plate.SetDimmed(false);
    EXPECT_FALSE(plate.dimmed());
}

TEST(Switch, TheChoiceItIsOnAlreadyIsNoGesture)
{
    // The plate, as a JUCE combo box, sets nothing for the choice it shows already, and a gesture is what a host, or
    // the series' mode starting points, reads as a hand on the setting: picking it again is nothing.
    MenuScene    s;
    FakeChoice   law;
    view::Switch plate(law, "CLIP", [](Canvas &, RectI, bool) {}, view::Switch::Style{});
    plate.Choose(1);
    EXPECT_TRUE(law.log.empty());
    plate.Choose(2);
    EXPECT_EQ(law.log, (std::vector<std::string>{"begin", "set 1.000000", "end"}));
}

// ---- Buttons and text fields (view/Button.h, TextField.h) -------------------------------------------------

#include "Button.h"
#include "TextField.h"

TEST(Button, ActsOnAReleaseOverItAndAToggleFlips)
{
    Scene        s;
    int          clicks = 0;
    view::Button save("SAVE", [](Canvas &, RectI, std::string_view, view::Button::Look) {}, [&] { ++clicks; }, true);
    save.SetBounds({10, 10, 60, 24});
    s.left.AddChild(save); // at (20, 20) in the root

    s.router.Down({30.0f, 30.0f}, 1, false, {});
    s.router.Drag({150.0f, 30.0f}, {}); // dragged off
    s.router.Up({150.0f, 30.0f}, {});
    EXPECT_EQ(clicks, 0) << "released elsewhere: nothing";

    s.router.Down({30.0f, 30.0f}, 1, false, {});
    s.router.Up({31.0f, 30.0f}, {});
    EXPECT_EQ(clicks, 1);
    EXPECT_TRUE(save.on()) << "a toggle flips on its click";

    save.SetDimmed(true);
    s.router.Down({30.0f, 30.0f}, 1, false, {});
    s.router.Up({30.0f, 30.0f}, {});
    EXPECT_EQ(clicks, 1) << "dimmed: no action";
}

TEST(Button, AToggleShowsItsStateOnALampAndAnOwnersButtonMayCarryOne)
{
    // The painter is told to draw the lamp for a toggle, which shows its own state, and for a button its owner sets on
    // and off and has given one (a drawer's button, lit while its drawer is open) - and for no other.
    view::Button::Look painted;
    const auto         painter = [&](Canvas &, RectI, std::string_view, view::Button::Look look) { painted = look; };
    RecordingCanvas    c;

    view::Button toggle("ON", painter, nullptr, true);
    toggle.Paint(c);
    EXPECT_TRUE(painted.lamp) << "a toggle carries the lamp";

    view::Button drawer("CABINET MAKER", painter, nullptr);
    drawer.Paint(c);
    EXPECT_FALSE(painted.lamp) << "a button that acts carries none";
    drawer.SetShowsLamp(true);
    drawer.SetOn(true);
    drawer.Paint(c);
    EXPECT_TRUE(painted.lamp) << "given one by its owner";
    EXPECT_TRUE(painted.on) << "lit while its owner says on";
}

TEST(Button, ARightClickIsItsSecondActionWhenItHasOne)
{
    Scene              s;
    int                clicks = 0, seconds = 0;
    view::Button::Look painted;
    view::Button       roll(
        "ROLL", [&](Canvas &, RectI, std::string_view, view::Button::Look look) { painted = look; }, [&] { ++clicks; });
    roll.SetBounds({10, 10, 60, 24});
    s.left.AddChild(roll); // at (20, 20) in the root
    const auto paint = [&] {
        RecordingCanvas canvas;
        roll.Paint(canvas);
    };

    s.router.Down({30.0f, 30.0f}, 1, true, {});
    s.router.Up({30.0f, 30.0f}, {});
    EXPECT_EQ(clicks, 1) << "with no second action, a right click clicks";

    roll.on_secondary_click = [&] { ++seconds; };
    s.router.Move({30.0f, 30.0f}, {});
    s.router.Down({30.0f, 30.0f}, 1, true, {});
    paint();
    EXPECT_TRUE(painted.over);
    EXPECT_FALSE(painted.held) << "a right press holds no plate down";
    s.router.Up({31.0f, 30.0f}, {});
    EXPECT_EQ(seconds, 1) << "released over it: the second action";
    EXPECT_EQ(clicks, 1) << "and not the click";

    s.router.Down({30.0f, 30.0f}, 1, true, {});
    s.router.Drag({150.0f, 30.0f}, {});
    s.router.Up({150.0f, 30.0f}, {});
    EXPECT_EQ(seconds, 1) << "released elsewhere: nothing";

    s.router.Down({30.0f, 30.0f}, 1, false, {});
    s.router.Up({30.0f, 30.0f}, {});
    EXPECT_EQ(clicks, 2) << "a left click still clicks";
    EXPECT_EQ(seconds, 1);

    roll.SetDimmed(true);
    s.router.Down({30.0f, 30.0f}, 1, true, {});
    s.router.Up({30.0f, 30.0f}, {});
    EXPECT_EQ(seconds, 1) << "dimmed: nothing";
}

TEST(Button, AMomentaryActionIsToldTheHeldStateAsAJuceButtonsIsDownReadsIt)
{
    // The Comp's LISTEN: held while pressed with the pointer over it, let go when dragged off, held again when dragged
    // back, let go on the release - each change told once, and a hover alone tells nothing.
    Scene             s;
    std::vector<bool> told;
    view::Button      listen("LISTEN", [](Canvas &, RectI, std::string_view, view::Button::Look) {}, nullptr);
    listen.OnHeldChanged([&](bool held) { told.push_back(held); });
    listen.SetBounds({10, 10, 60, 24});
    s.left.AddChild(listen); // at (20, 20) in the root

    s.router.Move({30.0f, 30.0f}, {});
    EXPECT_TRUE(told.empty()) << "under the pointer is not held";
    s.router.Down({30.0f, 30.0f}, 1, false, {});
    EXPECT_TRUE(listen.held());
    s.router.Drag({32.0f, 31.0f}, {});
    s.router.Drag({150.0f, 30.0f}, {}); // dragged off
    EXPECT_FALSE(listen.held());
    s.router.Drag({40.0f, 30.0f}, {}); // and back
    s.router.Up({40.0f, 30.0f}, {});
    EXPECT_EQ(told, (std::vector<bool>{true, false, true, false}));

    listen.SetDimmed(true);
    s.router.Down({30.0f, 30.0f}, 1, false, {});
    s.router.Up({30.0f, 30.0f}, {});
    EXPECT_EQ(told.size(), 4u) << "dimmed: never held";
}

TEST(TextField, TypingReplacesTheSelectionAndReturnCommits)
{
    Scene           s;
    view::TextField field(view::TextField::Style{});
    field.SetBounds({10, 10, 60, 17});
    s.left.AddChild(field);
    std::string committed;
    field.on_commit               = [&](const std::string &text) { committed = text; };
    field.select_all_when_focused = true;
    field.SetText("440 Hz");

    field.GrabKeyboardFocus(); // all of it selected, the caret at the end
    EXPECT_EQ(field.selection(), (std::pair<std::size_t, std::size_t>{0, 6}));
    EXPECT_EQ(field.caret(), 6u);
    for (const char32_t c : U"2k")
    {
        if (c != 0)
        {
            s.router.Key({view::Key::kCharacter, c, {}});
        }
    }
    EXPECT_EQ(field.text(), "2k");
    s.router.Key({view::Key::kLeft, 0, {}});
    s.router.Key({view::Key::kBackspace, 0, {}});
    EXPECT_EQ(field.text(), "k");
    s.router.Key({view::Key::kCharacter, U'7', {}});
    EXPECT_EQ(field.text(), "7k");
    s.router.Key({view::Key::kEnd, 0, {}});
    s.router.Key({view::Key::kLeft, 0, {view::Modifiers{true, false, false}}}); // Shift: extend
    EXPECT_EQ(field.selection(), (std::pair<std::size_t, std::size_t>{1, 2}));
    s.router.Key({view::Key::kCharacter, U'a', {view::Modifiers{false, true, false}}}); // Command-A
    EXPECT_EQ(field.selection(), (std::pair<std::size_t, std::size_t>{0, 2}));
    EXPECT_EQ(field.caret(), 0u) << "selected all, the caret at the start";
    s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_EQ(committed, "7k");
}

TEST(TextField, TypingPutsInOnlyWhatItsRestrictionsLetIn)
{
    Scene                  s;
    view::TextField::Style style;
    style.max_length = 4;
    style.allowed    = "0123456789";
    view::TextField field(style);
    field.SetBounds({10, 10, 60, 17});
    s.left.AddChild(field);
    field.select_all_when_focused = true;
    field.SetText("17");
    field.GrabKeyboardFocus();
    const auto type = [&](std::u32string_view keys) {
        for (const char32_t c : keys)
        {
            s.router.Key({view::Key::kCharacter, c, {}});
        }
    };
    type(U"4");
    EXPECT_EQ(field.text(), "4") << "the selection replaced";
    type(U"2x1");
    EXPECT_EQ(field.text(), "421") << "a letter is not a digit";
    type(U"35");
    EXPECT_EQ(field.text(), "4213") << "four at most";
    s.router.Key({view::Key::kCharacter, U'a', {view::Modifiers{false, true, false}}}); // Command-A
    type(U"x");
    EXPECT_EQ(field.text(), "") << "a refused key still takes the selection out, as the JUCE editor's does";
    type(U"98765");
    EXPECT_EQ(field.text(), "9876");
}

TEST(TextField, EscapeCancelsAndLosingTheKeyboardCommits)
{
    Scene           s;
    view::TextField field(view::TextField::Style{});
    field.SetBounds({10, 10, 60, 17});
    s.left.AddChild(field);
    int         cancels = 0;
    std::string committed;
    field.on_commit               = [&](const std::string &text) { committed = text; };
    field.on_cancel               = [&] { ++cancels; };
    field.select_all_when_focused = true;
    field.SetText("12 ms");
    field.GrabKeyboardFocus();
    s.router.Key({view::Key::kCharacter, U'9', {}});
    s.router.Key({view::Key::kEscape, 0, {}});
    EXPECT_EQ(cancels, 1);
    EXPECT_EQ(field.text(), "9") << "the owner, not the field, decides what a cancel restores";
    s.router.Focus(nullptr);
    EXPECT_TRUE(committed.empty()) << "a cancelled edit commits nothing on the way out";

    field.GrabKeyboardFocus();
    s.router.Key({view::Key::kCharacter, U'5', {}});
    s.router.Focus(nullptr); // the keyboard taken elsewhere
    EXPECT_EQ(committed, "5");
}

TEST(TextField, OneThatKeepsTheKeyboardKeepsItFromAPressOnAViewThatWantsNone)
{
    // A box open to be typed into keeps the keyboard, as a JUCE editor keeps its focus against a press on a component
    // that takes none: a press beside the field, on a view that wants no keys, leaves typing in it - where a field that
    // does not keep the keyboard loses it - and a press on a view that wants the keyboard still takes it.
    for (const bool keeps : {false, true})
    {
        Scene           s;
        view::TextField field(view::TextField::Style{});
        field.SetBounds({10, 10, 60, 17});
        s.left.AddChild(field);
        field.keeps_keyboard_focus = keeps;
        field.GrabKeyboardFocus();
        ASSERT_TRUE(field.has_focus());
        s.router.Down({15.0f, 60.0f}, 1, false, {}); // on the field's parent, beside it: no keys wanted
        s.router.Up({15.0f, 60.0f}, {});
        EXPECT_EQ(field.has_focus(), keeps) << (keeps ? "kept" : "lost, as before");
        if (keeps)
        {
            EXPECT_TRUE(s.router.Key({view::Key::kCharacter, U'4', {}}));
            EXPECT_EQ(field.text(), "4") << "typing still the field's";
            s.router.Down({150.0f, 60.0f}, 1, false, {}); // on a view that wants the keyboard
            s.router.Up({150.0f, 60.0f}, {});
            EXPECT_FALSE(field.has_focus()) << "which takes it";
        }
    }
}

TEST(TextField, WordsMoveAndDeleteAsTheEditorsDo)
{
    Scene           s;
    view::TextField field(view::TextField::Style{});
    field.SetBounds({10, 10, 120, 17});
    s.left.AddChild(field);
    field.SetText("-7.0 dB gain");
    field.GrabKeyboardFocus();
    const view::Modifiers alt{false, false, true};
    s.router.Key({view::Key::kLeft, 0, alt}); // from the end, back over "gain"
    EXPECT_EQ(field.caret(), 8u);
    s.router.Key({view::Key::kLeft, 0, alt}); // over "dB "
    EXPECT_EQ(field.caret(), 5u);
    s.router.Key({view::Key::kHome, 0, {}});
    s.router.Key({view::Key::kRight, 0, alt}); // the punctuation "-" is a word of its own
    EXPECT_EQ(field.caret(), 1u);
    s.router.Key({view::Key::kEnd, 0, {}});
    s.router.Key({view::Key::kBackspace, 0, alt});
    EXPECT_EQ(field.text(), "-7.0 dB ");
    s.router.Key({view::Key::kHome, 0, {}});
    s.router.Key({view::Key::kDelete, 0, {}});
    EXPECT_EQ(field.text(), "7.0 dB ");
}

TEST(TextField, TheEndsTheSelectionAndThePointerAreTheEditorsOwn)
{
    // The keys that go to the ends - Command with Left or Right, Home and End, Up and Down, Page Up and Page Down -
    // each with Shift extending the selection from where it began; Shift with Alt selecting a word at a time; Backspace
    // and Delete taking a selection out whole. And the pointer: a press puts the caret on the nearer side of the
    // character under it, before the first word or past the last, Shift extending the selection to it; a drag selects
    // from the press; a double click selects the word under it, letters and digits but not the punctuation round them.
    Scene           s;
    view::TextField field(view::TextField::Style{});
    field.SetBounds({10, 10, 160, 17}); // on the root, at (10, 10)
    s.root.AddChild(field);
    field.SetText("-7.0 dB gain"); // 12 characters, 7 px each in a window with no canvas
    field.GrabKeyboardFocus();
    const auto            key = [&](view::Key k, view::Modifiers modifiers = {}) { s.router.Key({k, 0, modifiers}); };
    const view::Modifiers shift{true, false, false};
    const view::Modifiers command{false, true, false};
    const view::Modifiers shift_command{true, true, false};
    const view::Modifiers shift_alt{true, false, true};
    const auto            selection = [&] { return field.selection(); };
    using Range                     = std::pair<std::size_t, std::size_t>;

    key(view::Key::kLeft, command);
    EXPECT_EQ(field.caret(), 0u) << "Command-Left: the start";
    key(view::Key::kRight, command);
    EXPECT_EQ(field.caret(), 12u) << "Command-Right: the end";
    key(view::Key::kUp);
    EXPECT_EQ(field.caret(), 0u) << "Up: the start of the one line";
    key(view::Key::kDown);
    EXPECT_EQ(field.caret(), 12u) << "Down: its end";
    key(view::Key::kPageUp);
    EXPECT_EQ(field.caret(), 0u);
    key(view::Key::kPageDown);
    EXPECT_EQ(field.caret(), 12u);
    EXPECT_EQ(selection(), (Range{12, 12})) << "moving alone selects nothing";

    key(view::Key::kLeft, shift_command);
    EXPECT_EQ(selection(), (Range{0, 12})) << "Shift-Command-Left: everything back to the start";
    EXPECT_EQ(field.caret(), 0u);
    key(view::Key::kEnd, shift);
    EXPECT_EQ(selection(), (Range{12, 12})) << "Shift-End from there: back to the anchor, nothing";
    key(view::Key::kHome, shift);
    EXPECT_EQ(selection(), (Range{0, 12}));
    key(view::Key::kEnd);
    key(view::Key::kLeft, shift_alt);
    EXPECT_EQ(selection(), (Range{8, 12})) << "Shift-Alt-Left: the last word";
    key(view::Key::kLeft, shift_alt);
    EXPECT_EQ(selection(), (Range{5, 12})) << "and the word before it";

    key(view::Key::kBackspace);
    EXPECT_EQ(field.text(), "-7.0 ") << "Backspace takes the selection out whole";
    EXPECT_EQ(field.caret(), 5u);
    field.SetText("-7.0 dB gain");
    field.SetHighlightedRegion(1, 4);
    key(view::Key::kDelete);
    EXPECT_EQ(field.text(), "- dB gain") << "and so does Delete";
    EXPECT_EQ(field.caret(), 1u);

    // The pointer, at the field's text: character k spans 7 k to 7 k + 7 from where the text starts.
    field.SetText("-7.0 dB gain");
    const reckless::gfx::PointI text = field.TextOffset();
    const auto                  at   = [&](float x) {
        return reckless::gfx::PointF{10.0f + static_cast<float>(text.x) + x, 10.0f + static_cast<float>(text.y) + 3.0f};
    };
    const auto click = [&](float x, view::Modifiers modifiers = {}, int clicks = 1) {
        s.router.Down(at(x), clicks, false, modifiers);
        s.router.Up(at(x), modifiers);
    };
    click(7.0f * 5.0f + 2.0f);
    EXPECT_EQ(field.caret(), 5u) << "a press on the near half of the 'd': before it";
    EXPECT_EQ(selection(), (Range{5, 5}));
    click(7.0f * 5.0f + 5.0f);
    EXPECT_EQ(field.caret(), 6u) << "on its far half: after it";
    click(-3.0f);
    EXPECT_EQ(field.caret(), 0u) << "before the first word";
    click(7.0f * 20.0f);
    EXPECT_EQ(field.caret(), 12u) << "past the last";
    click(7.0f * 5.0f + 2.0f);
    click(7.0f * 11.0f + 2.0f, shift);
    EXPECT_EQ(selection(), (Range{5, 11})) << "Shift extends the selection to the press";
    EXPECT_EQ(field.caret(), 11u);

    s.router.Down(at(7.0f * 1.0f + 1.0f), 1, false, {});
    s.router.Drag(at(7.0f * 4.0f + 1.0f), {});
    s.router.Up(at(7.0f * 4.0f + 1.0f), {});
    EXPECT_EQ(selection(), (Range{1, 4})) << "a drag selects from the press";
    EXPECT_EQ(field.caret(), 4u);

    click(7.0f * 9.0f + 1.0f, {}, 2);
    EXPECT_EQ(selection(), (Range{8, 12})) << "a double click: the word under it";
    EXPECT_EQ(field.caret(), 8u) << "the caret at its start";
    click(7.0f * 1.0f + 1.0f, {}, 2);
    EXPECT_EQ(selection(), (Range{1, 2})) << "a word stops at the punctuation round it";
}

TEST(TextField, Utf8RoundTripsThroughCodePoints)
{
    const std::string text = "Hall \xc2\xb5s \xe2\x86\x92 \xf0\x9f\x8e\xb8";
    EXPECT_EQ(view::CodePointsOf(text).size(),
              11u); // H a l l, a space, the micro sign, s, a space, the arrow, a space, the guitar
    EXPECT_EQ(view::Utf8Of(view::CodePointsOf(text)), text);
    EXPECT_EQ(view::CodePointsOf("\xff")[0], U'\xfffd') << "a malformed byte";

    // A preset's name from a file name in a code page: each broken part one U+FFFD, the good bytes after it kept.
    EXPECT_EQ(view::CodePointsOf("Caf\xE9 Lead"), std::u32string(U"Caf\uFFFD Lead"));
    EXPECT_EQ(view::CodePointsOf("\xC3"
                                 "A"),
              std::u32string(U"\uFFFDA"))
        << "a lead byte's lone start, and the A after it";
    EXPECT_EQ(view::CodePointsOf("\xC0\xAF"), std::u32string(U"\uFFFD\uFFFD")) << "an overlong slash is no slash";
    EXPECT_EQ(view::Utf8Of(std::u32string{0xD800, U'a', 0x110000}), "\xEF\xBF\xBD"
                                                                    "a\xEF\xBF\xBD")
        << "a surrogate and a value past U+10FFFF are no characters";
}

// ---- Labels (view/Label.h) -------------------------------------------------------------------------------

#include "Label.h"

TEST(Label, ItsTextIsFittedInsideItsBorderAtHalfStrengthDisabled)
{
    view::Label::Style style;
    style.font    = reckless::gfx::Font(13.31f);
    style.justify = reckless::gfx::Justify::kCentred;
    style.text    = Color::FromRgb(46, 16, 25);
    view::Label caption(style);
    caption.SetText("OCT 1");
    caption.SetBounds({24, 78, 146, 18});
    EXPECT_EQ(caption.TextArea(), (RectI{5, 1, 136, 16})) << "a pixel above and below, five at each side";

    RecordingCanvas canvas;
    caption.Paint(canvas);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts[0].text, "OCT 1");
    EXPECT_EQ(canvas.texts[0].area, (RectI{5, 1, 136, 16}));
    EXPECT_EQ(canvas.texts[0].lines, 1) << "16 px of room holds one line of 13.31";
    EXPECT_EQ(canvas.texts[0].colour, style.text);

    Probe parent("knob");
    parent.AddChild(caption);
    parent.SetEnabled(false);
    canvas.texts.clear();
    caption.Paint(canvas);
    ASSERT_EQ(canvas.texts.size(), 1u);
    EXPECT_EQ(canvas.texts[0].colour, style.text.WithMultipliedAlpha(0.5f)) << "disabled with its parent";
}

TEST(Label, AClickOpensItsFieldWhereTheJuceEditorLaysItsTextOut)
{
    Scene              s;
    view::Label::Style style;
    style.font = reckless::gfx::Font(16.5f);
    view::Label box(style);
    box.SetText("100 %");
    box.SetEditable(true);
    box.SetBounds({0, 0, 62, 17});
    s.root.AddChild(box);
    std::vector<std::string> edits;
    box.on_edited = [&](const std::string &text) { edits.push_back(text); };

    s.router.Down({30.0f, 8.0f}, 1, false, {});
    EXPECT_FALSE(box.editing()) << "on the release, as the JUCE label opens";
    s.router.Up({30.0f, 8.0f}, {});
    ASSERT_TRUE(box.editing());
    view::TextField &field = *box.editor();
    EXPECT_EQ(field.bounds(), (RectI{0, 0, 62, 17}));
    EXPECT_EQ(field.selection(), (std::pair<std::size_t, std::size_t>{0, 5}));
    EXPECT_EQ(field.caret(), 0u) << "the selection grown back to the start";
    // The editor's steps - 10 px square, the text, the selection, the label's size - leave the text holder scrolled
    // to its start and up three pixels: the text starts a border and an indent in, two pixels down.
    EXPECT_EQ(field.TextOffset(), (reckless::gfx::PointI{5, 2}));

    s.router.Key({view::Key::kCharacter, U'4', {}});
    s.router.Key({view::Key::kCharacter, U'0', {}});
    s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_FALSE(box.editing());
    EXPECT_EQ(box.text(), "40");
    EXPECT_EQ(edits, (std::vector<std::string>{"40"}));
    EXPECT_EQ(s.router.focused(), nullptr) << "the keyboard handed back";

    s.router.Down({30.0f, 8.0f}, 1, false, {});
    s.router.Up({30.0f, 8.0f}, {});
    s.router.Key({view::Key::kCharacter, U'9', {}});
    s.router.Key({view::Key::kEscape, 0, {}});
    EXPECT_FALSE(box.editing());
    EXPECT_EQ(box.text(), "40") << "Escape keeps the text";
    EXPECT_EQ(edits.size(), 1u);

    s.router.Down({30.0f, 8.0f}, 1, false, {});
    s.router.Up({30.0f, 8.0f}, {});
    s.router.Down({150.0f, 50.0f}, 1, false, {}); // elsewhere: the keyboard goes, and the edit with it
    EXPECT_FALSE(box.editing());
    EXPECT_EQ(edits.size(), 1u) << "unchanged text is no edit";
}

TEST(Label, ALabelThatGoesWhileItsFieldIsOpenTakesTheFieldAndTheKeyboardWithIt)
{
    // A restyled label draws in its new style; one that goes while it is typed into takes its open field with it, and
    // the keyboard goes back to nobody rather than to a field that is gone.
    Scene              s;
    view::Label::Style style;
    style.font = reckless::gfx::Font(12.0f);
    {
        view::Label entry(style);
        entry.SetText("17");
        entry.SetBounds({0, 0, 80, 20});
        s.root.AddChild(entry);
        view::Label::Style bigger = style;
        bigger.font               = reckless::gfx::Font(16.5f);
        entry.SetStyle(bigger);
        EXPECT_EQ(entry.style().font.height, 16.5f);
        entry.ShowEditor();
        ASSERT_TRUE(entry.editing());
        EXPECT_NE(s.router.focused(), nullptr) << "its field has the keyboard";
    }
    EXPECT_EQ(s.router.focused(), nullptr) << "gone with the label";
    s.router.Key({view::Key::kCharacter, U'4', {}}); // and a key now lands nowhere
}

TEST(Label, ItsOwnerHearsEveryEditEndKeptOrNotBeforeTheText)
{
    // The JUCE label's onEditorHide: a seed's entry, shown over its button only while it is typed into, hides itself
    // as the edit ends - Return, Escape or the keyboard taken elsewhere - and the typed seed, when it changed, comes
    // after.
    Scene              s;
    view::Label::Style style;
    style.font = reckless::gfx::Font(12.0f);
    view::Label entry(style);
    entry.SetText("1");
    entry.SetBounds({0, 0, 80, 20});
    s.root.AddChild(entry);
    std::vector<std::string> heard;
    entry.on_editor_hide = [&] {
        heard.push_back(entry.editing() ? "hidden while editing" : "hidden");
        entry.SetVisible(false);
    };
    entry.on_edited = [&](const std::string &text) { heard.push_back("edited " + text); };

    entry.ShowEditor();
    s.router.Key({view::Key::kCharacter, U'4', {}});
    s.router.Key({view::Key::kCharacter, U'2', {}});
    s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_EQ(heard, (std::vector<std::string>{"hidden", "edited 42"})) << "the field gone first, then the text";
    EXPECT_FALSE(entry.visible());

    heard.clear();
    entry.SetVisible(true);
    entry.ShowEditor();
    s.router.Key({view::Key::kCharacter, U'7', {}});
    s.router.Key({view::Key::kEscape, 0, {}});
    EXPECT_EQ(heard, (std::vector<std::string>{"hidden"})) << "Escape ends the edit with nothing typed kept";
    EXPECT_EQ(entry.text(), "42");

    heard.clear();
    entry.SetVisible(true);
    entry.ShowEditor();
    s.router.Down({150.0f, 50.0f}, 1, false, {});
    EXPECT_EQ(heard, (std::vector<std::string>{"hidden"})) << "the keyboard taken elsewhere, the text unchanged";
    EXPECT_FALSE(entry.editing());
}

// ---- The knob (view/Knob.h) ------------------------------------------------------------------------------

#include "Knob.h"

namespace
{
/** A parameter that reads typed text as a percentage, recording its gestures. */
class FakeLevel final : public view::ParameterControl
{
  public:
    double                   position{0.5};
    std::vector<std::string> log;

    double Normalized() const override
    {
        return position;
    }

    double DefaultNormalized() const override
    {
        return 0.5;
    }

    std::string Text() const override
    {
        return std::to_string(static_cast<int>(std::lround(position * 100.0)));
    }

    void BeginGesture() override
    {
        log.push_back("begin");
    }

    void SetNormalized(double value) override
    {
        position = value;
        log.push_back("set");
    }

    void EndGesture() override
    {
        log.push_back("end");
    }

    std::optional<double> NormalizedFromText(std::string_view text) const override
    {
        try
        {
            return std::stod(std::string(text)) / 100.0;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
};

/** A knob of the series' size at the root's origin, its dial's draws recorded. */
struct KnobScene
{
    Scene                                s;
    FakeLevel                            level;
    std::vector<std::pair<RectI, float>> dials;
    view::Knob knob{level, "LEVEL", [this](Canvas &, RectI area, float angle) { dials.emplace_back(area, angle); },
                    view::Knob::Style{}, " %"};

    KnobScene()
    {
        s.root.SetBounds({200, 200});
        knob.SetBounds({0, 0, 120, 122});
        s.root.AddChild(knob); // frontmost
    }

    void Press(PointF at, view::Modifiers modifiers = {}, int clicks = 1)
    {
        s.router.Down(at, clicks, false, modifiers);
    }
};
} // namespace

TEST(Knob, ItsDialAndBoxFallWhereTheJuceSlidersDo)
{
    KnobScene k;
    EXPECT_EQ(k.knob.BoxBounds(), (RectI{29, 105, 62, 17})) << "centred at the foot, the series' box";
    EXPECT_EQ(k.knob.DialArea(), (RectI{0, 0, 120, 105}));
    EXPECT_EQ(k.knob.value_box().bounds(), k.knob.BoxBounds());
    EXPECT_EQ(k.knob.value_box().text(), "50 %") << "the parameter's text with the suffix";

    k.knob.SetBounds({0, 0, 50, 25}); // a box held to the width and to all but 15 px of the height
    EXPECT_EQ(k.knob.BoxBounds(), (RectI{0, 15, 50, 10}));

    RecordingCanvas canvas;
    k.knob.Paint(canvas);
    ASSERT_EQ(k.dials.size(), 1u);
    EXPECT_EQ(k.dials[0].first, (RectI{0, 0, 50, 15}));
    EXPECT_FLOAT_EQ(k.dials[0].second,
                    view::Knob::kStartAngle + 0.5f * (view::Knob::kEndAngle - view::Knob::kStartAngle));
}

TEST(Knob, ADragTurnsAFullSweepIn250PixelsFromWhereThePressBegan)
{
    KnobScene k;
    k.Press({60.0f, 50.0f});
    EXPECT_EQ(k.level.log.front(), "begin");
    k.s.router.Drag({60.0f, 25.0f}, {}); // 25 up
    EXPECT_NEAR(k.level.position, 0.6, 1.0e-9);
    k.s.router.Drag({70.0f, 25.0f}, {}); // and 10 right
    EXPECT_NEAR(k.level.position, 0.64, 1.0e-9);
    k.s.router.Drag({70.0f, -200.0f}, {}); // far past the top
    EXPECT_DOUBLE_EQ(k.level.position, 1.0);
    k.s.router.Drag({70.0f, 25.0f}, {}); // and back: measured from the press, so at once where it was
    EXPECT_NEAR(k.level.position, 0.64, 1.0e-9);
    k.s.router.Up({70.0f, 25.0f}, {});
    EXPECT_EQ(k.level.log.back(), "end");
    EXPECT_EQ(std::count(k.level.log.begin(), k.level.log.end(), "begin"), 1) << "one gesture round the drag";
}

TEST(Knob, ADragWithCommandOrAltIsFine)
{
    KnobScene             k;
    const view::Modifiers command{false, true, false};
    k.Press({60.0f, 50.0f}, command);
    k.s.router.Drag({60.0f, 40.0f}, command); // 10 px: through the velocity curve, a fraction of a percent
    const double moved = k.level.position - 0.5;
    EXPECT_GT(moved, 0.0);
    EXPECT_LT(moved, 0.01);
    EXPECT_NEAR(moved, 0.2 * (1.0 + std::sin(3.14159265358979 * (1.5 + 9.0 / 200.0))), 1.0e-9);
}

TEST(Knob, ADoubleClickPutsItBackToItsDefaultAfterTheReleaseAndAnAltClickAtOnce)
{
    KnobScene k;
    k.level.position = 0.8;
    k.Press({60.0f, 50.0f});
    k.s.router.Up({60.0f, 50.0f}, {});
    k.Press({60.0f, 50.0f}, {}, 2);
    EXPECT_DOUBLE_EQ(k.level.position, 0.8) << "not on the press";
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_DOUBLE_EQ(k.level.position, 0.5);
    const std::vector<std::string> tail(k.level.log.end() - 4, k.level.log.end());
    EXPECT_EQ(tail, (std::vector<std::string>{"end", "begin", "set", "end"})) << "after the press's own gesture";

    k.level.position = 0.2;
    k.level.log.clear();
    k.Press({60.0f, 50.0f}, view::Modifiers{false, false, true});
    EXPECT_DOUBLE_EQ(k.level.position, 0.5);
    EXPECT_EQ(k.level.log, (std::vector<std::string>{"begin", "set", "end"}));
    k.s.router.Up({60.0f, 50.0f}, {});
}

TEST(Knob, ADoubleClicksSecondPressDraggedOrHeldIsThePressAndNoDoubleClick)
{
    // As JUCE decides a double click at the release, and only for a press that neither moved 4 pixels nor lasted past
    // 300 ms: the second press of a double click dragged 40 pixels turns the knob and leaves it there, and one held
    // 350 ms leaves it as it was - where a double click decided at the press put it back to its default after either.
    KnobScene k;
    k.Press({60.0f, 50.0f});
    k.s.router.Up({60.0f, 50.0f}, {});
    k.Press({60.0f, 50.0f}, {}, 2);
    k.s.router.Drag({60.0f, 10.0f}, {});
    k.s.router.Up({60.0f, 10.0f}, {});
    const double dragged = k.level.position;
    EXPECT_GT(dragged, 0.6) << "the drag turned it, and nothing put it home";
    EXPECT_EQ(k.level.log.back(), "end");

    k.level.log.clear();
    k.Press({60.0f, 50.0f});
    k.s.router.Up({60.0f, 50.0f}, {});
    k.Press({60.0f, 50.0f}, {}, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_DOUBLE_EQ(k.level.position, dragged) << "a long press: no double click";
    EXPECT_EQ(k.level.log, (std::vector<std::string>{"begin", "set", "end", "begin", "set", "end"}))
        << "the presses' own alone, each setting the value it stands at";

    k.Press({60.0f, 50.0f});
    k.s.router.Up({60.0f, 50.0f}, {});
    k.Press({60.0f, 50.0f}, {}, 2);
    k.s.router.Drag({62.0f, 51.0f}, {}); // under 4 pixels: still a double click
    k.s.router.Up({62.0f, 51.0f}, {});
    EXPECT_DOUBLE_EQ(k.level.position, 0.5) << "a quick one that barely moved puts it home";
}

TEST(Knob, TheWheelMovesItFifteenHundredthsOfItsSweepANotchOverItsDialAndItsBox)
{
    KnobScene k;
    EXPECT_TRUE(k.s.router.Wheel({60.0f, 50.0f}, 0.0f, 100.0f, {})) << "the knob's";
    EXPECT_NEAR(k.level.position, 0.65, 1.0e-9);
    EXPECT_EQ(k.level.log, (std::vector<std::string>{"begin", "set", "end"}));
    k.s.router.Wheel({60.0f, 50.0f}, 0.0f, 100.0f, {}, true); // a reversed device: down
    EXPECT_NEAR(k.level.position, 0.5, 1.0e-9);
    k.s.router.Wheel({60.0f, 50.0f}, -50.0f, 10.0f, {}); // the wider of the two, the horizontal one inverted
    EXPECT_NEAR(k.level.position, 0.575, 1.0e-9);
    k.level.log.clear();
    // Over the box: the box takes no wheel and hands it to the knob, as the JUCE slider's label hands it on.
    EXPECT_TRUE(k.s.router.Wheel({60.0f, 112.0f}, 0.0f, 100.0f, {}));
    EXPECT_EQ(k.level.log, (std::vector<std::string>{"begin", "set", "end"}));
    EXPECT_NEAR(k.level.position, 0.725, 1.0e-9);
}

TEST(Knob, AClickOnTheValueBoxTypesAValueThatReturnSets)
{
    KnobScene k;
    k.s.router.Down({60.0f, 112.0f}, 1, false, {});
    k.s.router.Up({60.0f, 112.0f}, {});
    ASSERT_TRUE(k.knob.value_box().editing());
    EXPECT_NE(k.s.router.focused(), nullptr) << "the box has the keyboard";
    EXPECT_TRUE(k.level.log.empty()) << "a press on the box turns nothing";

    for (const char c : std::string("25 %"))
    {
        k.s.router.Key({view::Key::kCharacter, static_cast<char32_t>(c), {}});
    }
    k.s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_FALSE(k.knob.value_box().editing());
    EXPECT_EQ(k.s.router.focused(), nullptr) << "the keyboard given back";
    EXPECT_DOUBLE_EQ(k.level.position, 0.25) << "read with the suffix taken off";
    EXPECT_EQ(k.level.log, (std::vector<std::string>{"begin", "set", "end"})) << "one gesture";
    EXPECT_EQ(k.knob.value_box().text(), "25 %") << "the box shows the value as the readout writes it";
}

TEST(Knob, EscapeOrUnreadableTextLeavesTheValue)
{
    KnobScene k;
    k.s.router.Down({60.0f, 112.0f}, 1, false, {});
    k.s.router.Up({60.0f, 112.0f}, {});
    k.s.router.Key({view::Key::kCharacter, U'9', {}});
    k.s.router.Key({view::Key::kEscape, 0, {}});
    EXPECT_FALSE(k.knob.value_box().editing());

    k.s.router.Down({60.0f, 112.0f}, 1, false, {});
    k.s.router.Up({60.0f, 112.0f}, {});
    k.s.router.Key({view::Key::kCharacter, U'x', {}});
    k.s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_FALSE(k.knob.value_box().editing());
    EXPECT_DOUBLE_EQ(k.level.position, 0.5);
    EXPECT_TRUE(k.level.log.empty()) << "no gesture for text the readout cannot read";
    EXPECT_EQ(k.knob.value_box().text(), "50 %");
}

TEST(Knob, DimmedItFadesWithItsCaptionAndTurnsNoMore)
{
    KnobScene k;
    k.knob.SetDimmed(true);
    EXPECT_FLOAT_EQ(k.knob.alpha(), 102.0f / 255.0f) << "0.4, kept in 256 steps as a component's alpha";
    EXPECT_FLOAT_EQ(k.knob.caption().alpha(), k.knob.alpha());
    EXPECT_FALSE(k.knob.value_box().IsEnabled()) << "its box draws at half strength again";
    k.Press({60.0f, 50.0f});
    k.s.router.Drag({60.0f, 0.0f}, {});
    k.s.router.Up({60.0f, 0.0f}, {});
    k.s.router.Wheel({60.0f, 50.0f}, 0.0f, 100.0f, {});
    k.s.router.Down({60.0f, 112.0f}, 1, false, {});
    k.s.router.Up({60.0f, 112.0f}, {});
    EXPECT_FALSE(k.knob.value_box().editing());
    EXPECT_TRUE(k.level.log.empty());

    k.knob.SetDimmed(false);
    EXPECT_FLOAT_EQ(k.knob.alpha(), 1.0f);
    EXPECT_TRUE(k.knob.value_box().IsEnabled());
}

TEST(Knob, ASecondaryClickRunsOnARightPressOnTheDialInsteadOfTurning)
{
    // With no secondary click a right press turns the knob, as the slider's does; given one, a right press on the dial
    // runs it and turns nothing - dimmed too, a double press as well - and a right click on the value box does
    // nothing at all.
    KnobScene k;
    k.s.router.Down({60.0f, 50.0f}, 1, true, {});
    k.s.router.Drag({60.0f, 25.0f}, {});
    k.s.router.Up({60.0f, 25.0f}, {});
    EXPECT_NEAR(k.level.position, 0.6, 1.0e-9) << "no secondary click: a right drag turns it";
    k.level.log.clear();

    int menus                 = 0;
    k.knob.on_secondary_click = [&] { ++menus; };
    k.s.router.Down({60.0f, 50.0f}, 1, true, {});
    k.s.router.Drag({60.0f, 0.0f}, {});
    k.s.router.Up({60.0f, 0.0f}, {});
    EXPECT_EQ(menus, 1);
    k.s.router.Down({60.0f, 50.0f}, 2, true, {});
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_EQ(menus, 2) << "the second press of a double one is a secondary click again, not a reset";
    k.knob.SetDimmed(true);
    k.s.router.Down({60.0f, 50.0f}, 1, true, {});
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_EQ(menus, 3) << "dimmed, the right press still runs it";
    k.knob.SetDimmed(false);
    k.s.router.Down({60.0f, 112.0f}, 1, true, {});
    k.s.router.Up({60.0f, 112.0f}, {});
    EXPECT_EQ(menus, 3) << "not on the value box";
    EXPECT_FALSE(k.knob.value_box().editing());
    EXPECT_TRUE(k.level.log.empty()) << "and no gesture throughout";
    EXPECT_NEAR(k.level.position, 0.6, 1.0e-9);

    k.s.router.Down({60.0f, 50.0f}, 1, false, {}); // a left press turns it as ever
    k.s.router.Drag({60.0f, 25.0f}, {});
    k.s.router.Up({60.0f, 25.0f}, {});
    EXPECT_NEAR(k.level.position, 0.7, 1.0e-9);
    EXPECT_EQ(menus, 3);
}

TEST(Knob, ItFollowsTheHostOnceAFrame)
{
    KnobScene k;
    k.level.position = 0.3;
    EXPECT_EQ(k.knob.value_box().text(), "50 %") << "until the frame";
    k.knob.Refresh();
    EXPECT_EQ(k.knob.value_box().text(), "30 %");
    RecordingCanvas canvas;
    k.knob.Paint(canvas);
    ASSERT_FALSE(k.dials.empty());
    EXPECT_FLOAT_EQ(k.dials.back().second,
                    view::Knob::kStartAngle + 0.3f * (view::Knob::kEndAngle - view::Knob::kStartAngle));
}

namespace
{
/** A parameter on a dial from `low` to `high` in its own units, linear, `step` apart when stepped - the slider's
    arithmetic the knob runs in a parameter's units - recording each set; its text the value to two places in hertz. */
class FakeDial final : public view::ParameterControl
{
  public:
    FakeDial(double low, double high, double value, double step = 0.0)
        : low_(low), high_(high), value_(value), step_(step)
    {
    }

    std::vector<std::string> log;

    double value() const noexcept
    {
        return value_;
    }

    double Normalized() const override
    {
        return PositionOf(value_);
    }

    double DefaultNormalized() const override
    {
        return 0.5;
    }

    std::string Text() const override
    {
        return TextAt(Normalized());
    }

    void BeginGesture() override
    {
        log.push_back("begin");
    }

    void SetNormalized(double position) override
    {
        value_ = Snapped(ValueAt(position));
        char text[32];
        std::snprintf(text, sizeof(text), "set %.2f", value_);
        log.emplace_back(text);
    }

    void EndGesture() override
    {
        log.push_back("end");
    }

    double Value() const override
    {
        return value_;
    }

    double ValueAt(double position) const override
    {
        return low_ + (high_ - low_) * std::clamp(position, 0.0, 1.0);
    }

    double PositionOf(double value) const override
    {
        return std::clamp((value - low_) / (high_ - low_), 0.0, 1.0);
    }

    double Interval() const override
    {
        return step_;
    }

    std::string TextAt(double position) const override
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f Hz", ValueAt(position));
        return text;
    }

  private:
    double low_;
    double high_;
    double value_;
    double step_;
};

/** A knob of the series' size on a FakeDial, its dial's angles recorded. */
struct DialScene
{
    Scene              s;
    FakeDial           dial;
    std::vector<float> angles;
    view::Knob         knob{dial, "RATE", [this](Canvas &, RectI, float angle) { angles.push_back(angle); },
                            view::Knob::Style{}};

    explicit DialScene(FakeDial d) : dial(std::move(d))
    {
        s.root.SetBounds({200, 200});
        knob.SetBounds({0, 0, 120, 122});
        s.root.AddChild(knob);
    }
};

float AngleAt(double position)
{
    return view::Knob::kStartAngle + static_cast<float>(position) * (view::Knob::kEndAngle - view::Knob::kStartAngle);
}
} // namespace

TEST(Knob, ATempoDivisionDimsItAndShowsWhatTheDivisionGivesItsParameterUntouched)
{
    DialScene k(FakeDial(1.0, 10.0, 2.5));
    k.knob.FollowSync(false, 4.0);
    EXPECT_FALSE(k.knob.following()) << "Free: the knob is its own";
    EXPECT_EQ(k.knob.value_box().text(), "2.50 Hz");

    k.knob.FollowSync(true, 4.0);
    EXPECT_TRUE(k.knob.following());
    EXPECT_TRUE(k.knob.dimmed()) << "dimmed, so it reads but does not turn";
    EXPECT_FLOAT_EQ(k.knob.caption().alpha(), k.knob.alpha());
    EXPECT_EQ(k.knob.value_box().text(), "4.00 Hz") << "the division's rate, in the knob's own text";
    EXPECT_FLOAT_EQ(k.knob.Angle(), AngleAt(1.0 / 3.0));
    k.knob.Refresh();
    EXPECT_EQ(k.knob.value_box().text(), "4.00 Hz") << "a frame's refresh leaves the division's value up";
    k.knob.FollowSync(true, 40.0);
    EXPECT_EQ(k.knob.value_box().text(), "10.00 Hz") << "held to the dial";
    EXPECT_FLOAT_EQ(k.knob.Angle(), AngleAt(1.0));

    k.s.router.Down({60.0f, 50.0f}, 1, false, {});
    k.s.router.Drag({60.0f, 0.0f}, {});
    k.s.router.Up({60.0f, 0.0f}, {});
    k.s.router.Wheel({60.0f, 50.0f}, 0.0f, 100.0f, {});
    EXPECT_TRUE(k.dial.log.empty()) << "the parameter never moves: no gesture, no set";

    k.knob.FollowSync(false, 40.0);
    EXPECT_FALSE(k.knob.dimmed());
    EXPECT_EQ(k.knob.value_box().text(), "2.50 Hz") << "its own setting back the moment the division lets go";
    EXPECT_FLOAT_EQ(k.knob.Angle(), AngleAt(1.5 / 9.0));
    EXPECT_DOUBLE_EQ(k.dial.value(), 2.5);
}

TEST(Knob, ADoubleClickGoesToTheValueThePanelGivesItInsideTheDial)
{
    DialScene k(FakeDial(1.0, 10.0, 7.0));
    k.knob.SetDoubleClickValue(4.0);
    k.s.router.Down({60.0f, 50.0f}, 1, false, {});
    k.s.router.Up({60.0f, 50.0f}, {});
    k.s.router.Down({60.0f, 50.0f}, 2, false, {});
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_DOUBLE_EQ(k.dial.value(), 4.0) << "the panel's value, not the default's 5.5";
    k.dial.log.clear();
    k.s.router.Down({60.0f, 50.0f}, 1, false, view::Modifiers{false, false, true});
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_EQ(k.dial.log, (std::vector<std::string>{"begin", "set 4.00", "end"})) << "an Alt-click too, one gesture";

    k.knob.SetDoubleClickValue(12.0); // past the dial's top: nothing, as the slider's
    k.dial.log.clear();
    k.s.router.Down({60.0f, 50.0f}, 1, false, view::Modifiers{false, false, true});
    k.s.router.Up({60.0f, 50.0f}, {});
    EXPECT_TRUE(k.dial.log.empty());
    EXPECT_DOUBLE_EQ(k.dial.value(), 4.0);
}

TEST(Knob, AStepppedParameterTakesAtLeastAStepAWheelNotchAndADragLandsOnItsSteps)
{
    // LENGTH-like: 1 to 32 in whole steps. A notch's 0.15 of the sweep is 4.65 steps, landed on 6 from 1 (the
    // slider snaps the sum); a sliver of a notch, far under a step, still moves one whole step, either way.
    DialScene k(FakeDial(1.0, 32.0, 1.0, 1.0));
    k.s.router.Wheel({60.0f, 50.0f}, 0.0f, 100.0f, {});
    EXPECT_DOUBLE_EQ(k.dial.value(), 6.0);
    k.s.router.Wheel({60.0f, 50.0f}, 0.0f, 2.0f, {}); // 0.003 of the sweep, a tenth of a step
    EXPECT_DOUBLE_EQ(k.dial.value(), 7.0) << "at least one step";
    k.s.router.Wheel({60.0f, 50.0f}, 0.0f, -2.0f, {});
    EXPECT_DOUBLE_EQ(k.dial.value(), 6.0) << "and back";
    EXPECT_EQ(k.knob.value_box().text(), "6.00 Hz");

    // A drag of 25 px is a tenth of the sweep, 3.1 steps: the value lands on a step at every move.
    k.dial.log.clear();
    k.s.router.Down({60.0f, 50.0f}, 1, false, {});
    k.s.router.Drag({60.0f, 25.0f}, {});
    EXPECT_DOUBLE_EQ(k.dial.value(), 9.0);
    k.s.router.Drag({60.0f, 21.0f}, {});
    EXPECT_DOUBLE_EQ(k.dial.value(), 10.0);
    k.s.router.Up({60.0f, 21.0f}, {});
    EXPECT_FLOAT_EQ(k.knob.Angle(), AngleAt(9.0 / 31.0)) << "the knob shows the step, not where the drag was";
}

TEST(Knob, AFineDragRunsOnFromItsLastMoveCountsAMoveToTwoHundredPixelsAndAddsSliversToAStep)
{
    // The slider's velocity drag, under Command or under Alt with Shift: each move's own pixels - from the move before,
    // not from the press, across and up counted alike - through a quarter of a sine that tops out at 200 px, a fifth
    // of the sweep, and on from the value the last move left, unsnapped, so a stepped dial's slivers add up to a step.
    const auto fine = [](double pixels) {
        return 0.2 * (1.0 + std::sin(std::numbers::pi * (1.5 + std::min(0.5, std::max(0.0, pixels - 1.0) / 200.0))));
    };
    for (const view::Modifiers held : {view::Modifiers{false, true, false}, view::Modifiers{true, false, true}})
    {
        KnobScene k;
        k.Press({60.0f, 50.0f}, held);
        k.s.router.Drag({60.0f, 40.0f}, held); // 10 up
        k.s.router.Drag({60.0f, 30.0f}, held); // 10 more
        double expected = 0.5 + 2.0 * fine(10.0);
        EXPECT_NEAR(k.level.position, expected, 1.0e-12) << "two ten-pixel moves, each from the one before";
        EXPECT_GT(fine(20.0), 2.0 * fine(10.0)) << "where one twenty-pixel move from the press would go further";
        k.s.router.Drag({63.0f, 30.0f}, held); // 3 right
        expected += fine(3.0);
        EXPECT_NEAR(k.level.position, expected, 1.0e-12) << "across as up";
        k.s.router.Drag({63.0f, 40.0f}, held); // 10 down
        expected -= fine(10.0);
        EXPECT_NEAR(k.level.position, expected, 1.0e-12) << "and down back";
        k.s.router.Drag({63.0f, -460.0f}, held); // 500 up: counted as 200
        expected += 0.2;
        EXPECT_NEAR(k.level.position, expected, 1.0e-12) << "a fast move is a fifth of the sweep at most";
        EXPECT_DOUBLE_EQ(fine(500.0), fine(201.0));
        k.s.router.Up({63.0f, -460.0f}, held);
    }

    // LENGTH-like, 1 to 32 in whole steps from 16: a ten-pixel fine move is 0.062 of a step, so eight leave the value
    // on 16 and the ninth carries it past the half to 17 - every landing on a step.
    DialScene             d(FakeDial(1.0, 32.0, 16.0, 1.0));
    const view::Modifiers command{false, true, false};
    d.s.router.Down({60.0f, 50.0f}, 1, false, command);
    for (int move = 1; move <= 9; ++move)
    {
        d.s.router.Drag({60.0f, 50.0f - 10.0f * static_cast<float>(move)}, command);
        EXPECT_DOUBLE_EQ(d.dial.value(), move < 9 ? 16.0 : 17.0) << "after move " << move;
    }
    d.s.router.Up({60.0f, -40.0f}, command);
}

// ---- Dialogs (view/Dialog.h) -----------------------------------------------------------------------------

#include "Dialog.h"

namespace
{
/** A scene whose host queues CallSoon, as a window's loop does, with a dialog's answer recorded. */
struct DialogScene
{
    std::vector<std::function<void()>> queue;

    struct QueueHost final : RecordingHost
    {
        std::vector<std::function<void()>> *calls{nullptr};

        void CallSoon(std::function<void()> call) override
        {
            calls->push_back(std::move(call));
        }
    } host;

    view::MouseRouter                       router{root, &host};
    Probe                                   root{"root"};
    std::unique_ptr<view::Dialog>           dialog;
    std::vector<std::optional<std::string>> answers;

    DialogScene()
    {
        host.calls  = &queue;
        host.router = &router;
        root.SetBounds({632, 238});
        root.SetHost(&host);
    }

    ~DialogScene()
    {
        dialog.reset();
        root.SetHost(nullptr);
    }

    void Ask(std::optional<std::string> text)
    {
        dialog = std::make_unique<view::Dialog>(root, "Save the preset as", std::move(text), "SAVE", "CANCEL",
                                                view::Dialog::Style{}, [this](std::optional<std::string> answer) {
                                                    answers.push_back(std::move(answer));
                                                    dialog.reset();
                                                });
    }

    void Type(std::string_view text)
    {
        for (const char c : text)
        {
            router.Key({view::Key::kCharacter, static_cast<char32_t>(c), {}});
        }
    }
};
} // namespace

TEST(Dialog, ReturnAnswersWithTheTextTyped)
{
    DialogScene s;
    s.Ask("Warm");
    EXPECT_NE(s.router.focused(), nullptr) << "the field has the keyboard, its text selected";
    s.Type("Bright lead");
    s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_TRUE(s.answers.empty()) << "after the event, not inside it";
    Drain(s.queue);
    ASSERT_EQ(s.answers.size(), 1u);
    EXPECT_EQ(s.answers[0], std::optional<std::string>("Bright lead"));
    EXPECT_EQ(s.dialog, nullptr);
    EXPECT_TRUE(s.root.children().empty()) << "the dialog has left the panel";
}

TEST(Dialog, CancelOrAPressOnTheVeilAnswersNothingEvenWithTextTyped)
{
    DialogScene s;
    s.Ask("Warm");
    s.Type("x");
    // CANCEL: the second of the two buttons from the right, at the card's foot. Find it by its text.
    view::View *cancel = nullptr;
    for (view::View *child : s.dialog->children())
    {
        if (auto *button = dynamic_cast<view::Button *>(child); button != nullptr && button->text() == "CANCEL")
        {
            cancel = button;
        }
    }
    ASSERT_NE(cancel, nullptr);
    const PointF at = cancel->ToRoot(PointF{5.0f, 5.0f});
    s.router.Down(at, 1, false, {}); // takes the keyboard from the field first
    s.router.Up(at, {});
    Drain(s.queue);
    ASSERT_EQ(s.answers.size(), 1u);
    EXPECT_FALSE(s.answers[0].has_value()) << "the field losing the keyboard did not answer for it";

    s.Ask(std::nullopt);
    s.router.Down({3.0f, 3.0f}, 1, false, {}); // the veil
    Drain(s.queue);
    ASSERT_EQ(s.answers.size(), 2u);
    EXPECT_FALSE(s.answers[1].has_value());
}

TEST(Dialog, AQuestionWithNoTextTakesReturnAndEscapeItself)
{
    DialogScene s;
    s.Ask(std::nullopt);
    EXPECT_EQ(s.router.focused(), s.dialog.get());
    s.router.Key({view::Key::kReturn, 0, {}});
    Drain(s.queue);
    ASSERT_EQ(s.answers.size(), 1u);
    EXPECT_EQ(s.answers[0], std::optional<std::string>("")) << "affirmative, with no text";

    s.Ask(std::nullopt);
    s.router.Key({view::Key::kEscape, 0, {}});
    Drain(s.queue);
    ASSERT_EQ(s.answers.size(), 2u);
    EXPECT_FALSE(s.answers[1].has_value());
}

TEST(Dialog, AMessageWithNoAnswerHidesAndStaysUntilItsOwnerDropsIt)
{
    // A message nobody waits on: Return hides it and gives the keyboard up, and runs nothing - there is nothing to
    // run - and it is in the panel until its owner destroys it.
    DialogScene s;
    s.dialog = std::make_unique<view::Dialog>(s.root, "Saved.", std::nullopt, "OK", "", view::Dialog::Style{},
                                              view::Dialog::Answer{});
    s.router.Key({view::Key::kReturn, 0, {}});
    EXPECT_FALSE(s.dialog->visible());
    EXPECT_EQ(s.router.focused(), nullptr) << "the keyboard given up";
    EXPECT_NO_THROW(Drain(s.queue));
    EXPECT_EQ(s.root.children().size(), 1u) << "hidden, still the panel's child";
    s.dialog.reset();
    EXPECT_TRUE(s.root.children().empty()) << "and gone with its owner's word";
}

// ---- A control on a value the panel owns (view/LocalControl.h) ---------------------------------------------

#include "LocalControl.h"

TEST(LocalControl, ALinearDialSnapsToItsStepsAndItsEnds)
{
    std::vector<double> moves;
    auto                material = view::LocalControl::Linear(
        0.0, 9.0, 1.0, 3.0, [](double v) { return "m" + std::to_string(static_cast<int>(std::lround(v))); },
        [](std::string_view, double current) { return std::optional<double>(current); },
        [&](double v) { moves.push_back(v); });
    EXPECT_EQ(material.Value(), 3.0);
    EXPECT_EQ(material.Text(), "m3");
    EXPECT_EQ(material.Interval(), 1.0);
    EXPECT_DOUBLE_EQ(material.Normalized(), 3.0 / 9.0);
    EXPECT_EQ(material.ValueAt(0.5), 4.5) << "the dial before the snap";
    material.SetValue(4.4);
    material.SetValue(4.2); // the same step: no move
    material.SetValue(12.0);
    material.SetNormalized(-0.5);
    EXPECT_EQ(moves, (std::vector<double>{4.0, 9.0, 0.0}));
    material.SetQuietly(7.6);
    EXPECT_EQ(material.Value(), 8.0) << "the refresh snaps too";
    EXPECT_EQ(moves.size(), 3u) << "and tells no one";
    EXPECT_EQ(material.DefaultValue(), 3.0);
    material.SetDefault(5.0);
    EXPECT_DOUBLE_EQ(material.DefaultNormalized(), 5.0 / 9.0);
}

TEST(LocalControl, ARefreshWithinARoundingLeavesTheValueWhereItIs)
{
    // The slider's setValue with dontSendNotification: a value within a double's rounding of the one it holds changes
    // nothing. The Space Craft's drawer refreshes its knobs to the recipe a wheel turn just sent, kept to nine
    // significant figures on its way, and the knob stays on the value the turn gave, its box printing what it printed.
    std::vector<double> moves;
    auto                spread = view::LocalControl::Linear(
        0.0, 100.0, 0.0, 50.0, [](double v) { return std::to_string(std::lrint(v)) + " %"; },
        [](std::string_view, double current) { return std::optional<double>(current); },
        [&](double v) { moves.push_back(v); });
    const double turned = 0.575 * 100.0; // 57.499999999999993: where the wheel's arithmetic lands
    spread.SetValue(turned);
    EXPECT_EQ(spread.Text(), "57 %");
    spread.SetQuietly(57.5); // the recipe's nine figures
    EXPECT_EQ(spread.Value(), turned) << "within a rounding: where the turn left it";
    EXPECT_EQ(spread.Text(), "57 %");
    spread.SetQuietly(57.6);
    EXPECT_EQ(spread.Value(), 57.6) << "a real move lands";
    EXPECT_EQ(moves.size(), 1u) << "and neither refresh tells anyone";
}

TEST(LocalControl, AGeometricDialHasTheGeometricMeanAtItsCentre)
{
    std::vector<double> moves;
    auto                size = view::LocalControl::Geometric(
        10.0, 1000.0, 100.0, [](double v) { return std::to_string(v); },
        [](std::string_view text, double) -> std::optional<double> {
            return text.empty() ? std::nullopt : std::optional<double>(std::stod(std::string(text)));
        },
        [&](double v) { moves.push_back(v); });
    EXPECT_DOUBLE_EQ(size.Normalized(), 0.5);
    EXPECT_EQ(size.ValueAt(0.5), 10.0 * std::pow(100.0, 0.5)) << "a pow, as the JUCE range's";
    EXPECT_EQ(size.ValueAt(-1.0), 10.0);
    EXPECT_EQ(size.ValueAt(2.0), 1000.0);
    EXPECT_EQ(size.PositionOf(1.0), 0.0) << "a value under the dial is held to it before the logarithm";
    EXPECT_EQ(size.Interval(), 0.0);
    EXPECT_EQ(size.ValueFromText("250"), std::optional<double>(250.0));
    EXPECT_FALSE(size.ValueFromText("").has_value());
    size.SetValue(5000.0);
    EXPECT_EQ(moves, (std::vector<double>{1000.0})) << "held to its top";
    size.SetValue(1000.0 * (1.0 + 1.0e-17)); // within a double's rounding
    EXPECT_EQ(moves.size(), 1u);
}

TEST(LocalControl, ChoicesAreAPlatesIndexAndAChoiceMovesOnce)
{
    std::vector<double> moves;
    auto type = view::LocalControl::Choices({"ROOM", "HALL", "PLATE"}, 1, [&](double v) { moves.push_back(v); });
    view::Switch plate(type, "TYPE", nullptr, view::Switch::Style{});
    EXPECT_EQ(type.ChoiceCount(), 3);
    EXPECT_EQ(type.ChoiceIndex(), 1);
    EXPECT_EQ(type.Text(), "HALL");
    EXPECT_EQ(plate.legend().text(), "HALL");
    plate.Choose(1);
    EXPECT_TRUE(moves.empty()) << "the choice it is on: no move";
    plate.Choose(2);
    EXPECT_EQ(moves, (std::vector<double>{2.0}));
    EXPECT_EQ(plate.legend().text(), "PLATE");
    type.SetQuietly(0.0);
    plate.Refresh();
    EXPECT_EQ(plate.legend().text(), "ROOM");
    EXPECT_EQ(moves.size(), 1u);
}
