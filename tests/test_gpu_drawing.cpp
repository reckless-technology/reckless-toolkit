// What a window decides about drawing on the GPU (window/GpuDrawing.h), which needs no window, no GPU and no display
// and so is held on every platform and in the builds with no GUI: which a request asks for, the order the reasons it is
// refused are checked in, the environment's word, the GL versions that are drawn on, how often a lost GPU is made
// again, every reason said in words, and what a child window with no GPU path of its own answers.
#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <set>
#include <string>

#include "GpuDrawing.h"
#include "View.h"
#include "Window.h"

using reckless::window::ChosenDrawing;
using reckless::window::Drawing;
using reckless::window::DrawingFallback;
using reckless::window::DrawingRequest;
using reckless::window::DrawingState;

namespace
{
/** A back end to name in a request: the policy reads only whether there is one, never what it is. */
const reckless::window::GpuBackend *SomeBackEnd()
{
    static const int stand_in = 0;
    return reinterpret_cast<const reckless::window::GpuBackend *>(&stand_in);
}

/** A child window with nothing of a platform's: the defaults every platform's window inherits until it has a GPU path
    of its own, as macOS's and Linux's do. */
class WindowOfNoPlatform final : public reckless::window::ChildWindow
{
  public:
    void SetScale(float) override
    {
    }

    void SetFrameCallback(std::function<void()>) override
    {
    }
};
} // namespace

TEST(GpuDrawing, ADefaultRequestAsksForTheCpuRasterAndIsAnsweredNotAsked)
{
    const DrawingRequest unasked;
    EXPECT_EQ(unasked.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(unasked.gpu_backend, nullptr);
    for (const bool is_forced_off : {false, true})
    {
        for (const bool has_platform_gpu : {false, true})
        {
            const DrawingState state = ChosenDrawing(unasked, is_forced_off, has_platform_gpu);
            EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
            EXPECT_EQ(state.why, DrawingFallback::kNotAsked) << "nothing refused what was never asked for";
            EXPECT_TRUE(state.renderer.empty());
            EXPECT_EQ(state.contexts_lost, 0);
        }
    }
    const DrawingState never;
    EXPECT_EQ(never.drawing, Drawing::kCpuRaster) << "a window's state before anyone asks";
    EXPECT_EQ(never.why, DrawingFallback::kNotAsked);
}

TEST(GpuDrawing, TheGpuAskedForIsTriedOnlyWhereNothingRefusesItAndEachRefusalIsSaidInItsOrder)
{
    const DrawingRequest asked{Drawing::kGpu, SomeBackEnd()};
    const DrawingState   tried = ChosenDrawing(asked, false, true);
    EXPECT_EQ(tried.drawing, Drawing::kGpu) << "the platform is to try";
    EXPECT_EQ(tried.why, DrawingFallback::kNone);

    // The environment first, so a run with the GPU turned off says so on every platform; then the platform; then the
    // back end.
    EXPECT_EQ(ChosenDrawing(asked, true, true).why, DrawingFallback::kForcedOff);
    EXPECT_EQ(ChosenDrawing(asked, true, false).why, DrawingFallback::kForcedOff);
    EXPECT_EQ(ChosenDrawing({Drawing::kGpu, nullptr}, true, true).why, DrawingFallback::kForcedOff);
    EXPECT_EQ(ChosenDrawing(asked, false, false).why, DrawingFallback::kNoGpuOnThisPlatform);
    EXPECT_EQ(ChosenDrawing({Drawing::kGpu, nullptr}, false, false).why, DrawingFallback::kNoGpuOnThisPlatform);
    EXPECT_EQ(ChosenDrawing({Drawing::kGpu, nullptr}, false, true).why, DrawingFallback::kNotLinked);
    for (const bool is_forced_off : {false, true})
    {
        for (const bool has_platform_gpu : {false, true})
        {
            const DrawingState state = ChosenDrawing(asked, is_forced_off, has_platform_gpu);
            EXPECT_EQ(state.drawing == Drawing::kGpu, state.why == DrawingFallback::kNone)
                << "on the GPU exactly where nothing fell back";
        }
    }
}

TEST(GpuDrawing, OnlyOffInTheEnvironmentTurnsTheGpuOff)
{
    EXPECT_STREQ(reckless::window::kGpuDrawingEnvironmentVariable, "RECKLESS_GPU_DRAWING");
    EXPECT_TRUE(reckless::window::IsGpuDrawingForcedOff(std::optional<std::string>("off")));
    EXPECT_FALSE(reckless::window::IsGpuDrawingForcedOff(std::nullopt)) << "unset leaves it to the request";
    for (const char *other : {"", "on", "OFF", "0", "1", "cpu", "off "})
    {
        EXPECT_FALSE(reckless::window::IsGpuDrawingForcedOff(std::optional<std::string>(other)))
            << "'" << other << "' is not off, as RECKLESS_UPDATE_CHECK reads only off";
    }
}

TEST(GpuDrawing, AGlIsDrawnOnFromThreeAndWindowsOwnSoftwareGlIsNot)
{
    using reckless::window::IsGlNewEnough;
    EXPECT_TRUE(IsGlNewEnough("4.6.0 NVIDIA 610.62"));
    EXPECT_TRUE(IsGlNewEnough("3.0 Mesa 23.1"));
    EXPECT_TRUE(IsGlNewEnough("3.3 (Compatibility Profile) Mesa 24.0.5"));
    EXPECT_TRUE(IsGlNewEnough("OpenGL ES 3.2 Mesa 24.0"));
    EXPECT_TRUE(IsGlNewEnough("10.0"));
    EXPECT_TRUE(IsGlNewEnough("99999999999999999999.0")) << "a number past any GL's neither overflows nor refuses";
    EXPECT_FALSE(IsGlNewEnough("1.1.0")) << "Windows' own software GL, on a machine with no GPU driver";
    EXPECT_FALSE(IsGlNewEnough("2.1 Mesa 7.0"));
    EXPECT_FALSE(IsGlNewEnough("OpenGL ES 2.0"));
    EXPECT_FALSE(IsGlNewEnough(""));
    EXPECT_FALSE(IsGlNewEnough("no version"));
    EXPECT_FALSE(IsGlNewEnough(nullptr)) << "a context that names no version is not drawn on";
}

TEST(GpuDrawing, ALostGpuIsMadeAgainAFewTimesAndThenGivenUp)
{
    using reckless::window::kMostGpuContextsLost;
    using reckless::window::MayMakeTheGpuContextAgain;
    EXPECT_GE(kMostGpuContextsLost, 1) << "a driver that resets once is drawn on again";
    for (int lost = 1; lost <= kMostGpuContextsLost; ++lost)
    {
        EXPECT_TRUE(MayMakeTheGpuContextAgain(lost)) << lost;
    }
    EXPECT_FALSE(MayMakeTheGpuContextAgain(kMostGpuContextsLost + 1));
    EXPECT_FALSE(MayMakeTheGpuContextAgain(kMostGpuContextsLost + 100));
}

TEST(GpuDrawing, EveryReasonIsSaidInWordsOfItsOwn)
{
    std::set<std::string> said;
    for (const DrawingFallback why :
         {DrawingFallback::kNone, DrawingFallback::kNotAsked, DrawingFallback::kForcedOff, DrawingFallback::kNotLinked,
          DrawingFallback::kNoGpuOnThisPlatform, DrawingFallback::kNoPixelFormat, DrawingFallback::kNoContext,
          DrawingFallback::kGlTooOld, DrawingFallback::kNoSkiaInterface, DrawingFallback::kNoSkiaContext,
          DrawingFallback::kNoSurface, DrawingFallback::kContextLostTooOften, DrawingFallback::kContextNotMadeAgain})
    {
        const char *text = reckless::window::TextOf(why);
        ASSERT_NE(text, nullptr);
        EXPECT_NE(std::string(text), "");
        EXPECT_NE(std::string(text), "unknown");
        EXPECT_TRUE(said.insert(text).second) << "said twice: " << text;
    }
    EXPECT_STREQ(reckless::window::TextOf(static_cast<DrawingFallback>(999)), "unknown");
}

TEST(GpuDrawing, AWindowWithNoGpuPathOfItsOwnAnswersTheCpuRasterAndSaysWhy)
{
    WindowOfNoPlatform window;
    EXPECT_EQ(window.DrawingNow().why, DrawingFallback::kNotAsked);
    const DrawingState asked = window.SetDrawing({Drawing::kGpu, SomeBackEnd()});
    EXPECT_EQ(asked.drawing, Drawing::kCpuRaster);
    // The environment may turn the GPU off for the run this suite is in; either answer is a refusal said.
    EXPECT_TRUE(asked.why == DrawingFallback::kNoGpuOnThisPlatform || asked.why == DrawingFallback::kForcedOff)
        << reckless::window::TextOf(asked.why);
    EXPECT_EQ(window.SetDrawing({}).why, DrawingFallback::kNotAsked);
}
