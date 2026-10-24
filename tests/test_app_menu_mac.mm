// The macOS platform's menu bar (AppPlatform::SetMenuBar): the screen's bar holding the app menu and then the bar's
// menus, each item with its title, its tick, whether it is enabled, and its shortcut as its key equivalent - Command
// for the primary modifier. The window takes a key that is one of the bar's shortcuts before the bar does (its
// performKeyEquivalent, which AppKit asks first) and hands it to the views as a key down: a view with the keyboard may
// take it, else it reaches the window's keyDown, which chooses the entry. Any other key, Command-Q among them, is left
// to AppKit; a disabled entry's is no shortcut; a key with Control held, which no shortcut is written with, chooses
// nothing. The events are made and sent in process; nothing is typed.
#import <AppKit/AppKit.h>

#include <gtest/gtest.h>

#include <memory>

#include "AppPlatform.h"

/** A view that takes the keyboard and every key down, as a text view with the keyboard takes its own keys. */
@interface RecklessKeyTaker : NSView
{
  @public
    int taken; ///< how many key downs it took
}
@end

@implementation RecklessKeyTaker

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)keyDown:(NSEvent *)event
{
    (void)event;
    ++taken;
}

@end

namespace
{
/** A key down in `window` as the keyboard sends one.
    @param window     the window
    @param character  the character it types, unshifted
    @param code       the key's code
    @param flags      the modifiers held
    @return           the event */
NSEvent *KeyDown(NSWindow *window, NSString *character, unsigned short code, NSEventModifierFlags flags)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:flags
                           timestamp:0
                        windowNumber:window.windowNumber
                             context:nil
                          characters:character
         charactersIgnoringModifiers:character
                           isARepeat:NO
                             keyCode:code];
}
} // namespace

TEST(AppMenuBar, TheScreensBarShowsTheMenusAndTheWindowTakesTheirShortcuts)
{
    using reckless::app::MenuBar;
    using reckless::app::MenuEntry;
    using reckless::app::Shortcut;
    int        new_games = 0;
    int        saves     = 0;
    const auto platform  = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    EXPECT_TRUE(platform->HasNativeMenuBar());
    EXPECT_EQ(platform->MakeMenuBarView({}), nullptr);
    MenuBar   bar;
    MenuEntry save = MenuEntry::Item("Save", 2, Shortcut::Primary('s'), [&] { ++saves; });
    save.enabled   = false;
    MenuEntry flip = MenuEntry::Item("Flip Board", 3, Shortcut::Primary('f', true));
    flip.checked   = true;
    bar.menus.push_back(MenuEntry::Menu(
        "File", {MenuEntry::Item("New Game", 1, Shortcut::Primary('n'), [&] { ++new_games; }), save, flip}));
    platform->SetMenuBar(bar, nullptr);

    NSMenu *main = NSApp.mainMenu;
    ASSERT_EQ(main.numberOfItems, 2) << "the app menu, then File";
    NSMenu *file = [main itemAtIndex:1].submenu;
    ASSERT_NE(file, nil);
    EXPECT_TRUE([file.title isEqualToString:@"File"]);
    NSMenuItem *item = [file itemAtIndex:0];
    EXPECT_TRUE([item.title isEqualToString:@"New Game"]);
    EXPECT_TRUE([item.keyEquivalent isEqualToString:@"n"]);
    EXPECT_EQ(item.keyEquivalentModifierMask, NSEventModifierFlagCommand) << "the primary modifier is Command";
    EXPECT_FALSE([file itemAtIndex:1].enabled) << "the menus enable nothing of their own";
    EXPECT_EQ([file itemAtIndex:2].state, NSControlStateValueOn);
    EXPECT_EQ([file itemAtIndex:2].keyEquivalentModifierMask, NSEventModifierFlagCommand | NSEventModifierFlagShift);

    reckless::app::WindowOptions options;
    options.title     = "Menu";
    options.width     = 300;
    options.height    = 200;
    const auto window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    NSWindow *ns = [(__bridge NSView *)window->Handle() window];
    ASSERT_NE(ns, nil);

    // AppKit asks the key window for a key equivalent before the menu bar: the window takes the bar's shortcuts, to
    // hand them to the views first, and leaves every other key to AppKit.
    NSEvent *command_n = KeyDown(ns, @"n", 45, NSEventModifierFlagCommand);
    EXPECT_TRUE([ns performKeyEquivalent:command_n]) << "the bar's shortcut is the window's to hand on";
    EXPECT_EQ(new_games, 1) << "no view took it: it reached the window's keyDown, which chose the entry";
    EXPECT_FALSE([ns performKeyEquivalent:KeyDown(ns, @"q", 12, NSEventModifierFlagCommand)])
        << "Command-Q, no shortcut of the bar's, left to AppKit and the app menu";
    EXPECT_FALSE([ns performKeyEquivalent:KeyDown(ns, @"s", 1, NSEventModifierFlagCommand)])
        << "nor a disabled entry's";
    EXPECT_EQ(saves, 0);
    [ns sendEvent:command_n];
    EXPECT_EQ(new_games, 2) << "a key down reaching the window with no view to take it";
    [ns sendEvent:KeyDown(ns, @"n", 45, NSEventModifierFlagControl)];
    EXPECT_EQ(new_games, 2) << "Control is no modifier a shortcut is written with";

    // A view with the keyboard that takes the key: the shortcut never fires.
    RecklessKeyTaker *taker = [[RecklessKeyTaker alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    [ns.contentView addSubview:taker];
    ASSERT_TRUE([ns makeFirstResponder:taker]);
    EXPECT_TRUE([ns performKeyEquivalent:command_n]);
    EXPECT_EQ(taker->taken, 1) << "the view had the key first";
    EXPECT_EQ(new_games, 2) << "and took it";
    [ns makeFirstResponder:nil];
    [taker removeFromSuperview];

    // A click on the item: its target, by its command number.
    [file performActionForItemAtIndex:0];
    EXPECT_EQ(new_games, 3);
}
