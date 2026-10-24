#pragma once

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "Color.h"
#include "Geometry.h"
#include "Path.h"
#include "Text.h"

namespace reckless::gfx
{
/** How a stroked path's corners and ends are drawn, and how wide it is. The defaults are a hard corner and a square
    end flush with the path's end. */
struct Stroke
{
    /** The shape of a stroked path's corners. */
    enum class Join
    {
        kMitered,  ///< the outer edges extended to a point
        kCurved,   ///< a circular arc about the corner
        kBevelled, ///< the corner cut off flat
    };

    /** The shape of a stroked path's open ends. */
    enum class Cap
    {
        kButt,    ///< flush with the path's end
        kSquare,  ///< extended past the end by half the thickness
        kRounded, ///< a half circle past the end
    };

    float thickness{1.0f};      ///< the stroke's width, in the canvas's units
    Join  join{Join::kMitered}; ///< the corner shape
    Cap   cap{Cap::kButt};      ///< the end shape
    /** Dash and gap lengths in turn, from the path's start; empty for a solid stroke. */
    std::vector<float> dashes{};

    /** A one-pixel, mitered, butt-capped, solid stroke. */
    constexpr Stroke() = default;

    /** A solid stroke.
        @param width  the thickness, in the canvas's units
        @param j      the corner shape
        @param c      the end shape */
    Stroke(float width, Join j = Join::kMitered, Cap c = Cap::kButt) : thickness(width), join(j), cap(c)
    {
    }
};

/** A colour blend between two points - along the line through them, or outward from the first for a radial one -
    with stops between the ends at proportions of the way. */
struct Gradient
{
    Color  start_color;   ///< the colour at `start`
    PointF start;         ///< where the blend begins, in the canvas's units
    Color  end_color;     ///< the colour at `end`
    PointF end;           ///< where the blend ends: a linear one's other point, a radial one's rim
    bool   radial{false}; ///< true to blend outward from `start` to a circle through `end`
    /** Intermediate stops, (proportion of the way, colour), in order. */
    std::vector<std::pair<float, Color>> stops{};
};

/** How a scaled image is resampled: the nearest pixel, a smoothed blend, or the best the backend has. */
enum class Resampling
{
    kLow,    ///< the nearest pixel
    kMedium, ///< a smoothed blend
    kHigh,   ///< the best the backend has
};

/**
    A picture a canvas draws: a file's pixels or what was rendered into it. Its pixels are the backend's - each
    backend makes its own images and draws only those - and it is shared, copying the handle and not the pixels.
*/
class Image
{
  public:
    /** The backend's pixels. */
    class Data
    {
      public:
        virtual ~Data() = default;

        /** @return the width, in pixels */
        virtual int width() const noexcept = 0;
        /** @return the height, in pixels */
        virtual int height() const noexcept = 0;
    };

    /** An empty image: no pixels, IsValid() false. */
    Image() = default;

    /** An image over a backend's pixels.
        @param data  the backend's pixels, shared with every copy of this handle; nullptr for none */
    explicit Image(std::shared_ptr<Data> data) : data_(std::move(data))
    {
    }

    /** @return true when there are pixels and both sides are at least one */
    bool IsValid() const noexcept
    {
        return data_ != nullptr && data_->width() > 0 && data_->height() > 0;
    }

    /** @return the width, in pixels; 0 for none */
    int width() const noexcept
    {
        return data_ != nullptr ? data_->width() : 0;
    }

    /** @return the height, in pixels; 0 for none */
    int height() const noexcept
    {
        return data_ != nullptr ? data_->height() : 0;
    }

    /** @return the rectangle of pixels, at the origin, `width()` by `height()` */
    RectI Bounds() const noexcept
    {
        return {width(), height()};
    }

    /** @return the backend's pixels, owned by this handle and its copies; nullptr for none */
    Data *data() const noexcept
    {
        return data_.get();
    }

  private:
    std::shared_ptr<Data> data_; ///< the backend's pixels, shared by every copy
};

class Canvas;

/** A picture drawn off screen (Canvas::MakeOffscreen): an image of a canvas's own kind, transparent to begin with,
    and a canvas drawing into it. The drawing is done, and the canvas gone, before the image is drawn anywhere. */
struct Offscreen
{
    Image                   image;  ///< the picture, transparent until drawn into
    std::unique_ptr<Canvas> canvas; ///< draws into `image`; null where the backend makes no images
};

/**
    What a panel draws on, written against no toolkit: the subset of drawing the series' panels use - a current
    brush (a colour or a gradient, at an opacity), a current font, a stack of transform and clip states, rectangles,
    rounded rectangles, ellipses, lines, paths filled or stroked, text placed in a box or fitted to one, and images.
    Coordinates are the panel's design pixels; the backend maps them onto the device, whose physical pixels per
    design pixel PhysicalPixelScale() tells, for a cached image to be rendered at the size it will be shown.

    A backend implements it on its toolkit's own drawing (gfx/skia's SkiaCanvas on Skia; a suite's recording
    canvas keeps the calls), so the same panel code draws through any of them.
*/
class Canvas
{
  public:
    virtual ~Canvas() = default;

    // ---- State: the transform, the clip and the brush, pushed and popped together --------------------------------

    /** Push the transform, the clip and the brush, to be popped by the matching Restore(). */
    virtual void Save() = 0;
    /** Pop the state the matching Save() pushed. */
    virtual void Restore() = 0;

    /** Everything drawn after transformed by `t`, after the transform already in force.
        @param t  the transform to add */
    virtual void AddTransform(const Affine &t) = 0;

    /** The clip narrowed to `r`; whether anything is left to draw in.
        @param r  the rectangle to keep, in the current coordinates
        @return   true when the clip is not empty */
    virtual bool ClipToRect(RectI r) = 0;

    /** The clip narrowed to the inside of `path` moved by `t`.
        @param path  the outline to keep
        @param t     the transform `path` is taken through */
    virtual void ClipToPath(const Path &path, const Affine &t = {}) = 0;

    /** The clip narrowed to leave `r` out - what a parent's painting is clipped round an opaque view over it, as JUCE
        clips a component's painting round its opaque children (view::PaintTree); whether anything is left to draw in. A
        canvas that keeps no clip of its own (a suite's recording canvas) leaves it as it is.
        @param r  the rectangle to leave out, in the current coordinates
        @return   true when the clip is not empty */
    virtual bool ExcludeClipRect(RectI r)
    {
        (void)r;
        return !ClipBounds().IsEmpty();
    }

    /** The clip in the current coordinates.
        @return the bounds of what can still be drawn in */
    virtual RectI ClipBounds() const = 0;

    /** Physical pixels per design pixel where the drawing lands.
        @return the scale, 1 or more on a display of the usual density */
    virtual float PhysicalPixelScale() const = 0;

    // ---- The brush, the font and the resampling ------------------------------------------------------------------

    /** The brush a solid colour.
        @param c  the colour */
    virtual void SetColor(Color c) = 0;
    /** The brush a gradient.
        @param g  the blend */
    virtual void SetGradient(const Gradient &g) = 0;
    /** The opacity everything drawn after is multiplied by.
        @param opacity  0 to 1 */
    virtual void SetOpacity(float opacity) = 0;
    /** The font text is drawn in.
        @param font  the font */
    virtual void SetFont(const Font &font) = 0;
    /** How images are resampled when scaled.
        @param r  the quality */
    virtual void SetResampling(Resampling r) = 0;
    /** @return the font in force, valid until the next SetFont() */
    virtual const Font &CurrentFont() const = 0;

    // ---- Shapes, in the current brush ----------------------------------------------------------------------------

    /** The whole clip filled with the current brush. */
    virtual void FillAll() = 0;
    /** The whole clip filled with a colour, the brush left as it is.
        @param c  the colour */
    virtual void FillAll(Color c) = 0;
    /** A rectangle on whole pixels, filled.
        @param r  the rectangle, in pixels */
    virtual void FillRect(RectI r) = 0;
    /** A rectangle with antialiased edges, filled.
        @param r  the rectangle */
    virtual void FillRect(RectF r) = 0;
    /** A rectangle's outline, drawn inside it.
        @param r          the rectangle
        @param thickness  the line's width */
    virtual void DrawRect(RectF r, float thickness) = 0;
    /** A rounded rectangle, filled.
        @param r              the rectangle
        @param corner_radius  the radius of each corner */
    virtual void FillRoundedRect(RectF r, float corner_radius) = 0;
    /** A rounded rectangle's outline, drawn inside it.
        @param r              the rectangle
        @param corner_radius  the radius of each corner
        @param thickness      the line's width */
    virtual void DrawRoundedRect(RectF r, float corner_radius, float thickness) = 0;
    /** The ellipse inscribed in a rectangle, filled.
        @param r  the rectangle */
    virtual void FillEllipse(RectF r) = 0;
    /** The outline of the ellipse inscribed in a rectangle, drawn inside it.
        @param r          the rectangle
        @param thickness  the line's width */
    virtual void DrawEllipse(RectF r, float thickness) = 0;
    /** A line between two points.
        @param from       where it starts
        @param to         where it ends
        @param thickness  the line's width */
    virtual void DrawLine(PointF from, PointF to, float thickness) = 0;
    /** A one-pixel line along pixel row `y` from `left` to `right`.
        @param y      the pixel row
        @param left   where it starts, in pixels
        @param right  where it ends, in pixels */
    virtual void DrawHorizontalLine(int y, float left, float right) = 0;
    /** A one-pixel line along pixel column `x` from `top` to `bottom`.
        @param x       the pixel column
        @param top     where it starts, in pixels
        @param bottom  where it ends, in pixels */
    virtual void DrawVerticalLine(int x, float top, float bottom) = 0;
    /** A path taken through a transform and filled.
        @param path  the outline
        @param t     the transform `path` is taken through */
    virtual void FillPath(const Path &path, const Affine &t = {}) = 0;
    /** `path` taken through `t` and then stroked: the stroke's thickness is in the canvas's units, after `t`, and never
        scaled with the path - a drawing in millimetres strokes its lines in the canvas's pixels.
        @param path    the outline
        @param stroke  the width, joins, ends and dashes
        @param t       the transform `path` is taken through */
    virtual void StrokePath(const Path &path, const Stroke &stroke, const Affine &t = {}) = 0;

    // ---- Text, in the current font and brush ---------------------------------------------------------------------

    /** `text` on one line in `area`, placed by `justify`, cut with an ellipsis when it would not fit and `ellipsis`
        asks for one. UTF-8.
        @param text      the text, UTF-8
        @param area      the box it is placed in
        @param justify   where in `area` it sits
        @param ellipsis  true to cut an over-long line with an ellipsis */
    virtual void DrawText(std::string_view text, RectF area, Justify justify, bool ellipsis = true) = 0;

    /** `text` in `area` on up to `max_lines` lines, squeezed sideways down to `min_horizontal_scale` before it is
        cut (0: the backend's own floor).
        @param text                  the text, UTF-8
        @param area                  the box it is placed in, in pixels
        @param justify               where in `area` it sits
        @param max_lines             the most lines it may take
        @param min_horizontal_scale  the narrowest the text is squeezed to, 0 to 1; 0 for the backend's own floor */
    virtual void DrawFittedText(std::string_view text, RectI area, Justify justify, int max_lines,
                                float min_horizontal_scale = 0.0f) = 0;

    /** The width `text` takes in `font`.
        @param font  the font
        @param text  the text, UTF-8
        @return      the width, in the canvas's units */
    virtual float TextWidth(const Font &font, std::string_view text) const = 0;

    /** The height above the baseline of `font`'s glyphs.
        @param font  the font
        @return      the ascent, in the canvas's units */
    virtual float Ascent(const Font &font) const = 0;
    /** The height below the baseline of `font`'s glyphs.
        @param font  the font
        @return      the descent, in the canvas's units */
    virtual float Descent(const Font &font) const = 0;

    // ---- Images --------------------------------------------------------------------------------------------------

    /** `image` stretched over `dest`; with `alpha_as_mask`, its alpha filled with the current brush instead.
        @param image          the picture, one of this backend's
        @param dest           the rectangle it is stretched over
        @param alpha_as_mask  true to fill its alpha with the brush in place of its pixels */
    virtual void DrawImage(const Image &image, RectF dest, bool alpha_as_mask = false) = 0;

    /** `image` drawn through `t`, its pixel (0, 0) at the origin before it.
        @param image  the picture, one of this backend's
        @param t      the transform from the image's pixels */
    virtual void DrawImageTransformed(const Image &image, const Affine &t) = 0;

    // ---- Layers --------------------------------------------------------------------------------------------------

    /** Everything until the matching End drawn on its own, then composited at `opacity`.
        @param opacity  0 to 1 */
    virtual void BeginTransparencyLayer(float opacity) = 0;
    /** Composite the layer the matching Begin started. */
    virtual void EndTransparencyLayer() = 0;

    // ---- Pictures kept between paints ----------------------------------------------------------------------------

    /** An image `width` x `height` pixels of this canvas's own kind, transparent, with a canvas drawing into it at a
        unit a pixel: what a view renders a picture it keeps into, at the physical pixels this canvas lands on
        (view/CachedPicture.h). None - no canvas - where the backend makes no images; the view then draws the
        picture straight onto this canvas every paint.
        @param width   the width, in physical pixels
        @param height  the height, in physical pixels
        @return        the image and its canvas, or an empty Offscreen with no canvas */
    virtual Offscreen MakeOffscreen(int width, int height) const
    {
        (void)width;
        (void)height;
        return {};
    }
};

/** Save on construction, Restore on destruction. */
class ScopedState
{
  public:
    /** Save the canvas's state.
        @param canvas  the canvas, which outlives this object */
    explicit ScopedState(Canvas &canvas) : canvas_(canvas)
    {
        canvas_.Save();
    }

    /** Restore the state. */
    ~ScopedState()
    {
        canvas_.Restore();
    }

    /** Not copyable. */
    ScopedState(const ScopedState &) = delete;
    /** Not copyable. */
    ScopedState &operator=(const ScopedState &) = delete;

  private:
    Canvas &canvas_; ///< the canvas whose state is held
};
} // namespace reckless::gfx
