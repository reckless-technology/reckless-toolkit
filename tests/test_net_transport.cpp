// The platform's HTTP stack (net/mac, win, linux) held to a real exchange with a server of the suite's own on
// the loopback interface (TestHttpServer.h): the status, the headers and the body; a redirect handed back, not
// followed, and Fetch following it; a POST's body and headers as sent; a cancellation ending a request the server sits
// on at once; a port nobody listens on an error, not a hang. Nothing leaves the machine - unless RECKLESS_NET_LIVE is
// set, when one GET of a public HTTPS endpoint checks the system's TLS and certificates.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "Http.h"
#include "TestHttpServer.h"

namespace
{
using reckless::net::Cancellation;
using reckless::net::Request;
using reckless::net::Response;
using reckless::test::HttpAnswer;
using reckless::test::SeenRequest;
using reckless::test::TestHttpServer;

/** The answers the suites' server gives, by path. */
std::string Answer(const SeenRequest &request, const std::atomic<bool> &stopping)
{
    const std::string path = request.target.substr(0, request.target.find('?'));
    if (path == "/hello")
    {
        return HttpAnswer("200 OK", "hello, world", "Content-Type: text/plain\r\nX-Test: yes\r\n");
    }
    if (path == "/redirect")
    {
        return HttpAnswer("302 Found", "", "Location: /hello\r\n");
    }
    if (path == "/echo")
    {
        return HttpAnswer("201 Created", request.method + "|" + request.Header("Content-Type") + "|" +
                                             request.Header("Authorization") + "|" + request.body);
    }
    if (path == "/slow")
    {
        // Sits on the request for up to ten seconds, or until the server stops.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stopping.load() && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return HttpAnswer("200 OK", "late");
    }
    return HttpAnswer("404 Not Found", "nothing here");
}

/** The platform's transport, or a skip where Linux has no libcurl installed. */
#define REQUIRE_TRANSPORT(response)                            \
    if ((response).error.find("libcurl") != std::string::npos) \
    {                                                          \
        GTEST_SKIP() << (response).error;                      \
    }

TEST(PlatformTransport, ReadsTheStatusTheHeadersAndTheBody)
{
    TestHttpServer server(Answer);
    auto           transport = reckless::net::MakePlatformTransport();
    Request        request;
    request.url = server.Url("/hello?x=1");
    Cancellation   cancellation;
    const Response response = transport->Send(request, cancellation);
    REQUIRE_TRANSPORT(response);
    ASSERT_TRUE(response.ok()) << response.error;
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "hello, world");
    ASSERT_NE(response.Find("x-test"), nullptr);
    EXPECT_EQ(*response.Find("X-Test"), "yes");
    ASSERT_EQ(server.seen().size(), 1u);
    EXPECT_EQ(server.seen()[0].target, "/hello?x=1");

    Request missing;
    missing.url              = server.Url("/nothing");
    const Response not_found = transport->Send(missing, cancellation);
    EXPECT_TRUE(not_found.error.empty());
    EXPECT_EQ(not_found.status, 404);
    EXPECT_FALSE(not_found.ok());
}

TEST(PlatformTransport, HandsARedirectBackAndFetchFollowsIt)
{
    TestHttpServer server(Answer);
    auto           transport = reckless::net::MakePlatformTransport();
    Request        request;
    request.url = server.Url("/redirect");
    Cancellation   cancellation;
    const Response redirect = transport->Send(request, cancellation);
    REQUIRE_TRANSPORT(redirect);
    EXPECT_EQ(redirect.status, 302);
    ASSERT_NE(redirect.Find("Location"), nullptr);
    EXPECT_EQ(server.seen().size(), 1u); // nothing followed

    const Response followed = reckless::net::Fetch(*transport, request, cancellation);
    ASSERT_TRUE(followed.ok()) << followed.error;
    EXPECT_EQ(followed.body, "hello, world");
}

TEST(PlatformTransport, SendsAPostsBodyAndHeaders)
{
    TestHttpServer server(Answer);
    auto           transport = reckless::net::MakePlatformTransport();
    Request        request;
    request.method = "POST";
    request.url    = server.Url("/echo");
    request.body   = "grant_type=authorization_code&code=a%20b";
    request.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    request.headers.push_back({"Authorization", "Bearer t0ken"});
    Cancellation   cancellation;
    const Response response = transport->Send(request, cancellation);
    REQUIRE_TRANSPORT(response);
    ASSERT_EQ(response.status, 201) << response.error;
    EXPECT_EQ(response.body,
              "POST|application/x-www-form-urlencoded|Bearer t0ken|grant_type=authorization_code&code=a%20b");
}

TEST(PlatformTransport, ACancellationEndsARequestTheServerSitsOn)
{
    TestHttpServer server(Answer);
    auto           transport = reckless::net::MakePlatformTransport();
    {
        // Settle whether a transport is there before timing one.
        Request probe;
        probe.url = server.Url("/hello");
        Cancellation   none;
        const Response first = transport->Send(probe, none);
        REQUIRE_TRANSPORT(first);
    }
    Request request;
    request.url = server.Url("/slow");
    Cancellation   cancellation;
    std::thread    canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancellation.Cancel();
    });
    const auto     start    = std::chrono::steady_clock::now();
    const Response response = transport->Send(request, cancellation);
    const double   waited   = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    canceller.join();
    EXPECT_TRUE(response.cancelled);
    EXPECT_FALSE(response.error.empty());
    EXPECT_EQ(response.status, 0);
    EXPECT_LT(waited, 3.0) << "the cancellation waited for the server";

    // And one cancelled before it starts never waits at all.
    Cancellation before;
    before.Cancel();
    const Response refused = transport->Send(request, before);
    EXPECT_TRUE(refused.cancelled);
}

TEST(PlatformTransport, APortNobodyListensOnIsAnError)
{
    int port = 0;
    {
        TestHttpServer gone(Answer);
        port = gone.port();
    }
    auto    transport = reckless::net::MakePlatformTransport();
    Request request;
    request.url             = "http://127.0.0.1:" + std::to_string(port) + "/hello";
    request.timeout_seconds = 5.0;
    Cancellation   cancellation;
    const Response response = transport->Send(request, cancellation);
    REQUIRE_TRANSPORT(response);
    EXPECT_EQ(response.status, 0);
    EXPECT_FALSE(response.error.empty());
    EXPECT_FALSE(response.cancelled);
}

TEST(PlatformTransport, ReachesAPublicHttpsServerWhenAskedTo)
{
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const bool live = std::getenv("RECKLESS_NET_LIVE") != nullptr;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!live)
    {
        GTEST_SKIP() << "RECKLESS_NET_LIVE is not set: nothing leaves the machine";
    }
    auto    transport = reckless::net::MakePlatformTransport();
    Request request;
    request.url = "https://www.tone3000.com/api/v1/tones/trending";
    request.headers.push_back({"Accept", "application/json"});
    Cancellation   cancellation;
    const Response response = reckless::net::Fetch(*transport, request, cancellation);
    ASSERT_TRUE(response.ok()) << response.status << " " << response.error;
    EXPECT_NE(response.body.find("\"data\""), std::string::npos);
}
} // namespace
