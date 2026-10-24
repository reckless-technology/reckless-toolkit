#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "Http.h"
#include "Json.h"
#include "JsonFile.h"
#include "PresetFiles.h"

namespace reckless::update
{
/**
    Whether a newer release of the suite is out: what every plugin's settings gear and header say, and nothing more -
    the check never downloads or installs anything, it only points the person at the download page.

    The release's word is a small static file on the suite's own website (kManifestUrl), written there as a release is
    promoted to the public. A check is one plain GET of it - no machine id, no account, nothing about the person - made
    at most once a day per machine, only while a plugin's window is on a screen (so never in a host's scan, a validator,
    a suite or an offline render) and never with "Check automatically" off; "Check for updates now" makes one whatever
    the clock and the setting say. What a check found and when, and the setting, are one file every plugin on the
    machine shares - `reckless-technology/updates.json` in the user's settings folder - so twenty plugins in a session
    make one request, and a check one plugin makes is the answer every other shows.
*/

/** The newest release's word, on the suite's own website. */
inline constexpr std::string_view kManifestUrl = "https://recklesstechnology.com/updates/latest.json";

/** The only pages a manifest may point the person at: the suite's own website, so a manifest that is not ours can
    open nothing else in their browser. */
inline constexpr std::string_view kTrustedPagePrefix = "https://recklesstechnology.com/";

/** A version of the suite, MAJOR.MINOR.BUILD, ordered as numbers. */
struct Version
{
    int major = 0; ///< the release's major version
    int minor = 0; ///< its minor version
    int build = 0; ///< the commits since the minor version's tag

    /** Ordered by major, then minor, then build.
        @return  the order of the two versions */
    auto operator<=>(const Version &) const = default;
};

/** `text` as a version: three whole numbers 0 to 65535 joined by points ("1.2.3"), nothing before or after.
    @param text  the version's text
    @return      the version, or nothing for anything else */
inline std::optional<Version> ParseVersion(std::string_view text) noexcept
{
    int         parts[3] = {0, 0, 0};
    const char *at       = text.data();
    const char *end      = text.data() + text.size();
    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
        {
            if (at == end || *at != '.')
            {
                return std::nullopt;
            }
            ++at;
        }
        if (at == end || *at < '0' || *at > '9')
        {
            return std::nullopt;
        }
        const auto read = std::from_chars(at, end, parts[i]);
        if (read.ec != std::errc() || parts[i] > 65535)
        {
            return std::nullopt;
        }
        at = read.ptr;
    }
    if (at != end)
    {
        return std::nullopt;
    }
    return Version{parts[0], parts[1], parts[2]};
}

/** `version` as text, "1.2.3".
    @param version  the version
    @return         its text */
inline std::string VersionText(const Version &version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.build);
}

/** A release as the manifest names it. */
struct Release
{
    Version     version;  ///< its version
    std::string released; ///< the day it was published, as the manifest gives it ("2026-11-02"); may be empty
    std::string summary;  ///< a line on what it brings; may be empty
    std::string page;     ///< where to get it: a page on the suite's own website

    /** Every field equal.
        @return  true when every field is equal */
    bool operator==(const Release &) const = default;
};

/** The release a manifest names: a JSON object with "version" (MAJOR.MINOR.BUILD) and "page" (a page on the suite's
    own website, kTrustedPagePrefix), and optionally "released" and "summary".
    @param text  the manifest's bytes
    @return      the release, or nothing for a manifest that is not one */
inline std::optional<Release> ReadManifest(std::string_view text)
{
    const core::json::ParseResult parsed = core::json::Parse(text);
    if (!parsed.IsOk() || !parsed.value.IsObject())
    {
        return std::nullopt;
    }
    const core::json::Value *version = parsed.value.Find("version");
    const core::json::Value *page    = parsed.value.Find("page");
    if (version == nullptr || !version->IsString() || page == nullptr || !page->IsString())
    {
        return std::nullopt;
    }
    const std::optional<Version> number = ParseVersion(version->AsString());
    if (!number || !page->AsString().starts_with(kTrustedPagePrefix))
    {
        return std::nullopt;
    }
    Release release;
    release.version = *number;
    release.page    = page->AsString();
    if (const core::json::Value *released = parsed.value.Find("released"); released != nullptr && released->IsString())
    {
        release.released = released->AsString();
    }
    if (const core::json::Value *summary = parsed.value.Find("summary"); summary != nullptr && summary->IsString())
    {
        release.summary = summary->AsString();
    }
    return release;
}

/** `release` as the manifest and the shared file hold it.
    @param release  the release
    @return         its JSON object */
inline core::json::Value ManifestOf(const Release &release)
{
    core::json::Value document = core::json::Value::MakeObject();
    document.Set("version", VersionText(release.version));
    document.Set("released", release.released);
    document.Set("summary", release.summary);
    document.Set("page", release.page);
    return document;
}

/** Where the last check stands, as the settings gear says it. */
enum class State
{
    kUnchecked, ///< no check has answered on this machine
    kChecking,  ///< a check is in flight from this process
    kCurrent,   ///< the last check found nothing newer than this build
    kAvailable, ///< a newer release is out
    kFailed,    ///< the last check had no answer: no network, or no manifest where it should be
};

/**
    The update check one process runs and every plugin in it reads: the shared file read whenever it moves, a check
    started when one is due and a window is on a screen, its answer written back for every other process. Every member
    is for the message thread but the worker's own; a check runs on a thread of its own and is cancelled and joined
    when the checker goes, so a host that unloads a plugin mid-check never waits for the network's timeout.
*/
class Checker
{
  public:
    /** Seconds since the epoch on the wall clock: what the once-a-day is counted in, across processes. */
    using Clock = std::function<double()>;
    /** Makes the transport a check sends its request through: the platform's own, or a suite's stand-in. */
    using TransportMaker = std::function<std::unique_ptr<net::Transport>()>;

    /** How long a successful check stands before the next is due. */
    static constexpr double kIntervalSeconds = 24.0 * 60.0 * 60.0;
    /** How long after a failed check the next is due: a laptop that was offline tries again within the hour. */
    static constexpr double kRetrySeconds = 60.0 * 60.0;
    /** The most a check may take, end to end. */
    static constexpr double kTimeoutSeconds = 15.0;

    /** A checker on `file`, read from it now.
        @param file       the shared file (DefaultFile in the plugins); its folder is made when it is first written
        @param current    the version this build is
        @param url        the manifest's address (kManifestUrl in the plugins)
        @param transport  makes the transport each check uses, on the check's thread
        @param clock      seconds since the epoch; the system clock by default
        @param allowed    false where checks are switched off for the whole process (a CI run's environment): no check
                          starts, by the clock or by hand
        @param product    the product the request's User-Agent names, before its version: the plugins' by default, a
                          separate product's own (Reckless IR Capture's RecklessIRCapture) */
    Checker(std::filesystem::path file, Version current, std::string url, TransportMaker transport,
            Clock clock = SystemSeconds, bool allowed = true, std::string product = "RecklessPlugins")
        : file_(std::move(file)), current_(current), url_(std::move(url)), transport_(std::move(transport)),
          clock_(std::move(clock)), allowed_(allowed), product_(std::move(product))
    {
        Read();
    }

    /** Any check in flight cancelled and its thread joined. */
    ~Checker()
    {
        cancellation_.Cancel();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    Checker(const Checker &)            = delete;
    Checker &operator=(const Checker &) = delete;

    /** The shared file: updates.json in the suite's folder of the user's settings.
        @return  the path; empty where the platform names no settings folder */
    static std::filesystem::path DefaultFile()
    {
        const std::filesystem::path folder = core::UserSettingsFolder(core::kSuiteFolder);
        return folder.empty() ? folder : folder / "updates.json";
    }

    /** The wall clock in seconds since the epoch.
        @return  the time now */
    static double SystemSeconds()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /** What a timer calls: a finished check's answer taken and written, the file read again if another process moved
        it, and a check started where one is due - a window on a screen, checks automatic, and a day since the last
        that answered, or an hour since one that did not.
        @param shown  whether a window of this process's plugins is on a screen a person can see */
    void Poll(bool shown)
    {
        Collect();
        if (StampOf(file_) != stamp_)
        {
            Read();
        }
        if (!shown || !automatic_ || in_flight_ || !allowed_)
        {
            return;
        }
        const double wait = succeeded_ ? kIntervalSeconds : kRetrySeconds;
        if (attempted_ <= 0.0 || clock_() - attempted_ >= wait || clock_() < attempted_)
        {
            Start();
        }
    }

    /** A check now, whatever the clock and the setting say, unless one is in flight or checks are switched off for the
        process. */
    void CheckNow()
    {
        Collect();
        if (!in_flight_ && allowed_)
        {
            Start();
        }
    }

    /** Whether checks are made by the clock.
        @return  true unless the person turned them off */
    bool automatic() const noexcept
    {
        return automatic_;
    }

    /** Checks by the clock on or off, for every plugin on the machine.
        @param on  true to check once a day */
    void SetAutomatic(bool on)
    {
        if (on == automatic_)
        {
            return;
        }
        automatic_ = on;
        Write();
        ++revision_;
    }

    /** Where the last check stands.
        @return  checking while one is in flight from here; else what the last answer said */
    State state() const noexcept
    {
        if (in_flight_)
        {
            return State::kChecking;
        }
        if (latest_ && latest_->version > current_)
        {
            return State::kAvailable;
        }
        if (!succeeded_ && attempted_ > 0.0)
        {
            return State::kFailed;
        }
        return latest_ ? State::kCurrent : State::kUnchecked;
    }

    /** The newer release, where one is out.
        @return  the release; nothing where none newer than this build is known */
    std::optional<Release> available() const
    {
        if (latest_ && latest_->version > current_)
        {
            return latest_;
        }
        return std::nullopt;
    }

    /** The version this build is.
        @return  the version the checker compares with */
    Version current() const noexcept
    {
        return current_;
    }

    /** Moves whenever what the gear or the header would show may have changed.
        @return  the count, which only rises */
    std::uint64_t revision() const noexcept
    {
        return revision_;
    }

    /** The shared file.
        @return  the path */
    const std::filesystem::path &file() const noexcept
    {
        return file_;
    }

  private:
    /** A file's size and last write, to tell whether another process moved it. */
    struct Stamp
    {
        bool                            exists = false; ///< whether the file is there
        std::uintmax_t                  size   = 0;     ///< its size in bytes
        std::filesystem::file_time_type time{};         ///< its last write

        /** Every field equal.
            @return  true when every field is equal */
        bool operator==(const Stamp &) const = default;
    };

    /** What a check found, handed from its thread to the message thread. */
    struct Answer
    {
        bool                   done = false; ///< the check has finished
        std::optional<Release> release;      ///< what the manifest named; nothing where the check had no answer
    };

    /** `file`'s stamp.
        @param file  the file
        @return      its existence, size and last write */
    static Stamp StampOf(const std::filesystem::path &file)
    {
        Stamp           stamp;
        std::error_code error;
        if (file.empty() || !std::filesystem::is_regular_file(file, error))
        {
            return stamp;
        }
        stamp.exists = true;
        stamp.size   = std::filesystem::file_size(file, error);
        stamp.time   = std::filesystem::last_write_time(file, error);
        return stamp;
    }

    /** The shared file read: the setting, when a check was last made and whether it answered, and the release it
        named. A file that is missing or malformed reads as nothing checked, checks automatic. */
    void Read()
    {
        stamp_                           = StampOf(file_);
        bool                   automatic = true;
        double                 attempted = 0.0;
        bool                   succeeded = false;
        std::optional<Release> latest;
        if (const std::optional<core::json::Value> document = core::ReadJsonFile(file_);
            document && document->IsObject())
        {
            if (const auto *value = document->Find("automatic"); value != nullptr && value->IsBool())
            {
                automatic = value->AsBool();
            }
            if (const auto *value = document->Find("attempted"); value != nullptr && value->IsNumber())
            {
                attempted = value->AsDouble();
            }
            if (const auto *value = document->Find("succeeded"); value != nullptr && value->IsBool())
            {
                succeeded = value->AsBool();
            }
            if (const auto *value = document->Find("release"); value != nullptr && value->IsObject())
            {
                latest = ReadManifest(core::json::Write(*value));
            }
        }
        if (automatic != automatic_ || attempted != attempted_ || succeeded != succeeded_ || latest != latest_)
        {
            automatic_ = automatic;
            attempted_ = attempted;
            succeeded_ = succeeded;
            latest_    = std::move(latest);
            ++revision_;
        }
    }

    /** The shared file written from what this checker holds, and its stamp taken, so this process does not read its
        own write back as another's. */
    void Write()
    {
        if (file_.empty())
        {
            return;
        }
        core::json::Value document = core::json::Value::MakeObject();
        document.Set("automatic", automatic_);
        document.Set("attempted", attempted_);
        document.Set("succeeded", succeeded_);
        if (latest_)
        {
            document.Set("release", ManifestOf(*latest_));
        }
        core::WriteJsonFile(file_, document);
        stamp_ = StampOf(file_);
    }

    /** A check started on its own thread, its time written first so every other process waits for its answer rather
        than making one of its own. */
    void Start()
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
        attempted_ = clock_();
        Write();
        in_flight_ = true;
        ++revision_;
        {
            const std::lock_guard<std::mutex> lock(answer_lock_);
            answer_ = Answer{};
        }
        worker_ = std::thread([this] {
            Answer                          answer;
            std::unique_ptr<net::Transport> transport = transport_ ? transport_() : nullptr;
            if (transport && !cancellation_.cancelled())
            {
                net::Request request;
                request.url             = url_;
                request.timeout_seconds = kTimeoutSeconds;
                request.headers.push_back({"User-Agent", product_ + "/" + VersionText(current_)});
                request.headers.push_back({"Cache-Control", "no-cache"});
                const net::Response response = net::Fetch(*transport, request, cancellation_);
                if (response.status == 200)
                {
                    answer.release = ReadManifest(response.body);
                }
            }
            answer.done = true;
            const std::lock_guard<std::mutex> lock(answer_lock_);
            answer_ = std::move(answer);
        });
    }

    /** A finished check's answer taken on the message thread: what it found written for every process. */
    void Collect()
    {
        if (!in_flight_)
        {
            return;
        }
        Answer answer;
        {
            const std::lock_guard<std::mutex> lock(answer_lock_);
            if (!answer_.done)
            {
                return;
            }
            answer = std::move(answer_);
        }
        worker_.join();
        in_flight_ = false;
        succeeded_ = answer.release.has_value();
        if (answer.release)
        {
            latest_ = std::move(answer.release);
        }
        Write();
        ++revision_;
    }

    std::filesystem::path  file_;              ///< the shared file
    Version                current_;           ///< this build's version
    std::string            url_;               ///< the manifest's address
    TransportMaker         transport_;         ///< makes each check's transport
    Clock                  clock_;             ///< seconds since the epoch
    bool                   allowed_ = true;    ///< false where checks are off for the whole process
    std::string            product_;           ///< what the User-Agent names before the version
    Stamp                  stamp_;             ///< the shared file as last read or written
    bool                   automatic_ = true;  ///< checks by the clock
    double                 attempted_ = 0.0;   ///< when the last check began, in seconds since the epoch; 0 for never
    bool                   succeeded_ = false; ///< whether it answered
    std::optional<Release> latest_;            ///< the newest release any check on this machine has heard of
    bool                   in_flight_ = false; ///< a check from this process has not been collected
    std::uint64_t          revision_  = 1;     ///< moves with what the gear and the header show
    std::mutex             answer_lock_;       ///< guards answer_ between the check's thread and the message thread
    Answer                 answer_;            ///< the check's answer, handed over
    net::Cancellation      cancellation_;      ///< cancels a check in flight as the checker goes
    std::thread            worker_;            ///< the check's thread
};
} // namespace reckless::update
