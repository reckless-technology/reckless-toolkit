// WriteWhole's durability, as far as a test can show it without pulling the plug: the bytes land, nothing is left
// beside them, a failure leaves the old file whole, and - on Linux, under strace, from CMake (check_write_order.cmake)
// - that the file is flushed, then renamed, then its folder flushed, in that order.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "JsonFile.h"
#include "ScratchPath.h"

#include <gtest/gtest.h>

namespace
{
namespace core = reckless::core;

/** A scratch folder of this suite's own, emptied on the way in and out. */
class DurableScratch
{
  public:
    DurableScratch() : folder_(reckless::test::ScratchPath("reckless-core-durable-write-test"))
    {
        std::error_code ignored;
        std::filesystem::remove_all(folder_, ignored);
        std::filesystem::create_directories(folder_, ignored);
    }

    ~DurableScratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(folder_, ignored);
    }

    DurableScratch(const DurableScratch &)            = delete;
    DurableScratch &operator=(const DurableScratch &) = delete;

    const std::filesystem::path &folder() const
    {
        return folder_;
    }

    /** @return the names in the folder, sorted */
    std::vector<std::string> Names() const
    {
        std::vector<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator(folder_))
        {
            names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

  private:
    std::filesystem::path folder_;
};

TEST(CoreDurableWrite, ReplacesTheContentWholeAndLeavesNoTemporary)
{
    DurableScratch     scratch;
    const auto         file    = scratch.folder() / "project.json";
    core::WriteFailure failure = core::WriteFailure::kOpen;
    ASSERT_TRUE(core::WriteWhole(file, "first, and the longer of the two", &failure));
    EXPECT_EQ(failure, core::WriteFailure::kNone);
    ASSERT_TRUE(core::WriteWhole(file, "second", &failure));
    EXPECT_EQ(core::ReadBytes(file), "second") << "replaced, not appended over, and not left long";
    const std::string big(5 * 1024 * 1024 + 3, '\x5a');
    ASSERT_TRUE(core::WriteWhole(file, big));
    EXPECT_EQ(core::ReadBytes(file), big) << "a large file reaches the device whole";
    ASSERT_TRUE(core::WriteWhole(file, ""));
    EXPECT_EQ(core::ReadBytes(file), "") << "an empty write is a real, empty file";
    EXPECT_EQ(scratch.Names(), (std::vector<std::string>{"project.json"}));
}

TEST(CoreDurableWrite, AFailureLeavesTheOldFileIntactAndNothingBeside)
{
    DurableScratch     scratch;
    core::WriteFailure failure = core::WriteFailure::kNone;

    // The folder is missing: stopped at the temporary, and nothing was made.
    EXPECT_FALSE(core::WriteWhole(scratch.folder() / "absent" / "x.json", "x", &failure));
    EXPECT_EQ(failure, core::WriteFailure::kOpen);

    // The name is a folder holding a file: stopped at the rename, five tries in; that file and the temporary's removal.
    const auto taken = scratch.folder() / "taken.json";
    std::filesystem::create_directories(taken);
    {
        std::ofstream(taken / "inside", std::ios::binary) << "the old content";
    }
    EXPECT_FALSE(core::WriteWhole(taken, "new bytes", &failure));
    EXPECT_EQ(failure, core::WriteFailure::kReplace);
    EXPECT_EQ(core::ReadBytes(taken / "inside"), "the old content");
    EXPECT_EQ(scratch.Names(), (std::vector<std::string>{"taken.json"})) << "no temporary left";
}

TEST(CoreDurableWrite, ABareFileNameIsWrittenInTheCurrentFolder)
{
    DurableScratch scratch;
    const auto     before = std::filesystem::current_path();
    std::filesystem::current_path(scratch.folder());
    const bool isWritten = core::WriteWhole("bare.json", "x");
    std::filesystem::current_path(before);
    ASSERT_TRUE(isWritten);
    EXPECT_EQ(scratch.Names(), (std::vector<std::string>{"bare.json"}));
}

TEST(CoreDurableWrite, OrderMarker)
{
    // The write check_write_order.cmake traces: strace runs this test alone and reads the order of the calls.
    DurableScratch scratch;
    ASSERT_TRUE(core::WriteWhole(scratch.folder() / "durable-order-marker.bin", "marker"));
}
} // namespace
