#include "AppWindowSizes.h"

#include <algorithm>
#include <cmath>

namespace reckless::app
{
namespace
{
/** @param percent  a size, in percent
    @return         its zoom: exact for every size offered */
float ZoomOf(int percent)
{
    return static_cast<float>(percent) / 100.0f;
}
} // namespace

WindowSizes::WindowSizes(AppRoot &root, gfx::PointI design) : root_(root), design_(design)
{
}

void WindowSizes::Restore(int percent)
{
    windowed_percent_      = IsOffered(percent) ? percent : 100;
    const gfx::PointI size = root_.SetFixedZoom(design_, ZoomOf(windowed_percent_));
    root_.SetBounds({size.x, size.y});
}

void WindowSizes::SetWindow(AppWindow *window)
{
    window_          = window;
    room_            = {0, 0};
    was_full_screen_ = IsFullScreen();
    coming_back_.reset();
    RefreshRoom();
}

bool WindowSizes::Choose(int percent)
{
    if (!IsOffered(percent) || window_ == nullptr || !Fits(percent) || IsFullScreen())
    {
        return false;
    }
    const gfx::PointI size = root_.SetFixedZoom(design_, ZoomOf(percent));
    if (root_.width() != size.x || root_.height() != size.y)
    {
        window_->SetContentSizeOnScreen(size.x, size.y); // the root follows the window's resize
    }
    windowed_percent_ = percent;
    coming_back_.reset();
    RefreshRoom();
    return true;
}

void WindowSizes::Frame(double seconds)
{
    FollowFullScreen();
    elapsed_ += seconds;
    if (elapsed_ >= kRefreshSeconds)
    {
        RefreshRoom();
    }
}

WindowSizeMenu WindowSizes::Menu() const
{
    WindowSizeMenu menu;
    menu.has_window = window_ != nullptr;
    menu.in_force   = InForce();
    if (IsFullScreen())
    {
        return menu; // the size it will come back to ticked, and every size dim: none is chosen while full screen
    }
    for (std::size_t index = 0; index < kWindowSizes.size(); ++index)
    {
        menu.fits[index] = menu.has_window && Fits(kWindowSizes[index]);
    }
    if (!menu.has_window)
    {
        return menu;
    }
    // From the size in force, or - dragged to another - from the scale the window is nearest the design at, so a step
    // from a dragged window goes to the next size past it either way.
    const double     now = menu.in_force != 0
                               ? static_cast<double>(menu.in_force)
                               : 100.0 * std::min(static_cast<double>(root_.width()) / static_cast<double>(design_.x),
                                                  static_cast<double>(root_.height()) / static_cast<double>(design_.y));
    constexpr double kTolerance = 0.5; // half a percent: a window a pixel off a size is at it
    for (std::size_t index = 0; index < kWindowSizes.size(); ++index)
    {
        const int percent = kWindowSizes[index];
        if (menu.larger == 0 && percent > now + kTolerance && menu.fits[index])
        {
            menu.larger = percent;
        }
        if (percent < now - kTolerance && menu.fits[index])
        {
            menu.smaller = percent;
        }
    }
    return menu;
}

int WindowSizes::InForce() const
{
    // Full screen, or back from it and the size not held again yet: the size the window had, and comes back to.
    return IsFullScreen() || was_full_screen_ || coming_back_ ? windowed_percent_ : HeldByRoot();
}

int WindowSizes::HeldByRoot() const
{
    const std::optional<float> zoom = root_.fixed_zoom();
    if (!zoom)
    {
        return 0;
    }
    const int percent = static_cast<int>(std::lround(*zoom * 100.0f));
    return IsOffered(percent) ? percent : 0;
}

bool WindowSizes::IsOffered(int percent)
{
    return std::find(kWindowSizes.begin(), kWindowSizes.end(), percent) != kWindowSizes.end();
}

gfx::PointI WindowSizes::SizeAt(int percent) const
{
    return {static_cast<int>(std::lround(static_cast<double>(design_.x) * percent / 100.0)),
            static_cast<int>(std::lround(static_cast<double>(design_.y) * percent / 100.0))};
}

bool WindowSizes::Fits(int percent) const
{
    if (room_.x <= 0 || room_.y <= 0)
    {
        return true;
    }
    const gfx::PointI size = SizeAt(percent);
    return size.x <= room_.x && size.y <= room_.y;
}

bool WindowSizes::IsFullScreen() const
{
    return window_ != nullptr && window_->IsFullScreen();
}

void WindowSizes::FollowFullScreen()
{
    if (IsFullScreen())
    {
        was_full_screen_ = true; // the size it had kept, for the window it comes back to
        return;
    }
    if (was_full_screen_)
    {
        // Back: the size it had held again, at once where the window is at it already, else once it gets there.
        was_full_screen_ = false;
        if (IsOffered(windowed_percent_))
        {
            const gfx::PointI now{root_.width(), root_.height()};
            root_.SetFixedZoom(design_, ZoomOf(windowed_percent_));
            if (HeldByRoot() == 0)
            {
                coming_back_ = now;
            }
        }
    }
    if (coming_back_)
    {
        if (root_.width() == coming_back_->x && root_.height() == coming_back_->y)
        {
            return; // the window's own size not here yet
        }
        coming_back_.reset();
    }
    windowed_percent_ = HeldByRoot();
}

void WindowSizes::RefreshRoom()
{
    elapsed_ = 0.0;
    if (window_ != nullptr)
    {
        room_ = window_->LargestContent();
    }
}
} // namespace reckless::app
