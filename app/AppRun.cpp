#include "AppRun.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "FrameSteps.h"

namespace reckless::app
{
AppOptions ParseAppOptions(const std::vector<std::string> &arguments)
{
    AppOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string &argument = arguments[index];
        if (argument == "--smoke")
        {
            options.is_smoke = true;
            // An optional count of frames after it.
            if (index + 1 < arguments.size())
            {
                char      *end    = nullptr;
                const long frames = std::strtol(arguments[index + 1].c_str(), &end, 10);
                if (end != nullptr && end != arguments[index + 1].c_str() && *end == '\0' && frames > 0)
                {
                    options.smoke_frames = static_cast<int>(frames);
                    ++index;
                }
            }
        }
        else
        {
            options.unknown.push_back(argument);
        }
    }
    return options;
}

int RunApp(AppPlatform &platform, view::View &root, gfx::PointI minimum, const AppOptions &options,
           const std::string &name, const std::string &version, const ChildMaker &make_child, const AppRunHooks &hooks,
           std::optional<std::pair<int, int>> position)
{
    for (const std::string &argument : options.unknown)
    {
        std::fprintf(stderr, "%s: ignoring the argument '%s'\n", name.c_str(), argument.c_str());
    }
    if (options.is_smoke)
    {
        std::printf("%s %s\n", name.c_str(), version.c_str());
        std::fflush(stdout);
    }

    std::unique_ptr<AppWindow>           window;
    std::unique_ptr<window::ChildWindow> child;

    WindowOptions window_options;
    window_options.title     = name + "  v" + version;
    window_options.width     = root.width();
    window_options.height    = root.height();
    window_options.position  = position;
    window_options.menu_bar  = hooks.has_menu_bar;
    window_options.closing   = [&platform] { platform.CallSoon([&platform] { platform.Quit(); }); };
    window_options.constrain = [minimum](gfx::PointF proposed) {
        return gfx::PointF{std::max(proposed.x, static_cast<float>(minimum.x)),
                           std::max(proposed.y, static_cast<float>(minimum.y))};
    };
    // Held to what fits the display it opens on, so a window sized on a larger display - or the first run's design
    // size on a small laptop - never opens past the screen; never below the least it may be.
    if (const gfx::PointI largest = platform.LargestContent(window_options); largest.x > 0 && largest.y > 0)
    {
        const int width  = std::max(minimum.x, std::min(root.width(), largest.x));
        const int height = std::max(minimum.y, std::min(root.height(), largest.y));
        if (width != root.width() || height != root.height())
        {
            root.SetBounds({width, height});
            window_options.width  = width;
            window_options.height = height;
        }
    }
    window_options.resized = [&root, &window, &child](int width, int height) {
        if (width == root.width() && height == root.height())
        {
            return;
        }
        root.SetBounds({width, height});
        if (child && window)
        {
            child->SetScale(window->Scale());
        }
    };
    window_options.rescaled = [&child](float scale) {
        if (child)
        {
            child->SetScale(scale);
        }
    };
    window = platform.OpenWindow(window_options);
    if (window)
    {
        child = make_child(window->Handle(), root, root.width(), root.height(), window->Scale(), platform.ChildLoop());
    }
    if (!child)
    {
        std::fprintf(stderr, "%s: no window to draw in - no display, or none Skia can draw on\n", name.c_str());
        return 1;
    }
    child->TakeDropsFor(window->Handle());
    if (hooks.has_menu_bar)
    {
        child->HandOnKeysTo(window->Handle());
    }
    if (hooks.on_open)
    {
        hooks.on_open(*window);
    }

    int  drawn       = 0;
    bool is_quitting = false;
    child->SetFrameCallback([&, steps = view::FrameSteps{}]() mutable {
        const double seconds = steps.Next();
        if (hooks.on_frame)
        {
            hooks.on_frame(seconds);
        }
        if (options.is_smoke && !is_quitting && ++drawn >= options.smoke_frames)
        {
            is_quitting = true;
            platform.CallSoon([&platform] { platform.Quit(); });
        }
    });
    platform.SetQuitHandler([&platform] { platform.Quit(); });
    const auto started = std::chrono::steady_clock::now();
    platform.StartTimer(1000 / 30, [&] {
        // A smoke run whose window draws nothing in twenty seconds has no display to draw on.
        if (options.is_smoke && drawn == 0 && std::chrono::steady_clock::now() - started > std::chrono::seconds(20))
        {
            platform.Quit();
        }
    });

    const int code = platform.Run();
    if (hooks.on_end)
    {
        hooks.on_end(window ? std::optional<std::pair<int, int>>(window->Position()) : std::nullopt);
    }
    child->SetFrameCallback(nullptr);
    platform.SetQuitHandler(nullptr);
    child.reset();
    window.reset();
    if (options.is_smoke)
    {
        std::printf("%s: the window drew %d of %d frames\n", name.c_str(), drawn, options.smoke_frames);
        return code != 0 ? code : (drawn >= options.smoke_frames ? 0 : 3);
    }
    return code;
}
} // namespace reckless::app
