// The child window's raw keys on macOS (window/RawKey.h, ChildWindow::SetRawKeyCallback): the key going down and the
// key let go as two edges, auto-repeat marked as such, the virtual key code as the physical key, and the modifier
// keys, which a Mac reports through `flagsChanged:` and no other way - with the views given exactly the key downs they
// were always given and nothing else. The events are made and sent in process; nothing is typed. The Windows and Linux
// halves are in test_child_window.cpp.
#import <AppKit/AppKit.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "TestHostWindow.h"
#include "View.h"
#include "Window.h"

namespace
{
/** The panel, as far as the keys ask of one: it wants the keyboard, takes every key the views are given, and counts
    them. */
class KeyCounter final : public reckless::view::View
{
  public:
    bool WantsKeyboardFocus() const override
    {
        return true;
    }

    bool KeyPressed(const reckless::view::KeyEvent &) override
    {
        ++taken;
        return true;
    }

    int taken{0}; ///< how many keys the views were given
};

/** A key event as the keyboard sends one.
    @param type       the event's type: a key down, a key up or a change of the modifiers
    @param characters what the key types, unshifted; empty for a key that types nothing
    @param code       the key's virtual key code
    @param flags      the modifiers held once the event has happened
    @param is_repeat  whether the system marks the event as one of auto-repeat's
    @return           the event */
NSEvent *KeyEvent(NSEventType type, NSString *characters, unsigned short code, NSEventModifierFlags flags,
                  BOOL is_repeat)
{
    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:flags
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:characters
                           isARepeat:is_repeat
                             keyCode:code];
}

constexpr unsigned short kRightArrow = 124; ///< the right arrow's virtual key code
constexpr unsigned short kSpaceBar   = 49;  ///< the space bar's virtual key code
constexpr unsigned short kLeftShift  = 56;  ///< the left shift's virtual key code
constexpr unsigned short kRightShift = 60;  ///< the right shift's virtual key code
} // namespace

TEST(ChildWindow, TheRawKeysGiveEveryEdgeAndLeaveTheViewsWhatTheyAlwaysHad)
{
    reckless::test::TestHostWindow top(240, 160);
    ASSERT_TRUE(top.Valid());
    top.Show(true);
    KeyCounter root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    NSView *view = [[(__bridge NSView *)top.Parent() subviews] firstObject];
    ASSERT_NE(view, nil) << "the surface's view inside the host's";
    root.host()->RequestKeyboardFocus(&root);

    // First with no one asking: the right arrow's down is the views', its up AppKit's own, as it always was.
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, NO)];
    [view keyUp:KeyEvent(NSEventTypeKeyUp, @"", kRightArrow, 0, NO)];
    EXPECT_EQ(root.taken, 1) << "a key down to the views, and a key up to no one";
    const int before = root.taken;

    std::vector<reckless::window::RawKeyEvent> heard;
    child->SetRawKeyCallback([&](const reckless::window::RawKeyEvent &key) { heard.push_back(key); });

    // The right arrow held until it repeats and then let go: the first down is no repeat whatever the event's own
    // isARepeat says of it, since the window has not said the key is down.
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, YES)];
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, YES)];
    [view keyUp:KeyEvent(NSEventTypeKeyUp, @"", kRightArrow, 0, NO)];
    ASSERT_EQ(heard.size(), 3u);
    EXPECT_TRUE(heard[0].is_down);
    EXPECT_FALSE(heard[0].is_repeat) << "the first down the window delivers is never a repeat";
    EXPECT_EQ(heard[0].named_key, reckless::view::Key::kRight) << "the toolkit's own name, where it has one";
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kRightArrow))
        << "a Mac's key code, which is the physical key";
    EXPECT_EQ(heard[0].platform_key_code, heard[0].physical_key_code) << "and its own code besides";
    EXPECT_TRUE(heard[1].is_repeat) << "a down of a key the window has said is down";
    EXPECT_FALSE(heard[2].is_down);
    EXPECT_EQ(root.taken, before + 2) << "the views were given the two downs and nothing else: the key ups are not "
                                         "theirs, and the raw keys took none of the downs away";

    // The space bar, which the views' own enum does not name.
    heard.clear();
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @" ", kSpaceBar, 0, NO)];
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0].named_key, reckless::view::Key::kNone) << "the space bar has no name in the views' enum";
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kSpaceBar));

    // The two shift keys, which send no key down at all: `flagsChanged:` says which key changed, and its flag stays
    // set while the other is held, so the window answers from what it has said is down.
    heard.clear();
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kLeftShift, NSEventModifierFlagShift, NO)];
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kRightShift, NSEventModifierFlagShift, NO)];
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kLeftShift, NSEventModifierFlagShift, NO)];
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kRightShift, 0, NO)];
    ASSERT_EQ(heard.size(), 4u) << "a down and an up for each shift key";
    EXPECT_TRUE(heard[0].is_down);
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kLeftShift));
    EXPECT_TRUE(heard[1].is_down) << "the other shift key is another key, not a repeat";
    EXPECT_EQ(heard[1].physical_key_code, static_cast<std::uint32_t>(kRightShift));
    EXPECT_FALSE(heard[2].is_down) << "the left shift let go while the right one still holds the flag";
    EXPECT_EQ(heard[2].physical_key_code, static_cast<std::uint32_t>(kLeftShift));
    EXPECT_FALSE(heard[3].is_down) << "and then the right one, which clears it";

    // The keyboard gone elsewhere: the space bar is still held, and it is let go.
    heard.clear();
    root.host()->RequestKeyboardFocus(nullptr);
    ASSERT_EQ(heard.size(), 1u) << "an up for the key still held";
    EXPECT_FALSE(heard[0].is_down);
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kSpaceBar));

    // A callback taken back is told nothing more.
    heard.clear();
    child->SetRawKeyCallback(nullptr);
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, NO)];
    [view keyUp:KeyEvent(NSEventTypeKeyUp, @"", kRightArrow, 0, NO)];
    EXPECT_TRUE(heard.empty());
    child.reset(); // before the window it sits in
}
