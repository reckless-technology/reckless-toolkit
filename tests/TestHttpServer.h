#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// <winsock2.h> brings <windows.h>, which defines DrawText as DrawTextA or DrawTextW, and a suite that includes this
// goes on to the suite's drawing, whose canvas's DrawText the macro would rename there alone (NamModel.h's reason too).
#undef DrawText
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/**
    Test-only: HTTP on the loopback interface for the network's suites - a server that answers each request with what a
    handler makes of it and keeps what it was sent, so a platform's transport is held to a real exchange with nothing
    leaving the machine, and a one-shot GET a browser stands in for. Plain HTTP/1.1, one request a connection.
*/
namespace reckless::test
{
#if defined(_WIN32)
/** A socket. */
using TestSocket = SOCKET;
/** No socket. */
inline constexpr TestSocket kNoTestSocket = INVALID_SOCKET;

/** Winsock started once. */
inline void StartTestSockets()
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}

/** @param s  the socket to close */
inline void CloseTestSocket(TestSocket s)
{
    closesocket(s);
}

/** @return 1 readable, 0 timed out, -1 failed
    @param s   the socket
    @param ms  the most to wait */
inline int TestReadable(TestSocket s, int ms)
{
    WSAPOLLFD fd{};
    fd.fd     = s;
    fd.events = POLLRDNORM;
    return WSAPoll(&fd, 1, ms);
}
#else
/** A socket. */
using TestSocket = int;
/** No socket. */
inline constexpr TestSocket kNoTestSocket = -1;

/** Nothing to start on POSIX. */
inline void StartTestSockets()
{
}

/** @param s  the socket to close */
inline void CloseTestSocket(TestSocket s)
{
    close(s);
}

/** @return 1 readable, 0 timed out, -1 failed
    @param s   the socket
    @param ms  the most to wait */
inline int TestReadable(TestSocket s, int ms)
{
    pollfd fd{};
    fd.fd     = s;
    fd.events = POLLIN;
    return poll(&fd, 1, ms);
}
#endif

/** Every byte of `data` sent on `s`.
    @param s     the socket
    @param data  the bytes */
inline void TestSendAll(TestSocket s, std::string_view data)
{
    while (!data.empty())
    {
        const auto sent = send(s, data.data(), static_cast<int>(data.size()), 0);
        if (sent <= 0)
        {
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

/** A socket connected to 127.0.0.1:`port`, or kNoTestSocket.
    @param port  the port
    @return      the socket */
inline TestSocket ConnectLoopback(int port)
{
    StartTestSockets();
    TestSocket  s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
    {
        CloseTestSocket(s);
        return kNoTestSocket;
    }
    return s;
}

/** A GET of `target` from 127.0.0.1:`port`, as a browser sends one, and the whole answer read until the server closes.
    @param port    the port
    @param target  the path and query
    @return        the raw answer, its status line, headers and body; empty when nothing connected */
inline std::string HttpGet(int port, const std::string &target)
{
    const TestSocket s = ConnectLoopback(port);
    if (s == kNoTestSocket)
    {
        return {};
    }
    TestSendAll(s, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: test\r\n\r\n");
    std::string answer;
    char        buffer[4096];
    for (;;)
    {
        if (TestReadable(s, 5000) <= 0)
        {
            break;
        }
        const auto got = recv(s, buffer, sizeof(buffer), 0);
        if (got <= 0)
        {
            break;
        }
        answer.append(buffer, static_cast<std::size_t>(got));
    }
    CloseTestSocket(s);
    return answer;
}

/** What the server was sent: the request line's parts, the headers and the body. */
struct SeenRequest
{
    std::string                                      method;  ///< GET, POST
    std::string                                      target;  ///< the path and query
    std::vector<std::pair<std::string, std::string>> headers; ///< as sent, names as written
    std::string                                      body;    ///< as many bytes as Content-Length said

    /** The header `name`'s value, matched ignoring case, or empty.
        @param name  the header's name
        @return      its value */
    std::string Header(std::string_view name) const
    {
        for (const auto &[key, value] : headers)
        {
            if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                    return std::tolower(a) == std::tolower(b);
                }))
            {
                return value;
            }
        }
        return {};
    }
};

/** A whole HTTP/1.1 response with a body, closing the connection.
    @param status   "200 OK"
    @param body     the body
    @param headers  extra header lines, each ending "\r\n"
    @return         the response's bytes */
inline std::string HttpAnswer(std::string_view status, std::string_view body, std::string_view headers = "")
{
    return "HTTP/1.1 " + std::string(status) + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + std::string(headers) + "\r\n" + std::string(body);
}

/** A server on 127.0.0.1 on a port the system picks, answering on a thread of its own, one connection at a time. */
class TestHttpServer
{
  public:
    /** What answers a request: its raw response bytes. The server's stop flag is handed in for a handler that waits. */
    using Handler = std::function<std::string(const SeenRequest &request, const std::atomic<bool> &stopping)>;

    /** @param handler  what answers each request */
    explicit TestHttpServer(Handler handler) : handler_(std::move(handler))
    {
        StartTestSockets();
        listening_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listening_, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        listen(listening_, 8);
        socklen_t length = sizeof(address);
        getsockname(listening_, reinterpret_cast<sockaddr *>(&address), &length);
        port_   = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }

    /** The thread stopped and joined, the socket closed. */
    ~TestHttpServer()
    {
        stopping_.store(true);
        thread_.join();
        CloseTestSocket(listening_);
    }

    /** @return the port it listens on */
    int port() const noexcept
    {
        return port_;
    }

    /** @param target  the path and query
        @return        "http://127.0.0.1:<port><target>" */
    std::string Url(const std::string &target) const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + target;
    }

    /** @return every request answered so far, in order */
    std::vector<SeenRequest> seen() const
    {
        const std::lock_guard<std::mutex> lock(lock_);
        return seen_;
    }

  private:
    /** The accept loop, until the server stops. */
    void Serve()
    {
        while (!stopping_.load())
        {
            if (TestReadable(listening_, 50) <= 0)
            {
                continue;
            }
            const TestSocket connection = accept(listening_, nullptr, nullptr);
            if (connection == kNoTestSocket)
            {
                continue;
            }
            SeenRequest request;
            if (Read(connection, request))
            {
                {
                    const std::lock_guard<std::mutex> lock(lock_);
                    seen_.push_back(request);
                }
                TestSendAll(connection, handler_(request, stopping_));
            }
            CloseTestSocket(connection);
        }
    }

    /** One request read: its line, its headers and as much body as Content-Length says.
        @param connection  the socket
        @param request     where it goes
        @return            true when a whole request came */
    bool Read(TestSocket connection, SeenRequest &request)
    {
        std::string raw;
        char        buffer[4096];
        std::size_t head_end = std::string::npos;
        std::size_t wanted   = 0;
        for (;;)
        {
            if (head_end == std::string::npos)
            {
                head_end = raw.find("\r\n\r\n");
                if (head_end != std::string::npos)
                {
                    std::size_t       line_end = raw.find("\r\n");
                    const std::string line     = raw.substr(0, line_end);
                    const std::size_t sp1      = line.find(' ');
                    const std::size_t sp2      = line.find(' ', sp1 + 1);
                    request.method             = line.substr(0, sp1);
                    request.target             = line.substr(sp1 + 1, sp2 - sp1 - 1);
                    std::size_t at             = line_end + 2;
                    while (at < head_end)
                    {
                        const std::size_t end   = raw.find("\r\n", at);
                        const std::string h     = raw.substr(at, end - at);
                        const std::size_t colon = h.find(':');
                        std::string       value = h.substr(colon + 1);
                        while (!value.empty() && value.front() == ' ')
                        {
                            value.erase(value.begin());
                        }
                        request.headers.emplace_back(h.substr(0, colon), value);
                        at = end + 2;
                    }
                    const std::string length = request.Header("Content-Length");
                    wanted                   = length.empty() ? 0 : std::stoul(length);
                }
            }
            if (head_end != std::string::npos && raw.size() >= head_end + 4 + wanted)
            {
                request.body = raw.substr(head_end + 4, wanted);
                return true;
            }
            if (TestReadable(connection, 3000) <= 0)
            {
                return false;
            }
            const auto got = recv(connection, buffer, sizeof(buffer), 0);
            if (got <= 0)
            {
                return false;
            }
            raw.append(buffer, static_cast<std::size_t>(got));
        }
    }

    Handler                  handler_;                  ///< what answers
    TestSocket               listening_{kNoTestSocket}; ///< the listening socket
    int                      port_{0};                  ///< its port
    std::atomic<bool>        stopping_{false};          ///< the thread is to stop
    mutable std::mutex       lock_;                     ///< guards seen_
    std::vector<SeenRequest> seen_;                     ///< every request answered
    std::thread              thread_;                   ///< the accept loop
};
} // namespace reckless::test
