#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reckless::core
{
/**
    The machine's logical processors as a thread scheduler of an application's own needs to see them: which share a
    physical core (SMT siblings), which are the performance cores and which the efficiency cores of a hybrid machine
    (Intel's P and E cores, Apple's and arm64's big and little), and how fast one is against the fastest where the
    platform says. With the pinning of a calling thread to one of them (ThreadPin).

    Where it comes from: on Linux sysfs (`ReadCpuTopologyFromSysfs`, pure on a root it is handed, so a suite feeds it
    fixture trees) held to the process's affinity mask, which is the container's cpuset; on Windows
    `GetLogicalProcessorInformationEx`; on macOS `sysctlbyname`'s performance levels. `DetectCpuTopology()` reads
    the running machine. Nothing here is realtime-safe: it reads files and allocates, and is called at start-up, not
    on an audio thread.
*/

/// How a logical processor performs against the machine's others.
enum class CpuClass
{
    kPerformance, ///< a performance core, or any core of a machine with one class alone
    kEfficiency   ///< an efficiency core of a hybrid machine: slower, and not for a deadline's thread
};

/// One logical processor.
struct LogicalProcessor
{
    int      id{0};    ///< the platform's number: Linux's `cpuN`; Windows' `group * 64 + bit`; macOS's ordinal only
    int      group{0}; ///< Windows' processor group; 0 elsewhere
    int      core{0};  ///< its physical core, numbered from 0 in the order the processors list first meets each one
    bool     is_smt_sibling{false};             ///< true when an earlier processor in the list shares its physical core
    CpuClass cpu_class{CpuClass::kPerformance}; ///< performance or efficiency
    double   relative_capacity{0.0}; ///< against the fastest processor, 1.0 for it; 0 where the platform gives none
};

/// The logical processors a process may run on, in the platform's order.
struct CpuTopology
{
    std::vector<LogicalProcessor> processors;    ///< at least one: a broken or unreadable source gives one per thread
    int                           core_count{0}; ///< the distinct physical cores among `processors`
    bool        is_detected{false}; ///< false when the platform's source could not be read: `processors` is a guess
    std::string source;             ///< where it was read from, e.g. "sysfs", for a settings card

    /** @return  true when the machine has both performance and efficiency processors among these */
    bool IsHybrid() const
    {
        const bool has_performance = CountOf(CpuClass::kPerformance) > 0;
        return has_performance && CountOf(CpuClass::kEfficiency) > 0;
    }

    /** @param cpu_class  the class to count
        @return           how many of the logical processors are of it */
    int CountOf(CpuClass cpu_class) const
    {
        return static_cast<int>(
            std::count_if(processors.begin(), processors.end(),
                          [cpu_class](const LogicalProcessor &p) { return p.cpu_class == cpu_class; }));
    }
};

/** The machine's topology, read now: the processors this process may run on.
    @return  the topology; when the platform gives nothing, one single-threaded core per `hardware_concurrency`
             (`is_detected` false) */
CpuTopology DetectCpuTopology();

/**
    What happened to a thread pinned to one logical processor (ThreadPin): whether it is, and if not why - macOS has no
    pinning on Apple silicon, and a cpuset may forbid a processor.
*/
struct PinResult
{
    bool        is_pinned{false}; ///< true when the thread now runs on that processor alone
    std::string refused; ///< why not, e.g. "macOS gives a thread no way to pin it to a core"; empty when pinned
};

/**
    The calling thread pinned to one logical processor for the life of the object: `sched_setaffinity` on Linux,
    `SetThreadGroupAffinity` on Windows, and **not supported on macOS** (Apple silicon has no pinning; the old affinity
    tags are only a hint that two threads share a cache, so they are not used as one). Put back on destruction or
    Unpin(). Thread-bound: construct, use and destroy on one thread. Once per thread, so it may allocate.
*/
class ThreadPin
{
  public:
    /** The calling thread pinned to `processor`.
        @param processor  one of DetectCpuTopology()'s processors */
    explicit ThreadPin(const LogicalProcessor &processor);

    ThreadPin(const ThreadPin &)            = delete;
    ThreadPin &operator=(const ThreadPin &) = delete;

    /** The thread's earlier affinity put back. */
    ~ThreadPin()
    {
        Unpin();
    }

    /** @return  whether it is pinned and, if not, why */
    const PinResult &Result() const
    {
        return result_;
    }

    /** The thread's earlier affinity put back, on the thread that pinned it; harmless twice. */
    void Unpin() noexcept
    {
        if (undo_)
        {
            const std::function<void()> undo = std::exchange(undo_, nullptr);
            undo();
        }
        result_.is_pinned = false;
    }

  private:
    PinResult             result_; ///< what the pin did
    std::function<void()> undo_;   ///< puts the affinity back; empty when not pinned
};

// ---- Linux's sysfs, a pure function of the tree it is handed
// ---------------------------------------------------------

namespace sysfs
{
/** A kernel CPU list - "0-3,8,10-11", as `thread_siblings_list` and `/sys/devices/cpu_core/cpus` write it - as numbers.
    @param text  the list, which may end with a newline
    @return      the numbers in the order written; empty for text that is not a list */
inline std::vector<int> ParseCpuList(std::string_view text)
{
    std::vector<int> numbers;
    std::size_t      at   = 0;
    const auto       read = [&](int &value) {
        const char *first  = text.data() + at;
        const char *last   = text.data() + text.size();
        const auto  parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc() || value < 0)
        {
            return false;
        }
        at += static_cast<std::size_t>(parsed.ptr - first);
        return true;
    };
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
    {
        ++at;
    }
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) == 0)
    {
        int first = 0;
        if (!read(first))
        {
            return {};
        }
        int last = first;
        if (at < text.size() && text[at] == '-')
        {
            ++at;
            if (!read(last) || last < first || last - first > 65535)
            {
                return {};
            }
        }
        for (int n = first; n <= last; ++n)
        {
            numbers.push_back(n);
        }
        if (at < text.size() && text[at] == ',')
        {
            ++at;
            if (at >= text.size())
            {
                return {}; // a list does not end on its comma
            }
            continue;
        }
        break;
    }
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
    {
        ++at;
    }
    return at == text.size() ? numbers : std::vector<int>{};
}

/** A small text file's contents, with its trailing whitespace trimmed.
    @param path  the file
    @return      the text; nothing when the file cannot be read */
inline std::optional<std::string> ReadText(const std::filesystem::path &path)
{
    std::ifstream file(path);
    if (!file)
    {
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    std::string result = text.str();
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())) != 0)
    {
        result.pop_back();
    }
    return result;
}

/** A file holding one integer.
    @param path  the file
    @return      the number; nothing when the file is missing or holds anything else */
inline std::optional<long long> ReadNumber(const std::filesystem::path &path)
{
    const std::optional<std::string> text = ReadText(path);
    if (!text || text->empty())
    {
        return std::nullopt;
    }
    long long  value  = 0;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text->data() + text->size())
    {
        return std::nullopt;
    }
    return value;
}

/** The processors of one class of a hybrid machine, from a file like `/sys/devices/cpu_core/cpus`.
    @param path  the file
    @return      the set; empty when the file is missing */
inline std::set<int> ReadCpuSet(const std::filesystem::path &path)
{
    const std::optional<std::string> text = ReadText(path);
    if (!text)
    {
        return {};
    }
    const std::vector<int> numbers = ParseCpuList(*text);
    return {numbers.begin(), numbers.end()};
}

/** Below this share of the fastest processor's capacity (or frequency) a processor is an efficiency one, where the
    machine is not hybrid in the kernel's own words: big.LITTLE's middle cores, at about 0.8 of the biggest, stay
    performance cores, and its little ones, at 0.4 or less, are efficiency ones. */
inline constexpr double kEfficiencyBelow = 0.7;
} // namespace sysfs

/**
    The topology a Linux sysfs tree describes, read from `root` (`/sys` on the machine, a fixture's directory in a
    suite): `devices/system/cpu/cpuN/topology/{core_id,physical_package_id,thread_siblings_list}` for the cores and
    their SMT siblings; `devices/cpu_core/cpus` and `devices/cpu_atom/cpus` where an Intel hybrid has them, else
    `cpuN/cpu_capacity` (arm64), else `cpuN/cpufreq/cpuinfo_max_freq`, for the classes and the relative capacity
    (capacity, or the frequency against the highest).

    @param root              the root of the sysfs tree
    @param allowed           the processors the process may run on (its affinity mask, which a container's cpuset
                             narrows); none for all that are online. A processor outside it is not listed, and a
                             sibling outside it does not make one inside an SMT sibling
    @param fallback_count    the processors to list, one core each, when the tree gives none: the machine's
                             `hardware_concurrency`; at least one is listed
    @return                  the topology, `is_detected` false when the tree gave none and the fallback was used
*/
inline CpuTopology ReadCpuTopologyFromSysfs(const std::filesystem::path           &root,
                                            const std::optional<std::vector<int>> &allowed, int fallback_count)
{
    namespace fs = std::filesystem;
    CpuTopology topology;
    topology.source = "sysfs";

    // The processors the tree lists, as their numbers, online ones only.
    std::vector<int> listed;
    std::error_code  error;
    const fs::path   cpu_root = root / "devices" / "system" / "cpu";
    for (fs::directory_iterator it(cpu_root, error), end; !error && it != end; it.increment(error))
    {
        const std::string name = it->path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 ||
            !std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
        {
            continue;
        }
        const int id = std::stoi(name.substr(3));
        if (sysfs::ReadNumber(it->path() / "online").value_or(1) == 0)
        {
            continue;
        }
        if (allowed && std::find(allowed->begin(), allowed->end(), id) == allowed->end())
        {
            continue;
        }
        listed.push_back(id);
    }
    std::sort(listed.begin(), listed.end());

    if (listed.empty())
    {
        // Nothing readable: the processors the process may use, else as many as the machine reports, one core each.
        topology.source      = "fallback";
        topology.is_detected = false;
        std::vector<int> ids = allowed ? *allowed : std::vector<int>{};
        if (ids.empty())
        {
            for (int n = 0; n < std::max(1, fallback_count); ++n)
            {
                ids.push_back(n);
            }
        }
        std::sort(ids.begin(), ids.end());
        for (const int id : ids)
        {
            LogicalProcessor processor;
            processor.id   = id;
            processor.core = static_cast<int>(topology.processors.size());
            topology.processors.push_back(processor);
        }
        topology.core_count = static_cast<int>(topology.processors.size());
        return topology;
    }

    const std::set<int> performance_cores = sysfs::ReadCpuSet(root / "devices" / "cpu_core" / "cpus");
    const std::set<int> efficiency_cores  = sysfs::ReadCpuSet(root / "devices" / "cpu_atom" / "cpus");
    const bool          has_hybrid_lists  = !performance_cores.empty() && !efficiency_cores.empty();

    // Each processor's capacity figure: the kernel's cpu_capacity where it tells the processors apart (on an Intel
    // hybrid it reads 1024 for every one, which says nothing), else the highest frequency, else cpu_capacity as it is.
    const auto read_figures = [&](const char *file, const char *folder) {
        std::map<int, double> figures;
        for (const int id : listed)
        {
            fs::path path = cpu_root / ("cpu" + std::to_string(id));
            if (folder[0] != '\0')
            {
                path /= folder;
            }
            if (const auto value = sysfs::ReadNumber(path / file))
            {
                figures[id] = static_cast<double>(*value);
            }
        }
        return figures;
    };
    const auto tells_apart = [](const std::map<int, double> &figures) {
        return std::any_of(figures.begin(), figures.end(),
                           [&](const auto &entry) { return entry.second != figures.begin()->second; });
    };
    std::map<int, double> capacity = read_figures("cpu_capacity", "");
    if (!tells_apart(capacity))
    {
        const std::map<int, double> frequency = read_figures("cpuinfo_max_freq", "cpufreq");
        if (tells_apart(frequency) || capacity.empty())
        {
            capacity = frequency;
        }
    }
    double highest = 0.0;
    for (const auto &entry : capacity)
    {
        highest = std::max(highest, entry.second);
    }

    std::map<std::pair<int, long long>, int> core_of; // the core's identity -> its number
    for (const int id : listed)
    {
        const fs::path cpu      = cpu_root / ("cpu" + std::to_string(id));
        const auto     siblings = sysfs::ReadText(cpu / "topology" / "thread_siblings_list");
        const auto     core_id  = sysfs::ReadNumber(cpu / "topology" / "core_id");
        const int package = static_cast<int>(sysfs::ReadNumber(cpu / "topology" / "physical_package_id").value_or(0));
        // The core's identity: the lowest of its siblings, which every one of them names; else package and core id.
        std::pair<int, long long> identity{-1, id};
        if (siblings && !sysfs::ParseCpuList(*siblings).empty())
        {
            const std::vector<int> list = sysfs::ParseCpuList(*siblings);
            identity                    = {0, *std::min_element(list.begin(), list.end())};
        }
        else if (core_id)
        {
            identity = {1 + package, *core_id};
        }
        LogicalProcessor processor;
        processor.id             = id;
        const auto existing      = core_of.find(identity);
        processor.is_smt_sibling = existing != core_of.end();
        processor.core           = processor.is_smt_sibling ? existing->second : static_cast<int>(core_of.size());
        if (!processor.is_smt_sibling)
        {
            core_of[identity] = processor.core;
        }
        const auto figure = capacity.find(id);
        if (figure != capacity.end() && highest > 0.0)
        {
            processor.relative_capacity = figure->second / highest;
        }
        if (has_hybrid_lists)
        {
            processor.cpu_class = efficiency_cores.count(id) != 0 && performance_cores.count(id) == 0
                                      ? CpuClass::kEfficiency
                                      : CpuClass::kPerformance;
        }
        else if (processor.relative_capacity > 0.0 && processor.relative_capacity < sysfs::kEfficiencyBelow)
        {
            processor.cpu_class = CpuClass::kEfficiency;
        }
        topology.processors.push_back(processor);
    }
    topology.core_count  = static_cast<int>(core_of.size());
    topology.is_detected = true;
    return topology;
}
} // namespace reckless::core
