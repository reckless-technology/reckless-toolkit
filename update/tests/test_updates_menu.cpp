// The update check as a product makes it and offers it (update/UpdateProduct.h, update/UpdatesMenu.h): a product's
// description making its checker on its own file and version, switched off by the environment; and the Updates menu
// every product shows - its words in each state the check can stand in, its ids, and what choosing each item does - on
// a scripted transport, so nothing reaches the internet.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ScratchPath.h"
#include "UpdateCheck.h"
#include "UpdateProduct.h"
#include "UpdatesMenu.h"

#include <gtest/gtest.h>

namespace
{
namespace update = reckless::update;
namespace net    = reckless::net;
namespace view   = reckless::view;

/** What the scripted server answers; it holds each answer while `hold` is set, so a check stays in flight. */
struct Script
{
    std::atomic<int> requests{0};  ///< requests sent
    int              status = 200; ///< the status it answers; 0 for no network
    std::string      body   = R"({"version": "1.2.0", "page": "https://recklesstechnology.com/plugins/download/"})";
    std::mutex       lock;                ///< guards hold
    std::condition_variable released;     ///< told when hold is cleared
    bool                    hold = false; ///< answers wait while set
};

/** A transport answering from the script. */
class ScriptedTransport final : public net::Transport
{
  public:
    explicit ScriptedTransport(Script &script) : script_(script)
    {
    }

    net::Response Send(const net::Request &, net::Cancellation &) override
    {
        ++script_.requests;
        {
            std::unique_lock<std::mutex> lock(script_.lock);
            script_.released.wait(lock, [this] { return !script_.hold; });
        }
        net::Response response;
        response.status = script_.status;
        response.body   = script_.body;
        if (script_.status == 0)
        {
            response.error = "no network";
        }
        return response;
    }

  private:
    Script &script_;
};

/** A checker on the script with this build at 1.1.5, its file in a folder of the test's own. */
struct Rig
{
    Rig()
    {
        folder = reckless::test::ScratchPath(
            "reckless-updates-menu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);
        checker = std::make_unique<update::Checker>(
            folder / "updates.json", update::Version{1, 1, 5}, "https://recklesstechnology.com/updates/latest.json",
            [this] { return std::make_unique<ScriptedTransport>(script); }, [this] { return now; });
    }

    ~Rig()
    {
        Release();
        checker.reset();
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }

    /** Lets every held answer go. */
    void Release()
    {
        {
            const std::lock_guard<std::mutex> lock(script.lock);
            script.hold = false;
        }
        script.released.notify_all();
    }

    /** Polls until the check in flight has been collected. */
    void Settle()
    {
        for (int i = 0; i < 1000 && checker->state() == update::State::kChecking; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            checker->Poll(true);
        }
    }

    Script                           script;
    double                           now{1.0e9};
    std::filesystem::path            folder;
    std::unique_ptr<update::Checker> checker;
};

/** The texts of a menu's rows, a rule as "-". */
std::vector<std::string> TextsOf(const std::vector<view::MenuItem> &items)
{
    std::vector<std::string> texts;
    for (const view::MenuItem &item : items)
    {
        texts.push_back(item.separator ? "-" : item.text);
    }
    return texts;
}

/** RECKLESS_UPDATE_CHECK set for a scope, and put back as it was after.
    @param value  the value; none to unset it */
class ScopedSwitch
{
  public:
    explicit ScopedSwitch(std::optional<std::string> value)
        : saved_(reckless::core::EnvironmentVariable("RECKLESS_UPDATE_CHECK"))
    {
        Set(value);
    }

    ~ScopedSwitch()
    {
        Set(saved_);
    }

  private:
    static void Set(const std::optional<std::string> &value)
    {
#if defined(_WIN32)
        _putenv_s("RECKLESS_UPDATE_CHECK", value ? value->c_str() : "");
#else
        if (value)
        {
            setenv("RECKLESS_UPDATE_CHECK", value->c_str(), 1);
        }
        else
        {
            unsetenv("RECKLESS_UPDATE_CHECK");
        }
#endif
    }

    std::optional<std::string> saved_;
};
} // namespace

TEST(UpdateProduct, AProductsCheckerIsOnItsOwnFileAndVersionAndTheEnvironmentSwitchesItOff)
{
    const update::Product       product{"RecklessTest", "2.3.4", "https://recklesstechnology.com/updates/test.json",
                                        "test-updates.json"};
    const std::filesystem::path file = update::StateFile(product.state_file);
    if (!file.empty())
    {
        EXPECT_EQ(file.filename(), "test-updates.json");
        EXPECT_EQ(file.parent_path().filename(), "reckless-technology");
    }
    EXPECT_EQ(update::StateFile("updates.json"), update::Checker::DefaultFile()) << "the plugins' shared file";
    EXPECT_EQ(update::VersionOf("2.3.4"), (update::Version{2, 3, 4}));
    EXPECT_EQ(update::VersionOf("unversioned"), update::Version{}) << "every release is newer than no version";

    {
        const ScopedSwitch off(std::string("off"));
        EXPECT_FALSE(update::AllowedByEnvironment());
        const std::unique_ptr<update::Checker> checker = update::MakeChecker(product);
        ASSERT_NE(checker, nullptr) << "made all the same";
        EXPECT_EQ(checker->file(), file);
        EXPECT_EQ(checker->current(), (update::Version{2, 3, 4}));
        checker->CheckNow();
        EXPECT_NE(checker->state(), update::State::kChecking) << "switched off: not even by hand";
    }
    {
        const ScopedSwitch on(std::string("on"));
        EXPECT_TRUE(update::AllowedByEnvironment()) << "anything but off";
    }
    {
        const ScopedSwitch unset(std::nullopt);
        EXPECT_TRUE(update::AllowedByEnvironment()) << "not set at all";
    }
}

TEST(UpdatesMenu, SaysWhereTheCheckStandsInEachState)
{
    Rig rig;
    EXPECT_EQ(TextsOf(update::UpdatesMenuItems(*rig.checker)),
              (std::vector<std::string>{"Version 1.1.5, not checked yet", "-", "Check for updates now",
                                        "Check automatically"}));

    rig.script.hold = true;
    rig.checker->CheckNow();
    std::vector<view::MenuItem> checking = update::UpdatesMenuItems(*rig.checker);
    EXPECT_EQ(TextsOf(checking), (std::vector<std::string>{"Checking for updates...", "-", "Check for updates now",
                                                           "Check automatically"}));
    EXPECT_FALSE(checking[2].enabled) << "no second check while one runs";
    rig.Release();
    rig.Settle();
    EXPECT_EQ(TextsOf(update::UpdatesMenuItems(*rig.checker)),
              (std::vector<std::string>{"Version 1.2.0 is out", "Get version 1.2.0...", "-", "Check for updates now",
                                        "Check automatically"}));

    rig.script.body = R"({"version": "1.1.5", "page": "https://recklesstechnology.com/plugins/download/"})";
    rig.checker->CheckNow();
    rig.Settle();
    EXPECT_EQ(update::UpdatesStatusText(*rig.checker), "Version 1.1.5 is the newest");

    rig.script.status = 0;
    rig.checker->CheckNow();
    rig.Settle();
    EXPECT_EQ(update::UpdatesStatusText(*rig.checker), "Version 1.1.5, could not check");
}

TEST(UpdatesMenu, ItsRowsCarryTheOwnersIdsAndTheSubmenuIsTitledUpdates)
{
    Rig rig;
    rig.checker->CheckNow();
    rig.Settle();
    const update::UpdatesMenuIds ids{2210, 2211, 2212};
    const view::MenuItem         submenu = update::UpdatesSubmenu(*rig.checker, ids);
    EXPECT_EQ(submenu.text, "Updates");
    ASSERT_EQ(submenu.submenu.size(), 5u);
    const std::vector<view::MenuItem> &rows = submenu.submenu;
    EXPECT_EQ(rows[0].id, 0) << "the status chooses nothing";
    EXPECT_FALSE(rows[0].enabled);
    EXPECT_EQ(rows[1].id, 2210);
    EXPECT_TRUE(rows[1].enabled);
    EXPECT_TRUE(rows[2].separator);
    EXPECT_EQ(rows[3].id, 2211);
    EXPECT_TRUE(rows[3].enabled);
    EXPECT_EQ(rows[4].id, 2212);
    EXPECT_TRUE(rows[4].ticked) << "checks by the clock, on by default";
    for (const view::MenuItem &row : rows)
    {
        EXPECT_TRUE(row.submenu.empty());
        EXPECT_FALSE(row.ticked && row.id != 2212);
    }
    EXPECT_EQ(update::UpdatesMenuItems(*rig.checker)[1].id, update::UpdatesMenuIds{}.page) << "the default ids";
}

TEST(UpdatesMenu, ChoosingAnItemOpensThePageChecksOrTurnsTheClockOver)
{
    Rig                          rig;
    const update::UpdatesMenuIds ids;
    std::vector<std::string>     opened;
    const auto                   open = [&opened](const std::string &url) { opened.push_back(url); };

    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, ids.page, open), update::UpdatesChoice::kNone)
        << "nothing newer known: no page";
    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, ids.check_now, open),
              update::UpdatesChoice::kCheckRequested);
    rig.Settle();
    EXPECT_EQ(rig.script.requests, 1);
    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, ids.page, {}), update::UpdatesChoice::kNone)
        << "no browser to open it in";
    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, ids.page, open), update::UpdatesChoice::kOpenedPage);
    EXPECT_EQ(opened, (std::vector<std::string>{"https://recklesstechnology.com/plugins/download/"}));

    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, ids.automatic, open),
              update::UpdatesChoice::kAutomaticChanged);
    EXPECT_FALSE(rig.checker->automatic());
    EXPECT_FALSE(update::UpdatesMenuItems(*rig.checker).back().ticked);
    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, ids.automatic, open),
              update::UpdatesChoice::kAutomaticChanged);
    EXPECT_TRUE(rig.checker->automatic());

    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, 0, open), update::UpdatesChoice::kNone);
    EXPECT_EQ(update::ChooseUpdatesItem(*rig.checker, ids, 99, open), update::UpdatesChoice::kNone);
    EXPECT_EQ(rig.script.requests, 1) << "nothing else asked";
}
