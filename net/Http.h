#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Url.h"

namespace reckless::net
{
/** One header of a request or a response: its name as written and its value. */
struct Header
{
    std::string name;  ///< the header's name; compared ignoring case
    std::string value; ///< its value, trimmed of the space round it
};

/** Whether two header names are one, ignoring ASCII case (RFC 9110: field names are case-insensitive).
    @param a  a header name
    @param b  another
    @return   true when they name the same header */
inline bool SameHeaderName(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

/** An HTTP request: what a Transport sends. */
struct Request
{
    std::string         method{"GET"};         ///< GET or POST
    std::string         url;                   ///< the absolute URL, http or https
    std::vector<Header> headers;               ///< sent as given, beside what the platform adds itself
    std::string         body;                  ///< the body a POST carries; empty for none
    double              timeout_seconds{30.0}; ///< the most the whole exchange may take

    /** The header `name`'s value, or nullptr.
        @param name  the header's name, any case
        @return      its value, or nullptr when the request carries none */
    const std::string *Find(std::string_view name) const noexcept
    {
        for (const auto &header : headers)
        {
            if (SameHeaderName(header.name, name))
            {
                return &header.value;
            }
        }
        return nullptr;
    }

    /** Every header of `name` taken out.
        @param name  the header's name, any case */
    void Remove(std::string_view name)
    {
        headers.erase(std::remove_if(headers.begin(), headers.end(),
                                     [&](const Header &header) { return SameHeaderName(header.name, name); }),
                      headers.end());
    }
};

/** What came back: the status and the headers and body of the response, or why there was none. */
struct Response
{
    int                 status{0}; ///< the HTTP status; 0 when no response came (`error` says why)
    std::vector<Header> headers;   ///< the response's headers, in order
    std::string         body;      ///< the response's body, its bytes as sent
    std::string         error;     ///< why no response came - a connection, a timeout, a cancellation; else empty
    bool                cancelled{false}; ///< the request was cancelled before a response came

    /** Whether a response came and its status is a success.
        @return  true for a 2xx status with no transport error */
    bool ok() const noexcept
    {
        return error.empty() && status >= 200 && status < 300;
    }

    /** The header `name`'s value, or nullptr.
        @param name  the header's name, any case
        @return      its first value, or nullptr when the response carries none */
    const std::string *Find(std::string_view name) const noexcept
    {
        for (const auto &header : headers)
        {
            if (SameHeaderName(header.name, name))
            {
                return &header.value;
            }
        }
        return nullptr;
    }
};

/**
    A caller's word that what it asked for is no longer wanted: a flag a transport reads between its steps, and the
    one hook a transport may hang on it while a request is in flight - the platform's own abort (a task cancelled, a
    handle closed) - which Cancel runs at once, so a request blocked in the platform's call returns without waiting for
    its timeout. Shared between the thread that waits and the one that cancels; every member is safe from either.
*/
class Cancellation
{
  public:
    /** The hook a transport hangs on the cancellation while its request is in flight, taken off as the guard goes:
        Cancel runs it at most once, and never after the guard has gone. */
    class Watch
    {
      public:
        /** @param cancellation  the cancellation to hang on, outliving the guard
            @param abort         what stops the request in flight; run under the cancellation's lock, so it must not
                                 wait on anything that takes it */
        Watch(Cancellation &cancellation, std::function<void()> abort) : cancellation_(cancellation)
        {
            const std::lock_guard<std::mutex> lock(cancellation_.lock_);
            if (cancellation_.cancelled_.load())
            {
                abort();
                return;
            }
            cancellation_.abort_ = std::move(abort);
        }

        /** The hook taken off: from here Cancel runs nothing for this request. */
        ~Watch()
        {
            const std::lock_guard<std::mutex> lock(cancellation_.lock_);
            cancellation_.abort_ = nullptr;
        }

        Watch(const Watch &)            = delete;
        Watch &operator=(const Watch &) = delete;

      private:
        Cancellation &cancellation_; ///< the cancellation the hook hangs on
    };

    /** The request is no longer wanted: the flag set, and the hook in flight, if any, run once. */
    void Cancel()
    {
        const std::lock_guard<std::mutex> lock(lock_);
        cancelled_.store(true);
        if (abort_)
        {
            auto abort = std::move(abort_);
            abort_     = nullptr;
            abort();
        }
    }

    /** Whether Cancel has run.
        @return  true once cancelled */
    bool cancelled() const noexcept
    {
        return cancelled_.load();
    }

  private:
    mutable std::mutex    lock_;             ///< guards abort_ against a Watch coming or going
    std::atomic<bool>     cancelled_{false}; ///< Cancel has run
    std::function<void()> abort_;            ///< the request in flight's abort; empty for none
};

/**
    The one exchange the platform's HTTP stack makes: a request sent and its response read whole, on the calling thread,
    with no redirect followed - a 3xx comes back as itself, so Fetch follows it by the suite's rule wherever the
    request came from. Blocking: never called on an audio thread or a message thread that must stay live. A transport
    may be used from several threads at once.
*/
class Transport
{
  public:
    virtual ~Transport() = default;

    /** `request` sent, its response read whole.
        @param request       what to send
        @param cancellation  the caller's word that it is no longer wanted; checked between steps and hooked on the
                             platform's call while one is in flight
        @return              the response, or status 0 and the reason; a cancelled request says so */
    virtual Response Send(const Request &request, Cancellation &cancellation) = 0;
};

/** Whether a status asks the client to look elsewhere: 301, 302, 303, 307 and 308.
    @param status  the HTTP status
    @return        true for a redirect that carries a Location */
inline bool IsRedirect(int status) noexcept
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/** `request` sent and every redirect followed, at most `max_redirects` of them, by the rule browsers keep (RFC 9110
    15.4): the Location resolved against the URL that gave it; a 303, and a 301 or a 302 answering a POST, asked again
    as a GET with no body; and the credentials - Authorization and Cookie - dropped the moment the redirect leaves the
    origin they were given for, so a token meant for an API never reaches the storage it sends a download to.
    @param transport      the platform's exchange
    @param request        what to send first
    @param cancellation   the caller's word that it is no longer wanted
    @param max_redirects  how many redirects to follow before giving up
    @return               the last response, or status 0 and the reason */
inline Response Fetch(Transport &transport, Request request, Cancellation &cancellation, int max_redirects = 5)
{
    for (int hop = 0;; ++hop)
    {
        Response response = transport.Send(request, cancellation);
        if (!response.error.empty() || !IsRedirect(response.status))
        {
            return response;
        }
        const std::string *location = response.Find("Location");
        if (location == nullptr || location->empty())
        {
            return response;
        }
        if (hop >= max_redirects)
        {
            Response stopped;
            stopped.error = "too many redirects from " + request.url;
            return stopped;
        }
        const std::string next = ResolveReference(request.url, *location);
        const auto        from = Url::Parse(request.url);
        const auto        to   = Url::Parse(next);
        if (!to.has_value() || (to->scheme != "https" && to->scheme != "http"))
        {
            Response refused;
            refused.error = "a redirect to " + next + ", which is not an http or https URL";
            return refused;
        }
        if (!from.has_value() || from->Origin() != to->Origin())
        {
            request.Remove("Authorization");
            request.Remove("Cookie");
        }
        if (response.status == 303 || ((response.status == 301 || response.status == 302) && request.method == "POST"))
        {
            request.method = "GET";
            request.body.clear();
            request.Remove("Content-Type");
        }
        request.url = next;
    }
}

/** The platform's own HTTP stack as a Transport - NSURLSession on macOS, WinHTTP on Windows, libcurl loaded by name on
    Linux - each speaking TLS through the operating system's certificates and proxies, so the suite ships no TLS of its
    own. Defined per platform (net/mac, win, linux).
    @return  the transport; on Linux one whose every request fails with the reason when no libcurl is installed */
std::unique_ptr<Transport> MakePlatformTransport();
} // namespace reckless::net
