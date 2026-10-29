// The evdev back end's port on the kernel (EvdevGamepads.h says what a port is): /dev/input's event nodes listed,
// stamped, opened read-only and without blocking, asked what they are with the event interface's ioctls, and read;
// and /dev/input watched with inotify, so a controller plugged in - and udev's granting the seat's user access to it a
// moment later, which changes the node's access list and so its inode - is noticed at the next poll.
#include "EvdevSystemPort.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "EvdevGamepads.h"

namespace reckless::input::evdev
{
namespace
{
/** Where the event nodes are. */
constexpr const char *kInputDirectory = "/dev/input";

/** How many bits an unsigned long holds: the kernel's bit arrays are arrays of them. */
constexpr std::size_t kBitsPerLong = sizeof(unsigned long) * CHAR_BIT;

/** A bit array of the kernel's, `bits` long, as the kernel lays it out: bit n in long n / kBitsPerLong. */
template <std::size_t bits> using KernelBits = std::array<unsigned long, (bits + kBitsPerLong - 1) / kBitsPerLong>;

/** A kernel bit array as a bitset.
    @tparam bits  how many bits
    @param kernel  the kernel's array
    @return        the bitset */
template <std::size_t bits> std::bitset<bits> BitsetOf(const KernelBits<bits> &kernel)
{
    std::bitset<bits> set;
    for (std::size_t bit = 0; bit < bits; ++bit)
    {
        if (((kernel[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1ul) != 0)
        {
            set.set(bit);
        }
    }
    return set;
}

/** One of a node's names: EVIOCGNAME, EVIOCGUNIQ or EVIOCGPHYS.
    @param handle   the node
    @param request  the ioctl, sized to the buffer
    @param buffer   a buffer of the request's size, zeroed
    @return         the name; empty where the node has none */
std::string NameOf(int handle, unsigned long request, std::array<char, 256> &buffer)
{
    buffer.fill('\0');
    if (ioctl(handle, request, buffer.data()) < 0)
    {
        return {};
    }
    buffer.back() = '\0';
    return std::string(buffer.data());
}

/** Every axis's reading on a node, written where `values` says; an axis the node does not have reads 0.
    @param handle  the node
    @param info    where each axis's range and reading are written; may be null
    @param values  where each axis's reading is written; may be null */
void ReadAbsolutes(int handle, std::array<EvdevAbsoluteInfo, kAbsoluteCount> *info,
                   std::array<std::int32_t, kAbsoluteCount> *values)
{
    for (std::size_t code = 0; code < kAbsoluteCount; ++code)
    {
        input_absinfo absolute{};
        if (ioctl(handle, EVIOCGABS(code), &absolute) < 0)
        {
            continue;
        }
        if (info != nullptr)
        {
            (*info)[code] = EvdevAbsoluteInfo{absolute.value, absolute.minimum, absolute.maximum};
        }
        if (values != nullptr)
        {
            (*values)[code] = absolute.value;
        }
    }
}

/** The port on the kernel. */
class SystemEvdevPort final : public EvdevPort
{
  public:
    /** /dev/input watched, where inotify lets it be. */
    SystemEvdevPort() : watch_(inotify_init1(IN_NONBLOCK | IN_CLOEXEC))
    {
        if (watch_ >= 0 &&
            inotify_add_watch(watch_, kInputDirectory, IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO) < 0)
        {
            close(watch_);
            watch_ = -1;
        }
    }

    /** The watch closed. */
    ~SystemEvdevPort() override
    {
        if (watch_ >= 0)
        {
            close(watch_);
        }
    }

    SystemEvdevPort(const SystemEvdevPort &)            = delete; ///< not copied: it owns its watch
    SystemEvdevPort &operator=(const SystemEvdevPort &) = delete; ///< not copied: it owns its watch

    /** Whether /dev/input is watched.
        @return  true where inotify watches it */
    bool CanWatchNodes() const override
    {
        return watch_ >= 0;
    }

    /** Whether inotify said anything of /dev/input since the last call; everything it said is read.
        @return  true when it said anything */
    bool TakeNodesChanged() override
    {
        bool                                          has_changed = false;
        alignas(inotify_event) std::array<char, 4096> buffer{};
        while (watch_ >= 0)
        {
            const ssize_t bytes = read(watch_, buffer.data(), buffer.size());
            if (bytes <= 0)
            {
                break;
            }
            has_changed = true;
        }
        return has_changed;
    }

    /** The monotonic clock.
        @return  milliseconds since some fixed point */
    std::int64_t NowMilliseconds() const override
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /** /dev/input's event nodes.
        @return  each node's path */
    std::vector<std::string> ListNodes() const override
    {
        std::vector<std::string> paths;
        DIR                     *directory = opendir(kInputDirectory);
        if (directory == nullptr)
        {
            return paths;
        }
        while (const dirent *entry = readdir(directory))
        {
            if (NodeNumber(entry->d_name) >= 0)
            {
                paths.push_back(std::string(kInputDirectory) + "/" + entry->d_name);
            }
        }
        closedir(directory);
        return paths;
    }

    /** A node's inode and the time it last changed.
        @param path  the node
        @return      its stamp; zeros where it cannot be read */
    EvdevNodeStamp StampOf(const std::string &path) const override
    {
        struct stat status{};
        if (stat(path.c_str(), &status) != 0)
        {
            return {};
        }
        return EvdevNodeStamp{static_cast<std::uint64_t>(status.st_ino),
                              static_cast<std::int64_t>(status.st_ctim.tv_sec) * 1'000'000'000 +
                                  static_cast<std::int64_t>(status.st_ctim.tv_nsec)};
    }

    /** A node opened read-only and without blocking, and asked what it is.
        @param path          the node
        @param capabilities  where what it is is written
        @return              its descriptor; -1 where it cannot be opened or asked */
    int Open(const std::string &path, EvdevCapabilities &capabilities) override
    {
        const int handle = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (handle < 0)
        {
            return -1;
        }
        KernelBits<kKeyCount>      keys{};
        KernelBits<kAbsoluteCount> absolutes{};
        KernelBits<kPropertyCount> properties{};
        input_id                   id{};
        if (ioctl(handle, EVIOCGBIT(EV_KEY, sizeof(keys)), keys.data()) < 0 ||
            ioctl(handle, EVIOCGBIT(EV_ABS, sizeof(absolutes)), absolutes.data()) < 0 ||
            ioctl(handle, EVIOCGID, &id) < 0)
        {
            close(handle);
            return -1;
        }
        // EVIOCGPROP came with Linux 2.6.39: a kernel without it has no properties to give.
        if (ioctl(handle, EVIOCGPROP(sizeof(properties)), properties.data()) < 0)
        {
            properties.fill(0);
        }
        capabilities.keys       = BitsetOf<kKeyCount>(keys);
        capabilities.absolutes  = BitsetOf<kAbsoluteCount>(absolutes);
        capabilities.properties = BitsetOf<kPropertyCount>(properties);
        capabilities.vendor_id  = id.vendor;
        capabilities.product_id = id.product;
        ReadAbsolutes(handle, &capabilities.absolute_info, nullptr);
        std::array<char, 256> buffer{};
        capabilities.name          = NameOf(handle, EVIOCGNAME(buffer.size()), buffer);
        capabilities.unique_id     = NameOf(handle, EVIOCGUNIQ(buffer.size()), buffer);
        capabilities.physical_path = NameOf(handle, EVIOCGPHYS(buffer.size()), buffer);
        return handle;
    }

    /** What a node holds now.
        @param handle    the node
        @param snapshot  where its keys and readings are written
        @return          true when its keys were read */
    bool ReadSnapshot(int handle, EvdevSnapshot &snapshot) override
    {
        KernelBits<kKeyCount> keys{};
        if (ioctl(handle, EVIOCGKEY(sizeof(keys)), keys.data()) < 0)
        {
            return false;
        }
        snapshot.keys = BitsetOf<kKeyCount>(keys);
        snapshot.absolute_values.fill(0);
        ReadAbsolutes(handle, nullptr, &snapshot.absolute_values);
        return true;
    }

    /** Every event waiting on a node.
        @param handle  the node
        @param events  where its events are appended
        @return        kGone where the device has gone (ENODEV) or the node can no longer be read */
    EvdevReadResult Read(int handle, std::vector<EvdevEvent> &events) override
    {
        std::array<input_event, 64> buffer{};
        for (;;)
        {
            const ssize_t bytes = read(handle, buffer.data(), sizeof(buffer));
            if (bytes < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return errno == EAGAIN || errno == EWOULDBLOCK ? EvdevReadResult::kRead : EvdevReadResult::kGone;
            }
            const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(input_event);
            for (std::size_t event = 0; event < count; ++event)
            {
                events.push_back(EvdevEventOf(buffer[event]));
            }
            if (count < buffer.size())
            {
                return EvdevReadResult::kRead;
            }
        }
    }

    /** A node closed.
        @param handle  the node */
    void Close(int handle) override
    {
        close(handle);
    }

  private:
    int watch_; ///< inotify's descriptor watching /dev/input; -1 where there is no watch
};
} // namespace

std::unique_ptr<EvdevPort> MakeSystemEvdevPort()
{
    return std::make_unique<SystemEvdevPort>();
}
} // namespace reckless::input::evdev
