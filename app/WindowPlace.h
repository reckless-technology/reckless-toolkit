#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "Geometry.h"

namespace reckless::app
{
/**
    Where the Standalone's windows go so that all of each shows, as a JUCE Standalone places its own, written against no
    platform: each platform hands in its displays' work areas - what a menu bar, a dock or a task bar leave of each -
    and its windows' frames, in one coordinate space with y down.
*/

/** The work area `frame` overlaps most, else the one nearest its centre; null with no areas.
    @param frame  the window's frame, in screen pixels, y down
    @param areas  the displays' work areas, in the same coordinates; the span must outlive the returned pointer
    @return       a pointer into `areas`, or nullptr when `areas` is empty */
inline const gfx::RectI *AreaFor(gfx::RectI frame, std::span<const gfx::RectI> areas)
{
    const gfx::RectI *best    = nullptr;
    std::int64_t      overlap = 0;
    for (const gfx::RectI &area : areas)
    {
        const gfx::RectI   common = area.Intersection(frame);
        const std::int64_t size =
            common.IsEmpty() ? 0
                             : static_cast<std::int64_t>(common.width()) * static_cast<std::int64_t>(common.height());
        if (size > overlap)
        {
            best    = &area;
            overlap = size;
        }
    }
    if (best != nullptr)
    {
        return best;
    }
    std::int64_t nearest = std::numeric_limits<std::int64_t>::max();
    for (const gfx::RectI &area : areas)
    {
        const auto gap = [](int at, int low, int high) -> std::int64_t {
            return at < low ? low - at : (at > high ? at - high : 0);
        };
        const std::int64_t dx       = gap(frame.CentreX(), area.x(), area.right());
        const std::int64_t dy       = gap(frame.CentreY(), area.y(), area.bottom());
        const std::int64_t distance = dx * dx + dy * dy;
        if (distance < nearest)
        {
            best    = &area;
            nearest = distance;
        }
    }
    return best;
}

/** `frame` moved wholly onto `area`, in from any edge it crosses; a frame larger than the area keeps its top left on
    it, so its title bar shows - a JUCE Standalone's limit on each axis.
    @param frame  the window's frame, in screen pixels
    @param area   the work area to move onto, in the same coordinates
    @return       `frame` at its new position, its size unchanged */
inline gfx::RectI MovedOnto(gfx::RectI frame, gfx::RectI area)
{
    const int x = std::clamp(frame.x(), area.x(), std::max(area.x(), area.right() - frame.width()));
    const int y = std::clamp(frame.y(), area.y(), std::max(area.y(), area.bottom() - frame.height()));
    return frame.WithPosition(x, y);
}

/** Where a window put back at `frame` - its place when the app last ended - goes: wholly onto the work area it overlaps
    most, else the nearest, as a JUCE Standalone puts back its window, so a window left half on a display since
    unplugged comes back whole. The frame as it is with no areas.
    @param frame  the window's saved frame, in screen pixels
    @param areas  the displays' work areas, in the same coordinates
    @return       the frame to open the window at, its size unchanged */
inline gfx::RectI PlaceWithin(gfx::RectI frame, std::span<const gfx::RectI> areas)
{
    const gfx::RectI *area = AreaFor(frame, areas);
    return area != nullptr ? MovedOnto(frame, *area) : frame;
}

/** The largest content no larger than `content` whose window - the content with `decoration` round it, the title bar
    and the borders - fits inside `area`: a window sized from settings saved on a larger display brought within the
    one it opens on. Each side is held to at least one.
    @param content     the content's size, in the area's units
    @param decoration  the frame's own width and height round the content, in the same units
    @param area        the work area the window opens on
    @return            the content's size, no larger than `content` */
inline gfx::PointI FittedContent(gfx::PointI content, gfx::PointI decoration, gfx::RectI area)
{
    return {std::max(1, std::min(content.x, area.width() - decoration.x)),
            std::max(1, std::min(content.y, area.height() - decoration.y))};
}

/** `design` design pixels in the display's pixels at `scale`, to the nearest: what a window's content of that size
    takes on a display, as the Windows and Linux windows size it.
    @param design  a length in design pixels
    @param scale   the display's pixels per design pixel
    @return        the length in the display's pixels */
inline int PixelsOfDesign(int design, float scale)
{
    return static_cast<int>(std::lround(static_cast<float>(design) * scale));
}

/** The most design pixels whose length in the display's pixels at `scale` (PixelsOfDesign) is no more than `pixels`:
    the content's side that fits in that room, rounded and never past it - 1106 pixels at 1.25 is 885 design pixels,
    which a window takes as 1106, where a truncated 884.8 would give a pixel short.
    @param pixels  the room, in the display's pixels
    @param scale   the display's pixels per design pixel; one where it is not more than nothing
    @return        the length in design pixels; less than one where even one does not fit */
inline int DesignWithinPixels(int pixels, float scale)
{
    if (!(scale > 0.0f))
    {
        scale = 1.0f;
    }
    // The nearest first, then stepped to the most that fits: for a scale of one or more the nearest is it or one past
    // it, and below one a step or two up may be needed.
    int design = static_cast<int>(std::lround(static_cast<float>(pixels) / scale));
    while (design > 0 && PixelsOfDesign(design, scale) > pixels)
    {
        --design;
    }
    while (PixelsOfDesign(design + 1, scale) <= pixels)
    {
        ++design;
    }
    return design;
}

/** FittedContent for a content sized in design pixels on a work area in the display's pixels: the largest content,
    in design pixels, no larger than `design` whose window fits inside `area` at `scale`. A size that fits comes back
    whole, to the pixel; one that does not is the most whose pixels (PixelsOfDesign) fit. Each side is held to at
    least one.
    @param design      the content's size, in design pixels
    @param decoration  the frame's own width and height round the content, in the display's pixels
    @param area        the work area the window opens on, in the display's pixels
    @param scale       the display's pixels per design pixel
    @return            the content's size, in design pixels, no larger than `design` */
inline gfx::PointI FittedDesignContent(gfx::PointI design, gfx::PointI decoration, gfx::RectI area, float scale)
{
    const gfx::PointI fitted =
        FittedContent({PixelsOfDesign(design.x, scale), PixelsOfDesign(design.y, scale)}, decoration, area);
    return {std::max(1, std::min(design.x, DesignWithinPixels(fitted.x, scale))),
            std::max(1, std::min(design.y, DesignWithinPixels(fitted.y, scale)))};
}

/** The work area a window opens on: the one its frame, put where it stood, is most on (AreaFor); else, with no place
    kept, the first - the main display's, as every platform lists them.
    @param frame   the window's frame at the place it stood, in screen pixels, y down; empty position for none
    @param placed  whether it has a place kept
    @param areas   the displays' work areas, the main display's first
    @return        a pointer into `areas`, or nullptr when `areas` is empty */
inline const gfx::RectI *OpeningArea(gfx::RectI frame, bool placed, std::span<const gfx::RectI> areas)
{
    if (areas.empty())
    {
        return nullptr;
    }
    return placed ? AreaFor(frame, areas) : &areas.front();
}

/** A dialog's frame of `size`, centred over its owner's frame `owner` and kept 12 pixels inside the work area the
    owner is on, as JUCE centres a dialog round the component that opened it - without JUCE's shrinking a dialog
    larger than the area, which here keeps its top left on it, since its content has one size.
    @param owner  the owner window's frame, in screen pixels
    @param size   the dialog's width and height, in pixels
    @param areas  the displays' work areas, in the same coordinates
    @return       the dialog's frame, in screen pixels */
inline gfx::RectI CentredOver(gfx::RectI owner, gfx::PointI size, std::span<const gfx::RectI> areas)
{
    constexpr int     kInset = 12;
    const gfx::RectI  frame{owner.CentreX() - size.x / 2, owner.CentreY() - size.y / 2, size.x, size.y};
    const gfx::RectI *area = AreaFor(owner, areas);
    return area != nullptr ? MovedOnto(frame, area->Reduced(kInset)) : frame;
}
} // namespace reckless::app
