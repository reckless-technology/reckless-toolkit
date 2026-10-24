#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "Json.h"
#include "JsonFile.h"
#include "Licence.h"
#include "PresetFiles.h"

namespace reckless::licence
{
/** Where a licence was found. */
enum class Source
{
    kNone,    ///< in neither file
    kUser,    ///< the user's own file
    kMachine, ///< the file every user of the machine shares
};

/** The licence a machine holds as a panel reads it: the check, and where the text came from. */
struct Status
{
    Checked checked;                ///< the verdict and what the licence says; kMalformed with no licence when none
    Source  source = Source::kNone; ///< which file it is in
};

/**
    The licence every plugin on a machine runs under, kept in a file rather than in any session or preset, which go to
    other people: the user's own - `reckless-technology/licence.json` in the user's settings folder, readable by its
    owner alone on macOS and Linux (0600) - and, where that holds no valid licence, the one every user of the machine
    shares - `reckless-technology/licence.json` in the machine's settings folder, which an administrator puts there and
    which a host that keeps its plug-ins from the user's own folder (a sandboxed one) can still read. Each holds
    `{"licence": "<the text>"}`; the machine's may hold the bare text instead, as a licence file arrives.

    Read when made and again by Poll whenever either file's size or time moves, so a licence installed by another
    instance, another format or another process is the one every instance runs under. `licensed` is one atomic the
    audio thread may read; everything else is for the message thread and takes a lock.
*/
class Store
{
  public:
    /** A store on `user_file` and `machine_file`, read from them now.
        @param user_file     the user's own licence file; its folder is made when it is first written
        @param machine_file  the machine's, read and never written
        @param rules         what a licence is checked against; its spans must outlive the store */
    Store(std::filesystem::path user_file, std::filesystem::path machine_file, Rules rules)
        : user_file_(std::move(user_file)), machine_file_(std::move(machine_file)), rules_(rules)
    {
        Reload();
    }

    /** The user's licence file: licence.json in the suite's folder of the user's settings.
        @return  the path; empty where the platform names no settings folder */
    static std::filesystem::path DefaultUserFile()
    {
        const std::filesystem::path folder = core::UserSettingsFolder(core::kSuiteFolder);
        return folder.empty() ? folder : folder / "licence.json";
    }

    /** The machine's licence file: licence.json in the suite's folder of the machine's settings.
        @return  the path; empty where the platform names no such folder */
    static std::filesystem::path DefaultMachineFile()
    {
        const std::filesystem::path folder = core::MachineSettingsFolder(core::kSuiteFolder);
        return folder.empty() ? folder : folder / "licence.json";
    }

    /** Both files read again if either has moved since they were last read: cheap enough for a timer to call often.
        @return  true when they were read again */
    bool Poll()
    {
        {
            const std::lock_guard<std::mutex> lock(lock_);
            if (StampOf(user_file_) == user_stamp_ && StampOf(machine_file_) == machine_stamp_)
            {
                return false;
            }
        }
        Reload();
        return true;
    }

    /** Both files read now, whatever their stamps. */
    void Reload()
    {
        const Stamp  user_stamp    = StampOf(user_file_);
        const Stamp  machine_stamp = StampOf(machine_file_);
        const auto   user_text     = TextIn(user_file_);
        const auto   machine_text  = TextIn(machine_file_);
        const Status from_user     = StatusOf(user_text, Source::kUser);
        const Status from_machine  = StatusOf(machine_text, Source::kMachine);
        // The user's file first; the machine's where the user's holds no valid licence; a file that holds an invalid
        // one is reported over no file at all, so the dialog can say why.
        const Status chosen = from_user.checked.verdict == Verdict::kValid      ? from_user
                              : from_machine.checked.verdict == Verdict::kValid ? from_machine
                              : user_text                                       ? from_user
                                                                                : from_machine;

        const std::lock_guard<std::mutex> lock(lock_);
        user_stamp_    = user_stamp;
        machine_stamp_ = machine_stamp;
        if (!SameStatus(chosen, status_))
        {
            status_ = chosen;
            licensed_.store(chosen.checked.verdict == Verdict::kValid, std::memory_order_relaxed);
            revision_.fetch_add(1);
        }
    }

    /** A licence the person gives, checked and, when it is valid, written to the user's file in place of any there.
        Nothing is written for one that is not, so a mistyped licence never displaces a good one.
        @param text  the licence as the person has it, white space and all
        @return      the check; kValid when it is now the user's licence */
    Checked Install(std::string_view text)
    {
        const Checked checked = Check(text, rules_);
        if (checked.verdict != Verdict::kValid)
        {
            return checked;
        }
        core::json::Value document = core::json::Value::MakeObject();
        document.Set("licence", WithoutWhiteSpace(text));
        if (!core::WriteJsonFile(user_file_, document))
        {
            Checked unwritten = checked;
            unwritten.verdict = Verdict::kMalformed;
            return unwritten;
        }
#if !defined(_WIN32)
        std::error_code error;
        std::filesystem::permissions(user_file_,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, error);
#endif
        Reload();
        return checked;
    }

    /** The user's licence taken off the machine: its file deleted. A licence in the machine's file still holds.
        @return  true when no user file is left */
    bool Remove()
    {
        std::error_code error;
        std::filesystem::remove(user_file_, error);
        Reload();
        return !std::filesystem::exists(user_file_, error);
    }

    /** Whether a valid licence holds: one relaxed atomic read, safe on the audio thread.
        @return  true when licensed */
    bool licensed() const noexcept
    {
        return licensed_.load(std::memory_order_relaxed);
    }

    /** The flag `licensed` reads, for a demo gate to hold (core::DemoGate::SetLicence).
        @return  the store's own atomic, alive as long as the store */
    const std::atomic<bool> &licensed_flag() const noexcept
    {
        return licensed_;
    }

    /** The licence as a panel shows it.
        @return  a copy of the status */
    Status status() const
    {
        const std::lock_guard<std::mutex> lock(lock_);
        return status_;
    }

    /** Moves on every change of the status: a panel that reads the same number reads nothing else.
        @return  the count, which only rises */
    std::uint64_t revision() const noexcept
    {
        return revision_.load();
    }

    /** The user's file this store writes.
        @return  the path */
    const std::filesystem::path &user_file() const noexcept
    {
        return user_file_;
    }

  private:
    /** What says a file has moved: whether it is there, its size and its last write. */
    struct Stamp
    {
        bool                            exists = false; ///< whether the file is there
        std::uintmax_t                  size   = 0;     ///< its size in bytes
        std::filesystem::file_time_type time{};         ///< its last write

        /** Every field equal.
            @param other  the other stamp
            @return       true when the two are alike */
        bool operator==(const Stamp &other) const = default;
    };

    /** `file`'s stamp, an absent one for a file that is not there or an empty path.
        @param file  the file
        @return      its stamp */
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

    /** The licence text `file` holds: its `licence` member, or the whole file where it is not a JSON object - the bare
        text of a licence file copied in.
        @param file  the file
        @return      the text, or nothing when the file is missing or empty */
    static std::optional<std::string> TextIn(const std::filesystem::path &file)
    {
        const std::optional<std::string> bytes = file.empty() ? std::nullopt : core::ReadBytes(file);
        if (!bytes || bytes->empty())
        {
            return std::nullopt;
        }
        core::json::ParseResult parsed = core::json::Parse(*bytes);
        if (parsed.IsOk() && parsed.value.IsObject())
        {
            return parsed.value.StringOr("licence", "");
        }
        return *bytes;
    }

    /** The status a file's text gives.
        @param text    the text, or nothing for no file
        @param source  which file it came from
        @return        the status; none for no text */
    Status StatusOf(const std::optional<std::string> &text, Source source) const
    {
        Status status;
        if (text)
        {
            status.checked = Check(*text, rules_);
            status.source  = source;
        }
        return status;
    }

    /** Whether two statuses say the same, to the panel.
        @param a  one
        @param b  the other
        @return   true when the verdict, the source and the licence's serial are alike */
    static bool SameStatus(const Status &a, const Status &b)
    {
        const auto serial = [](const Status &s) { return s.checked.licence ? s.checked.licence->serial : ""; };
        const auto name   = [](const Status &s) { return s.checked.licence ? s.checked.licence->name : ""; };
        return a.checked.verdict == b.checked.verdict && a.source == b.source && serial(a) == serial(b) &&
               name(a) == name(b);
    }

    std::filesystem::path      user_file_;       ///< the user's licence file
    std::filesystem::path      machine_file_;    ///< the machine's licence file
    Rules                      rules_;           ///< what a licence is checked against
    mutable std::mutex         lock_;            ///< guards the stamps and the status
    Stamp                      user_stamp_;      ///< the user's file as last read
    Stamp                      machine_stamp_;   ///< the machine's file as last read
    Status                     status_;          ///< the licence as last read
    std::atomic<bool>          licensed_{false}; ///< whether status_ is valid, for the audio thread
    std::atomic<std::uint64_t> revision_{0};     ///< moves with every change of status_
};
} // namespace reckless::licence
