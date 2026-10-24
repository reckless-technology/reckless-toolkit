// The clipboard written and read back (view::Host::SetClipboardText, app::AppPlatform's SetClipboardText and
// ClipboardText). A copy replaces what a person copied, so the system's clipboard is used only where it is not a
// person's: in CI (GITHUB_ACTIONS - the Windows runners' services have a window station of their own, and the Linux
// runner's suites run on Xvfb), or where RECKLESS_TEST_CLIPBOARD asks; elsewhere those tests skip. The Mac runner is a
// person's session, so on macOS the pasteboard's text is held on a pasteboard of the suite's own
// (test_clipboard_mac.mm) and the general pasteboard is never touched.
//
// On Linux, on a display: X keeps no clipboard, so a copy is an owner of the CLIPBOARD selection answering every
// paste (window/linux/X11Clipboard.h). The suite holds the owner answering a reader on a connection of its own - its
// text as UTF8_STRING and its TARGETS - while the owner's loop turns, the owner letting go when another copy takes the
// selection (SelectionClear), the platform's copy answered from its loop, and a child window on the platform's loop
// copying through the platform, so the copy outlives the window.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "AppPlatform.h"
#include "TestHostWindow.h"
#include "View.h"

namespace
{
/** Text with what a clipboard must carry whole: letters beyond ASCII, a symbol beyond the BMP, a line break. */
const std::string kText = "1. e4 e5 2. Nf3 \xe2\x99\x9e\nCaf\xc3\xa9 \xf0\x9f\x98\x80 end";

/** Whether this run may use the system's clipboard: in CI, or where RECKLESS_TEST_CLIPBOARD asks.
    @return  true when a copy here replaces no person's */
bool SystemClipboardIsTheSuites()
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *ci    = std::getenv("GITHUB_ACTIONS");
    const char *asked = std::getenv("RECKLESS_TEST_CLIPBOARD");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__APPLE__)
    (void)ci; // the Mac runner is a person's session: only when asked
    return asked != nullptr && std::string_view(asked) == "1";
#else
    return (ci != nullptr && std::string_view(ci) == "true") || (asked != nullptr && std::string_view(asked) == "1");
#endif
}

/** Whether the platform has what it needs here: a display on Linux.
    @return  true where AppPlatform::Create makes a working platform */
bool PlatformHasADisplay()
{
#if defined(__linux__)
    const char *display = std::getenv("DISPLAY");
    return display != nullptr && *display != '\0';
#else
    return true;
#endif
}
} // namespace

TEST(Clipboard, AHostWithNoClipboardTakesNoCopy)
{
    class Bare final : public reckless::view::Host
    {
      public:
        void Invalidate(reckless::gfx::RectI) override
        {
        }
    } host;

    EXPECT_FALSE(host.SetClipboardText("anything")) << "a test's host has no clipboard";
    EXPECT_EQ(host.ClipboardText(), "");
}

TEST(Clipboard, ThePlatformsCopyIsWhatAPasteReads)
{
    if (!SystemClipboardIsTheSuites())
    {
        GTEST_SKIP() << "the system's clipboard may be a person's here (set RECKLESS_TEST_CLIPBOARD=1 to use it)";
    }
    if (!PlatformHasADisplay())
    {
        if (reckless::test::WindowRequired())
        {
            FAIL() << "a window was promised, and the platform has no display";
        }
        GTEST_SKIP() << "no display";
    }
    const auto platform = reckless::app::AppPlatform::Create("RecklessClipboardTest", {});
    ASSERT_TRUE(platform->SetClipboardText(kText));
    EXPECT_EQ(platform->ClipboardText(), kText) << "UTF-8 whole, a symbol beyond the BMP and a line break with it";
    ASSERT_TRUE(platform->SetClipboardText("second"));
    EXPECT_EQ(platform->ClipboardText(), "second") << "a copy replaces the one before";
}

#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "Window.h"

// Xlib last: its macros (None, Bool, Status) would reach into the headers above.
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "../window/linux/X11Clipboard.h"

namespace
{
/** Skips, or fails where a window is promised, with no display.
    @return  true when the test may go on */
bool DisplayOrSkip()
{
    return PlatformHasADisplay();
}

#define REQUIRE_DISPLAY()                                               \
    if (!DisplayOrSkip())                                               \
    {                                                                   \
        if (reckless::test::WindowRequired())                           \
        {                                                               \
            FAIL() << "a window was promised, and there is no display"; \
        }                                                               \
        GTEST_SKIP() << "no display";                                   \
    }

/** The names of the targets the CLIPBOARD selection's owner offers, asked on a connection of the caller's own as a
    pasting program asks: TARGETS converted into a property, the owner's answer waited for at most two seconds.
    @return  the targets' names; empty for no answer */
std::vector<std::string> AskTargets()
{
    std::vector<std::string> names;
    Display                 *display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        return names;
    }
    const Atom     clipboard = XInternAtom(display, "CLIPBOARD", False);
    const Atom     targets   = XInternAtom(display, "TARGETS", False);
    const Atom     property  = XInternAtom(display, "RECKLESS_TARGETS", False);
    const ::Window window    = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    XConvertSelection(display, clipboard, targets, property, window, CurrentTime);
    XFlush(display);
    for (int i = 0; i < 200; ++i)
    {
        XEvent event;
        if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event))
        {
            Atom           type   = None;
            int            format = 0;
            unsigned long  items = 0, after = 0;
            unsigned char *data = nullptr;
            if (event.xselection.property != None &&
                XGetWindowProperty(display, window, property, 0, 64, True, XA_ATOM, &type, &format, &items, &after,
                                   &data) == Success &&
                data != nullptr && format == 32)
            {
                const auto *atoms = reinterpret_cast<const Atom *>(data);
                for (unsigned long a = 0; a < items; ++a)
                {
                    char *name = XGetAtomName(display, atoms[a]);
                    names.emplace_back(name != nullptr ? name : "");
                    if (name != nullptr)
                    {
                        XFree(name);
                    }
                }
            }
            if (data != nullptr)
            {
                XFree(data);
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return names;
}

/** Every event `display` holds handed to `owner`, as an owner's loop hands them.
    @param display  the owner's connection
    @param owner    the owner */
void Turn(Display *display, reckless::window::x11::ClipboardOwner &owner)
{
    XSync(display, False);
    while (XPending(display) > 0)
    {
        XEvent event;
        XNextEvent(display, &event);
        owner.Handle(event);
    }
}

/** What a reader on another thread got: asked of the owner while the caller turns the owner's loop.
    @param turn  one turn of the owner's loop
    @param read  the read, on its own thread
    @return      what it read */
template <typename TurnLoop, typename Read> auto WhileTurning(TurnLoop turn, Read read)
{
    std::atomic<bool> done{false};
    decltype(read())  result{};
    std::thread       reader([&] {
        result = read();
        done   = true;
    });
    const auto        give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < give_up)
    {
        turn();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    reader.join();
    return result;
}
} // namespace

TEST(Clipboard, AnOwnerAnswersAPasteAndItsTargetsWhileItsLoopTurns)
{
    REQUIRE_DISPLAY();
    Display *display = XOpenDisplay(nullptr);
    ASSERT_NE(display, nullptr);
    {
        reckless::window::x11::ClipboardOwner owner(display);
        ASSERT_TRUE(owner.Take(kText));
        EXPECT_TRUE(owner.owns());
        EXPECT_EQ(reckless::window::x11::ReadClipboardText(), kText) << "this process's own copy, read without asking";

        const std::string asked =
            WhileTurning([&] { Turn(display, owner); }, [] { return reckless::window::x11::ReadClipboardText(true); });
        EXPECT_EQ(asked, kText) << "asked as another program asks: UTF8_STRING, whole";

        const std::vector<std::string> targets =
            WhileTurning([&] { Turn(display, owner); }, [] { return AskTargets(); });
        EXPECT_EQ(targets, (std::vector<std::string>{"TARGETS", "UTF8_STRING", "text/plain;charset=utf-8"}));

        ASSERT_TRUE(owner.Take("\xff not UTF-8"));
        const std::string repaired =
            WhileTurning([&] { Turn(display, owner); }, [] { return reckless::window::x11::ReadClipboardText(true); });
        EXPECT_EQ(repaired, "\xef\xbf\xbd not UTF-8") << "UTF8_STRING's promise kept: the stray byte one U+FFFD";
    }
    EXPECT_EQ(reckless::window::x11::ReadClipboardText(), "") << "an owner that has gone holds nothing";
    XCloseDisplay(display);
}

TEST(Clipboard, AnOwnerLetsGoWhenAnotherCopyTakesTheSelection)
{
    REQUIRE_DISPLAY();
    Display *first_display  = XOpenDisplay(nullptr);
    Display *second_display = XOpenDisplay(nullptr);
    ASSERT_NE(first_display, nullptr);
    ASSERT_NE(second_display, nullptr);
    {
        reckless::window::x11::ClipboardOwner first(first_display);
        reckless::window::x11::ClipboardOwner second(second_display);
        ASSERT_TRUE(first.Take("first"));
        ASSERT_TRUE(second.Take("second"));
        for (int i = 0; i < 100 && first.owns(); ++i)
        {
            Turn(first_display, first);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_FALSE(first.owns()) << "told, by SelectionClear, that another copy took it";
        EXPECT_TRUE(second.owns());
        EXPECT_EQ(reckless::window::x11::ReadClipboardText(), "second");
        const std::string asked = WhileTurning([&] { Turn(second_display, second); },
                                               [] { return reckless::window::x11::ReadClipboardText(true); });
        EXPECT_EQ(asked, "second");
    }
    XCloseDisplay(first_display);
    XCloseDisplay(second_display);
}

TEST(Clipboard, ThePlatformAnswersAPasteFromItsLoopAndAChildWindowOnItCopiesThroughIt)
{
    REQUIRE_DISPLAY();
    if (!SystemClipboardIsTheSuites())
    {
        GTEST_SKIP() << "the display's clipboard may be a person's here (set RECKLESS_TEST_CLIPBOARD=1 to use it)";
    }
    const auto platform = reckless::app::AppPlatform::Create("RecklessClipboardTest", {});
    ASSERT_TRUE(platform->SetClipboardText(kText));

    // Another program's paste, answered from Run's loop: the reader posts what it read and ends the loop.
    std::string asked;
    std::thread reader([&] {
        std::string text = reckless::window::x11::ReadClipboardText(true);
        platform->Post([&, text] {
            asked = text;
            platform->Quit();
        });
    });
    platform->StartTimer(10000, [&] { platform->Quit(); });
    platform->Run();
    reader.join();
    EXPECT_EQ(asked, kText) << "the platform's connection answered from its loop";

    // A child window on the platform's loop copies through the platform: the copy outlives the window.
    reckless::app::WindowOptions options;
    options.title  = "Clipboard";
    options.width  = 200;
    options.height = 100;
    auto window    = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    reckless::view::View root;
    root.SetBounds({200, 100});
    auto child =
        reckless::window::CreateChildWindow(window->Handle(), root, 200, 100, window->Scale(), platform->ChildLoop());
    ASSERT_NE(child, nullptr);
    ASSERT_NE(root.host(), nullptr);
    ASSERT_TRUE(root.host()->SetClipboardText("from the dialog"));
    EXPECT_EQ(root.host()->ClipboardText(), "from the dialog");
    child.reset();
    window.reset();
    EXPECT_EQ(platform->ClipboardText(), "from the dialog") << "held by the platform, not by the window that went";
}

TEST(Clipboard, AChildWindowOnAHostsLoopHoldsItsCopyOnItsOwnConnection)
{
    REQUIRE_DISPLAY();
    if (!SystemClipboardIsTheSuites())
    {
        GTEST_SKIP() << "the display's clipboard may be a person's here (set RECKLESS_TEST_CLIPBOARD=1 to use it)";
    }
    reckless::test::TestHostWindow top(200, 100);
    ASSERT_TRUE(top.Valid());

    /** A host's loop, turned by the suite. */
    class Turned final : public reckless::window::RunLoop
    {
      public:
        bool WatchFd(int, std::function<void()> ready) override
        {
            ready_ = std::move(ready);
            return true;
        }

        bool StartTimer(int, std::function<void()> tick) override
        {
            tick_ = std::move(tick);
            return true;
        }

        void Turn() const
        {
            if (ready_)
            {
                ready_();
            }
        }

      private:
        std::function<void()> ready_;
        std::function<void()> tick_;
    };

    auto                 loop  = std::make_unique<Turned>();
    Turned              &turns = *loop;
    reckless::view::View root;
    root.SetBounds({200, 100});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 200, 100, 1.0f, std::move(loop));
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(root.host()->SetClipboardText(kText)) << "a host's loop keeps no clipboard: the window holds it";
    EXPECT_EQ(root.host()->ClipboardText(), kText);
    const std::string asked =
        WhileTurning([&] { turns.Turn(); }, [] { return reckless::window::x11::ReadClipboardText(true); });
    EXPECT_EQ(asked, kText) << "answered by the window's connection, on the host's loop";
    child.reset();
    EXPECT_EQ(reckless::window::x11::ReadClipboardText(), "") << "the window's copy goes with it";
}
#endif
