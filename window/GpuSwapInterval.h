#pragma once

// The swap interval a GPU window presents with, and the rule that chooses it: policy only, nothing of a platform's
// API, so every platform's suite can hold it (tests/test_gpu_drawing.cpp). The back end that has a swap interval to set
// (WGL on Windows today) is told by SkiaGpuPresenter, which asks this (docs/window/gpu-drawing.md, "Pacing, and the
// swap interval").

#include <optional>
#include <string>

namespace reckless::window
{
/** The environment variable that overrides the swap interval, for diagnosis: `0`, `1` or `auto`. */
inline constexpr const char *kGpuSwapIntervalEnvironmentVariable = "RECKLESS_GPU_SWAP_INTERVAL";

/** What decides the swap interval. */
enum class SwapIntervalRequest
{
    /// The default: 1 while the window is full screen, 0 while it is windowed, where the back end sets an interval
    /// at all - WGL alone. EGL on Linux keeps 0 in both, and Metal's drawable is paced by the display link
    /// (docs/window/gpu-drawing.md, "Pacing, and the swap interval").
    kAuto,
    kNoWait,    ///< 0 always.
    kEveryBlank ///< 1 always.
};

/** The request an environment variable's value asks for: `0` and `1` as written, and anything else - `auto`, empty,
    unset, a word that is none of them - is the default, kAuto.
    @param value  the variable's value as core::EnvironmentVariable reads it; empty where it is not set
    @return       the request */
inline SwapIntervalRequest SwapIntervalRequestOf(const std::optional<std::string> &value)
{
    if (value == std::optional<std::string>("0"))
    {
        return SwapIntervalRequest::kNoWait;
    }
    if (value == std::optional<std::string>("1"))
    {
        return SwapIntervalRequest::kEveryBlank;
    }
    return SwapIntervalRequest::kAuto;
}

/** The swap interval to present with.
    @param request         what decides it
    @param is_full_screen  whether the window covers its monitor as the app's full screen
    @return                0 for a swap that does not wait for the display, 1 for one that waits for a blank */
constexpr int SwapIntervalFor(SwapIntervalRequest request, bool is_full_screen) noexcept
{
    switch (request)
    {
    case SwapIntervalRequest::kNoWait:
        return 0;
    case SwapIntervalRequest::kEveryBlank:
        return 1;
    case SwapIntervalRequest::kAuto:
        break;
    }
    return is_full_screen ? 1 : 0;
}
} // namespace reckless::window
