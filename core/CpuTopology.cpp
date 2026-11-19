// The machine's processors and the pinning of a thread to one (CpuTopology.h): the platform's part. Linux's topology is
// the header's own pure reader on `/sys`, held to the process's affinity; Windows' is GetLogicalProcessorInformationEx;
// macOS' is sysctl's performance levels, which name no processor and so give no pinning.
#include "CpuTopology.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace reckless::core
{
namespace
{
/** @return  the machine's thread count, at least one */
int HardwareThreads()
{
    return static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
}

#if defined(_WIN32)
/** The logical processors Windows lists, a core at a time: the core's `EfficiencyClass` (the higher is the faster; a
    machine whose cores all report one has one class), its SMT flag and every group mask's bits.
    @return  the topology, empty-handed (`is_detected` false) where the call fails */
CpuTopology DetectWindows()
{
    CpuTopology topology;
    topology.source = "GetLogicalProcessorInformationEx";
    DWORD bytes     = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<char> buffer(bytes);
    if (bytes == 0 ||
        !GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bytes))
    {
        return topology;
    }
    BYTE highest = 0;
    for (std::size_t at = 0; at < bytes;)
    {
        const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + at);
        highest           = std::max(highest, entry->Processor.EfficiencyClass);
        at += entry->Size;
    }
    int core = 0;
    for (std::size_t at = 0; at < bytes;)
    {
        const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + at);
        at += entry->Size;
        bool is_first = true;
        for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
        {
            const GROUP_AFFINITY &mask = entry->Processor.GroupMask[g];
            for (int bit = 0; bit < 64; ++bit)
            {
                if ((mask.Mask & (static_cast<KAFFINITY>(1) << bit)) == 0)
                {
                    continue;
                }
                LogicalProcessor processor;
                processor.id             = mask.Group * 64 + bit;
                processor.group          = mask.Group;
                processor.core           = core;
                processor.is_smt_sibling = !is_first;
                processor.cpu_class =
                    entry->Processor.EfficiencyClass < highest ? CpuClass::kEfficiency : CpuClass::kPerformance;
                topology.processors.push_back(processor);
                is_first = false;
            }
        }
        ++core;
    }
    topology.core_count  = core;
    topology.is_detected = !topology.processors.empty();
    return topology;
}
#elif defined(__APPLE__)
/** An integer sysctl.
    @param name  the sysctl's name
    @return      its value; nothing when the system has none by that name */
std::optional<int> ReadSysctl(const char *name)
{
    int         value = 0;
    std::size_t size  = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
    {
        return std::nullopt;
    }
    return value;
}

/** The performance levels `hw.nperflevels` counts - level 0 the performance cores, the others efficiency ones - or, on
    an Intel Mac that has none, `hw.physicalcpu` and `hw.logicalcpu` as one level. The processors are listed in
    ordinal order, performance first, which names no hardware thread: macOS cannot pin a thread to one.
    @return  the topology, `is_detected` false where sysctl gives none */
CpuTopology DetectMac()
{
    CpuTopology topology;
    topology.source      = "sysctl";
    const int  levels    = ReadSysctl("hw.nperflevels").value_or(0);
    int        ordinal   = 0;
    const auto add_level = [&](int logical, int physical, CpuClass cpu_class) {
        const int per_core = physical > 0 ? std::max(1, logical / physical) : 1;
        for (int n = 0; n < logical; ++n)
        {
            LogicalProcessor processor;
            processor.id             = ordinal++;
            processor.core           = topology.core_count + n / per_core;
            processor.is_smt_sibling = n % per_core != 0;
            processor.cpu_class      = cpu_class;
            topology.processors.push_back(processor);
        }
        topology.core_count += (logical + per_core - 1) / per_core;
    };
    for (int level = 0; level < levels; ++level)
    {
        const std::string prefix   = "hw.perflevel" + std::to_string(level);
        const int         logical  = ReadSysctl((prefix + ".logicalcpu").c_str()).value_or(0);
        const int         physical = ReadSysctl((prefix + ".physicalcpu").c_str()).value_or(logical);
        add_level(logical, physical, level == 0 ? CpuClass::kPerformance : CpuClass::kEfficiency);
    }
    if (topology.processors.empty())
    {
        add_level(ReadSysctl("hw.logicalcpu").value_or(0), ReadSysctl("hw.physicalcpu").value_or(0),
                  CpuClass::kPerformance);
    }
    topology.is_detected = !topology.processors.empty();
    return topology;
}
#else
/** The processors the process's affinity mask allows, which a container's cpuset narrows.
    @return  their numbers; nothing where the call fails */
std::optional<std::vector<int>> AllowedProcessors()
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
    {
        return std::nullopt;
    }
    std::vector<int> ids;
    for (int n = 0; n < CPU_SETSIZE; ++n)
    {
        if (CPU_ISSET(n, &set))
        {
            ids.push_back(n);
        }
    }
    return ids;
}
#endif
} // namespace

CpuTopology DetectCpuTopology()
{
    CpuTopology topology;
#if defined(_WIN32)
    topology = DetectWindows();
#elif defined(__APPLE__)
    topology = DetectMac();
#else
    topology = ReadCpuTopologyFromSysfs("/sys", AllowedProcessors(), HardwareThreads());
#endif
    if (topology.processors.empty())
    {
        topology        = ReadCpuTopologyFromSysfs("/nonexistent", std::nullopt, HardwareThreads());
        topology.source = "hardware_concurrency";
    }
    return topology;
}

ThreadPin::ThreadPin(const LogicalProcessor &processor)
{
#if defined(_WIN32)
    GROUP_AFFINITY wanted{};
    wanted.Group = static_cast<WORD>(processor.group);
    wanted.Mask  = static_cast<KAFFINITY>(1) << (processor.id - processor.group * 64);
    GROUP_AFFINITY before{};
    if (SetThreadGroupAffinity(GetCurrentThread(), &wanted, &before) == 0)
    {
        result_.refused =
            "SetThreadGroupAffinity failed: " + std::system_category().message(static_cast<int>(GetLastError()));
        return;
    }
    result_.is_pinned = true;
    undo_             = [before] { SetThreadGroupAffinity(GetCurrentThread(), &before, nullptr); };
#elif defined(__APPLE__)
    static_cast<void>(processor);
    result_.refused = "macOS gives a thread no way to be pinned to a core on Apple silicon";
#else
    if (processor.id < 0 || processor.id >= CPU_SETSIZE)
    {
        result_.refused = "processor " + std::to_string(processor.id) + " is outside the " +
                          std::to_string(CPU_SETSIZE) + " a cpu_set_t holds";
        return;
    }
    cpu_set_t before;
    CPU_ZERO(&before);
    pthread_getaffinity_np(pthread_self(), sizeof(before), &before);
    cpu_set_t wanted;
    CPU_ZERO(&wanted);
    CPU_SET(processor.id, &wanted);
    const int code = pthread_setaffinity_np(pthread_self(), sizeof(wanted), &wanted);
    if (code != 0)
    {
        result_.refused =
            std::string("pinning to processor ") + std::to_string(processor.id) + " refused: " + std::strerror(code);
        return;
    }
    result_.is_pinned = true;
    undo_             = [before] { pthread_setaffinity_np(pthread_self(), sizeof(before), &before); };
#endif
}
} // namespace reckless::core
