# The machine's processors, and pinning a thread to one

`core/CpuTopology.h` (namespace `reckless::core`, linked as `reckless_core_cpu`) tells an application's own thread
scheduler what the machine is made of, and pins the calling thread to one logical processor. It is the other half of
[the realtime thread scope](../audio/native-backends.md): the scope gives a thread the audio callback's standing; this
says where to put it.

```cpp
const reckless::core::CpuTopology topology = reckless::core::DetectCpuTopology();
for (const auto &processor : topology.processors)
{   // id, core, is_smt_sibling, cpu_class (kPerformance / kEfficiency), relative_capacity
}
const reckless::core::ThreadPin pin(topology.processors[2]); // the calling thread; undone as `pin` goes
if (!pin.Result().is_pinned) { ShowInSettings(pin.Result().refused); }
```

Called at start-up and at a thread's start and end, never on an audio thread: it reads files and allocates.

## What each platform reads

| Platform | The processors | Classes and capacity | Pinning |
|---|---|---|---|
| Linux | `/sys/devices/system/cpu/cpuN/topology/{core_id,physical_package_id,thread_siblings_list}`, held to the process's affinity mask (`sched_getaffinity`), so a container's cpuset gives the container's view; an offline processor is left out | Intel hybrid: `/sys/devices/cpu_core/cpus` and `cpu_atom/cpus`. Otherwise `cpu_capacity` (arm64) below 0.7 of the highest is an efficiency core, else `cpufreq/cpuinfo_max_freq` the same way. `relative_capacity` is `cpu_capacity` where it tells processors apart (on an Intel hybrid it reads 1024 for every one, so the frequency is used), else the frequency, else 0 | `pthread_setaffinity_np`; affinity put back |
| Windows | `GetLogicalProcessorInformationEx(RelationProcessorCore)`: a core per entry, its SMT flag, each group's mask (more than 64 logical processors); `id` is `group * 64 + bit` | `EfficiencyClass` below the highest is efficiency; no capacity figure (0) | `SetThreadGroupAffinity`; the earlier affinity put back |
| macOS | `sysctlbyname` `hw.nperflevels` and `hw.perflevelN.{logicalcpu,physicalcpu}`, level 0 the performance cores; an Intel Mac has one level (from `hw.logicalcpu` and `hw.physicalcpu`) | level 0 performance, others efficiency; no capacity figure (0) | **not supported**: Apple silicon has no pinning, and the old affinity tags only suggest two threads share a cache. `ThreadPin` refuses with that reason |

`ReadCpuTopologyFromSysfs(root, allowed, fallback_count)` is pure on the tree it is handed, which is how the suite feeds
it this machine's own layout (an i9-13900HX: 8 performance cores of two threads and 16 efficiency cores), a non-hybrid
8-core of two threads, a 4-core with no SMT, an arm64 big.LITTLE, a restricted cpuset and a broken or empty tree. A tree
that gives nothing falls back to one single-threaded performance core per `std::thread::hardware_concurrency()`, with
`is_detected` false and the source named.

`LogicalProcessor::core` numbers the physical cores in the order the list meets them, and `is_smt_sibling` is true when
an **earlier processor in the list** shares the core - so in a cpuset that holds only the second thread of a core, that
thread is its core's first and no sibling.
