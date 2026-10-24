#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Http.h"

namespace reckless::net
{
/**
    A one-shot HTTP listener on the loopback interface, the redirect a native app's OAuth sign-in comes back to in the
    system browser (RFC 8252 section 7.3): a socket bound to 127.0.0.1 on a port the system picks, listening from
    construction, so the URL that names it can be handed to the browser before anything waits on it. Await answers the
    one request for its path with a page of the caller's and hands back the request's target - its path and the query
    the authorisation server put on it; any other request (a browser's favicon) is answered 404 and the wait goes on.
    Nothing but 127.0.0.1 is bound, so no other machine can reach it; the socket closes with the listener. Not a web
    server: one request a connection, read up to its blank line, the body ignored.
*/
class LoopbackListener
{
  public:
    /** The socket bound and listening on 127.0.0.1, on a port the system picks; ok() says whether it worked. */
    LoopbackListener();

    /** The socket closed. */
    ~LoopbackListener();

    LoopbackListener(const LoopbackListener &)            = delete;
    LoopbackListener &operator=(const LoopbackListener &) = delete;

    /** Whether the socket is listening.
        @return  true when it is; else error() says why */
    bool ok() const noexcept
    {
        return socket_ >= 0;
    }

    /** Why the socket is not listening.
        @return  the reason, or an empty string */
    const std::string &error() const noexcept
    {
        return error_;
    }

    /** The port the system gave the socket.
        @return  the port, 0 when the socket is not listening */
    int port() const noexcept
    {
        return port_;
    }

    /** The URL the browser is sent back to: `http://127.0.0.1:` and the port, then `path`.
        @param path  the path, beginning with '/'
        @return      the redirect URI */
    std::string RedirectUri(std::string_view path) const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    /** The first GET of `path` waited for, answered 200 with `page`, and its target handed back.
        @param path          the path to wait for, beginning with '/'
        @param page          the HTML the browser shows once it is back
        @param cancellation  checked every tenth of a second; a cancelled wait answers nothing
        @param timeout       the most to wait, in seconds
        @return              the request's target - its path and query - or nothing when cancelled, timed out or the
                             socket failed */
    std::optional<std::string> Await(std::string_view path, std::string_view page, Cancellation &cancellation,
                                     double timeout);

  private:
    long long   socket_{-1}; ///< the listening socket, a descriptor or a SOCKET; -1 for none
    int         port_{0};    ///< the port bound
    std::string error_;      ///< why the socket is not listening
};
} // namespace reckless::net
