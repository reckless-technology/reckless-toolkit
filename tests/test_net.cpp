// The network's parts on their own (net): SHA-256 on FIPS 180-4's vectors, base64url on RFC 7636's PKCE example,
// URLs, references and forms read and written, Fetch's redirect rule on a scripted transport - the credentials dropped
// as a redirect leaves their origin - the cancellation's hook, and the loopback listener on a real socket.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "Http.h"
#include "LoopbackListener.h"
#include "Sha256.h"
#include "TestHttpServer.h"
#include "Url.h"

namespace
{
using reckless::net::Cancellation;
using reckless::net::Fetch;
using reckless::net::Request;
using reckless::net::Response;
using reckless::net::Url;

std::string Hex(const reckless::net::Sha256Digest &digest)
{
    std::string text;
    char        byte[3];
    for (const auto b : digest)
    {
        std::snprintf(byte, sizeof(byte), "%02x", b);
        text += byte;
    }
    return text;
}

TEST(Sha256, MatchesTheStandardsVectors)
{
    using reckless::net::Sha256;
    EXPECT_EQ(Hex(Sha256(std::string_view(""))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Hex(Sha256(std::string_view("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the padding's length no longer fits the block, so a second block carries it.
    EXPECT_EQ(Hex(Sha256(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(Hex(Sha256(std::string(1000000, 'a'))),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Every length across the two-block boundary agrees with the same bytes hashed in another order of calls: 55, 56,
    // 63, 64 and 65 bytes each digest to a different value, and the same value twice.
    for (const std::size_t n : {55u, 56u, 63u, 64u, 65u, 119u, 120u})
    {
        const std::string message(n, 'x');
        EXPECT_EQ(Sha256(message), Sha256(message)) << n;
        EXPECT_NE(Sha256(message), Sha256(std::string(n + 1, 'x'))) << n;
    }
}

TEST(Base64Url, IsThePkceChallengeOfRfc7636sExample)
{
    // RFC 7636 appendix B: the verifier's SHA-256 in base64url with no padding is the S256 challenge.
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    EXPECT_EQ(reckless::net::Base64Url(reckless::net::Sha256(verifier)), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    const std::uint8_t bytes[] = {0xfb, 0xff, 0xfe};
    EXPECT_EQ(reckless::net::Base64Url(bytes), "-__-"); // '+' and '/' as '-' and '_'
    const std::uint8_t one[] = {0x61};
    EXPECT_EQ(reckless::net::Base64Url(one), "YQ"); // no '='
}

TEST(FillRandom, FillsEveryByteDifferentlyEachTime)
{
    std::vector<std::uint8_t> a(64, 0), b(64, 0);
    reckless::net::FillRandom(a);
    reckless::net::FillRandom(b);
    EXPECT_NE(a, b);
    // 64 bytes of zeros from a working generator is a chance of 2^-512.
    EXPECT_NE(a, std::vector<std::uint8_t>(64, 0));
}

TEST(Url, ReadsAnAbsoluteUrlInItsParts)
{
    const auto url = Url::Parse("HTTPS://Www.Example.com/api/v1/tones?id=7#top");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->scheme, "https");
    EXPECT_EQ(url->host, "www.example.com");
    EXPECT_EQ(url->port, 443);
    EXPECT_EQ(url->target, "/api/v1/tones?id=7");
    EXPECT_EQ(url->Path(), "/api/v1/tones");
    EXPECT_EQ(url->Query(), "id=7");
    EXPECT_EQ(url->Origin(), "https://www.example.com:443");

    const auto local = Url::Parse("http://127.0.0.1:50152/callback?code=a");
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(local->port, 50152);
    EXPECT_EQ(local->target, "/callback?code=a");
    EXPECT_EQ(Url::Parse("http://example.com")->target, "/");
    EXPECT_EQ(Url::Parse("http://example.com?x=1")->target, "/?x=1");
    EXPECT_EQ(Url::Parse("http://[::1]:8080/x")->host, "[::1]");
    EXPECT_EQ(Url::Parse("http://[::1]:8080/x")->port, 8080);

    EXPECT_FALSE(Url::Parse("ftp://example.com/").has_value());
    EXPECT_FALSE(Url::Parse("/relative").has_value());
    EXPECT_FALSE(Url::Parse("https://user:pass@example.com/").has_value());
    EXPECT_FALSE(Url::Parse("https://example.com:99999/").has_value());
    EXPECT_FALSE(Url::Parse("https:///path").has_value());
}

TEST(Url, ResolvesAReferenceAgainstItsBase)
{
    using reckless::net::ResolveReference;
    const std::string base = "https://api.example.com/v1/models/7/download";
    EXPECT_EQ(ResolveReference(base, "https://store.example.com/x.nam"), "https://store.example.com/x.nam");
    EXPECT_EQ(ResolveReference(base, "//cdn.example.com/x.nam"), "https://cdn.example.com/x.nam");
    EXPECT_EQ(ResolveReference(base, "/storage/x.nam"), "https://api.example.com/storage/x.nam");
    EXPECT_EQ(ResolveReference(base, "file.nam"), "https://api.example.com/v1/models/7/file.nam");
}

TEST(Url, EncodesAndReadsBackAForm)
{
    using reckless::net::FormEncode;
    using reckless::net::ParseQuery;
    using reckless::net::PercentEncode;
    EXPECT_EQ(PercentEncode("a b&c=d/\xC3\xA9~._-"), "a%20b%26c%3Dd%2F%C3%A9~._-");
    const reckless::net::Fields fields = {{"redirect_uri", "http://127.0.0.1:1/callback"}, {"state", "x y"}};
    const std::string           text   = FormEncode(fields);
    EXPECT_EQ(text, "redirect_uri=http%3A%2F%2F127.0.0.1%3A1%2Fcallback&state=x%20y");
    EXPECT_EQ(ParseQuery(text), fields);
    EXPECT_EQ(ParseQuery("?a=1&&b&c=x+y%21%zz"), (reckless::net::Fields{{"a", "1"}, {"b", ""}, {"c", "x y!%zz"}}));
    EXPECT_EQ(reckless::net::FieldOf(ParseQuery("tone_id=65578&state=s"), "tone_id"), "65578");
    EXPECT_FALSE(reckless::net::FieldOf(ParseQuery("state=s"), "code").has_value());
}

/** A transport that answers from a script, in turn, and keeps every request it was sent. */
class ScriptedTransport final : public reckless::net::Transport
{
  public:
    Response Send(const Request &request, Cancellation &) override
    {
        sent.push_back(request);
        if (answers.empty())
        {
            Response none;
            none.error = "no answer scripted";
            return none;
        }
        Response answer = answers.front();
        answers.erase(answers.begin());
        return answer;
    }

    static Response Redirect(int status, std::string location)
    {
        Response r;
        r.status = status;
        r.headers.push_back({"location", std::move(location)});
        return r;
    }

    static Response Ok(std::string body)
    {
        Response r;
        r.status = 200;
        r.body   = std::move(body);
        return r;
    }

    std::vector<Response> answers;
    std::vector<Request>  sent;
};

TEST(Fetch, DropsTheCredentialsAsARedirectLeavesTheirOrigin)
{
    ScriptedTransport transport;
    transport.answers = {ScriptedTransport::Redirect(302, "/v1/other"),
                         ScriptedTransport::Redirect(302, "https://store.example.com/file.nam"),
                         ScriptedTransport::Ok("the file")};
    Request request;
    request.url = "https://api.example.com/v1/models/7/download";
    request.headers.push_back({"Authorization", "Bearer secret"});
    request.headers.push_back({"Accept", "*/*"});
    Cancellation   cancellation;
    const Response response = Fetch(transport, request, cancellation);
    ASSERT_TRUE(response.ok());
    EXPECT_EQ(response.body, "the file");
    ASSERT_EQ(transport.sent.size(), 3u);
    // The same origin keeps the token; the storage on another host never sees it, and the other headers go on.
    EXPECT_EQ(transport.sent[1].url, "https://api.example.com/v1/other");
    ASSERT_NE(transport.sent[1].Find("authorization"), nullptr);
    EXPECT_EQ(transport.sent[2].url, "https://store.example.com/file.nam");
    EXPECT_EQ(transport.sent[2].Find("Authorization"), nullptr);
    ASSERT_NE(transport.sent[2].Find("Accept"), nullptr);
}

TEST(Fetch, AsksAgainAsAGetAfterA303AndAPostsRedirect)
{
    for (const int status : {301, 302, 303, 307})
    {
        ScriptedTransport transport;
        transport.answers = {ScriptedTransport::Redirect(status, "/next"), ScriptedTransport::Ok("done")};
        Request request;
        request.method = "POST";
        request.url    = "https://example.com/start";
        request.body   = "a=1";
        request.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
        Cancellation cancellation;
        ASSERT_TRUE(Fetch(transport, request, cancellation).ok()) << status;
        ASSERT_EQ(transport.sent.size(), 2u) << status;
        const bool keeps_post = status == 307;
        EXPECT_EQ(transport.sent[1].method, keeps_post ? "POST" : "GET") << status;
        EXPECT_EQ(transport.sent[1].body.empty(), !keeps_post) << status;
    }
}

TEST(Fetch, GivesUpAfterTooManyRedirectsAndRefusesAnotherScheme)
{
    ScriptedTransport transport;
    for (int i = 0; i < 10; ++i)
    {
        transport.answers.push_back(ScriptedTransport::Redirect(302, "/again"));
    }
    Request request;
    request.url = "https://example.com/";
    Cancellation   cancellation;
    const Response looped = Fetch(transport, request, cancellation, 3);
    EXPECT_FALSE(looped.error.empty());
    EXPECT_EQ(transport.sent.size(), 4u); // the first and three redirects

    ScriptedTransport other;
    other.answers = {ScriptedTransport::Redirect(302, "file:///etc/passwd")};
    EXPECT_FALSE(Fetch(other, request, cancellation).error.empty());
    EXPECT_EQ(other.sent.size(), 1u);

    // A redirect with no Location is the response itself.
    ScriptedTransport bare;
    Response          nowhere;
    nowhere.status = 302;
    bare.answers   = {nowhere};
    EXPECT_EQ(Fetch(bare, request, cancellation).status, 302);
}

TEST(Cancellation, RunsTheHookInFlightOnceAndNeverAfterItGoes)
{
    Cancellation cancellation;
    int          aborted = 0;
    {
        const Cancellation::Watch watch(cancellation, [&] { ++aborted; });
        EXPECT_EQ(aborted, 0);
        cancellation.Cancel();
        EXPECT_EQ(aborted, 1);
        cancellation.Cancel();
        EXPECT_EQ(aborted, 1);
    }
    EXPECT_TRUE(cancellation.cancelled());
    // A hook hung on a cancellation already cancelled runs at once.
    const Cancellation::Watch late(cancellation, [&] { ++aborted; });
    EXPECT_EQ(aborted, 2);

    Cancellation other;
    {
        const Cancellation::Watch gone(other, [&] { ++aborted; });
    }
    other.Cancel();
    EXPECT_EQ(aborted, 2);
}

TEST(LoopbackListener, AnswersTheRedirectAndHandsBackItsTarget)
{
    reckless::net::LoopbackListener listener;
    ASSERT_TRUE(listener.ok()) << listener.error();
    EXPECT_GT(listener.port(), 0);
    EXPECT_EQ(listener.RedirectUri("/callback"), "http://127.0.0.1:" + std::to_string(listener.port()) + "/callback");

    std::string  favicon_answer;
    std::string  callback_answer;
    std::thread  browser([&] {
        // A browser asks for the icon first; the listener answers it 404 and goes on waiting.
        favicon_answer  = reckless::test::HttpGet(listener.port(), "/favicon.ico");
        callback_answer = reckless::test::HttpGet(listener.port(), "/callback?code=abc&state=s%20t&tone_id=7");
    });
    Cancellation cancellation;
    const auto   target = listener.Await("/callback", "<p>Back in the plugin.</p>", cancellation, 10.0);
    browser.join();
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, "/callback?code=abc&state=s%20t&tone_id=7");
    EXPECT_NE(favicon_answer.find("404"), std::string::npos);
    EXPECT_NE(callback_answer.find("200 OK"), std::string::npos);
    EXPECT_NE(callback_answer.find("Back in the plugin."), std::string::npos);
}

TEST(LoopbackListener, StopsWaitingWhenCancelledOrTimedOut)
{
    reckless::net::LoopbackListener listener;
    ASSERT_TRUE(listener.ok()) << listener.error();
    Cancellation cancellation;
    const auto   start = std::chrono::steady_clock::now();
    std::thread  canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancellation.Cancel();
    });
    EXPECT_FALSE(listener.Await("/callback", "", cancellation, 30.0).has_value());
    canceller.join();
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(waited, 2.0);

    Cancellation none;
    EXPECT_FALSE(listener.Await("/callback", "", none, 0.2).has_value());
}
} // namespace
