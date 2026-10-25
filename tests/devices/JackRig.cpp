// JACK's test device: a JACK server of the suite's own, on the dummy driver - no hardware, its period timed by the
// system clock - under a name no other server has, started for each test and stopped after it, and dying with the suite
// however the suite ends. Its "system" client has three capture ports and five playback ports, a signature no desktop's
// server has, so a test that finds them knows it reached this server and not a PipeWire graph or a person's JACK. A
// stream's outputs are wired to its own inputs through the server for the loop, and the server is stopped under a
// running stream for the failure. The server is reached through the same libjack, loaded by name, that the JACK back
// end uses (audio/linux/JackLoader.cpp): JackNames.h gives this file's calls the loader's names too.
#include "JackNames.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <jack/jack.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "DeviceRig.h"

namespace reckless::audio::test
{
namespace
{
/** The dummy driver's ports: "system" has this many capture ports (a stream's inputs) and playback ports (outputs). */
constexpr int kCapturePorts  = 3;     ///< the capture ports of the dummy driver's "system" client: a stream's inputs
constexpr int kPlaybackPorts = 5;     ///< the playback ports of the "system" client: a stream's outputs
constexpr int kRate          = 48000; ///< the dummy driver's sample rate, in hertz
/** The dummy driver's period, in frames: its cycles are timed by a thread of jackd's, which a runner schedules without
   real-time priority, and a cycle that comes later than its period is an xrun. At 256 frames (5.3 ms) a four-core
   runner's ordinary waits of a dozen milliseconds make about a dozen a second; 1024 frames (21.3 ms) rides them out. */
constexpr int kPeriod = 1024;

/** libjack's error and information printers silenced while the rig probes for a server that is not up yet. */
void Quiet(const char *)
{
}

/** The first `name` on the PATH that is a file this process may run.
    @param name  the program's file name
    @return      the path to it; empty for none */
std::string OnPath(const std::string &name)
{
    const char       *path = std::getenv("PATH");
    std::string       dirs = path != nullptr ? path : "/usr/bin:/bin";
    std::stringstream stream(dirs + ":/usr/bin:/usr/local/bin");
    std::string       dir;
    while (std::getline(stream, dir, ':'))
    {
        const std::string candidate = (dir.empty() ? std::string(".") : dir) + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0)
        {
            return candidate;
        }
    }
    return {};
}

/** The end of a log file, for a message.
    @param file   the path of the file; a file that cannot be read gives an empty text
    @param lines  the most lines to take, from the end
    @return       those lines joined by " | " */
std::string Tail(const std::string &file, std::size_t lines)
{
    std::ifstream            in(file);
    std::vector<std::string> all;
    std::string              line;
    while (std::getline(in, line))
    {
        all.push_back(line);
    }
    std::string text;
    for (std::size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); ++i)
    {
        text += (text.empty() ? "" : " | ") + all[i];
    }
    return text;
}

/** JACK's rig: a jackd of its own on the dummy driver, started with the rig and stopped with it. */
class JackRig final : public DeviceRig
{
  public:
    /** Name a server for this process, point every JACK client of the process at it with no auto-start, silence the
        client library's error output and start the dummy-driver jackd (Start). */
    JackRig()
    {
        static int count = 0;
        name_            = "reckless-test-" + std::to_string(getpid()) + "-" + std::to_string(++count);
        log_             = "/tmp/" + name_ + ".log";
        // Every client of this process - the stream's and the rig's own - reaches this server, and none starts one.
        setenv("JACK_DEFAULT_SERVER", name_.c_str(), 1);
        setenv("JACK_NO_START_SERVER", "1", 1);
        jack_set_error_function(&Quiet);
        Start();
    }

    /** Stop the jackd the rig started and remove its log file. */
    ~JackRig() override
    {
        Stop();
        std::remove(log_.c_str());
    }

    JackRig(const JackRig &)            = delete;
    JackRig &operator=(const JackRig &) = delete;

    TestDevice Find(const std::vector<DeviceInfo> &devices) override
    {
        TestDevice device;
        if (!missing_.empty())
        {
            device.missing = missing_;
            return device;
        }
        for (const DeviceInfo &info : devices)
        {
            if (info.name != "system")
            {
                continue;
            }
            if (info.input_channels != kCapturePorts || info.output_channels != kPlaybackPorts)
            {
                device.missing = "the JACK library reached a server that is not this test's: its system client has " +
                                 std::to_string(info.input_channels) + " capture and " +
                                 std::to_string(info.output_channels) + " playback ports where this one's has " +
                                 std::to_string(kCapturePorts) + " and " + std::to_string(kPlaybackPorts) +
                                 " (is PipeWire's libjack the one on the library path?)";
                return device;
            }
            device.found              = true;
            device.output             = info.name;
            device.input              = info.name;
            device.engine_input       = info.name;
            device.channels           = 2;
            device.sample_rate        = kRate;
            device.block_size         = kPeriod;
            device.loop               = Loop::kExact;
            device.xrun_by            = XrunBy::kStall;
            device.rate_tolerance     = 0.10; // a late cycle's time is lost (is_scheduler_timed)
            device.is_scheduler_timed = true;
            device.about = "jackd on the dummy driver, " + std::to_string(kRate) + " Hz, " + std::to_string(kPeriod) +
                           " frames a period, server '" + name_ + "', the stream's outputs wired to its own inputs";
            return device;
        }
        device.missing = "the test's JACK server ('" + name_ + "') is up but JACK lists no 'system' client";
        return device;
    }

    /** Each of the stream's outputs wired to its input of the same number: the client reads, a period later, what
        it wrote - the dummy's capture ports wired there too carry silence, which adds nothing.
        @param client  the name of the stream's JACK client
        @param why     set to the reason when the loop could not be wired
        @return        false when the loop could not be wired */
    bool Started(const std::string &client, std::string &why) override
    {
        jack_status_t  status = JackFailure;
        jack_client_t *wiring = jack_client_open("reckless-test-wiring", JackNoStartServer, &status);
        if (wiring == nullptr)
        {
            why = "the rig could not reach the test's JACK server to wire the loop";
            return false;
        }
        bool ok = true;
        for (int c = 0; c < 2 && ok; ++c)
        {
            const std::string out = client + ":outport " + std::to_string(c);
            const std::string in  = client + ":inport " + std::to_string(c);
            if (jack_connect(wiring, out.c_str(), in.c_str()) != 0)
            {
                why = "JACK would not connect '" + out + "' to '" + in + "'";
                ok  = false;
            }
        }
        jack_client_close(wiring);
        return ok;
    }

    /** The server stopped as a person stops it, with SIGTERM: it tells its clients it is going, and the JACK back end
        tells the stream it failed.
        @param why  set to the reason when the server is not running
        @return     false when the server is not running */
    bool Vanish(std::string &why) override
    {
        if (pid_ <= 0)
        {
            why = "the test's JACK server is not running";
            return false;
        }
        Stop();
        return true;
    }

    /** The server started again.
        @param why  set to the reason the server did not come up; empty when it did
        @return     true when the server is up */
    bool Return(std::string &why) override
    {
        Start();
        why = missing_;
        return missing_.empty();
    }

  private:
    /** Start jackd on the dummy driver and wait up to five seconds for a client to open on it. `missing_` says why when
        it did not come up. */
    void Start()
    {
        missing_.clear();
        const std::string jackd = OnPath("jackd");
        if (jackd.empty())
        {
            missing_ = "no jackd on the PATH: sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y jackd2 "
                       "(tools/audio-test-devices.sh does it)";
            return;
        }
        const int log = open(log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log < 0)
        {
            missing_ = "could not write the JACK server's log at " + log_;
            return;
        }
        const std::string        rate   = std::to_string(kRate);
        const std::string        period = std::to_string(kPeriod);
        const std::string        ins    = std::to_string(kCapturePorts);
        const std::string        outs   = std::to_string(kPlaybackPorts);
        std::vector<std::string> args   = {jackd, "--no-realtime", "--name", name_, "-d", "dummy", "-r", rate,
                                           "-p",  period,          "-C",     ins,   "-P", outs};
        std::vector<char *>      argv;
        for (std::string &arg : args)
        {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        const pid_t parent = getpid();
        const pid_t pid    = fork();
        if (pid == 0)
        {
            // The child: killed when the suite goes, however it goes, and its output into the log. Only calls a
            // forked child of a threaded process may make, until the exec.
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent)
            {
                _exit(1);
            }
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            execv(argv[0], argv.data());
            _exit(127);
        }
        close(log);
        if (pid < 0)
        {
            missing_ = std::string("could not start jackd: ") + std::strerror(errno);
            return;
        }
        pid_ = pid;

        // Up when a client can open on it: up to five seconds, the server's own exit read as its failure.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until)
        {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_)
            {
                pid_     = -1;
                missing_ = "jackd exited as it started: " + Tail(log_, 4);
                return;
            }
            jack_status_t  open_status = JackFailure;
            jack_client_t *probe       = jack_client_open("reckless-test-probe", JackNoStartServer, &open_status);
            if (probe != nullptr)
            {
                jack_client_close(probe);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        missing_ = "jackd did not come up within five seconds: " + Tail(log_, 4);
        Stop();
    }

    /** Stop the server with SIGTERM, then SIGKILL after three seconds; nothing when it is not running. */
    void Stop()
    {
        if (pid_ <= 0)
        {
            return;
        }
        kill(pid_, SIGTERM);
        const auto until  = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        int        status = 0;
        while (waitpid(pid_, &status, WNOHANG) == 0)
        {
            if (std::chrono::steady_clock::now() > until)
            {
                kill(pid_, SIGKILL);
                waitpid(pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        pid_ = -1;
    }

    std::string name_;    ///< the server's name, unique to this process and rig
    std::string log_;     ///< the path of the server's log, in /tmp
    std::string missing_; ///< why the server is not up; empty when it is
    pid_t       pid_{-1}; ///< the server's process; -1 for none
};
} // namespace

std::unique_ptr<DeviceRig> MakeJackRig()
{
    return std::make_unique<JackRig>();
}
} // namespace reckless::audio::test
