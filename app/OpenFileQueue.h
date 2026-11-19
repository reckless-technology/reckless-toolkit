#pragma once

#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace reckless::app
{
/**
    The files the system hands a program - a double-clicked document on macOS arrives as an open-file event, not a
    command-line argument - held until the product that wants them can take them, and then given to it in the order
    they came. No AppKit in it: the macOS platform feeds it from `application:openFiles:` and
    `application:openURLs:`, and a suite feeds it by hand. Main thread only.

    Paths that come while no handler is set are held, and given at once, in their batches, when one is. After that each
    batch goes straight to the handler. Taking the handler away (an empty one) holds paths again.
*/
class OpenFileQueue
{
  public:
    /** What takes the files: told each batch of paths (UTF-8), once, on the main thread. */
    using Handler = std::function<void(const std::vector<std::string> &paths)>;

    /** A batch of files the system opened: given to the handler, or held until there is one. An empty batch is nothing.
        @param paths  the paths, UTF-8, in the order the system gave them */
    void Receive(std::vector<std::string> paths)
    {
        if (paths.empty())
        {
            return;
        }
        held_.push_back(std::move(paths));
        Deliver();
    }

    /** The handler that takes the files, and so from here on every batch; the batches held until now are given to it
        first, in the order they came. An empty handler takes none and files are held again.
        @param handler  what takes each batch; empty for none */
    void SetHandler(Handler handler)
    {
        handler_ = std::move(handler);
        Deliver();
    }

    /** Whether a handler is set.
        @return  true when files are given as they come */
    bool HasHandler() const noexcept
    {
        return static_cast<bool>(handler_);
    }

    /** The batches held for want of a handler.
        @return  the number of batches waiting */
    std::size_t HeldBatches() const noexcept
    {
        return held_.size();
    }

  private:
    /** The held batches to the handler, oldest first, until none is left or the handler is taken away. A batch that
        arrives while the handler runs is delivered after it, never inside it. */
    void Deliver()
    {
        if (is_delivering_)
        {
            return;
        }
        is_delivering_ = true;
        while (handler_ && !held_.empty())
        {
            const std::vector<std::string> batch = std::move(held_.front());
            held_.pop_front();
            const Handler handler = handler_; // a copy: the handler may set another or clear itself
            handler(batch);
        }
        is_delivering_ = false;
    }

    Handler                              handler_;              ///< what takes each batch; empty for none
    std::deque<std::vector<std::string>> held_;                 ///< the batches that came with no handler, in order
    bool                                 is_delivering_{false}; ///< true while Deliver runs, which does not nest
};
} // namespace reckless::app
