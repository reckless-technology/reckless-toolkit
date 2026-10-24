#pragma once
// The X11 clipboard, written and read, for the child window (window/linux/WindowLinux.cpp) and the app's platform
// (app/linux/AppLinux.cpp), each including it last, after the headers Xlib's macros would reach into.
//
// X keeps no clipboard of its own: a copy makes a window of the copying process the owner of the CLIPBOARD selection,
// and a paste asks the owner for it, the owner answering each request (SelectionRequest) by writing the text into a
// property of the asker's window and telling it so (SelectionNotify) - so the text lives only as long as the owner's
// connection does, and only while its loop answers. Another copy, anywhere, takes the selection, and the old owner is
// told it has lost it (SelectionClear). A ClipboardOwner is that owner on one connection, its events handed it by the
// loop that reads the connection; what this process owns is noted in one place (OwnedClipboard), so a paste in the
// process reads its own copy there rather than asking an owner whose loop is the very thread waiting for the answer.
#include <poll.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "Text.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace reckless::window::x11
{
/** What this process holds on the CLIPBOARD selection: the owner window and its text, shared by every owner in the
    process (a module's, where the code is compiled into a plug-in module). Read by a paste from any thread. */
struct OwnedClipboard
{
    std::mutex  mutex;     ///< guards the two below
    ::Window    window{0}; ///< the window that owns the selection; 0 when this process owns none
    std::string text;      ///< the text it holds, UTF-8
};

/** The process's record of what it owns on the CLIPBOARD selection.
    @return  the record, made on first use */
inline OwnedClipboard &Owned()
{
    static OwnedClipboard owned;
    return owned;
}

/** The owner of the CLIPBOARD selection on one connection: an unmapped window of its own that takes the selection
    for a copy and answers every request for it - its targets (TARGETS) and its text as UTF-8 (UTF8_STRING, or the
    MIME type text/plain;charset=utf-8) - until another copy takes it away. The connection's loop hands it every event
    (Handle). Text longer than one request to the server carries is refused rather than sent in pieces (X's INCR
    transfer is not done): a megabyte at the least on a server with big requests, as every current one has. Main
    thread, the connection's. */
class ClipboardOwner
{
  public:
    /** An owner on `display`, holding nothing yet.
        @param display  the connection, which outlives the owner */
    explicit ClipboardOwner(Display *display)
        : display_(display), clipboard_(XInternAtom(display, "CLIPBOARD", False)),
          targets_(XInternAtom(display, "TARGETS", False)), utf8_(XInternAtom(display, "UTF8_STRING", False)),
          mime_(XInternAtom(display, "text/plain;charset=utf-8", False))
    {
        window_ = XCreateSimpleWindow(display_, DefaultRootWindow(display_), 0, 0, 1, 1, 0, 0, 0);
    }

    /** The selection let go if it is still held, the process's record of it cleared, and the window destroyed. */
    ~ClipboardOwner()
    {
        if (window_ != 0)
        {
            if (owns_ && XGetSelectionOwner(display_, clipboard_) == window_)
            {
                XSetSelectionOwner(display_, clipboard_, None, CurrentTime);
            }
            Forget();
            XDestroyWindow(display_, window_);
            XFlush(display_);
        }
    }

    /** Not copyable: the server knows the owner by its window. */
    ClipboardOwner(const ClipboardOwner &) = delete;

    /** Not assignable: the server knows the owner by its window.
        @return  never */
    ClipboardOwner &operator=(const ClipboardOwner &) = delete;

    /** The selection taken, holding `text`: what every paste reads until another copy takes it.
        @param text  the text, UTF-8
        @return      true when the server made this window the owner */
    bool Take(const std::string &text)
    {
        if (window_ == 0)
        {
            return false;
        }
        XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
        owns_ = XGetSelectionOwner(display_, clipboard_) == window_;
        XFlush(display_);
        if (!owns_)
        {
            Forget();
            text_.clear();
            return false;
        }
        text_ = gfx::IsValidUtf8(text) ? text : gfx::RepairedUtf8(text); // UTF8_STRING's promise kept
        OwnedClipboard                   &owned = Owned();
        const std::lock_guard<std::mutex> lock(owned.mutex);
        owned.window = window_;
        owned.text   = text_;
        return true;
    }

    /** An event of the connection's, answered where it is this owner's: a request for the selection (its targets or
        its text, written into the asker's property, or refused), or word that another copy has taken it.
        @param event  an event the connection's loop took
        @return       true when the event was this owner's, and is done with */
    bool Handle(const XEvent &event)
    {
        if (window_ == 0)
        {
            return false;
        }
        if (event.type == SelectionRequest && event.xselectionrequest.owner == window_)
        {
            Answer(event.xselectionrequest);
            return true;
        }
        if (event.type == SelectionClear && event.xselectionclear.window == window_)
        {
            if (event.xselectionclear.selection == clipboard_)
            {
                owns_ = false; // another copy, anywhere, took it
                text_.clear();
                Forget();
            }
            return true;
        }
        return false;
    }

    /** Whether this owner holds the selection, as far as it has been told.
        @return  true from a Take that the server granted until another copy takes it */
    bool owns() const noexcept
    {
        return owns_;
    }

    /** The owner's window, which the server names as the selection's owner while it holds it.
        @return  the window; 0 when the server made none */
    ::Window window() const noexcept
    {
        return window_;
    }

  private:
    /** The process's record cleared, where it names this owner. */
    void Forget()
    {
        OwnedClipboard                   &owned = Owned();
        const std::lock_guard<std::mutex> lock(owned.mutex);
        if (owned.window == window_)
        {
            owned.window = 0;
            owned.text.clear();
        }
    }

    /** A request for the selection answered: the targets or the text written into the asker's property, or the
        property None for a target not offered, a selection not held or text too long for one request; then the asker
        told (SelectionNotify).
        @param request  the request */
    void Answer(const XSelectionRequestEvent &request)
    {
        XSelectionEvent reply{};
        reply.type      = SelectionNotify;
        reply.display   = request.display;
        reply.requestor = request.requestor;
        reply.selection = request.selection;
        reply.target    = request.target;
        reply.time      = request.time;
        reply.property  = None;
        // An asker from before ICCCM names no property: the target is the property then.
        const Atom property = request.property != None ? request.property : request.target;
        if (owns_ && request.selection == clipboard_)
        {
            if (request.target == targets_)
            {
                const Atom offered[] = {targets_, utf8_, mime_};
                XChangeProperty(display_, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(offered), 3);
                reply.property = property;
            }
            else if ((request.target == utf8_ || request.target == mime_) && Fits())
            {
                XChangeProperty(display_, request.requestor, property, request.target, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(text_.data()), static_cast<int>(text_.size()));
                reply.property = property;
            }
        }
        XSendEvent(display_, request.requestor, False, NoEventMask, reinterpret_cast<XEvent *>(&reply));
        XFlush(display_);
    }

    /** Whether the text goes in one request to the server: the most a request carries, less its header.
        @return  true when it fits */
    bool Fits() const
    {
        const long units = std::max(XExtendedMaxRequestSize(display_), XMaxRequestSize(display_)); // of four bytes
        return static_cast<long>(text_.size()) <= units * 4 - 1024;
    }

    Display    *display_;     ///< the connection, which outlives the owner
    Atom        clipboard_;   ///< CLIPBOARD
    Atom        targets_;     ///< TARGETS
    Atom        utf8_;        ///< UTF8_STRING
    Atom        mime_;        ///< text/plain;charset=utf-8
    ::Window    window_{0};   ///< the unmapped window that owns the selection; 0 when the server made none
    bool        owns_{false}; ///< this owner holds the selection, as far as it has been told
    std::string text_;        ///< the text held, UTF-8
};

/** The CLIPBOARD selection as UTF-8 text, read on a display connection of its own so the caller's connection and its
    run loop never see the exchange: this process's own copy where a window of it owns the selection (OwnedClipboard),
    else the selection asked for as UTF8_STRING into a property of an unmapped window, the owner's answer waited for at
    most half a second, the property read whole and deleted. A paste is a person's single act, so a short wait on the
    owner is the X11 way; an owner that never answers gives an empty paste.
    @param ask_own  true to ask the owner even where it is this process's - for a suite that holds an owner's answers,
                    turning the owner's loop on another thread
    @return         the text, or empty when there is no owner, no text or no answer */
inline std::string ReadClipboardText(bool ask_own = false)
{
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        return {};
    }
    std::string    text;
    const Atom     clipboard = XInternAtom(display, "CLIPBOARD", False);
    const Atom     utf8      = XInternAtom(display, "UTF8_STRING", False);
    const Atom     property  = XInternAtom(display, "RECKLESS_PASTE", False);
    const ::Window window    = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    if (const ::Window owner = XGetSelectionOwner(display, clipboard); owner != None)
    {
        OwnedClipboard &owned = Owned();
        bool            own   = false;
        {
            const std::lock_guard<std::mutex> lock(owned.mutex);
            if (!ask_own && owned.window != 0 && owner == owned.window)
            {
                own  = true;
                text = owned.text;
            }
        }
        if (!own)
        {
            XConvertSelection(display, clipboard, utf8, property, window, CurrentTime);
            XFlush(display);
            const int fd = ConnectionNumber(display);
            for (int waited_ms = 0; waited_ms < 500;)
            {
                XEvent event;
                if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event))
                {
                    if (event.xselection.property != None)
                    {
                        Atom           type   = None;
                        int            format = 0;
                        unsigned long  items = 0, after = 0;
                        unsigned char *data = nullptr;
                        if (XGetWindowProperty(display, window, property, 0, 1 << 20, True, AnyPropertyType, &type,
                                               &format, &items, &after, &data) == Success &&
                            data != nullptr && format == 8)
                        {
                            text.assign(reinterpret_cast<const char *>(data), items);
                        }
                        if (data != nullptr)
                        {
                            XFree(data);
                        }
                    }
                    break;
                }
                pollfd wait{fd, POLLIN, 0};
                poll(&wait, 1, 20);
                waited_ms += 20;
            }
        }
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return text;
}
} // namespace reckless::window::x11
