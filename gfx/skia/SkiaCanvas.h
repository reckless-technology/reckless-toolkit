#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "Canvas.h"

class SkCanvas;
class SkImage;
class SkTypeface;
template <typename T> class sk_sp;

namespace reckless::gfx
{
/**
    The suite's drawing (Canvas.h) on Skia: a gfx::Canvas drawing through an SkCanvas, and the images and the faces it
    draws with. What a panel asks for keeps the suite's meaning where Skia's own calls may take another - a stroke
    centred on its outline, a rectangle's border inside it, a gradient blended on premultiplied colours, a font's height
    its ascent and descent together, its kerning a share of that height added after every glyph, text placed in its box
    by its justification - and is drawn by Skia's rasteriser.

    The faces: the series' serif, regular and bold, from their files' bytes (SetSerifFaces, once, before anything
    draws text); a bold face asked for draws in the bold file, anything else in the regular.

    @param regular_ttf  the regular face's font file, copied
    @param bold_ttf     the bold face's font file, copied */
void SetSerifFaces(std::span<const std::uint8_t> regular_ttf, std::span<const std::uint8_t> bold_ttf);

/** The monospaced family's faces (FontFamily::kMonospace), regular and bold, from their files' bytes - the toolkit's
    Monaspace Xenon (assets/fonts/monaspace), which a product embeds only when it asks for it
    (reckless_monospace_font_files, cmake/RecklessEmbed.cmake) - once, before anything draws text in the family. Until
    they are set, text in the family draws in the serif's faces. A font in the family is sized as the serif is, its
    height the face's ascent and descent together; a bold font draws in the bold file, anything else in the regular.
    @param regular_font  the regular face's font file (OpenType or TrueType), copied
    @param bold_font     the bold face's font file, copied */
void SetMonospaceFaces(std::span<const std::uint8_t> regular_font, std::span<const std::uint8_t> bold_font);

/** An image decoded from a PNG's bytes; an invalid image when they are not one - the series' images are all PNG.
    @param encoded  the PNG file's bytes
    @return        the image; invalid when the bytes are not a PNG */
Image DecodeImage(std::span<const std::uint8_t> encoded);

/** An image of pixels a decoder made outside Skia - a tone's picture from the network (vst-plugins' tone3000/Picture.h)
    - copied in: eight bits each of red, green, blue and alpha, unpremultiplied, row by row with nothing between the
    rows.
    @param width   the width, in pixels
    @param height  the height, in pixels
    @param rgba    width x height x 4 bytes
    @return        the image; invalid when the sizes and the bytes disagree */
Image ImageFromPixels(int width, int height, std::span<const std::uint8_t> rgba);

/** The part `area` of `image`, sharing its pixels; an invalid image when the part is outside it.
    @param image  the picture, one of Skia's
    @param area   the part, in `image`'s pixels
    @return       the part sharing the pixels; invalid when the part is outside */
Image SubImage(const Image &image, RectI area);

/** `image` resampled to `width` x `height` pixels: the nearest pixel at kLow, and above it a plain bilinear read - no
    mipmaps, no cubic - since the series' look halves its cached pictures (the knob's frames, the company mark) by 2:1
    resamples that rest on a bilinear read averaging each two by two block. An invalid image for none.
    @param image    the picture, one of Skia's
    @param width    the new width, in pixels
    @param height   the new height, in pixels
    @param quality  kLow for the nearest pixel, anything else a bilinear read
    @return         the resampled image; invalid for an invalid source or a size under one pixel */
Image RescaleImage(const Image &image, int width, int height, Resampling quality);

/** How wide `text` is in `font`, as a SkiaCanvas draws it - for a window that lays a view out before it paints.
    @param font  the font
    @param text  the text, UTF-8
    @return      the width, in the font's pixels */
float MeasureText(const Font &font, std::string_view text);

/** A blank image to draw into (MakeImageCanvas), `width` x `height` pixels, transparent.
    @param width   the width, in pixels; at least 1
    @param height  the height, in pixels; at least 1
    @return        the image, drawn into through MakeImageCanvas() */
Image MakeImage(int width, int height);

/** A canvas drawing into `image` (one MakeImage made), at `scale` physical pixels per unit.
    @param image  an image MakeImage() made, which outlives the canvas
    @param scale  the physical pixels per unit
    @return       the canvas; nullptr for an image that is not drawable */
std::unique_ptr<Canvas> MakeImageCanvas(Image &image, float scale = 1.0f);

/** `image`'s pixels as a PNG's bytes; empty for an image that holds none.
    @param image  the picture, one of Skia's
    @return       the PNG file's bytes */
std::vector<std::uint8_t> EncodePng(const Image &image);

/** A gfx::Canvas on an SkCanvas: every drawing call of Canvas.h in Skia's terms, with the brush, the font and the
    resampling kept per Save() and restored by Restore(). The calls that override Canvas are documented there. */
class SkiaCanvas final : public Canvas
{
  public:
    /** Draws on `canvas`, whose current matrix maps the panel's units to its pixels; `device_scale` is the
        physical pixels per unit of that matrix's identity (a window's backing scale).
        @param canvas        the Skia canvas, which outlives this one
        @param device_scale  the physical pixels per unit of the matrix's identity */
    explicit SkiaCanvas(SkCanvas &canvas, float device_scale = 1.0f);
    /** Destroy the canvas; the SkCanvas it draws on is left as it is. */
    ~SkiaCanvas() override;

    void  Save() override;
    void  Restore() override;
    void  AddTransform(const Affine &t) override;
    bool  ClipToRect(RectI r) override;
    void  ClipToPath(const Path &path, const Affine &t) override;
    bool  ExcludeClipRect(RectI r) override;
    RectI ClipBounds() const override;
    float PhysicalPixelScale() const override;

    void        SetColor(Color c) override;
    void        SetGradient(const Gradient &g) override;
    void        SetOpacity(float opacity) override;
    void        SetFont(const Font &font) override;
    void        SetResampling(Resampling r) override;
    const Font &CurrentFont() const override;

    void FillAll() override;
    void FillAll(Color c) override;
    void FillRect(RectI r) override;
    void FillRect(RectF r) override;
    void DrawRect(RectF r, float thickness) override;
    void FillRoundedRect(RectF r, float corner_radius) override;
    void DrawRoundedRect(RectF r, float corner_radius, float thickness) override;
    void FillEllipse(RectF r) override;
    void DrawEllipse(RectF r, float thickness) override;
    void DrawLine(PointF from, PointF to, float thickness) override;
    void DrawHorizontalLine(int y, float left, float right) override;
    void DrawVerticalLine(int x, float top, float bottom) override;
    void FillPath(const Path &path, const Affine &t) override;
    void StrokePath(const Path &path, const Stroke &stroke, const Affine &t) override;

    void  DrawText(std::string_view text, RectF area, Justify justify, bool ellipsis) override;
    void  DrawFittedText(std::string_view text, RectI area, Justify justify, int max_lines,
                         float min_horizontal_scale) override;
    float TextWidth(const Font &font, std::string_view text) const override;
    float Ascent(const Font &font) const override;
    float Descent(const Font &font) const override;

    void DrawImage(const Image &image, RectF dest, bool alpha_as_mask) override;
    void DrawImageTransformed(const Image &image, const Affine &t) override;

    void BeginTransparencyLayer(float opacity) override;
    void EndTransparencyLayer() override;

    /** A raster image of Skia's (MakeImage) and a canvas drawing into it (MakeImageCanvas).
        @param width   the width, in physical pixels
        @param height  the height, in physical pixels
        @return        the image and the canvas drawing into it */
    Offscreen MakeOffscreen(int width, int height) const override;

    /** The SkCanvas it draws on, for what Skia draws that the canvas has no call for - a meter's shadow, blurred as
        juce::DropShadow blurs it (SkiaArtwork.h) - between its own calls.
        @return the SkCanvas, valid as long as this canvas */
    SkCanvas &skia() noexcept;

  private:
    struct State;
    struct Impl;
    std::unique_ptr<Impl> impl_; ///< the Skia canvas, the brush state and the saved states
};
} // namespace reckless::gfx
