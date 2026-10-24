// The platform's HTTP stack on Windows: WinHTTP, with the system's TLS (Schannel), certificates and proxy settings.

#include "Http.h"
#include "Sha256.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <winhttp.h>

#include <memory>
#include <mutex>
#include <string>

namespace reckless::net
{
namespace
{
/** UTF-8 as UTF-16, for the API's wide strings.
    @param text  UTF-8
    @return      the wide text */
std::wstring WideOf(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int    length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

/** UTF-16 as UTF-8.
    @param wide  the wide text
    @return      UTF-8 */
std::string Utf8Of(std::wstring_view wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length, nullptr, nullptr);
    return text;
}

/** The system's words for a WinHTTP error.
    @param code  GetLastError's
    @return      the words, or the number */
std::string ErrorText(DWORD code)
{
    if (code == ERROR_WINHTTP_OPERATION_CANCELLED)
    {
        return "cancelled";
    }
    if (code == ERROR_WINHTTP_TIMEOUT)
    {
        return "the server did not answer in time";
    }
    if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED)
    {
        return "the server's name did not resolve";
    }
    if (code == ERROR_WINHTTP_CANNOT_CONNECT)
    {
        return "could not connect to the server";
    }
    wchar_t    *words = nullptr;
    const DWORD got =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<LPWSTR>(&words), 0, nullptr);
    std::string text = got > 0 ? Utf8Of(std::wstring_view(words, got)) : "WinHTTP error " + std::to_string(code);
    if (words != nullptr)
    {
        LocalFree(words);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
    {
        text.pop_back();
    }
    return text;
}

/** A request handle that either its thread or a cancellation closes, once. */
struct SharedHandle
{
    std::mutex lock;            ///< guards handle
    HINTERNET  handle{nullptr}; ///< the request; null once closed

    /** Closes the handle if it is still open. */
    void Close()
    {
        const std::lock_guard<std::mutex> guard(lock);
        if (handle != nullptr)
        {
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }
};

/** WinHTTP behind the suite's transport: one session, the system's proxy settings, no redirect followed. */
class WinTransport final : public Transport
{
  public:
    /** The session, on the system's automatic proxy settings where Windows has them. */
    WinTransport()
    {
        session_ = WinHttpOpen(L"RecklessTechnology/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_ == nullptr)
        {
            // Before Windows 8.1 there is no automatic proxy: the default settings then.
            session_ = WinHttpOpen(L"RecklessTechnology/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }

    /** The session closed. */
    ~WinTransport() override
    {
        if (session_ != nullptr)
        {
            WinHttpCloseHandle(session_);
        }
    }

    Response Send(const Request &request, Cancellation &cancellation) override
    {
        Response response;
        if (session_ == nullptr)
        {
            response.error = ErrorText(GetLastError());
            return response;
        }
        const auto url = Url::Parse(request.url);
        if (!url.has_value())
        {
            response.error = "not a URL: " + request.url;
            return response;
        }
        std::string host = url->host;
        if (host.size() > 2 && host.front() == '[')
        {
            host = host.substr(1, host.size() - 2); // WinHTTP takes an IPv6 literal without its brackets
        }
        HINTERNET connection = WinHttpConnect(session_, WideOf(host).c_str(), static_cast<INTERNET_PORT>(url->port), 0);
        if (connection == nullptr)
        {
            response.error = ErrorText(GetLastError());
            return response;
        }
        auto shared    = std::make_shared<SharedHandle>();
        shared->handle = WinHttpOpenRequest(connection, WideOf(request.method).c_str(), WideOf(url->target).c_str(),
                                            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            url->scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
        if (shared->handle == nullptr)
        {
            response.error = ErrorText(GetLastError());
            WinHttpCloseHandle(connection);
            return response;
        }
        HINTERNET handle = shared->handle;
        DWORD     never  = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
        const int ms = static_cast<int>(request.timeout_seconds * 1000.0);
        WinHttpSetTimeouts(handle, ms, ms, ms, ms);

        std::wstring headers;
        for (const auto &header : request.headers)
        {
            headers += WideOf(header.name) + L": " + WideOf(header.value) + L"\r\n";
        }
        {
            const Cancellation::Watch watch(cancellation, [shared] { shared->Close(); });
            BOOL                      done = WinHttpSendRequest(
                handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : static_cast<DWORD>(-1L),
                request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(request.body.data()),
                static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
            done = done && WinHttpReceiveResponse(handle, nullptr);
            if (done)
            {
                DWORD status = 0;
                DWORD size   = sizeof(status);
                WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                response.status = static_cast<int>(status);
                ReadHeaders(handle, response);
                for (;;)
                {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(handle, &available))
                    {
                        done = FALSE;
                        break;
                    }
                    if (available == 0)
                    {
                        break;
                    }
                    const std::size_t at = response.body.size();
                    response.body.resize(at + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(handle, response.body.data() + at, available, &read))
                    {
                        done = FALSE;
                        break;
                    }
                    response.body.resize(at + read);
                }
            }
            if (!done)
            {
                const DWORD code   = GetLastError();
                response           = Response();
                response.cancelled = cancellation.cancelled() || code == ERROR_WINHTTP_OPERATION_CANCELLED;
                response.error     = response.cancelled ? std::string("cancelled") : ErrorText(code);
            }
        }
        shared->Close();
        WinHttpCloseHandle(connection);
        return response;
    }

  private:
    /** The response's headers, from the raw block WinHTTP keeps: every line after the status line, split at its colon.
        @param handle    the request
        @param response  where they go */
    static void ReadHeaders(HINTERNET handle, Response &response)
    {
        DWORD size = 0;
        WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (size == 0)
        {
            return;
        }
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                 &size, WINHTTP_NO_HEADER_INDEX))
        {
            return;
        }
        const std::string text  = Utf8Of(std::wstring_view(raw.data(), size / sizeof(wchar_t)));
        std::size_t       start = text.find("\r\n");
        while (start != std::string::npos && start + 2 < text.size())
        {
            start += 2;
            const std::size_t end   = text.find("\r\n", start);
            const std::string line  = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                std::string value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ')
                {
                    value.erase(value.begin());
                }
                response.headers.push_back({line.substr(0, colon), value});
            }
            start = end;
        }
    }

    HINTERNET session_{nullptr}; ///< the session every request runs on
};
} // namespace

std::unique_ptr<Transport> MakePlatformTransport()
{
    return std::make_unique<WinTransport>();
}

void FillRandom(std::span<std::uint8_t> bytes)
{
    BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}
} // namespace reckless::net
