#pragma once
// Test-only: an app's platform (app/AppPlatform.h) of a suite's own, with no display and no toolkit - windows that are
// records of what they were opened with, timers a suite ticks, the calls after the current event, the menu the app set,
// and the queue other threads post to the main thread through, run by Run as the platforms' loops run it - so an app's
// suite holds what the app does on its platform with nothing on a screen. The "main thread" is whichever thread runs
// Run, RunCalls or RunPosted. Header only, in the suites' reckless_toolkit_test_support with the app's headers
// (reckless_app_headers).
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AppPlatform.h"
#include "PostQueue.h"
#include "WindowPlace.h"

namespace reckless::test
{
class FakeAppPlatform;

/** A window of the fake platform: what it was opened with, and the calls a person's acts would make, made by the
    suite. */
class FakeAppWindow final : public app::AppWindow
{
  public:
    /** A window opened with `options`, listed by `platform` until it goes.
        @param options   what the window was opened with
        @param platform  the platform that lists it; outlives the window */
    FakeAppWindow(app::WindowOptions options, FakeAppPlatform &platform);

    /** Taken off the platform's list. */
    ~FakeAppWindow() override;

    /** Not copyable: the platform lists it by address. */
    FakeAppWindow(const FakeAppWindow &) = delete;
    /** Not assignable: the platform lists it by address.
        @return  never */
    FakeAppWindow &operator=(const FakeAppWindow &) = delete;

    /** The window itself as its handle: unique while it lives, never dereferenced as a native window.
        @return  this window's address */
    void *Handle() const override
    {
        return const_cast<FakeAppWindow *>(this);
    }

    /** One window unit per design pixel.
        @return  1 */
    float Scale() const override
    {
        return 1.0f;
    }

    /** Where it was opened, else the origin.
        @return  the top left the options gave, or {0, 0} */
    std::pair<int, int> Position() const override
    {
        return options_.position.value_or(std::pair<int, int>{0, 0});
    }

    /** Counted. */
    void BringToFront() override
    {
        ++brought_to_front_;
    }

    /** The content's size taken.
        @param width   the content's width, in design pixels
        @param height  the content's height, in design pixels */
    void SetContentSize(int width, int height) override
    {
        options_.width  = width;
        options_.height = height;
    }

    /** The content's size taken, the window moved onto its work area where the suite gave it one (`work_area`), the
        frame being the content alone, and the size told to its `resized`, as every platform tells it.
        @param width   the content's width, in design pixels
        @param height  the content's height, in design pixels */
    void SetContentSizeOnScreen(int width, int height) override
    {
        options_.width  = width;
        options_.height = height;
        if (work_area)
        {
            const auto [x, y]      = Position();
            const gfx::RectI moved = app::MovedOnto({x, y, width, height}, *work_area);
            options_.position      = std::pair<int, int>{moved.x(), moved.y()};
        }
        ++on_screen_resizes_;
        if (options_.resized)
        {
            const auto resized = options_.resized;
            resized(width, height);
        }
    }

    /** The work area's size, where the suite gave one: the frame being the content alone.
        @return  `work_area`'s width and height, or {0, 0} with none */
    gfx::PointI LargestContent() const override
    {
        return work_area ? gfx::PointI{work_area->width(), work_area->height()} : gfx::PointI{0, 0};
    }

    /** How many times the app sized the window on its screen (SetContentSizeOnScreen).
        @return  the count */
    int on_screen_resizes() const noexcept
    {
        return on_screen_resizes_;
    }

    /** The display's work area the window stands on, in the window's units, which the suite sets; none for a
        platform that cannot say. */
    std::optional<gfx::RectI> work_area;

    /** The title taken, as the options' title.
        @param title  the title bar's text */
    void SetTitle(const std::string &title) override
    {
        options_.title = title;
    }

    /** The title as it is now.
        @return  the options' title */
    std::string Title() const override
    {
        return options_.title;
    }

    /** What the window was opened with, its size and its title as they are now.
        @return  the options */
    const app::WindowOptions &options() const noexcept
    {
        return options_;
    }

    /** How many times the app brought the window to the front.
        @return  the count */
    int brought_to_front() const noexcept
    {
        return brought_to_front_;
    }

    /** The person closes the window: its `closing`, where it has one. */
    void Close()
    {
        if (options_.closing)
        {
            const auto closing = options_.closing;
            closing();
        }
    }

    /** The person resizes the window: its content `width` x `height` design pixels, told to its `resized`.
        @param width   the content's width, in design pixels
        @param height  the content's height, in design pixels */
    void Resize(int width, int height)
    {
        options_.width  = width;
        options_.height = height;
        if (options_.resized)
        {
            const auto resized = options_.resized;
            resized(width, height);
        }
    }

  private:
    app::WindowOptions options_;              ///< what the window was opened with, its size as it is now
    FakeAppPlatform   &platform_;             ///< the platform that lists it
    int                brought_to_front_{0};  ///< how many times BringToFront ran
    int                on_screen_resizes_{0}; ///< how many times SetContentSizeOnScreen ran
};

/** The fake platform: windows, timers, the calls after the current event, the menu, the posts from other threads,
    and a Run that runs the calls and the posts until Quit, waiting for a post while it has nothing to do. */
class FakeAppPlatform final : public app::AppPlatform
{
  public:
    /** A platform whose posts wake its Run. */
    FakeAppPlatform()
    {
        posts_->SetWake([this] {
            const std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
            wakes_.notify_all();
        });
    }

    /** The posts' queue closed, as a platform's is when it goes. */
    ~FakeAppPlatform() override
    {
        posts_->Close();
    }

    /** Not copyable: its queue wakes it by address. */
    FakeAppPlatform(const FakeAppPlatform &) = delete;
    /** Not assignable: its queue wakes it by address.
        @return  never */
    FakeAppPlatform &operator=(const FakeAppPlatform &) = delete;

    /** A window of `options`, listed in windows() while it lives; null while `refuse_windows` is set, as a platform
        with no display gives.
        @param options  what the window is opened with
        @return         the window, owned by the caller, or null */
    std::unique_ptr<app::AppWindow> OpenWindow(const app::WindowOptions &options) override
    {
        if (refuse_windows)
        {
            return nullptr;
        }
        return std::make_unique<FakeAppWindow>(options, *this);
    }

    /** No run loop: the fake's child windows, if a suite makes any, bring their own.
        @return  null */
    std::unique_ptr<window::RunLoop> ChildLoop() override
    {
        return nullptr;
    }

    /** A timer the suite ticks (Tick): the period is kept, and not run on a clock.
        @param milliseconds  the period, in milliseconds
        @param tick          what each Tick runs */
    void StartTimer(int milliseconds, std::function<void()> tick) override
    {
        timers_.push_back({milliseconds, std::move(tick)});
    }

    /** `call` after the event being handled: run by Run, or by RunCalls.
        @param call  what runs, once */
    void CallSoon(std::function<void()> call) override
    {
        calls_.push_back(std::move(call));
    }

    /** The app's menu, kept for the suite.
        @param items   the commands, in order
        @param chosen  told the id of the item chosen (Choose) */
    void SetMenu(const std::vector<view::MenuItem> &items, std::function<void(int)> chosen) override
    {
        menu_   = items;
        chosen_ = std::move(chosen);
    }

    /** Run's loop: the calls after the current event and the posts, until Quit, waiting while there is nothing to
        do for a post to wake it; the posts' queue closed when it returns, as a platform's loop closes it.
        @return  0 */
    int Run() override
    {
        quit_ = false;
        while (!quit_)
        {
            RunCalls();
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                woken_ = false;
            }
            RunPosted();
            if (quit_ || !calls_.empty())
            {
                continue;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            wakes_.wait(lock, [this] { return woken_ || quit_; });
        }
        posts_->Close();
        return 0;
    }

    /** Run's loop ends once the call being run returns. */
    void Quit() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        wakes_.notify_all();
    }

    /** The app's ending, kept for RequestQuit.
        @param handler  what runs when the person asks the app to end from outside its windows */
    void SetQuitHandler(std::function<void()> handler) override
    {
        quit_handler_ = std::move(handler);
    }

    /** The posts' queue, woken into Run.
        @return  the queue */
    std::shared_ptr<core::PostQueue> Posts() override
    {
        return posts_;
    }

    /** The bar kept, for the suite to press keys on (PressKey) and choose from (ChooseCommand), and to show in the
        drawn bars MakeMenuBarView makes.
        @param bar     the menus
        @param chosen  told each chosen entry's id */
    void SetMenuBar(const app::MenuBar &bar, std::function<void(int id)> chosen) override
    {
        bar_->Set(bar, std::move(chosen));
    }

    /** A drawn bar on the fake's menus, as Linux's platform makes one.
        @param style  the bar's look
        @return       the bar */
    std::unique_ptr<app::MenuBarView> MakeMenuBarView(const app::MenuBarStyle &style) override
    {
        return std::make_unique<app::MenuBarView>(bar_, style);
    }

    /** The fake's clipboard: kept here, never the system's.
        @param text  the text, UTF-8
        @return      true */
    bool SetClipboardText(const std::string &text) override
    {
        clipboard = text;
        return true;
    }

    /** The fake's clipboard.
        @return  what the last copy, or the suite, put there */
    std::string ClipboardText() override
    {
        return clipboard;
    }

    std::string clipboard; ///< the fake's clipboard, which a suite may set or read as another program's copy would

    // ---- What the suite does as the person, the system and the clock -----------------------------------------------

    /** Every timer's tick, once, in the order started. */
    void Tick()
    {
        for (std::size_t i = 0; i < timers_.size(); ++i)
        {
            const auto tick = timers_[i].tick;
            tick();
        }
    }

    /** The calls CallSoon queued, in order, including any they queue.
        @return  how many ran */
    int RunCalls()
    {
        int ran = 0;
        while (!calls_.empty())
        {
            auto call = std::move(calls_.front());
            calls_.pop_front();
            call();
            ++ran;
        }
        return ran;
    }

    /** The posts waiting, run on this thread as the main thread.
        @return  how many ran */
    std::size_t RunPosted()
    {
        return posts_->RunPending();
    }

    /** The person asks the app to end from outside its windows (an app menu's Quit, a session's end): the quit
        handler, where one is set.
        @return  true when a handler ran */
    bool RequestQuit()
    {
        if (!quit_handler_)
        {
            return false;
        }
        const auto handler = quit_handler_;
        handler();
        return true;
    }

    /** The person chooses the menu item `id` (SetMenu's `chosen`).
        @param id  the item's id
        @return    true when a menu was set to tell */
    bool Choose(int id)
    {
        if (!chosen_)
        {
            return false;
        }
        const auto chosen = chosen_;
        chosen(id);
        return true;
    }

    /** What took a key the suite pressed (PressKey). */
    enum class Took
    {
        kNothing,  ///< neither a view nor a shortcut
        kView,     ///< the view with the keyboard
        kShortcut, ///< a menu bar entry's shortcut, which chose the entry
    };

    /** The person presses a key, as every platform hands it on: to the view with the keyboard first, through
        `router` - the key as the child windows deliver it (ViewKeyOf: Ctrl or Command with a letter as the letter with
        the primary modifier) - and, where no view takes it, to the menu bar's shortcuts.
        @param pressed  the key and its modifiers
        @param router   the window's router, whose focused view hears the key first; null for a window with no view
                        that has the keyboard
        @return         what took the key */
    Took PressKey(const app::Shortcut &pressed, view::MouseRouter *router = nullptr)
    {
        if (router != nullptr)
        {
            const view::KeyEvent key = ViewKeyOf(pressed);
            if (key.key != view::Key::kNone && router->Key(key))
            {
                return Took::kView;
            }
        }
        return bar_->PressShortcut(pressed) ? Took::kShortcut : Took::kNothing;
    }

    /** The person clicks a menu bar item.
        @param command  its command number (app::MenuBarController::CommandOf)
        @return         true when it was chosen */
    bool ChooseCommand(int command)
    {
        return bar_->Choose(command);
    }

    /** The bar SetMenuBar set, with its numbering.
        @return  the controller */
    app::MenuBarController &menu_bar() noexcept
    {
        return *bar_;
    }

    /** A key as a platform's child window hands it to the views: a letter, digit or mark as the character it types
        (a letter shifted in upper case, unless the primary modifier is held, when it stays the letter, as every
        platform now delivers Ctrl+letter and Command+letter), the editing and moving keys by name, and keys the views
        have no name for (the function keys) as none.
        @param pressed  the key and its modifiers
        @return         the views' key event */
    static view::KeyEvent ViewKeyOf(const app::Shortcut &pressed)
    {
        view::KeyEvent key;
        key.modifiers = {pressed.shift, pressed.primary, pressed.alt};
        switch (pressed.key)
        {
        case app::ShortcutKey::kCharacter:
            key.key       = view::Key::kCharacter;
            key.character = pressed.shift && !pressed.primary && pressed.character >= U'a' && pressed.character <= U'z'
                                ? pressed.character - U'a' + U'A'
                                : pressed.character;
            break;
        case app::ShortcutKey::kSpace:
            key.key       = view::Key::kCharacter;
            key.character = U' ';
            break;
        case app::ShortcutKey::kEscape:
            key.key = view::Key::kEscape;
            break;
        case app::ShortcutKey::kReturn:
            key.key = view::Key::kReturn;
            break;
        case app::ShortcutKey::kTab:
            key.key = view::Key::kTab;
            break;
        case app::ShortcutKey::kBackspace:
            key.key = view::Key::kBackspace;
            break;
        case app::ShortcutKey::kDelete:
            key.key = view::Key::kDelete;
            break;
        case app::ShortcutKey::kLeft:
            key.key = view::Key::kLeft;
            break;
        case app::ShortcutKey::kRight:
            key.key = view::Key::kRight;
            break;
        case app::ShortcutKey::kUp:
            key.key = view::Key::kUp;
            break;
        case app::ShortcutKey::kDown:
            key.key = view::Key::kDown;
            break;
        case app::ShortcutKey::kHome:
            key.key = view::Key::kHome;
            break;
        case app::ShortcutKey::kEnd:
            key.key = view::Key::kEnd;
            break;
        case app::ShortcutKey::kPageUp:
            key.key = view::Key::kPageUp;
            break;
        case app::ShortcutKey::kPageDown:
            key.key = view::Key::kPageDown;
            break;
        default:
            break; // a function key, or none: the views have no name for it
        }
        return key;
    }

    /** The windows open, oldest first.
        @return  the windows, owned by whoever opened them */
    const std::vector<FakeAppWindow *> &windows() const noexcept
    {
        return windows_;
    }

    /** The menu SetMenu set.
        @return  the items */
    const std::vector<view::MenuItem> &menu() const noexcept
    {
        return menu_;
    }

    /** How many timers were started.
        @return  the count */
    std::size_t timer_count() const noexcept
    {
        return timers_.size();
    }

    bool refuse_windows{false}; ///< OpenWindow gives null while set, as a platform with no display does

  private:
    friend class FakeAppWindow;

    /** A timer StartTimer started. */
    struct Timer
    {
        int                   milliseconds{0}; ///< the period it was asked for
        std::function<void()> tick;            ///< what each Tick runs
    };

    std::shared_ptr<core::PostQueue>  posts_{std::make_shared<core::PostQueue>()}; ///< the posts from other threads
    std::mutex                        mutex_;        ///< guards woken_ and quit_ between the posts' wake and Run
    std::condition_variable           wakes_;        ///< what Run waits on while it has nothing to do
    bool                              woken_{false}; ///< a post arrived since Run last looked
    std::atomic<bool>                 quit_{false};  ///< Quit has been asked; Run returns
    std::deque<std::function<void()>> calls_;        ///< the calls CallSoon queued, in order
    std::vector<Timer>                timers_;       ///< the timers, in the order started
    std::vector<FakeAppWindow *>      windows_;      ///< the windows open, not owned
    std::vector<view::MenuItem>       menu_;         ///< the menu SetMenu set
    std::function<void(int)>          chosen_;       ///< SetMenu's chosen
    std::function<void()>             quit_handler_; ///< SetQuitHandler's handler
    /** SetMenuBar's bar, shared with the drawn bars. */
    std::shared_ptr<app::MenuBarController> bar_{std::make_shared<app::MenuBarController>()};
};

inline FakeAppWindow::FakeAppWindow(app::WindowOptions options, FakeAppPlatform &platform)
    : options_(std::move(options)), platform_(platform)
{
    platform_.windows_.push_back(this);
}

inline FakeAppWindow::~FakeAppWindow()
{
    auto &list = platform_.windows_;
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}
} // namespace reckless::test
