// The CPU topology (core/CpuTopology.h): Linux's sysfs reader on fixture trees - this machine's own layout (an Intel
// hybrid, 8 P cores of two threads and 16 E cores), a non-hybrid 8-core of two threads, a 4-core with no SMT, an arm64
// big.LITTLE with cpu_capacity, a restricted cpuset, and a broken tree - the kernel's CPU list syntax, the running
// machine's sanity on every platform, and a thread pinned to each processor it may run on.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "CpuTopology.h"
#include "ScratchPath.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace
{
namespace fs = std::filesystem;
using reckless::core::CpuClass;
using reckless::core::CpuTopology;
using reckless::core::LogicalProcessor;

/// One processor of a fixture tree, with the files the kernel gives it where it has them.
struct FakeCpu
{
    int                        id{0};
    std::optional<int>         core_id;    ///< topology/core_id
    std::optional<std::string> siblings;   ///< topology/thread_siblings_list
    int                        package{0}; ///< topology/physical_package_id
    std::optional<int>         capacity;   ///< cpu_capacity
    std::optional<int>         max_khz;    ///< cpufreq/cpuinfo_max_freq
    std::optional<int>         online;     ///< online
};

/** @param path  the file
    @param text  what it holds */
void Write(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text << "\n";
}

/** A sysfs tree made under the process's scratch folder.
    @param name            the tree's folder name
    @param cpus            its processors
    @param performance     the text of devices/cpu_core/cpus, empty for none
    @param efficiency      the text of devices/cpu_atom/cpus, empty for none
    @return                the tree's root */
fs::path BuildTree(const std::string &name, const std::vector<FakeCpu> &cpus, const std::string &performance = "",
                   const std::string &efficiency = "")
{
    const fs::path root = reckless::test::ScratchPath(name);
    fs::remove_all(root);
    fs::create_directories(root / "devices" / "system" / "cpu");
    for (const FakeCpu &cpu : cpus)
    {
        const fs::path dir = root / "devices" / "system" / "cpu" / ("cpu" + std::to_string(cpu.id));
        fs::create_directories(dir);
        if (cpu.core_id)
        {
            Write(dir / "topology" / "core_id", std::to_string(*cpu.core_id));
            Write(dir / "topology" / "physical_package_id", std::to_string(cpu.package));
        }
        if (cpu.siblings)
        {
            Write(dir / "topology" / "thread_siblings_list", *cpu.siblings);
        }
        if (cpu.capacity)
        {
            Write(dir / "cpu_capacity", std::to_string(*cpu.capacity));
        }
        if (cpu.max_khz)
        {
            Write(dir / "cpufreq" / "cpuinfo_max_freq", std::to_string(*cpu.max_khz));
        }
        if (cpu.online)
        {
            Write(dir / "online", std::to_string(*cpu.online));
        }
    }
    if (!performance.empty())
    {
        Write(root / "devices" / "cpu_core" / "cpus", performance);
    }
    if (!efficiency.empty())
    {
        Write(root / "devices" / "cpu_atom" / "cpus", efficiency);
    }
    return root;
}

/** An i9-13900HX's layout as this machine reports it: cpu_core 0-15 (eight cores of two threads, core ids 0, 4, 8 ..),
    cpu_atom 16-31 (sixteen cores of one, ids 32 ..47), cpu_capacity 1024 throughout, 5.2 GHz against 3.9 GHz.
    @return the fixture's root */
fs::path HybridIntelTree()
{
    std::vector<FakeCpu> cpus;
    for (int id = 0; id < 16; ++id)
    {
        const int first = id / 2 * 2;
        cpus.push_back({id, id / 2 * 4, std::to_string(first) + "-" + std::to_string(first + 1), 0, 1024, 5200000, {}});
    }
    for (int id = 16; id < 32; ++id)
    {
        cpus.push_back({id, 32 + (id - 16), std::to_string(id), 0, 1024, 3900000, {}});
    }
    return BuildTree("topology-hybrid-intel", cpus, "0-15", "16-31");
}

/** @param tree  a fixture's root
    @param allowed  the processors the process may use, or none for all
    @return the topology read from it */
CpuTopology Read(const fs::path &tree, std::optional<std::vector<int>> allowed = std::nullopt)
{
    return reckless::core::ReadCpuTopologyFromSysfs(tree, allowed, 6);
}
} // namespace

TEST(CpuList, ReadsTheKernelsSyntax)
{
    using reckless::core::sysfs::ParseCpuList;
    EXPECT_EQ(ParseCpuList("0-3,8,10-11\n"), (std::vector<int>{0, 1, 2, 3, 8, 10, 11}));
    EXPECT_EQ(ParseCpuList("5"), (std::vector<int>{5}));
    EXPECT_EQ(ParseCpuList("0-1"), (std::vector<int>{0, 1}));
    EXPECT_TRUE(ParseCpuList("").empty());
    EXPECT_TRUE(ParseCpuList("x").empty());
    EXPECT_TRUE(ParseCpuList("3-1").empty());
    EXPECT_TRUE(ParseCpuList("1,").empty());
    EXPECT_TRUE(ParseCpuList("1-2-3").empty());
}

TEST(CpuTopologySysfs, ReadsAnIntelHybridAsThisMachineHasIt)
{
    const CpuTopology topology = Read(HybridIntelTree());
    ASSERT_TRUE(topology.is_detected);
    ASSERT_EQ(topology.processors.size(), 32U);
    EXPECT_EQ(topology.core_count, 24);
    EXPECT_TRUE(topology.IsHybrid());
    EXPECT_EQ(topology.CountOf(CpuClass::kPerformance), 16);
    EXPECT_EQ(topology.CountOf(CpuClass::kEfficiency), 16);
    for (const LogicalProcessor &p : topology.processors)
    {
        EXPECT_EQ(p.cpu_class, p.id < 16 ? CpuClass::kPerformance : CpuClass::kEfficiency) << p.id;
        EXPECT_EQ(p.is_smt_sibling, p.id < 16 && p.id % 2 == 1) << p.id;
        EXPECT_EQ(p.core, p.id < 16 ? p.id / 2 : p.id - 8) << p.id;
    }
    // cpu_capacity is 1024 for all, which says nothing: the frequencies give the efficiency cores' share.
    EXPECT_DOUBLE_EQ(topology.processors[0].relative_capacity, 1.0);
    EXPECT_NEAR(topology.processors[20].relative_capacity, 3.9 / 5.2, 1e-9);
}

TEST(CpuTopologySysfs, ReadsANonHybridEightCoresOfTwoThreadsAsOneClass)
{
    std::vector<FakeCpu> cpus;
    for (int id = 0; id < 16; ++id)
    {
        // Siblings are numbered 0-8, 1-9 ..: thread n and n + 8 share a core, as on many Intel desktops.
        const int core = id % 8;
        cpus.push_back(
            {id, core, std::to_string(core) + "," + std::to_string(core + 8), 0, {}, 4600000 - 10000 * id, {}});
    }
    const CpuTopology topology = Read(BuildTree("topology-smt8", cpus));
    ASSERT_EQ(topology.processors.size(), 16U);
    EXPECT_EQ(topology.core_count, 8);
    EXPECT_FALSE(topology.IsHybrid());
    for (const LogicalProcessor &p : topology.processors)
    {
        EXPECT_EQ(p.is_smt_sibling, p.id >= 8) << p.id;
        EXPECT_EQ(p.core, p.id % 8) << p.id;
        EXPECT_EQ(p.cpu_class, CpuClass::kPerformance);
        EXPECT_GT(p.relative_capacity, 0.9);
    }
}

TEST(CpuTopologySysfs, ReadsFourCoresWithNoSmtAndNoFurtherFiles)
{
    std::vector<FakeCpu> cpus;
    for (int id = 0; id < 4; ++id)
    {
        cpus.push_back({id, id, std::to_string(id), 0, {}, {}, {}});
    }
    const CpuTopology topology = Read(BuildTree("topology-four", cpus));
    ASSERT_EQ(topology.processors.size(), 4U);
    EXPECT_EQ(topology.core_count, 4);
    for (const LogicalProcessor &p : topology.processors)
    {
        EXPECT_FALSE(p.is_smt_sibling);
        EXPECT_EQ(p.relative_capacity, 0.0); // the platform gave none
    }
}

TEST(CpuTopologySysfs, TakesTheClassesOfAnArmBigLittleFromCpuCapacity)
{
    // Four little cores at 400, three middle at 850 (still performance), one prime at 1024.
    std::vector<FakeCpu> cpus;
    const int            capacities[8] = {400, 400, 400, 400, 850, 850, 850, 1024};
    for (int id = 0; id < 8; ++id)
    {
        cpus.push_back({id, id, std::to_string(id), 0, capacities[id], {}, {}});
    }
    const CpuTopology topology = Read(BuildTree("topology-arm", cpus));
    ASSERT_EQ(topology.processors.size(), 8U);
    EXPECT_TRUE(topology.IsHybrid());
    EXPECT_EQ(topology.CountOf(CpuClass::kEfficiency), 4);
    EXPECT_EQ(topology.CountOf(CpuClass::kPerformance), 4);
    EXPECT_DOUBLE_EQ(topology.processors[0].relative_capacity, 400.0 / 1024.0);
    EXPECT_DOUBLE_EQ(topology.processors[7].relative_capacity, 1.0);
    EXPECT_EQ(topology.core_count, 8);
}

TEST(CpuTopologySysfs, ListsOnlyTheProcessorsAContainersCpusetAllows)
{
    const fs::path tree = HybridIntelTree();
    // Processors 3, 4 and 5 (the second thread of one core, then a whole core) and an efficiency core, 20.
    const CpuTopology topology = Read(tree, std::vector<int>{3, 4, 5, 20});
    ASSERT_EQ(topology.processors.size(), 4U);
    EXPECT_EQ(topology.processors[0].id, 3);
    // 3's sibling, 2, is outside the cpuset, so 3 is the first of its core in this view and not a sibling.
    EXPECT_FALSE(topology.processors[0].is_smt_sibling);
    EXPECT_FALSE(topology.processors[1].is_smt_sibling);
    EXPECT_TRUE(topology.processors[2].is_smt_sibling);
    EXPECT_EQ(topology.processors[1].core, topology.processors[2].core);
    EXPECT_NE(topology.processors[0].core, topology.processors[1].core);
    EXPECT_EQ(topology.processors[3].cpu_class, CpuClass::kEfficiency);
    EXPECT_EQ(topology.core_count, 3);
}

TEST(CpuTopologySysfs, LeavesOutAnOfflineProcessor)
{
    std::vector<FakeCpu> cpus;
    for (int id = 0; id < 4; ++id)
    {
        cpus.push_back({id, id, std::to_string(id), 0, {}, {}, id == 2 ? 0 : 1});
    }
    const CpuTopology topology = Read(BuildTree("topology-offline", cpus));
    ASSERT_EQ(topology.processors.size(), 3U);
    EXPECT_EQ(topology.processors[2].id, 3);
}

TEST(CpuTopologySysfs, FallsBackToTheHardwareThreadsOnABrokenOrEmptyTree)
{
    for (const std::string name : {"topology-empty", "topology-missing"})
    {
        const fs::path    tree     = name == "topology-empty" ? BuildTree(name, {}) : reckless::test::ScratchPath(name);
        const CpuTopology topology = Read(tree);
        EXPECT_FALSE(topology.is_detected) << name;
        ASSERT_EQ(topology.processors.size(), 6U) << name; // the fallback count the call was given
        EXPECT_EQ(topology.core_count, 6);
        for (const LogicalProcessor &p : topology.processors)
        {
            EXPECT_FALSE(p.is_smt_sibling);
            EXPECT_EQ(p.cpu_class, CpuClass::kPerformance);
        }
    }
    // Processor directories with no topology files: each is a core of its own, and the tree is still read.
    std::vector<FakeCpu> bare;
    for (int id = 0; id < 3; ++id)
    {
        bare.push_back({id, {}, {}, 0, {}, {}, {}});
    }
    const CpuTopology topology = Read(BuildTree("topology-bare", bare));
    EXPECT_TRUE(topology.is_detected);
    EXPECT_EQ(topology.core_count, 3);
    // A cpuset over a tree that lists none of it falls back to the cpuset's own numbers.
    const CpuTopology narrowed = Read(BuildTree("topology-empty", {}), std::vector<int>{4, 5});
    ASSERT_EQ(narrowed.processors.size(), 2U);
    EXPECT_EQ(narrowed.processors[0].id, 4);
}

// ---- The machine running the suite ---------------------------------------------------------------------------------

TEST(CpuTopologyMachine, IsSaneOnThisMachine)
{
    const CpuTopology topology = reckless::core::DetectCpuTopology();
    std::printf("CPU topology (%s): %zu logical processors, %d cores, %d performance, %d efficiency, detected %s\n",
                topology.source.c_str(), topology.processors.size(), topology.core_count,
                topology.CountOf(CpuClass::kPerformance), topology.CountOf(CpuClass::kEfficiency),
                topology.is_detected ? "yes" : "no");
    ASSERT_GE(topology.processors.size(), 1U);
    EXPECT_GE(topology.core_count, 1);
    EXPECT_LE(static_cast<std::size_t>(topology.core_count), topology.processors.size());
    EXPECT_GE(topology.CountOf(CpuClass::kPerformance), 1); // a machine always has some performance class
    std::set<int> ids;
    std::set<int> cores_seen;
    for (const LogicalProcessor &p : topology.processors)
    {
        EXPECT_TRUE(ids.insert(p.id + p.group * 0).second) << "processor " << p.id << " is listed twice";
        EXPECT_GE(p.core, 0);
        EXPECT_LT(p.core, topology.core_count);
        EXPECT_GE(p.relative_capacity, 0.0);
        EXPECT_LE(p.relative_capacity, 1.0);
        // A sibling refers to a core an earlier processor has; the first of a core is no sibling.
        EXPECT_EQ(p.is_smt_sibling, cores_seen.count(p.core) != 0) << "processor " << p.id;
        cores_seen.insert(p.core);
    }
    EXPECT_EQ(static_cast<int>(cores_seen.size()), topology.core_count);
}

TEST(ThreadPinning, PinsToEachProcessorTheProcessMayUseAndPutsTheAffinityBack)
{
    const CpuTopology topology = reckless::core::DetectCpuTopology();
    ASSERT_GE(topology.processors.size(), 1U);
    for (const LogicalProcessor &processor : topology.processors)
    {
        // Each on a thread of its own, since a pin is the calling thread's.
        std::thread([&processor] {
#if defined(__linux__)
            cpu_set_t original;
            CPU_ZERO(&original);
            pthread_getaffinity_np(pthread_self(), sizeof(original), &original);
            {
                const reckless::core::ThreadPin pin(processor);
                ASSERT_TRUE(pin.Result().is_pinned) << processor.id << ": " << pin.Result().refused;
                cpu_set_t now;
                CPU_ZERO(&now);
                pthread_getaffinity_np(pthread_self(), sizeof(now), &now);
                EXPECT_EQ(CPU_COUNT(&now), 1) << processor.id;
                EXPECT_TRUE(CPU_ISSET(processor.id, &now)) << processor.id;
                EXPECT_EQ(sched_getcpu(), processor.id);
            }
            cpu_set_t after;
            CPU_ZERO(&after);
            pthread_getaffinity_np(pthread_self(), sizeof(after), &after);
            EXPECT_TRUE(CPU_EQUAL(&original, &after)) << "the affinity was not put back after " << processor.id;
#else
            reckless::core::ThreadPin pin(processor);
#if defined(__APPLE__)
            EXPECT_FALSE(pin.Result().is_pinned);
            EXPECT_FALSE(pin.Result().refused.empty());
#else
            EXPECT_TRUE(pin.Result().is_pinned) << processor.id << ": " << pin.Result().refused;
            pin.Unpin();
            EXPECT_FALSE(pin.Result().is_pinned);
#endif
#endif
        }).join();
    }
}

TEST(ThreadPinning, RefusesAProcessorOutsideTheSetWithAReason)
{
#if defined(__linux__)
    LogicalProcessor nowhere;
    nowhere.id = 100000;
    std::thread([&nowhere] {
        reckless::core::ThreadPin pin(nowhere);
        EXPECT_FALSE(pin.Result().is_pinned);
        EXPECT_FALSE(pin.Result().refused.empty());
    }).join();
#else
    GTEST_SKIP() << "Linux's cpu_set_t bound; Windows and macOS are judged by the case above";
#endif
}
