// The files the system hands a program (app/OpenFileQueue.h): held until the product can take them, then given in
// order. No AppKit: macOS's open-file events reach the queue through the same call these tests make.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "OpenFileQueue.h"

namespace
{
using reckless::app::OpenFileQueue;
using Batches = std::vector<std::vector<std::string>>;
} // namespace

TEST(OpenFileQueue, PathsThatComeBeforeAHandlerAreHeldAndGivenOnceItIsSetInTheOrderTheyCame)
{
    OpenFileQueue queue;
    queue.Receive({"/a/first.rsp"});
    queue.Receive({"/a/second.rsp", "/a/third.rsp"});
    EXPECT_EQ(queue.HeldBatches(), 2u);
    EXPECT_FALSE(queue.HasHandler());

    Batches heard;
    queue.SetHandler([&heard](const std::vector<std::string> &paths) { heard.push_back(paths); });
    EXPECT_EQ(heard, (Batches{{"/a/first.rsp"}, {"/a/second.rsp", "/a/third.rsp"}}));
    EXPECT_EQ(queue.HeldBatches(), 0u);

    queue.SetHandler([&heard](const std::vector<std::string> &paths) { heard.push_back(paths); });
    EXPECT_EQ(heard.size(), 2u) << "held paths are given once, never again";
}

TEST(OpenFileQueue, ADocumentOpenedWhileTheHandlerIsSetGoesStraightToIt)
{
    OpenFileQueue queue;
    Batches       heard;
    queue.SetHandler([&heard](const std::vector<std::string> &paths) { heard.push_back(paths); });
    EXPECT_TRUE(queue.HasHandler());
    queue.Receive({"/b/\xC3\xA9t\xC3\xA9.rsp"}); // UTF-8 goes through as it came
    EXPECT_EQ(heard, (Batches{{"/b/\xC3\xA9t\xC3\xA9.rsp"}}));
    EXPECT_EQ(queue.HeldBatches(), 0u);
}

TEST(OpenFileQueue, AnEmptyBatchIsNothingAndAnEmptyHandlerHoldsFilesAgain)
{
    OpenFileQueue queue;
    Batches       heard;
    queue.SetHandler([&heard](const std::vector<std::string> &paths) { heard.push_back(paths); });
    queue.Receive({});
    EXPECT_TRUE(heard.empty());

    queue.SetHandler(nullptr);
    EXPECT_FALSE(queue.HasHandler());
    queue.Receive({"/c/late.rsp"});
    EXPECT_TRUE(heard.empty()) << "the product's callback does not outlive its loop";
    EXPECT_EQ(queue.HeldBatches(), 1u);
}

TEST(OpenFileQueue, AFileThatArrivesWhileTheHandlerRunsIsGivenAfterItNotInsideIt)
{
    OpenFileQueue queue;
    Batches       heard;
    int           depth   = 0;
    int           deepest = 0;
    queue.SetHandler([&](const std::vector<std::string> &paths) {
        deepest = std::max(deepest, ++depth);
        heard.push_back(paths);
        if (paths.front() == "/d/one.rsp")
        {
            queue.Receive({"/d/two.rsp"});
        }
        --depth;
    });
    queue.Receive({"/d/one.rsp"});
    EXPECT_EQ(heard, (Batches{{"/d/one.rsp"}, {"/d/two.rsp"}}));
    EXPECT_EQ(deepest, 1) << "never re-entered";
}
