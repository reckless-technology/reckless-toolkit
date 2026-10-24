// The files a binary carries inside itself (cmake/RecklessEmbed.cmake, window/Embedded.h): every name its table
// lists found by reckless::embedded::Find as its file's bytes, whole, with a zero past the end that the size does not
// count, in the order the table was given them, and a name it does not list found empty. Built twice, on two tables
// that name one file in common - the serif, whose bytes every plugin's modules share - so a file's bytes made once for
// the configuration are each table's own. The names and the files are the build's, in "EmbeddedFiles.h"
// (the toolkit's CMakeLists.txt writes it beside the table it lists).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "Embedded.h"
#include "EmbeddedFiles.h"

#include <gtest/gtest.h>

namespace
{
std::vector<std::uint8_t> Read(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST(Embedded, TheTableListsEveryFileInTheOrderGiven)
{
    ASSERT_EQ(reckless::embedded::kEntryCount, std::size(reckless::test::kEmbeddedFiles));
    for (std::size_t i = 0; i < std::size(reckless::test::kEmbeddedFiles); ++i)
    {
        EXPECT_STREQ(reckless::embedded::kEntries[i].name, reckless::test::kEmbeddedFiles[i].name);
    }
}

TEST(Embedded, EveryNameIsFoundAsItsFilesBytesWithAZeroPastTheEnd)
{
    for (const auto &file : reckless::test::kEmbeddedFiles)
    {
        const std::vector<std::uint8_t> expected = Read(file.path);
        ASSERT_FALSE(expected.empty()) << file.path;
        const auto found = reckless::embedded::Find(file.name);
        ASSERT_EQ(found.size(), expected.size()) << file.name;
        EXPECT_TRUE(std::equal(found.begin(), found.end(), expected.begin())) << file.name;
        EXPECT_EQ(found.data()[found.size()], 0) << file.name;
    }
}

TEST(Embedded, ANameNotListedIsFoundEmpty)
{
    EXPECT_TRUE(reckless::embedded::Find("not-embedded.png").empty());
    EXPECT_TRUE(reckless::embedded::Find("").empty());
}
