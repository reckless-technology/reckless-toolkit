// ALSA on alsa-lib directly (AlsaDevice.h says what it does and why): the one translation unit that meets alsa-lib.
#include "AlsaDevice.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "SampleEncodings.h"

namespace reckless::audio
{
namespace
{
/** The rates a device is asked whether it takes, the ones RtAudio asked, so a device lists the rates it listed. */
constexpr unsigned int kRates[] = {4000,  5512,  8000,  9600,  11025, 16000,  22050,
                                   32000, 44100, 48000, 88200, 96000, 176400, 192000};

/** The periods an output's buffer holds where the request leaves them to the back end (StreamRequest::periods 0). The
    output is primed full and a period written each time one has played, so a callback has all but one of them to run
    before the device runs dry, and a sample written plays this many periods later: three gives two periods' margin -
    10.7 ms at 256 frames and 48 kHz - and a block's latency more than two. RtAudio's buffer was four. */
constexpr unsigned int kPeriods = 3;

/** The fewest periods an output's buffer may hold: one playing while the next is written. */
constexpr unsigned int kFewestPeriods = 2;

/** The frames an input's buffer holds at the least. An input is read a block at a time as soon as it has one, so its
    buffer adds no latency, only room: a sound server's ALSA device hands its input over a whole graph cycle at a time
    - PipeWire's quantum, 1024 frames by default - which a buffer of three small periods cannot take in. */
constexpr unsigned int kInputFrames = 4096;

/** How long the stream's thread waits for a device before it takes the device to have stopped. */
constexpr int kWaitTimeoutMs = 2000;

/** The round-robin real-time priority the stream's thread asks for, RtAudio's, held to what the system allows. */
constexpr int kPriority = 70;

/** The name the `default` PCM is listed under, RtAudio's. */
constexpr const char *kDefaultName = "Default ALSA Device";

/** One sample encoding a stream may open a device in, as ALSA names it. */
struct SampleFormat
{
    snd_pcm_format_t alsa;     ///< ALSA's name for it
    Encoding         encoding; ///< the encoding SampleEncodings.h converts
};

/** The encodings a stream asks a device for, the best first: float, which the callback is in, then the widest
    integers. */
constexpr SampleFormat kFormats[] = {{SND_PCM_FORMAT_FLOAT_LE, Encoding::kFloat32},
                                     {SND_PCM_FORMAT_S32_LE, Encoding::kInt32},
                                     {SND_PCM_FORMAT_S24_3LE, Encoding::kInt24},
                                     {SND_PCM_FORMAT_S16_LE, Encoding::kInt16}};

/** What an ALSA error code means, in words a person can act on.
    @param error  the negative code an ALSA call returned
    @return       the words */
std::string Describe(int error)
{
    if (error == -EBUSY)
    {
        return "the device is in use - another program, or a sound server such as PipeWire, has it open";
    }
    if (error == -ENODEV)
    {
        return "the device is gone - unplugged, or its driver removed";
    }
    return snd_strerror(error);
}

/** Whether alsa-lib's configuration defines `key` - "ctl.pulse" - so that opening it will not print alsa-lib's
    complaint that it is no such thing.
    @param key  a configuration key, its parts joined by dots
    @return     true when the configuration has it */
bool Defines(const char *key)
{
    if (snd_config_update() < 0 || snd_config == nullptr)
    {
        return false;
    }
    snd_config_t *node = nullptr;
    return snd_config_search(snd_config, key, &node) == 0;
}

/** Whether the control interface `id` opens, as RtAudio asked of the `default` and `pulse` devices before listing
    them.
    @param id  the control's name
    @return    true when it opens */
bool ControlOpens(const char *id)
{
    snd_ctl_t *control = nullptr;
    if (snd_ctl_open(&control, id, 0) < 0)
    {
        return false;
    }
    snd_ctl_close(control);
    return true;
}

/** What opening a PCM each way found: its channels, and the rates it takes. */
struct Probed
{
    int              inputs{0};    ///< its capture channels, 0 where it has none or would not open for capture
    int              outputs{0};   ///< its playback channels, 0 likewise
    std::vector<int> rates;        ///< the rates it takes the way it has the more channels, playback on a tie
    int              preferred{0}; ///< the highest of them up to 48 kHz, else the lowest, as RtAudio preferred
};

/** PCM `id` opened `stream`'s way for a moment: its channels and the rates it takes, or nothing where it will not
    open, takes no encoding a stream can open it in, or offers neither interleaved access.
    @param id        the PCM's name, as snd_pcm_open takes it
    @param stream    playback or capture
    @param channels  set to its most channels
    @param rates     set to the rates it takes, of kRates
    @return          true when it opened and can carry a stream */
bool ProbeWay(const std::string &id, snd_pcm_stream_t stream, int &channels, std::vector<int> &rates)
{
    snd_pcm_t *pcm = nullptr;
    if (snd_pcm_open(&pcm, id.c_str(), stream, SND_PCM_NONBLOCK) < 0)
    {
        return false;
    }
    snd_pcm_hw_params_t *params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    bool         usable = snd_pcm_hw_params_any(pcm, params) >= 0;
    unsigned int most   = 0;
    usable              = usable && snd_pcm_hw_params_get_channels_max(params, &most) >= 0 && most > 0;
    usable              = usable && (snd_pcm_hw_params_test_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0 ||
                                     snd_pcm_hw_params_test_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) == 0);
    usable = usable && std::any_of(std::begin(kFormats), std::end(kFormats), [&](const SampleFormat &format) {
                 return snd_pcm_hw_params_test_format(pcm, params, format.alsa) == 0;
             });
    if (usable)
    {
        channels = static_cast<int>(std::min(most, 1024u)); // a plugin's "any number" held to something countable
        for (const unsigned int rate : kRates)
        {
            if (snd_pcm_hw_params_test_rate(pcm, params, rate, 0) == 0)
            {
                rates.push_back(static_cast<int>(rate));
            }
        }
        usable = !rates.empty();
    }
    snd_pcm_close(pcm);
    return usable;
}

/** PCM `id` probed each way; nothing where it opens neither way.
    @param id  the PCM's name
    @return    what the probe found */
std::optional<Probed> Probe(const std::string &id)
{
    Probed           probed;
    std::vector<int> playback_rates, capture_rates;
    const bool       plays    = ProbeWay(id, SND_PCM_STREAM_PLAYBACK, probed.outputs, playback_rates);
    const bool       captures = ProbeWay(id, SND_PCM_STREAM_CAPTURE, probed.inputs, capture_rates);
    if (!plays && !captures)
    {
        return std::nullopt;
    }
    probed.rates = probed.outputs >= probed.inputs ? playback_rates : capture_rates;
    for (const int rate : probed.rates)
    {
        if (probed.preferred == 0 || (rate <= 48000 && rate > probed.preferred))
        {
            probed.preferred = rate;
        }
    }
    return probed;
}

/** Every probe that found a device, by its PCM's name and its listed name, for the program's life: a device is probed
    once, since one the program has open, or a sound server holds, will not open to be probed again. RtAudio kept its
    probes so, for each of its objects; these are the process's, so a system made while another has the device open
    lists it too. */
struct ProbeCache
{
    std::mutex                    mutex;  ///< guards `probed`
    std::map<std::string, Probed> probed; ///< each probe, by PCM name and listed name
};

/** The program's probes.
    @return  the cache, one for the program's life */
ProbeCache &Probes()
{
    static ProbeCache cache;
    return cache;
}

/** One device the API offers: the PCM it opens and how it is listed. */
struct Listed
{
    std::string id;   ///< the PCM's name, as snd_pcm_open takes it: "default", "hw:Loopback,0"
    DeviceInfo  info; ///< the device as listed, under the name NameApart gave it
};

/** The devices the machine offers now, as RtAudio listed them (AlsaDevice.h): the `default` and `pulse` PCMs where
    their controls open, then every card's every PCM device, each probed once and named apart.
    @return  the devices, in that order */
std::vector<Listed> Enumerate()
{
    std::vector<std::pair<std::string, std::string>> pcms; // PCM name, listed name
    std::string                                      default_name;
    if (ControlOpens("default"))
    {
        pcms.emplace_back("default", kDefaultName);
        default_name = kDefaultName;
    }
    if (Defines("ctl.pulse") && ControlOpens("pulse"))
    {
        pcms.emplace_back("pulse", "PulseAudio Sound Server");
    }
    snd_ctl_card_info_t *card_info = nullptr;
    snd_pcm_info_t      *pcm_info  = nullptr;
    snd_ctl_card_info_alloca(&card_info);
    snd_pcm_info_alloca(&pcm_info);
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0)
    {
        snd_ctl_t        *control = nullptr;
        const std::string hw      = "hw:" + std::to_string(card);
        if (snd_ctl_open(&control, hw.c_str(), 0) < 0)
        {
            continue;
        }
        if (snd_ctl_card_info(control, card_info) >= 0)
        {
            int device = -1;
            while (snd_ctl_pcm_next_device(control, &device) >= 0 && device >= 0)
            {
                // A device is named by its playback side's PCM, or its capture side's where it plays nothing.
                snd_pcm_info_set_device(pcm_info, static_cast<unsigned int>(device));
                snd_pcm_info_set_subdevice(pcm_info, 0);
                snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_PLAYBACK);
                int result = snd_ctl_pcm_info(control, pcm_info);
                if (result == -ENOENT)
                {
                    snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
                    result = snd_ctl_pcm_info(control, pcm_info);
                }
                if (result < 0)
                {
                    continue;
                }
                std::string name =
                    std::string(snd_ctl_card_info_get_name(card_info)) + " (" + snd_pcm_info_get_id(pcm_info) + ")";
                if (card == 0 && device == 0 && default_name.empty())
                {
                    default_name = name;
                }
                pcms.emplace_back("hw:" + std::string(snd_ctl_card_info_get_id(card_info)) + "," +
                                      std::to_string(device),
                                  std::move(name));
            }
        }
        snd_ctl_close(control);
    }

    std::vector<Listed> listed;
    ProbeCache         &cache = Probes();
    for (auto &[id, name] : pcms)
    {
        const std::string key = id + '\n' + name;
        Probed            probed;
        {
            const std::lock_guard lock(cache.mutex);
            const auto            known = cache.probed.find(key);
            if (known != cache.probed.end())
            {
                probed = known->second;
            }
        }
        if (probed.inputs == 0 && probed.outputs == 0)
        {
            const std::optional<Probed> found = Probe(id);
            if (!found)
            {
                continue; // will not open either way: in use since before the program started, or no PCM after all
            }
            probed = *found;
            const std::lock_guard lock(cache.mutex);
            cache.probed[key] = probed;
        }
        Listed device;
        device.id                         = id;
        device.info.name                  = name;
        device.info.input_channels        = probed.inputs;
        device.info.output_channels       = probed.outputs;
        device.info.duplex_channels       = std::min(probed.inputs, probed.outputs);
        device.info.is_default_input      = name == default_name && probed.inputs > 0;
        device.info.is_default_output     = name == default_name && probed.outputs > 0;
        device.info.sample_rates          = probed.rates;
        device.info.preferred_sample_rate = probed.preferred;
        listed.push_back(std::move(device));
    }

    std::vector<DeviceInfo> infos;
    infos.reserve(listed.size());
    for (const Listed &device : listed)
    {
        infos.push_back(device.info);
    }
    NameApart(infos);
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        listed[i].info.name = std::move(infos[i].name);
    }
    return listed;
}

/** The device named `name` with channels the way `direction` asks.
    @param listed     the devices Enumerate() gave
    @param name       the device's name, as a settings file keeps it
    @param direction  the way the device is wanted
    @return           the device, or null for none */
const Listed *Find(const std::vector<Listed> &listed, const std::string &name, Direction direction)
{
    for (const Listed &device : listed)
    {
        if (device.info.name == name && Carries(device.info, direction))
        {
            return &device;
        }
    }
    return nullptr;
}

/** One direction of a stream: its PCM and how it was opened. */
struct Side
{
    snd_pcm_t                *pcm{nullptr};               ///< the PCM; null where the stream has no such side
    bool                      is_mmap{false};             ///< true where its buffer is mapped, false for read and write
    Encoding                  encoding{Encoding::kInt16}; ///< how its samples are stored
    int                       channels{0};                ///< the device frame's channels
    int                       frame_bytes{0};             ///< the bytes a frame takes
    int                       period{0};                  ///< its period, in frames
    int                       buffer{0};                  ///< its buffer, in frames
    int                       first{0};                   ///< the device channel callback channel 0 is
    int                       count{0};                   ///< the callback's channels
    int                       descriptors{0};             ///< the poll descriptors it waits on
    std::vector<std::uint8_t> staging;                    ///< without a mapped buffer: a buffer's frames, interleaved

    /** The PCM closed and the side forgotten. */
    void Close() noexcept
    {
        if (pcm != nullptr)
        {
            snd_pcm_close(pcm);
        }
        *this = Side{};
    }
};

/** ALSA's AudioSystem: its devices, and its one stream on a thread of its own. Message thread throughout but for the
    stream's thread (Run and what it calls). */
class AlsaSystem final : public AudioSystem
{
  public:
    AlsaSystem() = default;

    /** The stream closed (Close). */
    ~AlsaSystem() override
    {
        Close();
    }

    AlsaSystem(const AlsaSystem &)            = delete;
    AlsaSystem &operator=(const AlsaSystem &) = delete;

    std::string Name() const override
    {
        return "ALSA";
    }

    std::vector<DeviceInfo> Devices() override
    {
        std::vector<DeviceInfo> devices;
        for (Listed &device : Enumerate())
        {
            devices.push_back(std::move(device.info));
        }
        return devices;
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        SetError({});
        const std::vector<Listed> listed = Enumerate();
        const Listed             *output = Find(listed, request.output_device, Direction::kOutput);
        if (output == nullptr || request.output_count <= 0)
        {
            return Refuse("no output device named '" + request.output_device + "' on ALSA");
        }
        if (request.first_output < 0 || request.first_output + request.output_count > output->info.output_channels)
        {
            return Refuse("'" + output->info.name + "' has " + std::to_string(output->info.output_channels) +
                          " output channels, and the stream asks for channels " +
                          std::to_string(request.first_output + 1) + " to " +
                          std::to_string(request.first_output + request.output_count));
        }
        const bool    with_input = !request.input_device.empty() && request.input_count > 0;
        const Listed *input      = nullptr;
        if (with_input)
        {
            input = Find(listed, request.input_device, Direction::kInput);
            if (input == nullptr)
            {
                return Refuse("no input device named '" + request.input_device + "' on ALSA");
            }
            if (request.first_input < 0 || request.first_input + request.input_count > input->info.input_channels)
            {
                return Refuse("'" + input->info.name + "' has " + std::to_string(input->info.input_channels) +
                              " input channels, and the stream asks for channels " +
                              std::to_string(request.first_input + 1) + " to " +
                              std::to_string(request.first_input + request.input_count));
            }
        }

        const int   rate  = request.sample_rate > 0 ? request.sample_rate : output->info.DefaultSampleRate();
        const int   asked = request.block_size > 0 ? request.block_size : 512;
        std::string why;
        out_.first = request.first_output;
        out_.count = request.output_count;
        // The product's periods, where it asks for some (StreamRequest::periods), else the back end's own.
        const unsigned int periods =
            request.periods > 0 ? std::max(kFewestPeriods, static_cast<unsigned int>(request.periods)) : kPeriods;
        if (!OpenSide(out_, output->id, SND_PCM_STREAM_PLAYBACK, rate, asked, periods, why))
        {
            Close();
            return Refuse("'" + output->info.name + "' would not open for output on ALSA: " + why);
        }
        block_ = out_.period; // the output's period is the callback's block; the input is read a block at a time
        if (input != nullptr)
        {
            in_.first = request.first_input;
            in_.count = request.input_count;
            if (!OpenSide(in_, input->id, SND_PCM_STREAM_CAPTURE, rate, block_, kPeriods, why))
            {
                Close();
                return Refuse("'" + input->info.name + "' would not open for input on ALSA: " + why);
            }
            // One card's two directions started, stopped and prepared as one, so neither runs a moment without the
            // other; two cards' cannot be, and run each on its own clock.
            linked_ = snd_pcm_link(in_.pcm, out_.pcm) == 0;
        }

        wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_ < 0)
        {
            Close();
            return Refuse("the stream's thread could not be given a way to be woken: " +
                          std::string(std::strerror(errno)));
        }
        const auto block = static_cast<std::size_t>(block_);
        input_storage_.assign(static_cast<std::size_t>(std::max(in_.count, 1)) * block, 0.0f);
        output_storage_.assign(static_cast<std::size_t>(out_.count) * block, 0.0f);
        inputs_.assign(static_cast<std::size_t>(std::max(in_.count, 1)), nullptr);
        outputs_.assign(static_cast<std::size_t>(out_.count), nullptr);
        // Each channel's frames where the channel before's end, the layout every API's callback hands the engine.
        for (std::size_t c = 0; c < static_cast<std::size_t>(in_.count); ++c)
        {
            inputs_[c] = input_storage_.data() + c * block;
        }
        for (std::size_t c = 0; c < static_cast<std::size_t>(out_.count); ++c)
        {
            outputs_[c] = output_storage_.data() + c * block;
        }
        input_part_.assign(inputs_.size(), nullptr);
        output_part_.assign(outputs_.size(), nullptr);
        descriptors_.assign(static_cast<std::size_t>(1 + out_.descriptors + in_.descriptors), pollfd{});

        format_.sample_rate     = rate;
        format_.block_size      = block_;
        format_.input_channels  = in_.count;
        format_.output_channels = out_.count;
        // A block written lands behind what the output already holds, its buffer less the period just played; a
        // block read is the input's latest.
        format_.reported_latency = out_.buffer - block_;
        format_.periods          = out_.buffer / std::max(block_, 1);
        callback_.store(&callback, std::memory_order_release);
        failed_.store(false, std::memory_order_relaxed);
        open_ = true;
        return true;
    }

    bool Start() override
    {
        if (!open_)
        {
            return false;
        }
        if (thread_.joinable())
        {
            return true;
        }
        std::uint64_t drained = 0;
        while (read(wake_, &drained, sizeof drained) > 0)
        {
        }
        stop_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        std::promise<std::string> started;
        std::future<std::string>  answer = started.get_future();
        thread_                          = std::thread([this, &started] { Run(started); });
        const std::string why            = answer.get();
        if (!why.empty())
        {
            thread_.join();
            running_.store(false, std::memory_order_release);
            SetError("the stream would not start on ALSA: " + why);
            return false;
        }
        return true;
    }

    void Stop() override
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable())
        {
            stop_.store(true, std::memory_order_release);
            const std::uint64_t            one   = 1;
            [[maybe_unused]] const ssize_t woken = write(wake_, &one, sizeof one); // the eventfd cannot be full
            thread_.join();
        }
        for (Side *side : {&in_, &out_})
        {
            if (side->pcm != nullptr)
            {
                snd_pcm_drop(side->pcm);
            }
        }
    }

    void Close() override
    {
        Stop();
        if (linked_)
        {
            snd_pcm_unlink(in_.pcm);
            linked_ = false;
        }
        in_.Close();
        out_.Close();
        if (wake_ >= 0)
        {
            close(wake_);
            wake_ = -1;
        }
        open_   = false;
        format_ = {};
        callback_.store(nullptr, std::memory_order_release);
    }

    bool IsOpen() const override
    {
        return open_;
    }

    StreamFormat Format() const override
    {
        return format_;
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return last_error_;
    }

  private:
    // ---- Opening ----------------------------------------------------------------------------------------------------

    /** One side opened on PCM `id`: interleaved, mapped where it can be, at `rate` exactly, in the best encoding it
        takes, with the device channels the side's channels need (or the fewest the device takes past them), a period
        of `block` frames or the nearest the device takes, `periods` of them buffered on an output (or the nearest the
        device takes) and at least kInputFrames on an input; started only when asked (the
        start threshold past the buffer), stopped by ALSA once it is past (the stop threshold the buffer), and waking a
        poll once a block is there.
        @param side    the side, its `first` and `count` set; filled in here
        @param id      the PCM's name
        @param stream  playback or capture
        @param rate    the rate, in hertz
        @param block   the period to ask for, and the frames a poll waits for, in frames
        @param periods the periods an output's buffer is to hold, at least kFewestPeriods; an input takes no notice
        @param why     set to the reason when it will not open
        @return        true when it opened */
    static bool OpenSide(Side &side, const std::string &id, snd_pcm_stream_t stream, int rate, int block,
                         unsigned int periods, std::string &why)
    {
        int result = snd_pcm_open(&side.pcm, id.c_str(), stream, SND_PCM_NONBLOCK);
        if (result < 0)
        {
            side.pcm = nullptr;
            why      = Describe(result);
            return false;
        }
        snd_pcm_hw_params_t *hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        if ((result = snd_pcm_hw_params_any(side.pcm, hw)) < 0)
        {
            why = Describe(result);
            return false;
        }
        side.is_mmap = snd_pcm_hw_params_set_access(side.pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0;
        if (!side.is_mmap && (result = snd_pcm_hw_params_set_access(side.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        {
            why = "it offers no interleaved access: " + Describe(result);
            return false;
        }
        const SampleFormat *format = nullptr;
        for (const SampleFormat &candidate : kFormats)
        {
            if (snd_pcm_hw_params_set_format(side.pcm, hw, candidate.alsa) == 0)
            {
                format = &candidate;
                break;
            }
        }
        if (format == nullptr)
        {
            why = "it takes none of 32-bit float, 32-bit, 24-bit or 16-bit integer";
            return false;
        }
        unsigned int channels = static_cast<unsigned int>(side.first + side.count);
        if (snd_pcm_hw_params_set_channels_min(side.pcm, hw, &channels) < 0 ||
            snd_pcm_hw_params_set_channels_first(side.pcm, hw, &channels) < 0)
        {
            why = "it does not take " + std::to_string(side.first + side.count) + " channels";
            return false;
        }
        if ((result = snd_pcm_hw_params_set_rate(side.pcm, hw, static_cast<unsigned int>(rate), 0)) < 0)
        {
            why = "it does not take " + std::to_string(rate) + " Hz";
            return false;
        }
        snd_pcm_uframes_t period    = static_cast<snd_pcm_uframes_t>(block);
        int               direction = 0;
        if ((result = snd_pcm_hw_params_set_period_size_near(side.pcm, hw, &period, &direction)) < 0)
        {
            why = "it takes no period near " + std::to_string(block) + " frames: " + Describe(result);
            return false;
        }
        // An output's latency is its buffer, so the periods asked for; an input's room costs nothing (kInputFrames).
        periods = stream == SND_PCM_STREAM_PLAYBACK
                      ? periods
                      : std::max(kPeriods, static_cast<unsigned int>((kInputFrames + period - 1) /
                                                                     std::max<snd_pcm_uframes_t>(period, 1)));
        const unsigned int asked_periods = periods;
        direction                        = 0;
        if ((result = snd_pcm_hw_params_set_periods_near(side.pcm, hw, &periods, &direction)) < 0)
        {
            why = "it takes no buffer of " + std::to_string(asked_periods) + " periods: " + Describe(result);
            return false;
        }
        if ((result = snd_pcm_hw_params(side.pcm, hw)) < 0)
        {
            why = Describe(result);
            return false;
        }
        snd_pcm_uframes_t buffer = 0;
        snd_pcm_hw_params_get_period_size(hw, &period, &direction);
        snd_pcm_hw_params_get_buffer_size(hw, &buffer);
        snd_pcm_hw_params_get_channels(hw, &channels);
        side.encoding    = format->encoding;
        side.channels    = static_cast<int>(channels);
        side.frame_bytes = side.channels * BytesOf(side.encoding);
        side.period      = static_cast<int>(period);
        side.buffer      = static_cast<int>(buffer);
        if (side.buffer < 2 * block)
        {
            why = "its buffer holds " + std::to_string(side.buffer) + " frames, fewer than two blocks of " +
                  std::to_string(block);
            return false;
        }

        snd_pcm_sw_params_t *sw = nullptr;
        snd_pcm_sw_params_alloca(&sw);
        snd_pcm_uframes_t boundary = 0;
        if ((result = snd_pcm_sw_params_current(side.pcm, sw)) < 0 ||
            (result = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
            (result = snd_pcm_sw_params_set_avail_min(side.pcm, sw, static_cast<snd_pcm_uframes_t>(block))) < 0 ||
            (result = snd_pcm_sw_params_set_start_threshold(side.pcm, sw, boundary)) < 0 ||
            (result = snd_pcm_sw_params_set_stop_threshold(side.pcm, sw, buffer)) < 0 ||
            (result = snd_pcm_sw_params(side.pcm, sw)) < 0)
        {
            why = Describe(result);
            return false;
        }
        side.descriptors = snd_pcm_poll_descriptors_count(side.pcm);
        if (side.descriptors <= 0)
        {
            why = "it offers nothing to wait on";
            return false;
        }
        if (!side.is_mmap)
        {
            side.staging.assign(static_cast<std::size_t>(side.buffer) * static_cast<std::size_t>(side.frame_bytes), 0);
        }
        return true;
    }

    /** Why an open failed, kept for LastError.
        @param text  the reason
        @return      false, the open's answer */
    bool Refuse(const std::string &text)
    {
        SetError(text);
        return false;
    }

    /** The last error replaced.
        @param text  the new text; empty to clear it */
    void SetError(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        last_error_ = text;
    }

    // ---- The stream's thread ----------------------------------------------------------------------------------------

    /** The stream's thread: its priority raised where the system allows it, the devices started - the answer handed
        back to Start - then the wait until Stop or a failure.
        @param started  where Start waits for the start's answer: empty, or why the stream did not start */
    void Run(std::promise<std::string> &started)
    {
        sched_param param{};
        param.sched_priority = std::min(kPriority, sched_get_priority_max(SCHED_RR));
        static_cast<void>(pthread_setschedparam(pthread_self(), SCHED_RR, &param)); // a user allowed none runs as is
        pending_xrun_    = false;
        has_called_      = false;
        const int result = Begin();
        started.set_value(result < 0 ? Describe(result) : std::string());
        if (result >= 0)
        {
            Loop();
        }
    }

    /** The devices prepared, the output primed with a buffer of silence and both started - together where they are
        linked - as the stream starts and again after an xrun.
        @return  0, or the error of the call that failed */
    int Begin() noexcept
    {
        for (Side *side : {&in_, &out_})
        {
            if (side->pcm != nullptr)
            {
                snd_pcm_drop(side->pcm);
                if (const int result = snd_pcm_prepare(side->pcm); result < 0)
                {
                    return result;
                }
            }
        }
        const snd_pcm_sframes_t room = snd_pcm_avail_update(out_.pcm);
        if (room < 0)
        {
            return static_cast<int>(room);
        }
        if (const int result = Write(static_cast<int>(room), true); result < 0)
        {
            return result;
        }
        if (in_.pcm != nullptr && !linked_)
        {
            if (const int result = snd_pcm_start(in_.pcm); result < 0)
            {
                return result;
            }
        }
        return snd_pcm_start(out_.pcm);
    }

    /** The wait: whenever the input has a block and the output room for one, a cycle; until then poll on the side
        that lacks, and on the wake Stop gives, for no longer than kWaitTimeoutMs. Ends at Stop, or at a failure. */
    void Loop()
    {
        while (!stop_.load(std::memory_order_acquire))
        {
            const snd_pcm_sframes_t room = snd_pcm_avail_update(out_.pcm);
            if (room < 0)
            {
                if (!Recover(static_cast<int>(room)))
                {
                    return;
                }
                continue;
            }
            const snd_pcm_sframes_t have = in_.pcm != nullptr ? snd_pcm_avail_update(in_.pcm) : block_;
            if (have < 0)
            {
                if (!Recover(static_cast<int>(have)))
                {
                    return;
                }
                continue;
            }
            if (room >= block_ && have >= block_)
            {
                if (const int result = Cycle(); result < 0 && !Recover(result))
                {
                    return;
                }
                continue;
            }

            descriptors_[0].fd     = wake_;
            descriptors_[0].events = POLLIN;
            int       count        = 1;
            const int out_at       = room < block_ ? count : -1;
            if (out_at >= 0)
            {
                count += snd_pcm_poll_descriptors(out_.pcm, &descriptors_[static_cast<std::size_t>(count)],
                                                  static_cast<unsigned int>(out_.descriptors));
            }
            const int in_at = have < block_ ? count : -1;
            if (in_at >= 0)
            {
                count += snd_pcm_poll_descriptors(in_.pcm, &descriptors_[static_cast<std::size_t>(count)],
                                                  static_cast<unsigned int>(in_.descriptors));
            }
            const int woke = poll(descriptors_.data(), static_cast<nfds_t>(count), kWaitTimeoutMs);
            if (stop_.load(std::memory_order_acquire))
            {
                return;
            }
            if (woke < 0 && errno == EINTR)
            {
                continue;
            }
            if (woke <= 0)
            {
                Fail(woke == 0 ? "the device stopped answering" : std::strerror(errno));
                return;
            }
            for (const auto &[side, at] : {std::pair<Side *, int>{&out_, out_at}, std::pair<Side *, int>{&in_, in_at}})
            {
                unsigned short events = 0;
                if (at < 0 ||
                    snd_pcm_poll_descriptors_revents(side->pcm, &descriptors_[static_cast<std::size_t>(at)],
                                                     static_cast<unsigned int>(side->descriptors), &events) < 0)
                {
                    continue;
                }
                if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0 && !Recover(ErrorOf(side->pcm)))
                {
                    return;
                }
            }
        }
    }

    /** What a PCM a poll reported an error on has come to, as the error its next call would return.
        @param pcm  the PCM
        @return     -EPIPE for an xrun, -ESTRPIPE for a suspend, -ENODEV for a device gone, else -EIO */
    static int ErrorOf(snd_pcm_t *pcm) noexcept
    {
        switch (snd_pcm_state(pcm))
        {
        case SND_PCM_STATE_XRUN:
            return -EPIPE;
        case SND_PCM_STATE_SUSPENDED:
            return -ESTRPIPE;
        case SND_PCM_STATE_DISCONNECTED:
            return -ENODEV;
        case SND_PCM_STATE_RUNNING:
        case SND_PCM_STATE_PREPARED:
            return -EAGAIN; // nothing wrong after all: the next pass asks again
        default:
            return -EIO;
        }
    }

    /** One block: the input read, the callback run - or silence while the stream is stopping - and the output written.
        Never allocates or locks.
        @return  0, or the error of the transfer that failed */
    int Cycle() noexcept
    {
        const bool xrun = pending_xrun_;
        pending_xrun_   = false;
        if (in_.pcm != nullptr)
        {
            if (const int result = Read(); result < 0)
            {
                return result;
            }
        }
        AudioCallback *run =
            running_.load(std::memory_order_acquire) ? callback_.load(std::memory_order_acquire) : nullptr;
        if (run == nullptr)
        {
            return Write(block_, true);
        }
        run->Process(in_.count > 0 ? inputs_.data() : nullptr, in_.count, outputs_.data(), out_.count, block_,
                     NowNanoseconds(), xrun);
        has_called_ = true;
        return Write(block_, false);
    }

    /** A block read from the input into the callback's input channels: from the mapped buffer, a contiguous run at a
        time, or through the staging buffer.
        @return  0, or the error of the call that failed */
    int Read() noexcept
    {
        int done = 0;
        while (done < block_)
        {
            for (std::size_t c = 0; c < input_part_.size(); ++c)
            {
                input_part_[c] = inputs_[c] + done;
            }
            snd_pcm_uframes_t frames = static_cast<snd_pcm_uframes_t>(block_ - done);
            if (in_.is_mmap)
            {
                const snd_pcm_channel_area_t *areas  = nullptr;
                snd_pcm_uframes_t             offset = 0;
                if (const int result = snd_pcm_mmap_begin(in_.pcm, &areas, &offset, &frames); result < 0)
                {
                    return result;
                }
                Decode(Address(areas, offset), in_.channels, static_cast<int>(frames), in_.encoding, in_.first,
                       in_.count, input_part_.data());
                const snd_pcm_sframes_t committed = snd_pcm_mmap_commit(in_.pcm, offset, frames);
                if (committed < 0)
                {
                    return static_cast<int>(committed);
                }
                if (static_cast<snd_pcm_uframes_t>(committed) != frames)
                {
                    return -EPIPE;
                }
            }
            else
            {
                const snd_pcm_sframes_t got = snd_pcm_readi(in_.pcm, in_.staging.data(), frames);
                if (got < 0)
                {
                    return static_cast<int>(got);
                }
                if (got == 0)
                {
                    return -EPIPE;
                }
                frames = static_cast<snd_pcm_uframes_t>(got);
                Decode(in_.staging.data(), in_.channels, static_cast<int>(frames), in_.encoding, in_.first, in_.count,
                       input_part_.data());
            }
            done += static_cast<int>(frames);
        }
        return 0;
    }

    /** `frames` written to the output - the callback's output channels, or silence - into the mapped buffer a
        contiguous run at a time, or through the staging buffer.
        @param frames   the frames to write: a block, or the buffer's room as it is primed
        @param silence  true to write silence, false the callback's output channels (`frames` a block)
        @return         0, or the error of the call that failed */
    int Write(int frames, bool silence) noexcept
    {
        int done = 0;
        while (done < frames)
        {
            for (std::size_t c = 0; c < output_part_.size() && !silence; ++c)
            {
                output_part_[c] = outputs_[c] + done;
            }
            snd_pcm_uframes_t part = static_cast<snd_pcm_uframes_t>(frames - done);
            if (!out_.is_mmap)
            {
                part = std::min(part, static_cast<snd_pcm_uframes_t>(out_.buffer));
            }
            const snd_pcm_channel_area_t *areas  = nullptr;
            snd_pcm_uframes_t             offset = 0;
            std::uint8_t                 *to     = out_.staging.data();
            if (out_.is_mmap)
            {
                if (const int result = snd_pcm_mmap_begin(out_.pcm, &areas, &offset, &part); result < 0)
                {
                    return result;
                }
                to = Address(areas, offset);
            }
            if (silence)
            {
                std::memset(to, 0, static_cast<std::size_t>(part) * static_cast<std::size_t>(out_.frame_bytes));
            }
            else
            {
                Encode(output_part_.data(), out_.count, out_.first, out_.channels, static_cast<int>(part),
                       out_.encoding, to);
            }
            const snd_pcm_sframes_t written =
                out_.is_mmap ? snd_pcm_mmap_commit(out_.pcm, offset, part) : snd_pcm_writei(out_.pcm, to, part);
            if (written < 0)
            {
                return static_cast<int>(written);
            }
            if (written == 0 || (out_.is_mmap && static_cast<snd_pcm_uframes_t>(written) != part))
            {
                return -EPIPE;
            }
            done += static_cast<int>(written);
        }
        return 0;
    }

    /** The first byte of the frame at `offset` in a mapped interleaved buffer, every channel's area sharing it.
        @param areas   the areas snd_pcm_mmap_begin gave
        @param offset  the frame's offset
        @return        its first byte */
    static std::uint8_t *Address(const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset) noexcept
    {
        return static_cast<std::uint8_t *>(areas[0].addr) + areas[0].first / 8 + offset * (areas[0].step / 8);
    }

    /** The stream after an error: an xrun or a suspend, the devices started again (Begin) and the next callback told -
        once the callback has run; anything else, or a start that fails, the stream failing.
        @param error  the error a call returned
        @return       true when the stream runs on */
    bool Recover(int error) noexcept
    {
        if (error == -EAGAIN)
        {
            return true;
        }
        if (error == -ESTRPIPE)
        {
            // Woken from a suspend: resumed where the driver can, else prepared afresh like an xrun.
            for (Side *side : {&in_, &out_})
            {
                for (int i = 0; side->pcm != nullptr && snd_pcm_resume(side->pcm) == -EAGAIN && i < 1000; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            error = -EPIPE;
        }
        if (error == -EPIPE)
        {
            // Before the first callback, a device run dry is the stream starting - a sound server waking a suspended
            // input while the primed output plays out - and not a dropout: nothing the callback made was lost.
            pending_xrun_ = pending_xrun_ || has_called_;
            error         = Begin();
            if (error >= 0)
            {
                return true;
            }
        }
        Fail(Describe(error));
        return false;
    }

    /** The stream failing, its reason kept - without waiting, should the message thread be reading the last - and told
        once.
        @param why  the reason */
    void Fail(const std::string &why) noexcept
    {
        {
            const std::unique_lock lock(error_mutex_, std::try_to_lock);
            if (lock.owns_lock())
            {
                try
                {
                    last_error_ = "the stream on ALSA failed: " + why;
                }
                catch (const std::exception &)
                {
                }
            }
        }
        if (!failed_.exchange(true, std::memory_order_acq_rel) && running_.load(std::memory_order_acquire))
        {
            if (AudioCallback *run = callback_.load(std::memory_order_acquire))
            {
                run->StreamFailed();
            }
        }
    }

    Side                         out_;            ///< the output side
    Side                         in_;             ///< the input side; its PCM null with no input
    bool                         linked_{false};  ///< true when the two sides are linked, started and stopped as one
    int                          block_{0};       ///< the frames a callback carries, the output's period
    int                          wake_{-1};       ///< the eventfd Stop wakes the stream's thread through
    std::vector<float>           input_storage_;  ///< the callback's input channels' samples, sized at the open
    std::vector<float>           output_storage_; ///< the callback's output channels' samples, sized at the open
    std::vector<float *>         inputs_;         ///< the input channel pointers the callback is handed
    std::vector<float *>         outputs_;        ///< the output channel pointers the callback is handed
    std::vector<float *>         input_part_;     ///< the input channels from a transfer's first frame
    std::vector<const float *>   output_part_;    ///< the output channels from a transfer's first frame
    std::vector<pollfd>          descriptors_;    ///< what a wait polls: the wake and the sides' descriptors
    bool                         pending_xrun_{false}; ///< an xrun to tell the next callback of
    bool                         has_called_{false};   ///< true once the callback has run since the Start
    StreamFormat                 format_;              ///< what the stream opened as
    bool                         open_{false};         ///< true from a successful Open until Close
    std::atomic<AudioCallback *> callback_{nullptr};   ///< what runs the stream; null while closed
    std::atomic<bool>            running_{false};      ///< true from Start until Stop; false writes silence
    std::atomic<bool>            stop_{false};         ///< true tells the stream's thread to end
    std::atomic<bool>            failed_{false};       ///< true once the stream's failure has been told
    mutable std::mutex           error_mutex_;         ///< guards `last_error_`
    std::string                  last_error_;          ///< the last error, for a person to read
    std::thread                  thread_;              ///< the stream's thread, from Start until Stop
};

/** The one name this back end serves.
    @return  "ALSA" */
std::vector<std::string> ApiNames()
{
    return {"ALSA"};
}

/** A system for ALSA.
    @param name  the API's name
    @return      the system, owned by the caller; nullptr for any name but "ALSA" */
std::unique_ptr<AudioSystem> MakeSystem(const std::string &name)
{
    if (name != "ALSA")
    {
        return nullptr;
    }
    return std::make_unique<AlsaSystem>();
}

/** What this back end is: ALSA, and the version of the alsa-lib it runs on.
    @return  the text */
std::string Library()
{
    return std::string("ALSA ") + snd_asoundlib_version();
}
} // namespace

const AudioBackend &AlsaBackend()
{
    static const AudioBackend backend{&Library, &ApiNames, &MakeSystem};
    return backend;
}
} // namespace reckless::audio
