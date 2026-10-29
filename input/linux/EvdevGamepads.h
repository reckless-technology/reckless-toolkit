#pragma once

// The Linux back end: every controller among /dev/input's event nodes, read without blocking, found as it is plugged
// in and let go of as it is pulled out. What touches the kernel is behind EvdevPort - listing the nodes, opening one,
// reading it - so that everything else, which is the back end's every decision, is here with none of the kernel's
// headers and held by the suites on a port of their own on every platform. The port on the kernel is
// input/linux/EvdevSystemPort.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "EvdevLayout.h"

namespace reckless::input::evdev
{
/** A node's identity on the file system: its inode, and when its inode last changed - which a change of its
    permissions or its access list moves, as udev's granting the seat's user access to a controller does. */
struct EvdevNodeStamp
{
    std::uint64_t inode{0};                   ///< the node's inode
    std::int64_t  change_time_nanoseconds{0}; ///< when its inode last changed

    /** Whether two stamps are the same.
        @param other  the other stamp
        @return       true when both members are equal */
    constexpr bool operator==(const EvdevNodeStamp &other) const = default;
};

/** What a read of a node gave. */
enum class EvdevReadResult : std::uint8_t
{
    kRead, ///< the events waiting, possibly none
    kGone, ///< the device has gone: it was pulled out, or the node can no longer be read
};

/**
    What the back end needs of the kernel, and nothing else. The one on the kernel is MakeSystemEvdevPort's; a suite
    gives its own.
*/
class EvdevPort
{
  public:
    /** Closes whatever it holds. */
    virtual ~EvdevPort() = default;

    /** Whether it is told when /dev/input changes (inotify); where not, the back end looks again once a second.
        @return  true when TakeNodesChanged is worth asking */
    virtual bool CanWatchNodes() const = 0;

    /** Whether /dev/input has changed - a node made, removed, or its permissions changed - since the last call.
        @return  true when it has */
    virtual bool TakeNodesChanged() = 0;

    /** A clock in milliseconds, for the look once a second where the nodes are not watched.
        @return  the time, from any fixed point */
    virtual std::int64_t NowMilliseconds() const = 0;

    /** The event nodes there are now.
        @return  each node's path ("/dev/input/event12"), in no particular order */
    virtual std::vector<std::string> ListNodes() const = 0;

    /** A node's stamp, so a node the back end has looked at and passed over is looked at again only once it changes.
        @param path  the node
        @return      its stamp; a stamp of zeros where it cannot be read */
    virtual EvdevNodeStamp StampOf(const std::string &path) const = 0;

    /** A node opened to be read without blocking, and what it says of itself.
        @param path          the node
        @param capabilities  where what it says of itself is written
        @return              a handle for the other calls; -1 where it cannot be opened or read */
    virtual int Open(const std::string &path, EvdevCapabilities &capabilities) = 0;

    /** What an open node holds now: its keys down and its axes' readings.
        @param handle    the node, as Open gave it
        @param snapshot  where they are written
        @return          true when they were read */
    virtual bool ReadSnapshot(int handle, EvdevSnapshot &snapshot) = 0;

    /** Every event waiting on an open node.
        @param handle  the node, as Open gave it
        @param events  where the events are appended
        @return        kGone where the device has gone */
    virtual EvdevReadResult Read(int handle, std::vector<EvdevEvent> &events) = 0;

    /** An open node closed.
        @param handle  the node, as Open gave it */
    virtual void Close(int handle) = 0;
};

/** The port on the kernel: /dev/input, watched with inotify (input/linux/EvdevSystemPort.cpp).
    @return  the port; never null */
std::unique_ptr<EvdevPort> MakeSystemEvdevPort();

/**
    The controllers among a port's event nodes. As it is made it looks at every node and opens the controllers, so
    Devices() lists them at once and the first Poll tells what they hold down. Each Poll reads every open node, then
    looks again at the nodes where the port says they changed (or a second has passed, where it cannot watch them).
*/
class EvdevGamepads final : public Gamepads
{
  public:
    /** How long between looks at the nodes where the port cannot watch them. */
    static constexpr std::int64_t kLookIntervalMilliseconds = 1000;

    /** Every controller the port has, opened.
        @param port        the port
        @param thresholds  where an axis's controls go down and are let go */
    EvdevGamepads(std::unique_ptr<EvdevPort> port, const GamepadAxisThresholds &thresholds)
        : port_(std::move(port)), roster_(thresholds)
    {
        Look(pending_);
    }

    /** Every node closed; no edge is told. */
    ~EvdevGamepads() override
    {
        for (const OpenNode &node : open_)
        {
            port_->Close(node.handle);
        }
    }

    EvdevGamepads(const EvdevGamepads &)            = delete; ///< not copied: it owns its nodes
    EvdevGamepads &operator=(const EvdevGamepads &) = delete; ///< not copied: it owns its nodes

    /** Every edge since the last call, and every node plugged in or pulled out found (Gamepads::Poll).
        @param edges  where the edges are appended */
    void Poll(std::vector<GamepadEdge> &edges) override
    {
        edges.insert(edges.end(), pending_.begin(), pending_.end());
        pending_.clear();
        for (std::size_t node = 0; node < open_.size();)
        {
            if (ReadNode(open_[node], edges))
            {
                ++node;
            }
            else
            {
                Close(node, edges);
            }
        }
        const bool has_changed = port_->CanWatchNodes()
                                     ? port_->TakeNodesChanged()
                                     : port_->NowMilliseconds() - last_look_ >= kLookIntervalMilliseconds;
        if (has_changed)
        {
            Look(edges);
        }
    }

    /** The devices open (Gamepads::Devices).
        @return  the devices, in the order they were opened */
    std::vector<GamepadInfo> Devices() const override
    {
        return roster_.Devices();
    }

  private:
    /** A controller's node, open. */
    struct OpenNode
    {
        std::string   path;               ///< the node's path
        int           handle{-1};         ///< the port's handle
        std::uint32_t device_id{0};       ///< the device's id in the roster
        EvdevLayout   layout;             ///< where each of its codes goes
        bool          is_dropping{false}; ///< the kernel dropped events: what follows is passed over until the frame
                                          ///< ends, and the device read again
    };

    /** A node looked at and not opened, and its stamp when it was. */
    struct PassedOver
    {
        std::string    path;  ///< the node's path
        EvdevNodeStamp stamp; ///< its stamp then
    };

    /** Every event waiting on a node applied.
        @param node   the node
        @param edges  where the edges are appended
        @return       false when the device has gone */
    bool ReadNode(OpenNode &node, std::vector<GamepadEdge> &edges)
    {
        events_.clear();
        if (port_->Read(node.handle, events_) == EvdevReadResult::kGone)
        {
            return false;
        }
        GamepadDevice &device = *roster_.Find(node.device_id);
        for (const EvdevEvent &event : events_)
        {
            if (event.type == kSynchronise && event.code == kSynchroniseDropped)
            {
                node.is_dropping = true;
            }
            else if (node.is_dropping)
            {
                if (event.type == kSynchronise && event.code == kSynchroniseReport)
                {
                    node.is_dropping = false;
                    EvdevSnapshot snapshot;
                    if (port_->ReadSnapshot(node.handle, snapshot))
                    {
                        node.layout.ApplySnapshot(snapshot, device, edges);
                    }
                }
            }
            else
            {
                node.layout.Apply(event, device, edges);
            }
        }
        return true;
    }

    /** An open node closed and its device let go of.
        @param node   its index in open_
        @param edges  where the device's ups are appended */
    void Close(std::size_t node, std::vector<GamepadEdge> &edges)
    {
        roster_.Remove(open_[node].device_id, edges);
        port_->Close(open_[node].handle);
        open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(node));
    }

    /** The nodes looked at: those gone closed, those new or changed opened where they are controllers.
        @param edges  where the edges of devices gone and found are appended */
    void Look(std::vector<GamepadEdge> &edges)
    {
        last_look_                     = port_->NowMilliseconds();
        std::vector<std::string> paths = port_->ListNodes();
        std::sort(paths.begin(), paths.end(), [](const std::string &left, const std::string &right) {
            return NodeNumber(FileName(left)) < NodeNumber(FileName(right));
        });
        for (std::size_t node = 0; node < open_.size();)
        {
            if (std::find(paths.begin(), paths.end(), open_[node].path) == paths.end())
            {
                Close(node, edges);
            }
            else
            {
                ++node;
            }
        }
        std::erase_if(passed_over_, [&paths](const PassedOver &passed) {
            return std::find(paths.begin(), paths.end(), passed.path) == paths.end();
        });
        for (const std::string &path : paths)
        {
            Consider(path, edges);
        }
    }

    /** A node opened if it is a controller not open already, and passed over otherwise until it changes.
        @param path   the node
        @param edges  where the edges of what it holds down are appended */
    void Consider(const std::string &path, std::vector<GamepadEdge> &edges)
    {
        const bool is_open =
            std::any_of(open_.begin(), open_.end(), [&path](const OpenNode &node) { return node.path == path; });
        if (is_open)
        {
            return;
        }
        const EvdevNodeStamp stamp  = port_->StampOf(path);
        const auto           passed = std::find_if(passed_over_.begin(), passed_over_.end(),
                                                   [&path](const PassedOver &over) { return over.path == path; });
        if (passed != passed_over_.end())
        {
            if (passed->stamp == stamp)
            {
                return;
            }
            passed_over_.erase(passed);
        }
        EvdevCapabilities capabilities;
        const int         handle = port_->Open(path, capabilities);
        if (handle < 0 || !IsController(capabilities))
        {
            if (handle >= 0)
            {
                port_->Close(handle);
            }
            passed_over_.push_back(PassedOver{path, stamp});
            return;
        }
        EvdevLayout    layout(capabilities);
        GamepadDevice &device = roster_.Add(layout.Layout(capabilities));
        EvdevSnapshot  snapshot;
        if (!port_->ReadSnapshot(handle, snapshot))
        {
            snapshot = EvdevLayout::ReadingsOf(capabilities);
        }
        layout.ApplySnapshot(snapshot, device, edges);
        open_.push_back(OpenNode{path, handle, device.device_id(), std::move(layout), false});
    }

    /** The file name of a path: what follows its last slash.
        @param path  the path
        @return      its file name */
    static std::string_view FileName(const std::string &path)
    {
        const std::size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string_view(path) : std::string_view(path).substr(slash + 1);
    }

    std::unique_ptr<EvdevPort> port_;         ///< the kernel, or a suite's port
    GamepadRoster              roster_;       ///< the devices open
    std::vector<OpenNode>      open_;         ///< the controllers' nodes open, in the order they were opened
    std::vector<PassedOver>    passed_over_;  ///< the nodes looked at and not opened
    std::vector<GamepadEdge>   pending_;      ///< what was found down as this was made, for the first Poll
    std::vector<EvdevEvent>    events_;       ///< the events of one read, kept so a frame's read allocates nothing
    std::int64_t               last_look_{0}; ///< when the nodes were last looked at
};
} // namespace reckless::input::evdev
