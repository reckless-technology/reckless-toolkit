// The licence check (licence/): the format held to the shared test vectors - made by tools/licence.py on
// OpenSSL's Ed25519, so the checker here, the licensing service and the script agree on bytes none of them alone made -
// Ed25519 itself held to RFC 8032's first test vector, every altered byte of a licence refused, and the store that
// keeps a machine's licence: written only when valid, owner-only, the machine's file read where the user's holds
// none, a change by another process found by Poll.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <monocypher-ed25519.h>

#include "Json.h"
#include "Licence.h"
#include "LicenceStore.h"
#include "PublicKeys.h"
#include "ScratchPath.h"

namespace
{
using namespace reckless;
using namespace reckless::licence;

std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::uint8_t> FromHex(std::string_view hex)
{
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return bytes;
}

template <std::size_t N> std::array<std::uint8_t, N> ArrayFromHex(std::string_view hex)
{
    const std::vector<std::uint8_t> bytes = FromHex(hex);
    std::array<std::uint8_t, N>     out{};
    EXPECT_EQ(bytes.size(), N) << hex;
    std::copy_n(bytes.begin(), std::min(N, bytes.size()), out.begin());
    return out;
}

Verdict VerdictNamed(std::string_view name)
{
    if (name == "valid")
    {
        return Verdict::kValid;
    }
    if (name == "unknown-key")
    {
        return Verdict::kUnknownKey;
    }
    if (name == "bad-signature")
    {
        return Verdict::kBadSignature;
    }
    if (name == "wrong-product")
    {
        return Verdict::kWrongProduct;
    }
    if (name == "revoked")
    {
        return Verdict::kRevoked;
    }
    if (name == "older-major-version")
    {
        return Verdict::kOlderMajorVersion;
    }
    EXPECT_EQ(name, "malformed") << "a verdict the vectors name and the suite does not know";
    return Verdict::kMalformed;
}

/** The vectors file read once: its keys, its rules and its documents, kept alive for the spans Rules holds. */
struct Vectors
{
    core::json::Value             document;
    std::vector<std::string>      key_ids;
    std::vector<PublicKey>        all_keys;
    std::vector<PublicKey>        rule_keys;
    std::vector<std::string>      revoked_text;
    std::vector<std::string_view> revoked;
    Rules                         rules;

    Vectors()
    {
        core::json::ParseResult parsed = core::json::Parse(ReadFile(RECKLESS_LICENCE_VECTORS));
        EXPECT_TRUE(parsed.IsOk()) << "could not read " << RECKLESS_LICENCE_VECTORS;
        document = std::move(parsed.value);
        for (const core::json::Value &key : *document.Find("keys")->AsArray())
        {
            key_ids.push_back(key.StringOr("kid", ""));
        }
        std::size_t index = 0;
        for (const core::json::Value &key : *document.Find("keys")->AsArray())
        {
            all_keys.push_back(PublicKey{key_ids[index++], ArrayFromHex<32>(key.StringOr("public", ""))});
        }
        const core::json::Value &rule = *document.Find("rules");
        for (const core::json::Value &id : *rule.Find("keys")->AsArray())
        {
            for (const PublicKey &key : all_keys)
            {
                if (key.id == id.AsString())
                {
                    rule_keys.push_back(key);
                }
            }
        }
        for (const core::json::Value &serial : *rule.Find("revoked")->AsArray())
        {
            revoked_text.push_back(serial.AsString());
        }
        for (const std::string &serial : revoked_text)
        {
            revoked.push_back(serial);
        }
        rules.keys        = rule_keys;
        rules.revoked     = revoked;
        rules.suite_major = static_cast<int>(rule.IntOr("suite_major", -1));
    }
};

const Vectors &TheVectors()
{
    static const Vectors vectors;
    return vectors;
}

std::string GoodLicence()
{
    return TheVectors().document.Find("licences")->AsArray()->front().StringOr("text", "");
}

/** A folder of the test's own under the system's temporary folder, gone when the test ends. */
class TempFolder
{
  public:
    TempFolder()
    {
        static std::atomic<int> count{0};
        path_ = reckless::test::ScratchPath("reckless-licence-test-" + std::to_string(count.fetch_add(1)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void WriteText(const std::filesystem::path &file, std::string_view text)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}
} // namespace

TEST(Ed25519, VerifiesRfc8032TestVectorOne)
{
    // RFC 8032 section 7.1, TEST 1: the empty message.
    const auto key       = ArrayFromHex<32>("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const auto signature = ArrayFromHex<64>("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590"
                                            "a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    EXPECT_TRUE(VerifyEd25519(signature, key, {}));

    auto altered = signature;
    altered[63] ^= 0x01;
    EXPECT_FALSE(VerifyEd25519(altered, key, {}));
    const std::uint8_t one = 0;
    EXPECT_FALSE(VerifyEd25519(signature, key, std::span(&one, 1)));
}

TEST(LicenceVectors, EveryCaseHasTheVerdictTheVectorsGive)
{
    const Vectors &vectors = TheVectors();
    ASSERT_EQ(vectors.rules.keys.size(), 1u);
    int cases = 0;
    for (const core::json::Value &entry : *vectors.document.Find("licences")->AsArray())
    {
        const std::string name    = entry.StringOr("case", "");
        const Checked     checked = Check(entry.StringOr("text", ""), vectors.rules);
        EXPECT_EQ(checked.verdict, VerdictNamed(entry.StringOr("verdict", ""))) << name;
        ++cases;
    }
    std::printf("%d licence cases checked against the vectors\n", cases);
    EXPECT_GE(cases, 20);
}

TEST(LicenceVectors, ThePayloadOfAValidLicenceIsRead)
{
    const Checked checked = Check(GoodLicence(), TheVectors().rules);
    ASSERT_EQ(checked.verdict, Verdict::kValid);
    ASSERT_TRUE(checked.licence.has_value());
    EXPECT_EQ(checked.licence->kid, "test-1");
    EXPECT_EQ(checked.licence->name, "Jo Buyer");
    EXPECT_EQ(checked.licence->email, "jo.buyer@example.com");
    EXPECT_EQ(checked.licence->product, kSuiteProduct);
    EXPECT_EQ(checked.licence->major, 1);
    EXPECT_EQ(checked.licence->issued, "2026-11-02");
    EXPECT_TRUE(checked.licence->serial.starts_with("RT-"));
}

TEST(LicenceVectors, MonocypherSignsEveryVectorToTheSameBytes)
{
    // Ed25519 is deterministic: the same key and message give one signature, so a signer that agrees with the vectors
    // agrees byte for byte. This holds Monocypher's signing to OpenSSL's, as the licensing service is held.
    const Vectors &vectors = TheVectors();
    for (const core::json::Value &key : *vectors.document.Find("keys")->AsArray())
    {
        auto                         seed = ArrayFromHex<32>(key.StringOr("seed", ""));
        std::array<std::uint8_t, 64> secret{};
        std::array<std::uint8_t, 32> public_key{};
        crypto_ed25519_key_pair(secret.data(), public_key.data(), seed.data());
        EXPECT_EQ(public_key, ArrayFromHex<32>(key.StringOr("public", ""))) << key.StringOr("kid", "");
    }

    auto seed = ArrayFromHex<32>(vectors.document.Find("keys")->AsArray()->front().StringOr("seed", ""));
    std::array<std::uint8_t, 64> secret{};
    std::array<std::uint8_t, 32> public_key{};
    crypto_ed25519_key_pair(secret.data(), public_key.data(), seed.data());
    for (const core::json::Value &entry : *vectors.document.Find("signing")->AsArray())
    {
        const std::string payload = entry.StringOr("payload", "");
        const std::string signed_text =
            std::string(kPrefix) +
            EncodeBase64Url(std::span(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()));
        std::array<std::uint8_t, 64> signature{};
        crypto_ed25519_sign(signature.data(), secret.data(), reinterpret_cast<const std::uint8_t *>(signed_text.data()),
                            signed_text.size());
        EXPECT_EQ(signed_text + "." + EncodeBase64Url(signature), entry.StringOr("licence", ""));
    }
}

TEST(Licence, EveryAlteredCharacterIsRefused)
{
    const std::string good    = GoodLicence();
    int               refused = 0;
    for (std::size_t i = 0; i < good.size(); ++i)
    {
        std::string altered   = good;
        altered[i]            = altered[i] == 'A' ? 'B' : 'A';
        const Checked checked = Check(altered, TheVectors().rules);
        EXPECT_NE(checked.verdict, Verdict::kValid) << "character " << i << " altered still checks";
        refused += checked.verdict != Verdict::kValid;
    }
    std::printf("%d of %zu single-character alterations refused\n", refused, good.size());
}

TEST(Licence, NoProductionKeyAcceptsATestLicence)
{
    Rules production;
    production.keys    = kProductionKeys;
    production.revoked = kRevokedSerials;
    for (const PublicKey &key : kProductionKeys)
    {
        EXPECT_NE(key.id.substr(0, 5), "test-") << "a test key among the production keys";
    }
    const Verdict verdict = Check(GoodLicence(), production).verdict;
    EXPECT_TRUE(verdict == Verdict::kUnknownKey || verdict == Verdict::kBadSignature);
}

TEST(Licence, TextPastTheLimitIsRefusedUnread)
{
    EXPECT_EQ(Check(std::string(kMaxTextLength + 1, ' ') + GoodLicence(), TheVectors().rules).verdict,
              Verdict::kMalformed);
}

TEST(Base64Url, RoundTripsAndRefusesEveryOtherSpelling)
{
    for (std::size_t length = 0; length < 70; ++length)
    {
        std::vector<std::uint8_t> bytes(length);
        for (std::size_t i = 0; i < length; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(i * 37 + 11);
        }
        const std::string text = EncodeBase64Url(bytes);
        EXPECT_EQ(text.find_first_of("+/="), std::string::npos);
        const auto back = DecodeBase64Url(text);
        ASSERT_TRUE(back.has_value()) << length;
        EXPECT_EQ(*back, bytes);
    }
    EXPECT_FALSE(DecodeBase64Url("QQ==").has_value()) << "padding";
    EXPECT_FALSE(DecodeBase64Url("Q").has_value()) << "a lone character";
    EXPECT_FALSE(DecodeBase64Url("QR").has_value()) << "bits past the last byte";
    EXPECT_FALSE(DecodeBase64Url("a+b/").has_value()) << "standard base64's alphabet";
    EXPECT_TRUE(DecodeBase64Url("QQ").has_value());
}

TEST(LicenceStore, AValidLicenceIsWrittenOwnerOnlyAndHolds)
{
    TempFolder folder;
    Store      store(folder.path() / "user" / "Licence.json", folder.path() / "machine" / "Licence.json",
                     TheVectors().rules);
    EXPECT_FALSE(store.licensed());
    EXPECT_EQ(store.status().source, Source::kNone);
    const std::uint64_t before = store.revision();

    const std::string wrapped = GoodLicence().substr(0, 50) + "\r\n" + GoodLicence().substr(50) + "\n";
    EXPECT_EQ(store.Install(wrapped).verdict, Verdict::kValid);
    EXPECT_TRUE(store.licensed());
    EXPECT_EQ(store.status().source, Source::kUser);
    EXPECT_GT(store.revision(), before);

    const auto document = core::ReadJsonFile(store.user_file());
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(document->StringOr("licence", ""), GoodLicence()) << "the file holds the line, its white space gone";
#if !defined(_WIN32)
    const auto perms = std::filesystem::status(store.user_file()).permissions();
    EXPECT_EQ(perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
              std::filesystem::perms::none);
#endif

    Store again(store.user_file(), folder.path() / "machine" / "Licence.json", TheVectors().rules);
    EXPECT_TRUE(again.licensed()) << "a fresh store reads the file the first one wrote";
}

TEST(LicenceStore, AnInvalidLicenceNeverDisplacesAValidOne)
{
    TempFolder folder;
    Store      store(folder.path() / "Licence.json", folder.path() / "machine.json", TheVectors().rules);
    ASSERT_EQ(store.Install(GoodLicence()).verdict, Verdict::kValid);
    const std::string written = ReadFile(store.user_file());

    std::string altered = GoodLicence();
    altered[30]         = altered[30] == 'A' ? 'B' : 'A';
    EXPECT_NE(store.Install(altered).verdict, Verdict::kValid);
    EXPECT_NE(store.Install("not a licence").verdict, Verdict::kValid);
    EXPECT_EQ(ReadFile(store.user_file()), written);
    EXPECT_TRUE(store.licensed());
}

TEST(LicenceStore, RemoveTakesTheUsersLicenceOff)
{
    TempFolder folder;
    Store      store(folder.path() / "Licence.json", folder.path() / "machine.json", TheVectors().rules);
    ASSERT_EQ(store.Install(GoodLicence()).verdict, Verdict::kValid);
    EXPECT_TRUE(store.Remove());
    EXPECT_FALSE(std::filesystem::exists(store.user_file()));
    EXPECT_FALSE(store.licensed());
    EXPECT_EQ(store.status().source, Source::kNone);
}

TEST(LicenceStore, TheMachinesLicenceHoldsWhereTheUsersFileHasNone)
{
    TempFolder folder;
    const auto machine = folder.path() / "machine" / "Licence.json";
    WriteText(machine, GoodLicence() + "\n"); // the bare text of a licence file, copied in by an administrator
    Store store(folder.path() / "user" / "Licence.json", machine, TheVectors().rules);
    EXPECT_TRUE(store.licensed());
    EXPECT_EQ(store.status().source, Source::kMachine);

    // A user file holding a licence that does not check: the machine's still holds.
    WriteText(store.user_file(), "{\"licence\": \"RTL1.nonsense.nonsense\"}\n");
    store.Reload();
    EXPECT_TRUE(store.licensed());
    EXPECT_EQ(store.status().source, Source::kMachine);
}

TEST(LicenceStore, AnInvalidFileIsReportedWithItsReason)
{
    TempFolder  folder;
    std::string altered = GoodLicence();
    altered[30]         = altered[30] == 'A' ? 'B' : 'A';
    WriteText(folder.path() / "Licence.json", "{\"licence\": \"" + altered + "\"}\n");
    Store store(folder.path() / "Licence.json", folder.path() / "machine.json", TheVectors().rules);
    EXPECT_FALSE(store.licensed());
    EXPECT_EQ(store.status().source, Source::kUser);
    EXPECT_NE(store.status().checked.verdict, Verdict::kValid);
}

TEST(LicenceStore, PollFindsAWriteByAnotherProcess)
{
    TempFolder folder;
    Store      store(folder.path() / "Licence.json", folder.path() / "machine.json", TheVectors().rules);
    EXPECT_FALSE(store.Poll()) << "nothing moved";
    EXPECT_FALSE(store.licensed());

    // Another process installs a licence: the stamp moves and the next poll reads it.
    WriteText(store.user_file(), "{\"licence\": \"" + GoodLicence() + "\"}\n");
    EXPECT_TRUE(store.Poll());
    EXPECT_TRUE(store.licensed());
    EXPECT_FALSE(store.Poll());

    std::filesystem::remove(store.user_file());
    EXPECT_TRUE(store.Poll());
    EXPECT_FALSE(store.licensed());
}
