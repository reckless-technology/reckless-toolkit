// The update check (update/UpdateCheck.h) on a scripted transport and a clock the suite moves, so nothing
// reaches the internet: the version and the manifest read strictly; a check made only with a window on a screen and
// checks automatic, once a day after an answer and once an hour after none, and by hand whenever; nothing at all where
// the process is switched off; the shared file written for every other process and read back when another moves it.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "ScratchPath.h"
#include "UpdateCheck.h"

#include <gtest/gtest.h>

namespace
{
namespace update = reckless::update;
namespace net    = reckless::net;

/** What the scripted server answers, and how often it was asked. */
struct Script
{
    std::atomic<int> requests{0};  ///< requests sent
    int              status = 200; ///< the status it answers
    std::string      body   = R"({"version": "1.2.0", "released": "2026-11-02",
                                 "summary": "A new pedal.",
                                 "page": "https://recklesstechnology.com/plugins/download/"})"; ///< its body
    std::string      last_url;        ///< the URL last asked for
    std::string      last_user_agent; ///< the User-Agent it carried
};

/** A transport answering from the script. */
class ScriptedTransport : public net::Transport
{
  public:
    explicit ScriptedTransport(Script &script) : script_(script)
    {
    }

    net::Response Send(const net::Request &request, net::Cancellation &) override
    {
        ++script_.requests;
        script_.last_url = request.url;
        if (const std::string *agent = request.Find("User-Agent"))
        {
            script_.last_user_agent = *agent;
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

/** A temporary folder for one test's shared file, removed after. */
class TempFolder
{
  public:
    TempFolder()
    {
        path_ = reckless::test::ScratchPath(
            "reckless-update-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TempFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path File() const
    {
        return path_ / "Updates.json";
    }

  private:
    std::filesystem::path path_;
};

/** A checker on the script, a clock at `now` and this build at 1.1.5. */
std::unique_ptr<update::Checker> MakeChecker(const std::filesystem::path &file, Script &script, double &now,
                                             bool allowed = true)
{
    return std::make_unique<update::Checker>(
        file, update::Version{1, 1, 5}, "https://recklesstechnology.com/updates/latest.json",
        [&script] { return std::make_unique<ScriptedTransport>(script); }, [&now] { return now; }, allowed);
}

/** Polls until the check in flight has been collected. */
void Settle(update::Checker &checker, bool shown = true)
{
    for (int i = 0; i < 500 && checker.state() == update::State::kChecking; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        checker.Poll(shown);
    }
}
} // namespace

TEST(UpdateCheck, VersionsReadStrictlyAndOrderAsNumbers)
{
    EXPECT_EQ(update::ParseVersion("1.2.3"), (update::Version{1, 2, 3}));
    EXPECT_EQ(update::ParseVersion("0.20.28"), (update::Version{0, 20, 28}));
    for (const char *bad : {"", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.3 ", " 1.2.3", "1..3", "1.2.-3", "1.2.70000"})
    {
        EXPECT_FALSE(update::ParseVersion(bad).has_value()) << bad;
    }
    EXPECT_LT((update::Version{1, 9, 99}), (update::Version{1, 10, 0})) << "numbers, not text";
    EXPECT_LT((update::Version{1, 2, 3}), (update::Version{1, 2, 4}));
    EXPECT_EQ(update::VersionText({1, 10, 0}), "1.10.0");
}

TEST(UpdateCheck, AManifestNamesAVersionAndAPageOnOurOwnSite)
{
    const auto release = update::ReadManifest(
        R"({"version": "1.2.0", "page": "https://recklesstechnology.com/plugins/download/", "summary": "x"})");
    ASSERT_TRUE(release.has_value());
    EXPECT_EQ(release->version, (update::Version{1, 2, 0}));
    EXPECT_EQ(release->summary, "x");
    EXPECT_FALSE(update::ReadManifest(R"({"version": "1.2.0", "page": "https://elsewhere.example/"})").has_value())
        << "a page off our own site is never opened";
    EXPECT_FALSE(update::ReadManifest(R"({"version": "1.2", "page": "https://recklesstechnology.com/"})").has_value());
    EXPECT_FALSE(update::ReadManifest(R"({"page": "https://recklesstechnology.com/"})").has_value());
    EXPECT_FALSE(update::ReadManifest("<html>not found</html>").has_value());
    const update::Release round{{1, 2, 3}, "2026-11-02", "s", "https://recklesstechnology.com/plugins/download/"};
    EXPECT_EQ(update::ReadManifest(reckless::core::json::Write(update::ManifestOf(round))), round);
}

TEST(UpdateCheck, ChecksOnlyWithAWindowShowingAndThenOnceADay)
{
    TempFolder folder;
    Script     script;
    double     now     = 1.0e9;
    auto       checker = MakeChecker(folder.File(), script, now);
    EXPECT_EQ(checker->state(), update::State::kUnchecked);

    checker->Poll(false);
    EXPECT_EQ(script.requests, 0) << "no window on a screen: no check";

    checker->Poll(true);
    Settle(*checker);
    EXPECT_EQ(script.requests, 1);
    EXPECT_EQ(script.last_url, "https://recklesstechnology.com/updates/latest.json");
    EXPECT_EQ(script.last_user_agent, "RecklessPlugins/1.1.5") << "the version and nothing about the person";
    EXPECT_EQ(checker->state(), update::State::kAvailable);
    ASSERT_TRUE(checker->available().has_value());
    EXPECT_EQ(checker->available()->version, (update::Version{1, 2, 0}));

    now += update::Checker::kIntervalSeconds - 1.0;
    checker->Poll(true);
    EXPECT_EQ(script.requests, 1) << "within the day: no second check";
    now += 2.0;
    checker->Poll(true);
    Settle(*checker);
    EXPECT_EQ(script.requests, 2) << "a day on: checked again";
}

TEST(UpdateCheck, ASeparateProductNamesItselfInTheRequest)
{
    // Reckless IR Capture checks its own manifest with the same checker; its requests name it, not the plugins.
    TempFolder      folder;
    Script          script;
    double          now = 1.0e9;
    update::Checker checker(
        folder.File(), update::Version{1, 0, 0}, "https://recklesstechnology.com/updates/ir-capture.json",
        [&script] { return std::make_unique<ScriptedTransport>(script); }, [&now] { return now; }, true,
        "RecklessIRCapture");
    checker.Poll(true);
    Settle(checker);
    EXPECT_EQ(script.requests, 1);
    EXPECT_EQ(script.last_user_agent, "RecklessIRCapture/1.0.0");
}

TEST(UpdateCheck, SameOrOlderIsCurrentAndAFailureRetriesWithinTheHour)
{
    TempFolder folder;
    Script     script;
    script.body =
        R"({"version": "1.1.5", "page": "https://recklesstechnology.com/plugins/download/"})"; // this very build
    double now     = 1.0e9;
    auto   checker = MakeChecker(folder.File(), script, now);
    checker->Poll(true);
    Settle(*checker);
    EXPECT_EQ(checker->state(), update::State::kCurrent);
    EXPECT_FALSE(checker->available().has_value());

    script.status = 0; // the network gone
    checker->CheckNow();
    Settle(*checker);
    EXPECT_EQ(checker->state(), update::State::kFailed);
    const int after_failure = script.requests;
    now += update::Checker::kRetrySeconds - 1.0;
    checker->Poll(true);
    EXPECT_EQ(script.requests, after_failure);
    script.status = 200;
    now += 2.0;
    checker->Poll(true);
    Settle(*checker);
    EXPECT_EQ(script.requests, after_failure + 1) << "an hour after a failure: tried again";
    EXPECT_EQ(checker->state(), update::State::kCurrent);

    script.status = 404; // the manifest missing
    checker->CheckNow();
    Settle(*checker);
    EXPECT_EQ(checker->state(), update::State::kFailed);
}

TEST(UpdateCheck, OffMeansNoCheckButByHandAndSwitchedOffMeansNone)
{
    TempFolder folder;
    Script     script;
    double     now     = 1.0e9;
    auto       checker = MakeChecker(folder.File(), script, now);
    checker->SetAutomatic(false);
    checker->Poll(true);
    EXPECT_EQ(script.requests, 0) << "checks off: none by the clock";
    checker->CheckNow();
    Settle(*checker);
    EXPECT_EQ(script.requests, 1) << "by hand: one";

    TempFolder off_folder;
    Script     off_script;
    auto       off = MakeChecker(off_folder.File(), off_script, now, false);
    off->Poll(true);
    off->CheckNow();
    Settle(*off);
    EXPECT_EQ(off_script.requests, 0) << "a process switched off checks nothing, even by hand";
}

TEST(UpdateCheck, EveryPluginOnTheMachineSharesOneAnswer)
{
    TempFolder folder;
    Script     script;
    double     now   = 1.0e9;
    auto       first = MakeChecker(folder.File(), script, now);
    first->Poll(true);
    Settle(*first);
    ASSERT_EQ(script.requests, 1);

    // Another process, a moment later: it reads the first's answer and makes no request of its own.
    auto second = MakeChecker(folder.File(), script, now);
    second->Poll(true);
    Settle(*second);
    EXPECT_EQ(script.requests, 1) << "one request for the machine";
    EXPECT_EQ(second->state(), update::State::kAvailable);

    // A setting changed in one is the other's at its next poll.
    const auto revision = first->revision();
    second->SetAutomatic(false);
    first->Poll(true);
    EXPECT_FALSE(first->automatic());
    EXPECT_GT(first->revision(), revision);
}

TEST(UpdateCheck, AMalformedFileReadsAsNothingChecked)
{
    TempFolder folder;
    reckless::core::WriteWhole(folder.File(), "not json");
    Script script;
    double now     = 1.0e9;
    auto   checker = MakeChecker(folder.File(), script, now);
    EXPECT_TRUE(checker->automatic());
    EXPECT_EQ(checker->state(), update::State::kUnchecked);
    checker->Poll(true);
    Settle(*checker);
    EXPECT_EQ(script.requests, 1);
    EXPECT_EQ(checker->state(), update::State::kAvailable) << "and the file written whole again";
}
