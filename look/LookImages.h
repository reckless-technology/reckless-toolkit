#pragma once

#include <algorithm>
#include <cstddef>

#include "Canvas.h"
#include "Geometry.h"

namespace reckless::look
{
/**
    What the look asks of a toolkit's images. The chicken head and the company mark are cached at the physical pixels
    they are drawn at - a frame of the knob's film strip, the mark shrunk by halving - and that takes three things the
    canvas does not do: an asset decoded from memory, a part of an image, and an image resampled to another size at a
    quality. A backend implements them on its own images (panel/'s ContextImages on the functions a format's panel
    context carries, Skia's in every format: gfx/skia's DecodeImage, SubImage and RescaleImage), and the images
    it hands back are the ones its canvas draws. Each call makes a new image, or shares the pixels of the one it is
    given; none keeps state, so a backend is free to be one object for a whole process.
*/
class ImageBackend
{
  public:
    virtual ~ImageBackend() = default;

    /** The picture an image file in memory holds - the series' assets are PNGs - or an invalid image when the bytes are
        not one the backend reads.
        @param data  the encoded file's bytes, not kept past the call
        @param size  the number of bytes
        @return      the decoded picture, or an invalid image for bytes the backend cannot read */
    virtual gfx::Image Decode(const void *data, std::size_t size) const = 0;

    /** The part of `image` inside `area`, in its pixels: one frame of a film strip.
        @param image  the image to cut from
        @param area   the part to take, in the image's pixels
        @return       the part, a new image or one sharing the pixels of `image` */
    virtual gfx::Image Clipped(const gfx::Image &image, gfx::RectI area) const = 0;

    /** `image` resampled to `width` x `height` pixels at `quality`; an invalid image stays invalid. The halving the
        look's caches do (ShrunkImage, KnobStrip::FramesAtSize) asks for kHigh and rests on a 2:1 resample there
        averaging each two by two block of pixels, as a bilinear read does, so a backend's kHigh is a bilinear read, not
        a filter that reaches further.
        @param image    the image to resample
        @param width    the new width, in pixels
        @param height   the new height, in pixels
        @param quality  the resampling quality
        @return         the resampled image */
    virtual gfx::Image Rescaled(const gfx::Image &image, int width, int height, gfx::Resampling quality) const = 0;
};

/** `image` at `width` x `height` pixels: halved while it is still at least twice that size, then resampled once from
    there. A bilinear resample reads the two by two pixels nearest each pixel it writes, which at 2:1 is a clean box
    filter; straight from an image ten times the size it reads a few of the hundred pixels each one covers, and the
    smooth edge the image carries comes out in steps. A backend's resample is a bilinear read (Rescaled), so an image
    drawn much smaller than it is is made at the physical pixels it will be drawn at, once per size, and drawn there
    1:1: the header's company mark (CompanyMark) and the trade mark notices' logos. (The knob's film strip has its own
    rule, KnobStrip::FramesAtSize.)
    @param backend  the toolkit's image operations
    @param image    the image to shrink
    @param width    the width wanted, in pixels, at least 1
    @param height   the height wanted, in pixels, at least 1
    @return         the image at that size */
inline gfx::Image ShrunkImage(const ImageBackend &backend, gfx::Image image, int width, int height)
{
    width  = std::max(1, width);
    height = std::max(1, height);
    while (image.width() >= 2 * width && image.height() >= 2 * height)
    {
        image = backend.Rescaled(image, image.width() / 2, image.height() / 2, gfx::Resampling::kHigh);
    }
    return backend.Rescaled(image, width, height, gfx::Resampling::kHigh);
}
} // namespace reckless::look
