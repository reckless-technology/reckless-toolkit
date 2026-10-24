#include "LoopbackListener.h"

#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace reckless::net
{
namespace
{
#if defined(_WIN32)
using SocketHandle               = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;

/** Winsock started once for the process, as every Winsock call needs, and left running: the DLL counts its users. */
void StartSockets()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

/** A socket closed.
    @param socket  the socket */
void CloseSocket(SocketHandle socket)
{
    closesocket(socket);
}

/** The last socket error, in words.
    @return  the error's number */
std::string LastError()
{
    return "socket error " + std::to_string(WSAGetLastError());
}

/** Waits for `socket` to be readable.
    @param socket  the socket
    @param ms      the most to wait, in milliseconds
    @return        1 readable, 0 timed out, -1 failed */
int WaitReadable(SocketHandle socket, int ms)
{
    WSAPOLLFD fd{};
    fd.fd     = socket;
    fd.events = POLLRDNORM;
    return WSAPoll(&fd, 1, ms);
}

/** Sends every byte, as far as the socket takes them.
    @param socket  the socket
    @param data    the bytes */
void SendAll(SocketHandle socket, std::string_view data)
{
    while (!data.empty())
    {
        const int sent = send(socket, data.data(), static_cast<int>(data.size()), 0);
        if (sent <= 0)
        {
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

/** Reads what has arrived.
    @param socket  the socket
    @param buffer  where to read
    @param size    its size
    @return        the bytes read, 0 at the end, negative on failure */
long ReadSome(SocketHandle socket, char *buffer, std::size_t size)
{
    return recv(socket, buffer, static_cast<int>(size), 0);
}
#else
using SocketHandle               = int; ///< a socket
constexpr SocketHandle kNoSocket = -1;  ///< no socket

/** Nothing to start on POSIX. */
void StartSockets()
{
}

/** A socket closed.
    @param socket  the socket */
void CloseSocket(SocketHandle socket)
{
    close(socket);
}

/** The last socket error, in words.
    @return  the system's words for errno */
std::string LastError()
{
    return std::strerror(errno);
}

/** Waits for `socket` to be readable.
    @param socket  the socket
    @param ms      the most to wait, in milliseconds
    @return        1 readable, 0 timed out, -1 failed */
int WaitReadable(SocketHandle socket, int ms)
{
    pollfd fd{};
    fd.fd           = socket;
    fd.events       = POLLIN;
    const int ready = poll(&fd, 1, ms);
    return ready < 0 && errno == EINTR ? 0 : ready;
}

/** Sends every byte, as far as the socket takes them, with no SIGPIPE for a browser that has gone.
    @param socket  the socket
    @param data    the bytes */
void SendAll(SocketHandle socket, std::string_view data)
{
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    while (!data.empty())
    {
        const ssize_t sent = send(socket, data.data(), data.size(), kFlags);
        if (sent <= 0)
        {
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

/** Reads what has arrived.
    @param socket  the socket
    @param buffer  where to read
    @param size    its size
    @return        the bytes read, 0 at the end, negative on failure */
long ReadSome(SocketHandle socket, char *buffer, std::size_t size)
{
    return static_cast<long>(recv(socket, buffer, size, 0));
}
#endif

/** The most of a request the listener reads: a browser's GET with its headers is a few hundred bytes. */
constexpr std::size_t kMaxRequestBytes = 16384;

/** How long a connection may take to send its request line and headers, in milliseconds. */
constexpr int kRequestMs = 3000;

/** A response with `status` and `body`, which closes the connection.
    @param status  "200 OK" or "404 Not Found"
    @param body    the HTML
    @return        the response's bytes */
std::string HttpAnswer(std::string_view status, std::string_view body)
{
    return "HTTP/1.1 " + std::string(status) +
           "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

/** The target of a request's first line, "GET /path?query HTTP/1.1", or empty when it is not a GET.
    @param request  the request's bytes up to its blank line
    @return         the target, or an empty string */
std::string TargetOf(std::string_view request)
{
    const std::size_t line_end = request.find("\r\n");
    std::string_view  line     = request.substr(0, line_end);
    if (line.substr(0, 4) != "GET ")
    {
        return {};
    }
    line.remove_prefix(4);
    const std::size_t space = line.find(' ');
    return std::string(line.substr(0, space));
}
} // namespace

LoopbackListener::LoopbackListener()
{
    StartSockets();
    SocketHandle handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kNoSocket)
    {
        error_ = "no socket: " + LastError();
        return;
    }
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(handle, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || listen(handle, 4) != 0)
    {
        error_ = "the loopback port would not open: " + LastError();
        CloseSocket(handle);
        return;
    }
    socklen_t length = sizeof(address);
    if (getsockname(handle, reinterpret_cast<sockaddr *>(&address), &length) != 0)
    {
        error_ = "the loopback port is unknown: " + LastError();
        CloseSocket(handle);
        return;
    }
#if !defined(_WIN32)
    fcntl(handle, F_SETFD, FD_CLOEXEC); // a child the host starts does not inherit the port
#endif
    port_   = ntohs(address.sin_port);
    socket_ = static_cast<long long>(handle);
}

LoopbackListener::~LoopbackListener()
{
    if (socket_ >= 0)
    {
        CloseSocket(static_cast<SocketHandle>(socket_));
    }
}

std::optional<std::string> LoopbackListener::Await(std::string_view path, std::string_view page,
                                                   Cancellation &cancellation, double timeout)
{
    if (socket_ < 0)
    {
        return std::nullopt;
    }
    const auto listening = static_cast<SocketHandle>(socket_);
    const auto deadline  = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (!cancellation.cancelled() && std::chrono::steady_clock::now() < deadline)
    {
        const int ready = WaitReadable(listening, 100);
        if (ready < 0)
        {
            return std::nullopt;
        }
        if (ready == 0)
        {
            continue;
        }
        SocketHandle connection = accept(listening, nullptr, nullptr);
        if (connection == kNoSocket)
        {
            continue;
        }
        // The request up to its blank line, within a few seconds: a browser sends it at once.
        std::string request;
        char        buffer[2048];
        const auto  request_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRequestMs);
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < kMaxRequestBytes &&
               std::chrono::steady_clock::now() < request_deadline && !cancellation.cancelled())
        {
            if (WaitReadable(connection, 100) <= 0)
            {
                continue;
            }
            const long got = ReadSome(connection, buffer, sizeof(buffer));
            if (got <= 0)
            {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(got));
        }
        const std::string target = TargetOf(request);
        const std::string asked  = target.substr(0, target.find('?'));
        if (!target.empty() && asked == path)
        {
            SendAll(connection, HttpAnswer("200 OK", page));
            CloseSocket(connection);
            return target;
        }
        SendAll(connection, HttpAnswer("404 Not Found", "<!doctype html><title>Not found</title>"));
        CloseSocket(connection);
    }
    return std::nullopt;
}
} // namespace reckless::net
