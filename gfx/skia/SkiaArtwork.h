#pragma once

#include "Artwork.h"
#include "Canvas.h"

namespace reckless::gfx
{
/**
    A meter's artwork (gfx::ArtworkLayer, Artwork.h) drawn on Skia, as JUCE draws it: the shape's soft shadow first -
    its mask blurred as juce::DropShadow blurs it, two passes of a three-tap box per pixel of radius each way, rounded
    as JUCE rounds them, then painted in the shadow's colour - on a SkiaCanvas, which hands out its SkCanvas for the
    mask, at the canvas's own pixels; then the document, read by the series' reader of the meters' SVG (SvgArt.h, parsed
    once a document) and drawn on the canvas through the layer's transform. No module of Skia's takes part. A document
    the reader refuses draws nothing. The thread that paints, as every paint is.
    @param canvas  the canvas to draw on; a SkiaCanvas
    @param layer   the artwork's document, transform and shadow
*/
void DrawArtwork(Canvas &canvas, const ArtworkLayer &layer);
} // namespace reckless::gfx
