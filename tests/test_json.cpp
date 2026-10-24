// The suite's JSON (core/Json.h): the text the writer gives - two-space indents, LF, reals in the shortest form
// that reads back to the same double with ".0" on a whole one, text in UTF-8 with the controls escaped and a byte that
// is not UTF-8 written as U+FFFD - the strict RFC 8259 reader, which refuses what the grammar does not allow and
// reads back what the writer wrote as the same value, and the bytes as base-64.
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "Json.h"

#include <gtest/gtest.h>

namespace
{
namespace json = reckless::core::json;

json::Value Parsed(const std::string &text)
{
    const json::ParseResult result = json::Parse(text);
    EXPECT_TRUE(result.IsOk()) << text << ": " << result.error;
    return result.value;
}

bool Refused(const std::string &text)
{
    return !json::Parse(text).IsOk();
}
} // namespace

TEST(Json, AnObjectAndAnArrayAreWrittenAMemberOrAnElementALine)
{
    json::Value document = json::Value::MakeObject();
    document.Set("format", 1);
    document.Set("plugin", "Reckless Drive");
    json::Value list = json::Value::MakeArray();
    list.Append(true);
    list.Append(nullptr);
    document.Set("list", list);
    document.Set("empty", json::Value::MakeObject());
    document.Set("none", json::Value::MakeArray());
    EXPECT_EQ(json::Write(document), "{\n"
                                     "  \"format\": 1,\n"
                                     "  \"plugin\": \"Reckless Drive\",\n"
                                     "  \"list\": [\n"
                                     "    true,\n"
                                     "    null\n"
                                     "  ],\n"
                                     "  \"empty\": {},\n"
                                     "  \"none\": []\n"
                                     "}");
}

TEST(Json, ARealIsItsShortestTextThatReadsBackAndStaysAReal)
{
    EXPECT_EQ(json::Write(json::Value(0.3)), "0.3");
    EXPECT_EQ(json::Write(json::Value(50.0)), "50.0") << "a whole real keeps its point, so it reads back a real";
    EXPECT_EQ(json::Write(json::Value(-0.0)), "-0.0");
    EXPECT_EQ(json::Write(json::Value(1.0e21)), "1e+21");
    EXPECT_EQ(json::Write(json::Value(static_cast<double>(0.3f))), "0.30000001192092896");
    EXPECT_EQ(json::Write(json::Value(std::numeric_limits<double>::infinity())), "null") << "JSON holds no infinity";
    EXPECT_EQ(json::Write(json::Value(std::nan(""))), "null");
    for (const double real : {0.1, 1.0 / 3.0, -12345.678, 5e-324, 1.7976931348623157e308, 48000.0})
    {
        const json::Value back = Parsed(json::Write(json::Value(real)));
        ASSERT_TRUE(back.IsDouble()) << real;
        EXPECT_EQ(back.AsDouble(), real) << "to the bit";
    }
    EXPECT_EQ(json::Write(json::Value(std::int64_t{-9007199254740993})), "-9007199254740993") << "a whole number";
}

TEST(Json, TextIsUtf8WithItsControlsEscapedAndABrokenByteRepaired)
{
    EXPECT_EQ(json::Write(json::Value("say \"hi\"\\ / \x01\t\n\r\b\f")),
              "\"say \\\"hi\\\"\\\\ / \\u0001\\t\\n\\r\\b\\f\"");
    EXPECT_EQ(json::Write(json::Value("Br\xC3\xBC"
                                      "cke \xE2\x82\xAC \xF0\x9F\x8E\xB8")),
              "\"Br\xC3\xBC"
              "cke \xE2\x82\xAC \xF0\x9F\x8E\xB8\"")
        << "every other character as it is";
    EXPECT_EQ(json::Write(json::Value("a\xFF"
                                      "b\xC3")),
              "\"a\xEF\xBF\xBD"
              "b\xEF\xBF\xBD\"")
        << "a byte that is not UTF-8 is U+FFFD, so the text is always JSON";
    EXPECT_TRUE(json::Parse(json::Write(json::Value("a\xFF"))).IsOk());
}

TEST(Json, TheReaderTakesTheGrammar)
{
    const json::Value document =
        Parsed(" \t\r\n{ \"a\" : [1, -2.5e3, 0, -0, 0.25E-1, true, false, null, \"x\\u00e9\\ud83c\\udfb8\\/\"] } \n");
    const json::Array &a = *document["a"].AsArray();
    ASSERT_EQ(a.size(), 9u);
    EXPECT_TRUE(a[0].IsInt());
    EXPECT_EQ(a[0].AsInt(), 1);
    EXPECT_TRUE(a[1].IsDouble());
    EXPECT_EQ(a[1].AsDouble(), -2500.0);
    EXPECT_TRUE(a[3].IsInt()) << "-0 has no fraction or exponent";
    EXPECT_EQ(a[4].AsDouble(), 0.025);
    EXPECT_TRUE(a[5].AsBool());
    EXPECT_TRUE(a[6].IsBool());
    EXPECT_TRUE(a[7].IsNull());
    EXPECT_EQ(a[8].AsString(), "x\xC3\xA9\xF0\x9F\x8E\xB8/");
    EXPECT_TRUE(Parsed("[]").IsArray());
    EXPECT_EQ(Parsed("7").AsInt(), 7) << "any value is a document";
    EXPECT_TRUE(Parsed("99999999999999999999").IsDouble()) << "a whole number past 64 bits is a real";
    EXPECT_EQ(Parsed("{\"a\": 1, \"b\": 2, \"a\": 3}")["a"].AsInt(), 3) << "a name twice keeps its last value";
    EXPECT_EQ(Parsed("{\"a\": 1, \"b\": 2, \"a\": 3}").AsObject()->front().first, "a") << "and its first place";
}

TEST(Json, WhatTheGrammarDoesNotAllowIsRefusedSayingWhere)
{
    for (const char *text : {"",
                             "   ",
                             "{",
                             "[1,]",
                             "{\"a\": 1,}",
                             "{a: 1}",
                             "{'a': 1}",
                             "{\"a\" 1}",
                             "[1 2]",
                             "01",
                             "1.",
                             ".5",
                             "+1",
                             "-",
                             "1e",
                             "tru",
                             "nul",
                             "[\"a\tb\"]",
                             "[\"\\x\"]",
                             "[\"\\u12\"]",
                             "[\"\\ud800\"]",
                             "[\"\\udc00\"]",
                             "[\"a\xFF\"]",
                             "[\"\xC0\xAF\"]",
                             "{} x",
                             "[1] [2]",
                             "1e999",
                             "\"unterminated"})
    {
        EXPECT_TRUE(Refused(text)) << text;
    }
    const json::ParseResult refused = json::Parse("{\n  \"a\": [1, 2,]\n}");
    EXPECT_FALSE(refused.IsOk());
    EXPECT_TRUE(refused.value.IsNull()) << "nothing read";
    EXPECT_EQ(refused.error.substr(0, 4), "2:14") << refused.error;
    std::string deep;
    for (int i = 0; i <= json::kMaxDepth; ++i)
    {
        deep += '[';
    }
    EXPECT_TRUE(Refused(deep)) << "past the depth a damaged file is refused before the stack runs out";
}

TEST(Json, WhatTheWriterWritesTheReaderReadsAsTheSameValue)
{
    json::Value document = json::Value::MakeObject();
    document.Set("format", 1);
    document.Set("plugin", "Reckless Cab");
    json::Value &parameters = document.Set("parameters", json::Value::MakeObject());
    parameters.Set("gain_db", 20.0);
    parameters.Set("clip", 2);
    parameters.Set("depth", 0.30000001192092896);
    json::Value &state = document.Set("state", json::Value::MakeObject());
    state.Set("response_path", "/Users/me/IRs/Br\xC3\xBC"
                               "cke 4x12.wav");
    state.Set("analyser_shown", false);
    state.Set("nothing", nullptr);
    json::Value list = json::Value::MakeArray();
    list.Append(json::Value::MakeArray());
    list.Append(-1.5e-300);
    state.Set("list", list);
    EXPECT_EQ(Parsed(json::Write(document)), document);
}

TEST(Json, ReadersFallBackWhereAMemberIsMissingOrOfAnotherKind)
{
    const json::Value document = Parsed(R"({"n": 2.75, "i": 3, "b": true, "s": "text", "z": 0})");
    EXPECT_EQ(document.NumberOr("n", 1.0), 2.75);
    EXPECT_EQ(document.NumberOr("i", 1.0), 3.0);
    EXPECT_EQ(document.NumberOr("s", 1.0), 1.0);
    EXPECT_EQ(document.NumberOr("missing", 1.0), 1.0);
    EXPECT_EQ(document.IntOr("n", 9), 2) << "a real truncated";
    EXPECT_EQ(document.IntOr("b", 9), 9);
    EXPECT_TRUE(document.BoolOr("b", false));
    EXPECT_FALSE(document.BoolOr("z", true)) << "a number as a flag";
    EXPECT_TRUE(document.BoolOr("s", true)) << "text is no flag";
    EXPECT_EQ(document.StringOr("s", "x"), "text");
    EXPECT_EQ(document.StringOr("i", "x"), "x");
    EXPECT_TRUE(document["missing"].IsNull());
    json::Value edited = document;
    edited.Remove("s");
    edited.Remove("missing");
    EXPECT_FALSE(edited.Has("s"));
    EXPECT_EQ(edited.AsObject()->size(), 4u);
}

TEST(Json, BytesAreStandardBase64InAnObjectOfTheirOwn)
{
    EXPECT_EQ(json::EncodeBase64(std::vector<std::uint8_t>{}), "");
    EXPECT_EQ(json::EncodeBase64(std::vector<std::uint8_t>{'f'}), "Zg==");
    EXPECT_EQ(json::EncodeBase64(std::vector<std::uint8_t>{'f', 'o'}), "Zm8=");
    EXPECT_EQ(json::EncodeBase64(std::vector<std::uint8_t>{'f', 'o', 'o'}), "Zm9v");
    EXPECT_EQ(json::EncodeBase64(std::vector<std::uint8_t>{0xfb, 0xff, 0xbf}), "+/+/");
    std::vector<std::uint8_t> every(256);
    for (int i = 0; i < 256; ++i)
    {
        every[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t n : {0u, 1u, 2u, 3u, 4u, 255u, 256u})
    {
        const std::vector<std::uint8_t> bytes(every.begin(), every.begin() + static_cast<std::ptrdiff_t>(n));
        EXPECT_EQ(json::DecodeBase64(json::EncodeBase64(bytes)), bytes) << n;
        EXPECT_EQ(json::BytesOf(json::FromBytes(bytes)), bytes) << n;
    }
    EXPECT_EQ(json::Write(json::FromBytes(std::vector<std::uint8_t>{'f', 'o', 'o'})), "{\n  \"base64\": \"Zm9v\"\n}");
    for (const char *bad : {"Zg=", "Z===", "Zg==Zg==", "Zm9v!", "Zm 9v", "=m9v"})
    {
        EXPECT_FALSE(json::DecodeBase64(bad).has_value()) << bad;
    }
    EXPECT_FALSE(json::BytesOf(json::Value("Zm9v")).has_value()) << "bare text is no bytes";
    json::Value two = json::FromBytes(std::vector<std::uint8_t>{1});
    two.Set("other", 1);
    EXPECT_FALSE(json::BytesOf(two).has_value()) << "an object of more than the one member";
}
