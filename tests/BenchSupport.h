#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

/**
    What the throughput benches share (tests/, namespace reckless::test; no gtest): the stimulus, the sink, the
    failure count, the --quick flag, the median of timed runs, the result of an engine pass and the throughput line.
    Every bench prints the same columns from the same stimulus, so the ballparks the plugins' CLAUDE.md files quote stay
    comparable across plugins and across time.

    BenchLcg is not TestNoise.h's NoiseLcg on purpose: the benches drive their engines with this generator at half
    scale, and the clipping plugins' figures depend on how hard they are driven, so the generator stays as it is.
*/
namespace reckless::test
{
/** Deterministic noise in [-0.5, 0.5). No `<random>`, identical on every run. */
struct BenchLcg
{
    std::uint32_t s{0x12345678u}; ///< the generator's state, the last word of the sequence

    /** The next sample of the sequence.
        @return  a sample in [-0.5, 0.5) */
    double Next() noexcept
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(static_cast<std::int32_t>(s)) * (0.5 / 2147483648.0);
    }
};

/** Written after every timed loop so the optimiser cannot delete the work. */
inline volatile double g_bench_sink = 0.0;

/** The sanity failures a bench's passes counted: what main() prints and returns. */
inline int bench_failures = 0;

/** Whether --quick is among the arguments: the one-line smoke pass CI runs, against the full figures.
    @param argc  the argument count, as main() has it
    @param argv  the arguments, as main() has them; the first is the program's name and is not read
    @return      true when --quick is given */
inline bool QuickFlag(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--quick") == 0)
        {
            return true;
        }
    }
    return false;
}

/** The median wall-clock seconds of `runs` calls of `f` (the lower median for an even count).
    @tparam F   a callable taking no arguments
    @param runs  the number of timed calls, at least one
    @param f     the work to time
    @return      the median wall-clock time of one call, in seconds */
template <typename F> double MedianSeconds(int runs, F &&f)
{
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(runs));
    for (int r = 0; r < runs; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        t.push_back(std::chrono::duration<double>(t1 - t0).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

/** The result of an engine pass. */
struct EngineResult
{
    double ns_per_sample; ///< nanoseconds per sample, per channel
    double x_realtime;    ///< audio seconds processed per wall-clock second
};

/** Cost of the noise generator alone, subtracted from loops that use it.
    @param n     the samples a timed run generates
    @param runs  the number of timed runs, at least one
    @return      the median cost, in nanoseconds per sample */
inline double NoiseBaselineNs(int n, int runs)
{
    const double sec = MedianSeconds(runs, [&] {
        BenchLcg lcg;
        double   acc = 0.0;
        for (int i = 0; i < n; ++i)
        {
            acc += lcg.Next();
        }
        g_bench_sink = acc;
    });
    return sec * 1e9 / n;
}

/**
    One throughput line: `seconds` of stereo noise through a prepared engine in blocks of 512, timed, checked
    finite (a non-finite output prints FAIL and counts a failure) and printed as ns/sample/channel and the
    multiple of realtime, the label in a column `label_width` wide. The engine arrives prepared and set; what a
    bench varies - a rate, a law, a routing, a mode - it sets before the call.
    @tparam Engine      a type with `Process(double **channels, int num_channels, int num_samples)`
    @param what         the label printed at the start of the line
    @param engine       the prepared engine
    @param sample_rate  the sample rate the engine is prepared at, in hertz
    @param seconds      the length of noise to run, in seconds of audio
    @param label_width  the width of the label's column, in characters
*/
template <typename Engine>
void RunThroughput(const char *what, Engine &engine, double sample_rate, int seconds, int label_width = 34)
{
    constexpr int kStereo = 2; // every bench's kChannels, named apart so a bench's own does not hide it (C4459)
    const int     samples = static_cast<int>(sample_rate) * seconds;
    constexpr int block   = 512;

    BenchLcg                         lcg;
    std::vector<std::vector<double>> audio(kStereo, std::vector<double>(static_cast<std::size_t>(samples), 0.0));
    for (auto &channel : audio)
    {
        for (auto &x : channel)
        {
            x = lcg.Next();
        }
    }

    // The channel pointers on the stack: nothing in the timed loop allocates but the engine.
    double    *ptrs[kStereo] = {nullptr, nullptr};
    const auto start         = std::chrono::steady_clock::now();
    for (int offset = 0; offset < samples; offset += block)
    {
        const int n = std::min(block, samples - offset);
        ptrs[0]     = audio[0].data() + offset;
        ptrs[1]     = audio[1].data() + offset;
        engine.Process(ptrs, kStereo, n);
    }
    const auto finish = std::chrono::steady_clock::now();

    double sum = 0.0;
    for (const auto &channel : audio)
    {
        for (double x : channel)
        {
            if (!std::isfinite(x))
            {
                std::printf("FAIL %s: non-finite output\n", what);
                ++bench_failures;
                return;
            }
            sum += x;
        }
    }
    g_bench_sink = sum;

    const double nanoseconds = std::chrono::duration<double, std::nano>(finish - start).count();
    const double per_sample  = nanoseconds / (static_cast<double>(samples) * kStereo);
    std::printf("%-*s %6.1f ns/sample/channel   %5.0fx realtime\n", label_width, what, per_sample,
                1.0e9 / (per_sample * sample_rate * kStereo));
}
} // namespace reckless::test
