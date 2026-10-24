#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "Color.h"
#include "Geometry.h"
#include "Path.h"

namespace reckless::gfx
{
/** A soft shadow a shape casts on what is under it: the shape's outline, the shadow's colour where it is solid, how
    many pixels its edge is blurred over, and how far it falls from the shape - in the canvas's own units. */
struct Shadow
{
    Path   path;      ///< the outline of the shape that casts it
    Color  color;     ///< the shadow's colour where it is solid
    int    radius{0}; ///< the pixels the edge is blurred over, each way
    PointI offset{};  ///< how far the shadow falls from the shape, in pixels
};

/**
    A vector drawing's document - an SVG file's bytes - drawn on a canvas through a transform, with the soft shadow of
    a shape under it: what a series' backlit meter's face and glass are, which the meter renders once into a picture it
    keeps (view::CachedPicture) at the physical pixels the picture lands on, every number here in those pixels.

    The toolkit draws it (view::PanelContext::draw_artwork), since a document and a blur are the toolkit's to draw:
    every format on Skia, the document through the series' reader of the meters' SVG, which draws it as JUCE's parser
    does (SvgArt.h), and the shadow blurred as juce::DropShadow blurs it (gfx/skia/SkiaArtwork.h).
*/
struct ArtworkLayer
{
    std::span<const std::uint8_t> svg;       ///< the document; empty for none
    Affine                        transform; ///< the document's units onto the canvas's
    std::optional<Shadow>         shadow;    ///< drawn first, under the document
};
} // namespace reckless::gfx
