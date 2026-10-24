#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Menu.h"

namespace reckless::app
{
/** A key a shortcut names: a character, the editing and moving keys by name, and the function keys. */
enum class ShortcutKey
{
    kNone,      ///< no key: no shortcut
    kCharacter, ///< Shortcut::character says which
    kEscape,    ///< the escape key
    kReturn,    ///< the return or enter key
    kTab,       ///< the tab key
    kSpace,     ///< the space bar
    kBackspace, ///< the backspace key (delete on a Mac's keyboard)
    kDelete,    ///< the forward delete key
    kLeft,      ///< the left arrow
    kRight,     ///< the right arrow
    kUp,        ///< the up arrow
    kDown,      ///< the down arrow
    kHome,      ///< the home key
    kEnd,       ///< the end key
    kPageUp,    ///< the page up key
    kPageDown,  ///< the page down key
    kF1,        ///< the first function key
    kF2,        ///< the second function key
    kF3,        ///< the third function key
    kF4,        ///< the fourth function key
    kF5,        ///< the fifth function key
    kF6,        ///< the sixth function key
    kF7,        ///< the seventh function key
    kF8,        ///< the eighth function key
    kF9,        ///< the ninth function key
    kF10,       ///< the tenth function key
    kF11,       ///< the eleventh function key
    kF12,       ///< the twelfth function key
};

/**
    A keyboard shortcut, written once for every platform: a key and the modifiers held with it, the primary modifier
    standing for Command on macOS and Control on Windows and Linux, as the views' Modifiers::command does - so
    Shortcut::Primary('n') is Cmd+N on a Mac and Ctrl+N elsewhere. A character key is named by the character it types
    unshifted, a letter in lower case ('n', not 'N': Shift is a modifier of its own); letters and digits are the same
    key on every layout and platform, punctuation is matched as each platform reports the unshifted key's character.
    The same type is a key pressed (what the platforms hand the shortcuts), and Matches is how a press is matched.
*/
struct Shortcut
{
    ShortcutKey key{ShortcutKey::kNone}; ///< the key; kNone for no shortcut
    char32_t    character{0};            ///< for ShortcutKey::kCharacter: the character the key types unshifted
    bool        primary{false};          ///< Command on macOS, Control on Windows and Linux
    bool        shift{false};            ///< a shift key
    bool        alt{false};              ///< an alt key (Option on macOS)

    /** The primary modifier with a character key: Cmd+key on macOS, Ctrl+key elsewhere.
        @param character  the key's character, unshifted: a letter in lower case, a digit or a mark
        @param shift      true with Shift too
        @param alt        true with Alt (Option) too
        @return           the shortcut */
    static Shortcut Primary(char32_t character, bool shift = false, bool alt = false)
    {
        return {ShortcutKey::kCharacter, Lowered(character), true, shift, alt};
    }

    /** The primary modifier with a named key: Cmd+key on macOS, Ctrl+key elsewhere.
        @param key    the key
        @param shift  true with Shift too
        @param alt    true with Alt (Option) too
        @return       the shortcut */
    static Shortcut Primary(ShortcutKey key, bool shift = false, bool alt = false)
    {
        return {key, 0, true, shift, alt};
    }

    /** A key with no primary modifier: a function key, an arrow, a letter alone for a view that takes no typing.
        @param key    the key
        @param shift  true with Shift
        @param alt    true with Alt (Option)
        @return       the shortcut */
    static Shortcut Plain(ShortcutKey key, bool shift = false, bool alt = false)
    {
        return {key, 0, false, shift, alt};
    }

    /** A character key with no primary modifier.
        @param character  the key's character, unshifted
        @param shift      true with Shift
        @param alt        true with Alt (Option)
        @return           the shortcut */
    static Shortcut Plain(char32_t character, bool shift = false, bool alt = false)
    {
        return {ShortcutKey::kCharacter, Lowered(character), false, shift, alt};
    }

    /** Whether this names no key.
        @return  true for no shortcut */
    bool empty() const noexcept
    {
        return key == ShortcutKey::kNone || (key == ShortcutKey::kCharacter && character == 0);
    }

    /** Whether a key pressed is this shortcut: the same key - a letter in either case - with exactly these modifiers.
        @param pressed  the key pressed, as a platform hands it over
        @return         true when it chooses this shortcut */
    bool Matches(const Shortcut &pressed) const noexcept
    {
        if (empty() || pressed.key != key || pressed.primary != primary || pressed.shift != shift || pressed.alt != alt)
        {
            return false;
        }
        return key != ShortcutKey::kCharacter || Lowered(pressed.character) == Lowered(character);
    }

    /** A letter in lower case; anything else as it is.
        @param c  a character
        @return   the character, an ASCII capital lowered */
    static constexpr char32_t Lowered(char32_t c) noexcept
    {
        return c >= U'A' && c <= U'Z' ? c - U'A' + U'a' : c;
    }
};

/** The text a menu shows for a shortcut, as each platform writes one: "Ctrl+Shift+N" on Windows and Linux, and where
    `mac` is set the symbols a Mac draws - Option's, Shift's and Command's, then the key. Empty for no shortcut.
    @param shortcut  the shortcut
    @param mac       true for a Mac's symbols, false for the words Windows and Linux write
    @return          the text, UTF-8 */
inline std::string ShortcutText(const Shortcut &shortcut, bool mac)
{
    if (shortcut.empty())
    {
        return {};
    }
    std::string key;
    switch (shortcut.key)
    {
    case ShortcutKey::kCharacter: {
        const char32_t c = shortcut.character >= U'a' && shortcut.character <= U'z' ? shortcut.character - U'a' + U'A'
                                                                                    : shortcut.character;
        // UTF-8 by hand: the character is one code point.
        if (c < 0x80)
        {
            key = std::string(1, static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            key = {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};
        }
        else if (c < 0x10000)
        {
            key = {static_cast<char>(0xE0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)),
                   static_cast<char>(0x80 | (c & 0x3F))};
        }
        else
        {
            key = {static_cast<char>(0xF0 | (c >> 18)), static_cast<char>(0x80 | ((c >> 12) & 0x3F)),
                   static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        }
        break;
    }
    case ShortcutKey::kEscape:
        key = mac ? "\xe2\x8e\x8b" : "Esc"; // the broken circle with an arrow
        break;
    case ShortcutKey::kReturn:
        key = mac ? "\xe2\x86\xa9" : "Enter"; // the return arrow
        break;
    case ShortcutKey::kTab:
        key = mac ? "\xe2\x87\xa5" : "Tab"; // the tab arrow
        break;
    case ShortcutKey::kSpace:
        key = "Space";
        break;
    case ShortcutKey::kBackspace:
        key = mac ? "\xe2\x8c\xab" : "Backspace"; // the erase-left key
        break;
    case ShortcutKey::kDelete:
        key = mac ? "\xe2\x8c\xa6" : "Del"; // the erase-right key
        break;
    case ShortcutKey::kLeft:
        key = mac ? "\xe2\x86\x90" : "Left"; // an arrow left
        break;
    case ShortcutKey::kRight:
        key = mac ? "\xe2\x86\x92" : "Right"; // an arrow right
        break;
    case ShortcutKey::kUp:
        key = mac ? "\xe2\x86\x91" : "Up"; // an arrow up
        break;
    case ShortcutKey::kDown:
        key = mac ? "\xe2\x86\x93" : "Down"; // an arrow down
        break;
    case ShortcutKey::kHome:
        key = mac ? "\xe2\x86\x96" : "Home"; // an arrow to the top left
        break;
    case ShortcutKey::kEnd:
        key = mac ? "\xe2\x86\x98" : "End"; // an arrow to the bottom right
        break;
    case ShortcutKey::kPageUp:
        key = mac ? "\xe2\x87\x9e" : "PgUp"; // a double arrow up
        break;
    case ShortcutKey::kPageDown:
        key = mac ? "\xe2\x87\x9f" : "PgDn"; // a double arrow down
        break;
    case ShortcutKey::kNone:
        break;
    default:
        key = "F" + std::to_string(static_cast<int>(shortcut.key) - static_cast<int>(ShortcutKey::kF1) + 1);
        break;
    }
    if (mac)
    {
        // Option, Shift, Command, in the order a Mac's menus draw them.
        return std::string(shortcut.alt ? "\xe2\x8c\xa5" : "") + (shortcut.shift ? "\xe2\x87\xa7" : "") +
               (shortcut.primary ? "\xe2\x8c\x98" : "") + key;
    }
    return std::string(shortcut.primary ? "Ctrl+" : "") + (shortcut.alt ? "Alt+" : "") +
           (shortcut.shift ? "Shift+" : "") + key;
}

/** One entry of a menu: an item to choose, a rule between groups, or an item that opens a submenu - and at the top of
    a menu bar, a menu, its title the menu's and its submenu the menu's items. */
struct MenuEntry
{
    std::string            title;            ///< the entry's text
    int                    id{0};            ///< what choosing it reports to the bar's `chosen`; 0 to report nothing
    std::function<void()>  action;           ///< run when it is chosen, before `chosen` hears the id; may be empty
    bool                   enabled{true};    ///< false: shown dim, and neither its click nor its shortcut chooses it
    bool                   checked{false};   ///< a tick beside it
    bool                   separator{false}; ///< a rule between groups, with no text
    Shortcut               shortcut;         ///< the key that chooses it from anywhere in the app; empty for none
    std::vector<MenuEntry> submenu;          ///< not empty: it opens these (a menu of the bar, or a submenu)
    /** Further keys that choose it as `shortcut` does, shown nowhere: a second way to press the same thing - the plus
        key Shift turns `=` into, and the keypad's, beside Ctrl+= - where the menu shows the one. */
    std::vector<Shortcut> other_shortcuts;

    /** An item to choose.
        @param title     its text
        @param id        what choosing it reports; 0 for nothing
        @param shortcut  the key that chooses it; empty for none
        @param action    what choosing it runs; may be empty
        @return          the entry */
    static MenuEntry Item(std::string title, int id, Shortcut shortcut = {}, std::function<void()> action = {})
    {
        MenuEntry entry;
        entry.title    = std::move(title);
        entry.id       = id;
        entry.shortcut = shortcut;
        entry.action   = std::move(action);
        return entry;
    }

    /** A rule between groups.
        @return  the entry */
    static MenuEntry Separator()
    {
        MenuEntry entry;
        entry.separator = true;
        return entry;
    }

    /** A menu, or a submenu: a title and its entries.
        @param title    its text
        @param entries  what it opens
        @return         the entry */
    static MenuEntry Menu(std::string title, std::vector<MenuEntry> entries)
    {
        MenuEntry entry;
        entry.title   = std::move(title);
        entry.submenu = std::move(entries);
        return entry;
    }
};

/** A menu bar: its menus, left to right, each a MenuEntry whose title is the menu's and whose submenu is its items. */
struct MenuBar
{
    std::vector<MenuEntry> menus; ///< the menus, left to right
    /** The id of the bar's entry that macOS's app menu's About item chooses, as a click on that entry chooses it (the
        app's own About window); 0 for the system's standard About panel. The other platforms have no app menu. */
    int about_id{0};
};

/**
    A menu bar as every platform shows it and every key reaches it: the bar the app set, its entries numbered one by
    one as commands (in order, depth first, from 1 - each platform's native item is tagged with its number), and the
    one way an entry is chosen, by its click or by its shortcut - its action, then the bar's `chosen` with its id. A
    disabled entry, or one under a disabled menu, is chosen by neither. Main thread.

    The app sets the bar again to change it (a tick, an item enabled), from anywhere - an entry's own action included,
    since Choose copies what it runs before it runs it.
*/
class MenuBarController
{
  public:
    /** The bar shown from now on, its entries numbered afresh.
        @param bar     the menus
        @param chosen  told the id of each entry chosen that has one; may be empty */
    void Set(MenuBar bar, std::function<void(int id)> chosen)
    {
        bar_    = std::move(bar);
        chosen_ = std::move(chosen);
        commands_.clear();
        for (const MenuEntry &menu : bar_.menus)
        {
            Number(menu.submenu);
        }
        ++revision_;
        const std::vector<std::pair<int, std::function<void()>>> watchers = watchers_; // one may unwatch as it hears
        for (const auto &[token, changed] : watchers)
        {
            changed();
        }
    }

    /** `changed` told after each Set until Unwatch: a drawn bar painting itself again.
        @param changed  what runs, on the main thread
        @return         a token for Unwatch */
    int Watch(std::function<void()> changed)
    {
        const int token = next_token_++;
        watchers_.emplace_back(token, std::move(changed));
        return token;
    }

    /** A watcher let go; one already gone is ignored.
        @param token  a token Watch gave */
    void Unwatch(int token)
    {
        std::erase_if(watchers_, [token](const auto &watcher) { return watcher.first == token; });
    }

    /** The bar set.
        @return  the menus */
    const MenuBar &bar() const noexcept
    {
        return bar_;
    }

    /** A count that moves whenever the bar is set: what a drawn bar compares to know it must lay itself out again.
        @return  the count */
    std::size_t revision() const noexcept
    {
        return revision_;
    }

    /** How many entries are numbered as commands.
        @return  the count; the commands are 1 to it */
    int command_count() const noexcept
    {
        return static_cast<int>(commands_.size());
    }

    /** The command number of `entry`, an entry of the bar.
        @param entry  an entry the bar holds
        @return       its number, from 1; 0 for one it does not hold, a separator or a menu */
    int CommandOf(const MenuEntry &entry) const noexcept
    {
        for (std::size_t i = 0; i < commands_.size(); ++i)
        {
            if (commands_[i] == &entry)
            {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    }

    /** The command number of the bar's first entry, depth first, whose id is `id`.
        @param id  an entry's id, not 0
        @return    its number, from 1; 0 for an id no entry has, and for 0 */
    int CommandOfId(int id) const noexcept
    {
        if (id == 0)
        {
            return 0;
        }
        for (std::size_t i = 0; i < commands_.size(); ++i)
        {
            if (commands_[i]->id == id)
            {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    }

    /** The entry a command number names.
        @param command  the number, from 1
        @return         the entry, or null for a number the bar has none for */
    const MenuEntry *EntryOf(int command) const noexcept
    {
        return command >= 1 && command <= command_count() ? commands_[static_cast<std::size_t>(command) - 1] : nullptr;
    }

    /** The command `command` chosen - by a click on its native item or a drawn one: its action, then `chosen` with its
        id, when it is enabled and every menu over it is.
        @param command  the number, from 1
        @return         true when an entry was chosen */
    bool Choose(int command)
    {
        const MenuEntry *entry = EntryOf(command);
        if (entry == nullptr || !Reachable(*entry))
        {
            return false;
        }
        Run(*entry);
        return true;
    }

    /** A key pressed that no view took: the enabled entry whose shortcut it is, chosen.
        @param pressed  the key and its modifiers, as the platform read them
        @return         true when it was an entry's shortcut, and the entry was chosen */
    bool PressShortcut(const Shortcut &pressed)
    {
        const MenuEntry *entry = FindShortcut(pressed);
        if (entry == nullptr)
        {
            return false;
        }
        Run(*entry);
        return true;
    }

    /** The enabled entry a key pressed is the shortcut of - its `shortcut` or one of its `other_shortcuts` - under
        menus that are all enabled.
        @param pressed  the key and its modifiers
        @return         the entry, or null for none */
    const MenuEntry *FindShortcut(const Shortcut &pressed) const
    {
        for (const MenuEntry *entry : commands_)
        {
            const bool matches = entry->shortcut.Matches(pressed) ||
                                 std::any_of(entry->other_shortcuts.begin(), entry->other_shortcuts.end(),
                                             [&pressed](const Shortcut &other) { return other.Matches(pressed); });
            if (matches && Reachable(*entry))
            {
                return entry;
            }
        }
        return nullptr;
    }

    /** The entries of a menu as a drawn menu's rows (view::Menu): each one's command number its id, its shortcut's
        text beside it in Windows' and Linux's words.
        @param entries  a menu's entries, held by this bar
        @return         the rows, submenus with theirs */
    std::vector<view::MenuItem> ItemsOf(const std::vector<MenuEntry> &entries) const
    {
        std::vector<view::MenuItem> items;
        items.reserve(entries.size());
        for (const MenuEntry &entry : entries)
        {
            view::MenuItem item;
            item.separator = entry.separator;
            if (!entry.separator)
            {
                item.text     = entry.title;
                item.id       = entry.submenu.empty() ? CommandOf(entry) : 0;
                item.enabled  = entry.enabled;
                item.ticked   = entry.checked;
                item.shortcut = ShortcutText(entry.shortcut, false);
                item.submenu  = ItemsOf(entry.submenu);
            }
            items.push_back(std::move(item));
        }
        return items;
    }

  private:
    /** Each entry under `entries` numbered, depth first, separators and menus left out.
        @param entries  a menu's entries */
    void Number(const std::vector<MenuEntry> &entries)
    {
        for (const MenuEntry &entry : entries)
        {
            if (entry.separator)
            {
                continue;
            }
            if (!entry.submenu.empty())
            {
                Number(entry.submenu);
                continue;
            }
            commands_.push_back(&entry);
        }
    }

    /** Whether `entry` may be chosen: it and every menu over it enabled.
        @param entry  an entry of the bar
        @return       true when it may be chosen */
    bool Reachable(const MenuEntry &entry) const
    {
        for (const MenuEntry &menu : bar_.menus)
        {
            if (const int found = Within(menu, entry); found != 0)
            {
                return found > 0;
            }
        }
        return false;
    }

    /** Where `entry` lies under `menu`.
        @param menu   a menu or submenu
        @param entry  the entry looked for
        @return       1 when it lies there with every menu between enabled, -1 when it lies there under a disabled
                      one, 0 when it does not lie there */
    static int Within(const MenuEntry &menu, const MenuEntry &entry)
    {
        for (const MenuEntry &child : menu.submenu)
        {
            const int found = &child == &entry ? 1 : (child.submenu.empty() ? 0 : Within(child, entry));
            if (found != 0)
            {
                return found > 0 && menu.enabled && child.enabled ? 1 : -1;
            }
        }
        return 0;
    }

    /** An entry's action, then `chosen` with its id: each copied first, so either may set the bar again.
        @param entry  the entry */
    void Run(const MenuEntry &entry)
    {
        const std::function<void()>    action = entry.action;
        const int                      id     = entry.id;
        const std::function<void(int)> chosen = chosen_;
        if (action)
        {
            action();
        }
        if (id != 0 && chosen)
        {
            chosen(id);
        }
    }

    MenuBar                                            bar_;      ///< the bar set
    std::function<void(int)>                           chosen_;   ///< told each chosen entry's id
    std::vector<const MenuEntry *>                     commands_; ///< the entries by command number less one, into bar_
    std::size_t                                        revision_{0};   ///< moves at each Set
    std::vector<std::pair<int, std::function<void()>>> watchers_;      ///< told after each Set, by token
    int                                                next_token_{1}; ///< the token the next Watch gives
};
} // namespace reckless::app
