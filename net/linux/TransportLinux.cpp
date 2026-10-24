// The platform's HTTP stack on Linux: libcurl, loaded by name when the first request is made - the JACK loader's way
// (audio/linux/JackLoader.cpp) - so no binary links it and a machine without it fails a request with the reason
// rather than a plugin that will not load. Every desktop distribution installs libcurl with its package manager, and
// its TLS is the distribution's, with the distribution's certificates. curl's ABI is stable across its major version
// 4, which every distribution of the last fifteen years ships: the few functions and option numbers this file uses are
// written out from curl.h here, so no curl header is needed to build.

#include "Http.h"
#include "Sha256.h"

#include <dlfcn.h>
#include <sys/random.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace reckless::net
{
namespace
{
// ---- curl.h's ABI, as far as this file needs it ---------------------------------------------------------------------

using CurlHandle = void; ///< CURL, opaque
using CurlCode   = int;  ///< CURLcode
using CurlOption = int;  ///< CURLoption
using CurlInfo   = int;  ///< CURLINFO

/** curl_slist, the list of header lines. */
struct CurlList
{
    char     *data; ///< the line
    CurlList *next; ///< the next, or null
};

constexpr CurlCode   kCurlOk                = 0;        ///< CURLE_OK
constexpr CurlCode   kCurlAbortedByCallback = 42;       ///< CURLE_ABORTED_BY_CALLBACK
constexpr CurlCode   kCurlTimedOut          = 28;       ///< CURLE_OPERATION_TIMEDOUT
constexpr long       kCurlGlobalDefault     = 3;        ///< CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32
constexpr CurlOption kOptWriteData          = 10001;    ///< CURLOPT_WRITEDATA
constexpr CurlOption kOptUrl                = 10002;    ///< CURLOPT_URL
constexpr CurlOption kOptTimeout            = 13;       ///< CURLOPT_TIMEOUT
constexpr CurlOption kOptPostFields         = 10015;    ///< CURLOPT_POSTFIELDS
constexpr CurlOption kOptUserAgent          = 10018;    ///< CURLOPT_USERAGENT
constexpr CurlOption kOptHttpHeader         = 10023;    ///< CURLOPT_HTTPHEADER
constexpr CurlOption kOptHeaderData         = 10029;    ///< CURLOPT_HEADERDATA
constexpr CurlOption kOptCustomRequest      = 10036;    ///< CURLOPT_CUSTOMREQUEST
constexpr CurlOption kOptNoProgress         = 43;       ///< CURLOPT_NOPROGRESS
constexpr CurlOption kOptPost               = 47;       ///< CURLOPT_POST
constexpr CurlOption kOptFollowLocation     = 52;       ///< CURLOPT_FOLLOWLOCATION
constexpr CurlOption kOptXferInfoData       = 10057;    ///< CURLOPT_XFERINFODATA
constexpr CurlOption kOptPostFieldSize      = 60;       ///< CURLOPT_POSTFIELDSIZE
constexpr CurlOption kOptConnectTimeout     = 78;       ///< CURLOPT_CONNECTTIMEOUT
constexpr CurlOption kOptHeaderFunction     = 20079;    ///< CURLOPT_HEADERFUNCTION
constexpr CurlOption kOptHttpGet            = 80;       ///< CURLOPT_HTTPGET
constexpr CurlOption kOptNoSignal           = 99;       ///< CURLOPT_NOSIGNAL
constexpr CurlOption kOptWriteFunction      = 20011;    ///< CURLOPT_WRITEFUNCTION
constexpr CurlOption kOptXferInfoFunction   = 20219;    ///< CURLOPT_XFERINFOFUNCTION
constexpr CurlInfo   kInfoResponseCode      = 0x200002; ///< CURLINFO_RESPONSE_CODE, a long

using GlobalInit   = CurlCode (*)(long);                          ///< curl_global_init's type
using EasyInit     = CurlHandle *(*)();                           ///< curl_easy_init's type
using EasySetopt   = CurlCode (*)(CurlHandle *, CurlOption, ...); ///< curl_easy_setopt's type
using EasyPerform  = CurlCode (*)(CurlHandle *);                  ///< curl_easy_perform's type
using EasyGetinfo  = CurlCode (*)(CurlHandle *, CurlInfo, ...);   ///< curl_easy_getinfo's type
using EasyCleanup  = void (*)(CurlHandle *);                      ///< curl_easy_cleanup's type
using EasyStrerror = const char *(*)(CurlCode);                   ///< curl_easy_strerror's type
using SlistAppend  = CurlList *(*)(CurlList *, const char *);     ///< curl_slist_append's type
using SlistFreeAll = void (*)(CurlList *);                        ///< curl_slist_free_all's type

/** curl's entry points, looked up once in the first libcurl that loads. */
struct Curl
{
    void        *library{nullptr};        ///< the dlopen handle; null when none loaded
    GlobalInit   global_init{nullptr};    ///< curl_global_init
    EasyInit     easy_init{nullptr};      ///< curl_easy_init
    EasySetopt   easy_setopt{nullptr};    ///< curl_easy_setopt
    EasyPerform  easy_perform{nullptr};   ///< curl_easy_perform
    EasyGetinfo  easy_getinfo{nullptr};   ///< curl_easy_getinfo
    EasyCleanup  easy_cleanup{nullptr};   ///< curl_easy_cleanup
    EasyStrerror easy_strerror{nullptr};  ///< curl_easy_strerror
    SlistAppend  slist_append{nullptr};   ///< curl_slist_append
    SlistFreeAll slist_free_all{nullptr}; ///< curl_slist_free_all
    std::string  missing;                 ///< why no libcurl is usable; empty when one is
};

/** The process's libcurl: the OpenSSL build's soname first, then the GnuTLS build's (Debian and Ubuntu ship both,
    either one installed), then the bare name; curl_global_init run once, which is not thread-safe and so runs here,
    under the once.
    @return  the entry points, or the reason none loaded */
const Curl &LoadCurl()
{
    static const Curl curl = [] {
        Curl c;
        for (const char *name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"})
        {
            c.library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (c.library != nullptr)
            {
                break;
            }
        }
        if (c.library == nullptr)
        {
            c.missing = "no libcurl is installed (the package libcurl4)";
            return c;
        }
        const auto find = [&](auto &slot, const char *symbol) {
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(c.library, symbol));
            if (slot == nullptr && c.missing.empty())
            {
                c.missing = std::string("libcurl lacks ") + symbol;
            }
        };
        find(c.global_init, "curl_global_init");
        find(c.easy_init, "curl_easy_init");
        find(c.easy_setopt, "curl_easy_setopt");
        find(c.easy_perform, "curl_easy_perform");
        find(c.easy_getinfo, "curl_easy_getinfo");
        find(c.easy_cleanup, "curl_easy_cleanup");
        find(c.easy_strerror, "curl_easy_strerror");
        find(c.slist_append, "curl_slist_append");
        find(c.slist_free_all, "curl_slist_free_all");
        if (c.missing.empty() && c.global_init(kCurlGlobalDefault) != kCurlOk)
        {
            c.missing = "libcurl would not start";
        }
        return c;
    }();
    return curl;
}

/** What the callbacks write into: the response and the cancellation they read. */
struct Exchange
{
    Response           *response;     ///< where the status, the headers and the body go
    const Cancellation *cancellation; ///< read by the progress callback
};

/** curl's body callback: the bytes appended to the body.
    @param data   the bytes
    @param size   the size of an item, 1
    @param count  the items
    @param user   the Exchange
    @return       the bytes taken, all of them */
std::size_t OnBody(char *data, std::size_t size, std::size_t count, void *user)
{
    static_cast<Exchange *>(user)->response->body.append(data, size * count);
    return size * count;
}

/** curl's header callback, once a line: a status line starts the headers again (a 100 Continue's precede the real
    response's), any other line with a colon is a header.
    @param data   the line's bytes
    @param size   the size of an item, 1
    @param count  the items
    @param user   the Exchange
    @return       the bytes taken, all of them */
std::size_t OnHeader(char *data, std::size_t size, std::size_t count, void *user)
{
    auto       &headers = static_cast<Exchange *>(user)->response->headers;
    std::string line(data, size * count);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    {
        line.pop_back();
    }
    if (line.rfind("HTTP/", 0) == 0)
    {
        headers.clear();
    }
    else if (const std::size_t colon = line.find(':'); colon != std::string::npos)
    {
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ')
        {
            value.erase(value.begin());
        }
        headers.push_back({line.substr(0, colon), value});
    }
    return size * count;
}

/** curl's progress callback, many times a second while a transfer runs, connecting included: nonzero aborts.
    @param user           the Exchange
    @param download_total the bytes to come down, as far as known; unread
    @param download_now   the bytes come down; unread
    @param upload_total   the bytes to go up; unread
    @param upload_now     the bytes gone up; unread
    @return               1 once the request is cancelled, else 0 */
int OnProgress(void *user, long long download_total, long long download_now, long long upload_total,
               long long upload_now)
{
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return static_cast<Exchange *>(user)->cancellation->cancelled() ? 1 : 0;
}

/** libcurl behind the suite's transport: an easy handle a request, no redirect followed, no signal raised. */
class LinuxTransport final : public Transport
{
  public:
    Response Send(const Request &request, Cancellation &cancellation) override
    {
        Response    response;
        const Curl &curl = LoadCurl();
        if (!curl.missing.empty())
        {
            response.error = curl.missing;
            return response;
        }
        if (cancellation.cancelled())
        {
            response.cancelled = true;
            response.error     = "cancelled";
            return response;
        }
        CurlHandle *easy = curl.easy_init();
        if (easy == nullptr)
        {
            response.error = "libcurl made no handle";
            return response;
        }
        Exchange  exchange{&response, &cancellation};
        CurlList *headers = nullptr;
        for (const auto &header : request.headers)
        {
            headers = curl.slist_append(headers, (header.name + ": " + header.value).c_str());
        }
        // No "Expect: 100-continue" on a POST: the servers answer at once.
        headers = curl.slist_append(headers, "Expect:");
        curl.easy_setopt(easy, kOptUrl, request.url.c_str());
        curl.easy_setopt(easy, kOptNoSignal, 1L);
        curl.easy_setopt(easy, kOptFollowLocation, 0L);
        curl.easy_setopt(easy, kOptTimeout, static_cast<long>(request.timeout_seconds + 0.5));
        curl.easy_setopt(easy, kOptConnectTimeout, static_cast<long>(request.timeout_seconds + 0.5));
        curl.easy_setopt(easy, kOptUserAgent, "RecklessTechnology/1.0");
        curl.easy_setopt(easy, kOptHttpHeader, headers);
        curl.easy_setopt(easy, kOptWriteFunction, &OnBody);
        curl.easy_setopt(easy, kOptWriteData, &exchange);
        curl.easy_setopt(easy, kOptHeaderFunction, &OnHeader);
        curl.easy_setopt(easy, kOptHeaderData, &exchange);
        curl.easy_setopt(easy, kOptNoProgress, 0L);
        curl.easy_setopt(easy, kOptXferInfoFunction, &OnProgress);
        curl.easy_setopt(easy, kOptXferInfoData, &exchange);
        if (request.method == "POST")
        {
            curl.easy_setopt(easy, kOptPost, 1L);
            curl.easy_setopt(easy, kOptPostFieldSize, static_cast<long>(request.body.size()));
            curl.easy_setopt(easy, kOptPostFields, request.body.c_str());
        }
        else if (request.method == "GET")
        {
            curl.easy_setopt(easy, kOptHttpGet, 1L);
        }
        else
        {
            curl.easy_setopt(easy, kOptCustomRequest, request.method.c_str());
        }

        const CurlCode code = curl.easy_perform(easy);
        if (code == kCurlOk)
        {
            long status = 0;
            curl.easy_getinfo(easy, kInfoResponseCode, &status);
            response.status = static_cast<int>(status);
        }
        else
        {
            response           = Response();
            response.cancelled = code == kCurlAbortedByCallback || cancellation.cancelled();
            response.error     = response.cancelled      ? std::string("cancelled")
                                 : code == kCurlTimedOut ? std::string("the server did not answer in time")
                                                         : std::string(curl.easy_strerror(code));
        }
        curl.slist_free_all(headers);
        curl.easy_cleanup(easy);
        return response;
    }
};
} // namespace

std::unique_ptr<Transport> MakePlatformTransport()
{
    return std::make_unique<LinuxTransport>();
}

void FillRandom(std::span<std::uint8_t> bytes)
{
    std::size_t filled = 0;
    while (filled < bytes.size())
    {
        const ssize_t got = getrandom(bytes.data() + filled, bytes.size() - filled, 0);
        if (got > 0)
        {
            filled += static_cast<std::size_t>(got);
        }
        else if (got < 0 && errno != EINTR)
        {
            break;
        }
    }
}
} // namespace reckless::net
