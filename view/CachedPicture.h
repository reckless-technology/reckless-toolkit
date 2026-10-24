#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"

namespace reckless::view
{
/**
    A picture a view keeps between paints and draws back each paint - a screen's grid with its figures, a stack of
    curves that changes only when data lands - rendered at the physical pixels the canvas says it lands on, into an
    image of the canvas's own kind (gfx::Canvas::MakeOffscreen), so it is as sharp on a 2x display or under a shell's
    zoom as what is drawn live beside it; and drawn again when the view lets it go (Drop: in Resized, and when what
    it shows moves) or the scale it lands at moves. It keeps its picture by one of the two rules JUCE's screens kept
    theirs by, so a screen keeps the pixels the releases built on JUCE drew:

    - kTwiceScale, the series' grids (the EQ's graph, the Tone's Bode plot, a meter's face): rendered at twice the
      physical pixel scale, and never under twice the design size, into an image the bounds times that scale, each
      side rounded; drawn again when that scale moves by a thousandth; stretched back over the bounds at the canvas's
      resampling.
    - kBuffered, as a JUCE component buffered to an image (setBufferedToImage: the waterfall): rendered at the physical
      pixel scale into the smallest whole image that holds the bounds at it; drawn again only when that image's size
      changes; drawn back opaque through the inverse of that scale.

    Where the canvas makes no images (a suite's recording canvas) the picture is drawn straight onto it, every paint.
    The painter draws in the view's own coordinates, as its Paint does (Draw); or, where a toolkit renders part of the
    picture at its own pixels - a meter's soft shadow, blurred a pixel at a time as juce::DropShadow blurs it - in the
    picture's pixels, handed the canvas at a unit a pixel and the picture's pixels per design pixel (DrawInPixels; 1
    on a canvas that makes no images). Painting is one thread's, so it needs no lock.
*/
class CachedPicture
{
  public:
    /** Which rule keeps the picture; see the class comment. */
    enum class Kind
    {
        kTwiceScale, ///< rendered at twice the physical pixel scale and stretched back: the series' grids
        kBuffered,   ///< rendered at the physical pixel scale into the smallest whole image: the waterfall's stack
    };

    /** @param kind  the rule the picture is kept by */
    explicit CachedPicture(Kind kind) noexcept : kind_(kind)
    {
    }

    /** The picture over `bounds` (the view's local bounds) on `c`: drawn first through `paint` - a callable taking
        a gfx::Canvas & - when there is none at the scale the canvas lands at, then drawn back.
        @tparam Painter  a callable taking a `gfx::Canvas &`
        @param c       the canvas to draw on
        @param bounds  the view's local bounds, in design pixels; nothing is drawn when empty
        @param paint   draws the picture in the view's own coordinates */
    template <typename Painter> void Draw(gfx::Canvas &c, gfx::RectI bounds, const Painter &paint)
    {
        Show(
            c, bounds,
            [&paint](gfx::Canvas &on, float scale) {
                on.AddTransform(gfx::Affine::Scale(scale));
                paint(on);
            },
            [&paint](gfx::Canvas &on) { paint(on); });
    }

    /** As Draw, the painter - a callable taking a gfx::Canvas & and a float - handed the picture's canvas at a unit a
        pixel and the picture's pixels per design pixel, to draw in its own pixels.
        @tparam Painter  a callable taking a `gfx::Canvas &` and a `float`
        @param c       the canvas to draw on
        @param bounds  the view's local bounds, in design pixels; nothing is drawn when empty
        @param paint   draws the picture in pixels, given the picture's canvas and its pixels per design pixel (1 on a
                       canvas that makes no images) */
    template <typename Painter> void DrawInPixels(gfx::Canvas &c, gfx::RectI bounds, const Painter &paint)
    {
        Show(
            c, bounds, [&paint](gfx::Canvas &on, float scale) { paint(on, scale); },
            [&paint](gfx::Canvas &on) { paint(on, 1.0f); });
    }

    /** The picture let go of: drawn again at the next Draw. */
    void Drop() noexcept
    {
        image_ = {};
    }

    /** For the tests: the picture as it stands, the scale it was drawn at, and how many times it has been drawn.
        @return  the kept picture, invalid when there is none; valid until the next Draw or Drop */
    const gfx::Image &image() const noexcept
    {
        return image_;
    }

    /** For the tests.
        @return  the pixels per design pixel the picture was drawn at; 0 before the first draw */
    float scale() const noexcept
    {
        return scale_;
    }

    /** For the tests.
        @return  how many times the picture has been drawn since construction */
    int renders() const noexcept
    {
        return renders_;
    }

  private:
    /** The picture drawn when there is none at the scale `c` lands at - `render` handed its canvas and that scale -
        and drawn back; on a canvas that makes no images, `direct` drawing straight onto it.
        @tparam Renderer  a callable taking a `gfx::Canvas &` and the picture's scale
        @tparam Direct    a callable taking a `gfx::Canvas &`
        @param c       the canvas the picture lands on
        @param bounds  the view's local bounds, in design pixels
        @param render  draws the picture onto its own canvas, given that canvas and its scale
        @param direct  draws the picture straight onto `c` */
    template <typename Renderer, typename Direct>
    void Show(gfx::Canvas &c, gfx::RectI bounds, const Renderer &render, const Direct &direct)
    {
        if (bounds.IsEmpty())
        {
            return;
        }
        const float physical = c.PhysicalPixelScale();
        const auto  w        = static_cast<float>(bounds.width());
        const auto  h        = static_cast<float>(bounds.height());
        if (kind_ == Kind::kTwiceScale)
        {
            const float wanted = 2.0f * std::max(1.0f, physical);
            if (!image_.IsValid() || std::fabs(scale_ - wanted) > 1.0e-3f)
            {
                Render(c, gfx::RoundToInt(w * wanted), gfx::RoundToInt(h * wanted), wanted, render);
            }
            if (image_.IsValid())
            {
                c.DrawImage(image_, bounds.ToFloat());
                return;
            }
        }
        else
        {
            const auto size   = gfx::RectF(w * physical, h * physical).SmallestIntegerContainer();
            const int  width  = std::max(1, size.width());
            const int  height = std::max(1, size.height());
            if (!image_.IsValid() || image_.width() != width || image_.height() != height)
            {
                Render(c, width, height, physical, render);
            }
            if (image_.IsValid())
            {
                c.SetColor(gfx::colors::kBlack);
                c.DrawImageTransformed(image_, gfx::Affine::Scale(w / static_cast<float>(image_.width()),
                                                                  h / static_cast<float>(image_.height())));
                return;
            }
        }
        direct(c); // a canvas that makes no images: the picture drawn straight onto it
    }

    /** Draw the picture into a new image, kept as the picture; leaves none when the canvas makes no images.
        @tparam Renderer  a callable taking a `gfx::Canvas &` and the picture's scale
        @param c       the canvas whose kind of image is made
        @param width   the image's width, in pixels
        @param height  the image's height, in pixels
        @param scale   the picture's pixels per design pixel
        @param render  draws the picture onto the image's canvas, given that canvas and `scale` */
    template <typename Renderer> void Render(gfx::Canvas &c, int width, int height, float scale, const Renderer &render)
    {
        gfx::Offscreen offscreen = c.MakeOffscreen(width, height);
        if (offscreen.canvas == nullptr)
        {
            image_ = {};
            return;
        }
        render(*offscreen.canvas, scale);
        offscreen.canvas.reset(); // the drawing done before the picture is read
        image_ = std::move(offscreen.image);
        scale_ = scale;
        ++renders_;
    }

    Kind       kind_;        ///< the rule the picture is kept by
    gfx::Image image_;       ///< the picture; invalid when there is none
    float      scale_{0.0f}; ///< the scale image_ was drawn at, in its pixels per design pixel; 0 before the first
    int        renders_{0};  ///< how many times the picture has been drawn
};
} // namespace reckless::view
