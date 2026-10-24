#include "AppRoot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <utility>

#include "LookMetrics.h"

namespace reckless::app
{
namespace
{
/** @return  seconds on the steady clock */
double SteadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

AppRoot::AppRoot(view::View &content, view::TooltipStyle style, Clock clock, gfx::PointI layout)
    : content_(content), clock_(clock ? std::move(clock) : Clock(SteadySeconds)), layout_(layout), zoom_(content),
      tooltip_(std::move(style)), tracker_(look::kTooltipDelayMs / 1000.0)
{
    SetOpaque(true);
    AddChild(zoom_);
    AddChild(tooltip_); // last: in front of everything, taking no mouse
    zoom_.watch = [this](view::PointerEvent event, gfx::PointF at) {
        using view::PointerEvent;
        if (event != PointerEvent::kExit)
        {
            pointer_ = at;
        }
        switch (event)
        {
        case PointerEvent::kMove:
            tracker_.PointerAt(at);
            if (tooltip_.visible() && tooltip_.bounds().ToFloat().Contains(at))
            {
                tracker_.Hide(clock_()); // onto the tip: it goes
                ShowTracked();
            }
            break;
        case PointerEvent::kExit:
            tracker_.PointerLeft();
            break;
        case PointerEvent::kDown:
            tracker_.Pressed(at);
            break;
        case PointerEvent::kUp:
            tracker_.Released();
            break;
        case PointerEvent::kWheel:
            tracker_.Wheeled();
            break;
        }
    };
    SetBounds({content_.width(), content_.height()});
    last_tick_ = clock_();
}

gfx::PointI AppRoot::SetFixedZoom(gfx::PointI design, float zoom)
{
    design = {std::max(1, design.x), std::max(1, design.y)};
    const gfx::PointI size{std::max(1, static_cast<int>(std::lround(static_cast<float>(design.x) * zoom))),
                           std::max(1, static_cast<int>(std::lround(static_cast<float>(design.y) * zoom)))};
    fixed_ = FixedZoom{design, zoom, size};
    if (width() == size.x && height() == size.y)
    {
        Resized(); // the root that size already: no resize will come to lay it out
    }
    return size;
}

void AppRoot::Resized()
{
    if (fixed_ && width() == fixed_->size.x && height() == fixed_->size.y)
    {
        if (content_.width() != fixed_->design.x || content_.height() != fixed_->design.y)
        {
            content_.SetBounds({fixed_->design.x, fixed_->design.y});
        }
        zoom_.SetZoom(fixed_->zoom);
        zoom_.SetBounds(LocalBounds());
        return;
    }
    fixed_.reset(); // sized to anything else: the layout's own again

    // The zoom that brings the layout's least within the window, whichever side is short; one where it fits.
    float zoom = 1.0f;
    if (layout_.x > 0 && layout_.y > 0 && width() > 0 && height() > 0)
    {
        zoom = std::min({1.0f, static_cast<float>(width()) / static_cast<float>(layout_.x),
                         static_cast<float>(height()) / static_cast<float>(layout_.y)});
    }
    const int content_width  = static_cast<int>(std::ceil(static_cast<float>(width()) / zoom));
    const int content_height = static_cast<int>(std::ceil(static_cast<float>(height()) / zoom));
    if (content_.width() != content_width || content_.height() != content_height)
    {
        content_.SetBounds({content_width, content_height});
    }
    zoom_.SetZoom(zoom);
    zoom_.SetBounds(LocalBounds());
}

void AppRoot::Frame()
{
    const double now = clock_();
    if (now - last_tick_ < view::TooltipTracker::kTickSeconds)
    {
        return;
    }
    last_tick_ = now;
    view::TooltipTracker::Under under;
    under.view = this;
    if (const view::View *found = view::ViewAt(*this, pointer_.ToNearestInt()))
    {
        under.view = found;
    }
    if (under.view == &zoom_)
    {
        under.tip = view::TipOf(zoom_.router().hovered());
    }
    tracker_.Tick(now, under);
    ShowTracked();
}

void AppRoot::ShowTracked()
{
    if (!tracker_.showing())
    {
        if (tooltip_.visible())
        {
            tooltip_.Hide();
        }
        return;
    }
    if (tooltip_.visible() && shows_ == tracker_.shows())
    {
        return;
    }
    shows_                          = tracker_.shows();
    const view::Host         *h     = host();
    const view::TooltipStyle &style = tooltip_.style();
    view::TooltipLayout       text  = view::LayOutTooltip(tracker_.tip(), style, [h, &style](std::string_view line) {
        return h != nullptr ? h->TextWidth(style.font, line)
                            : 0.5f * style.font.height * static_cast<float>(line.size());
    });
    const gfx::RectI          at    = view::TooltipBounds(text, tracker_.shown_at(), LocalBounds());
    tooltip_.Show(tracker_.tip(), std::move(text), at);
}
} // namespace reckless::app
