#pragma once

#include <algorithm>
#include <cmath>

#include "Geometry.h"

namespace reckless::look
{
/** Where `source` lands scaled to fit inside `destination` and centred in it, its aspect kept, the scale and the
    offsets computed in double: where every shrunk image is drawn (the header's mark, the trade mark notices' logos).
    A source with no area stays where it is.
    @param source       the rectangle whose size and aspect are kept
    @param destination  the rectangle it is fitted inside
    @return             `source` scaled to fit and centred in `destination` */
inline gfx::RectF CentredFit(gfx::RectF source, gfx::RectF destination) noexcept
{
    double x = source.x(), y = source.y(), w = source.width(), h = source.height();
    if (!(w > 0.0) || !(h > 0.0))
    {
        return source;
    }
    const double dx = destination.x(), dy = destination.y(), dw = destination.width(), dh = destination.height();
    const double scale = std::min(dw / w, dh / h);
    w *= scale;
    h *= scale;
    x = dx + (dw - w) * 0.5;
    y = dy + (dh - h) * 0.5;
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
}

/** Where to draw an image shrunk for `area` so that it lands on whole physical pixels at `scale` physical pixels a
    logical one: `area`'s position rounded to that grid and its size the shrunk image's own, so the draw copies it
    1:1 rather than resampling it by a fraction of a pixel.
    @param area    the area the image is drawn for, in logical pixels
    @param width   the shrunk image's width, in physical pixels
    @param height  the shrunk image's height, in physical pixels
    @param scale   the physical pixels to a logical one, floored at 0.001
    @return        the rectangle to draw the image in, in logical pixels */
inline gfx::RectF OnPhysicalPixels(gfx::RectF area, int width, int height, float scale)
{
    scale = std::max(1.0e-3f, scale);
    return {std::round(area.x() * scale) / scale, std::round(area.y() * scale) / scale,
            static_cast<float>(width) / scale, static_cast<float>(height) / scale};
}

/** The one uniform scale that fits `source` inside `target`: the smaller of the two ratios of their sides, so
    the source is letterboxed and never stretched - a tube's millimetres to its cell (TubeGlyph), the meter
    artwork's housing to its component. Target pixels per source unit.
    @param source  the rectangle to fit
    @param target  the rectangle to fit it inside
    @return        the scale, in target pixels per source unit */
inline float FitScale(gfx::RectF source, gfx::RectF target) noexcept
{
    return std::min(target.width() / source.width(), target.height() / source.height());
}

/** `source` onto `target` at `scale` (FitScale's, or one the caller has already taken), centred: the source's
    centre to the origin, the scale, the origin to the target's centre.
    @param source  the rectangle to place
    @param target  the rectangle to centre it in
    @param scale   the scale, in target pixels per source unit
    @return        the transform from source coordinates to target coordinates */
inline gfx::Affine FitTransform(gfx::RectF source, gfx::RectF target, float scale) noexcept
{
    return gfx::Affine::Translation(-source.CentreX(), -source.CentreY())
        .FollowedBy(gfx::Affine::Scale(scale))
        .Translated(target.CentreX(), target.CentreY());
}

/** `source` fitted inside `target` and centred in it, at FitScale.
    @param source  the rectangle to place
    @param target  the rectangle to fit it inside and centre it in
    @return        the transform from source coordinates to target coordinates */
inline gfx::Affine FitTransform(gfx::RectF source, gfx::RectF target) noexcept
{
    return FitTransform(source, target, FitScale(source, target));
}
} // namespace reckless::look
