#include "SkiaCanvas.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "include/codec/SkBmpDecoder.h"
#include "include/codec/SkCodec.h"
#include "include/codec/SkGifDecoder.h"
#include "include/codec/SkIcoDecoder.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkRawDecoder.h"
#include "include/codec/SkWbmpDecoder.h"
#include "include/codec/SkWebpDecoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/encode/SkPngEncoder.h"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#else
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

namespace reckless::gfx
{
namespace
{
constexpr float kPi = 3.14159265358979323846f; ///< pi, in float

// ---- The faces ----------------------------------------------------------------------------------------------------

/** The series' two faces, each with its ascent and descent together at an em of one (UnitSpan), read once as the faces
    are set rather than at every piece of text, and the monospaced family's two beside them. */
struct Faces
{
    sk_sp<SkFontMgr>  manager;      ///< makes the typefaces from the files' bytes
    sk_sp<SkTypeface> regular;      ///< the regular face; null before SetSerifFaces()
    sk_sp<SkTypeface> bold;         ///< the bold face; null before SetSerifFaces()
    float regular_span{0.0f};       ///< the regular face's ascent and descent together at an em of one; 0 for no face
    float bold_span{0.0f};          ///< the bold face's ascent and descent together at an em of one; 0 for no face
    sk_sp<SkTypeface> mono_regular; ///< the monospaced family's regular face; null before SetMonospaceFaces()
    sk_sp<SkTypeface> mono_bold;    ///< the monospaced family's bold face; null before SetMonospaceFaces()
    float             mono_regular_span{0.0f}; ///< the monospaced regular face's span at an em of one; 0 for no face
    float             mono_bold_span{0.0f};    ///< the monospaced bold face's span at an em of one; 0 for no face
};

/** @return the one set of faces the process draws text in */
Faces &TheFaces()
{
    static Faces faces;
    return faces;
}

/** @return the platform's font manager: Core Text, DirectWrite or fontconfig on FreeType */
sk_sp<SkFontMgr> MakeFontManager()
{
#if defined(__APPLE__)
    return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
    return SkFontMgr_New_DirectWrite();
#else
    return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
}

/** `face`'s ascent and descent together at an em of one; 0 for no face.
    @param face  the typeface; may be null
    @return      the span, in ems */
float UnitSpan(const sk_sp<SkTypeface> &face)
{
    if (!face)
    {
        return 0.0f;
    }
    SkFont        unit(face, 1.0f);
    SkFontMetrics metrics{};
    unit.getMetrics(&metrics);
    return -metrics.fAscent + metrics.fDescent;
}

/** The face `font` is drawn in, at the em size at which its ascent and descent together are the font's height - a
    font's height, as the panels mean it.
    @param font  the panel's font
    @return      the Skia font, anti-aliased with subpixel positioning and the font's horizontal scale */
SkFont SkFontOf(const Font &font)
{
    const Faces &faces = TheFaces();
    if (font.family == FontFamily::kMonospace && faces.mono_regular)
    {
        // The monospaced family, sized as the serif is; the serif's faces stand in until it is set.
        const bool  mono_bold = font.bold && faces.mono_bold;
        const auto &face      = mono_bold ? faces.mono_bold : faces.mono_regular;
        const float span      = mono_bold ? faces.mono_bold_span : faces.mono_regular_span;
        SkFont      out(face, span > 0.0f ? font.height / span : font.height);
        out.setEdging(SkFont::Edging::kAntiAlias);
        out.setSubpixel(true);
        out.setScaleX(font.horizontal_scale);
        return out;
    }
    const bool  bold = font.bold && faces.bold;
    const auto &face = bold ? faces.bold : faces.regular;
    const float span = bold ? faces.bold_span : faces.regular_span;
    SkFont      out(face, face && span > 0.0f ? font.height / span : font.height);
    out.setEdging(SkFont::Edging::kAntiAlias);
    out.setSubpixel(true);
    out.setScaleX(font.horizontal_scale);
    return out;
}

/** The glyphs of UTF-8 `text` in `font` and the x of each, kerning added after each glyph; the width in all. */
struct Run
{
    std::vector<SkGlyphID> glyphs;      ///< the glyph ids, one per character of the text
    std::vector<SkScalar>  x;           ///< each glyph's x from the start of the run, in pixels
    float                  width{0.0f}; ///< the whole run's width, in pixels, the last glyph's kerning left off
};

/** Text that is not UTF-8 - a device's name in a code page, a system's message in another - has each ill-formed part
    drawn as U+FFFD (RepairedUtf8): Skia counts such text as SIZE_MAX glyphs, which no vector holds, so it would throw
    out of the paint. And no count past the text's bytes, one glyph a byte at most, is ever trusted.
    @param font  the panel's font, whose kerning and height set the spacing
    @param sk    the Skia font made from `font` (SkFontOf)
    @param text  the text, UTF-8 or not
    @return      the glyphs and their positions; empty for a count past the text's bytes */
Run Shape(const Font &font, const SkFont &sk, std::string_view text)
{
    std::string repaired;
    if (!IsValidUtf8(text))
    {
        repaired = RepairedUtf8(text);
        text     = repaired;
    }
    Run          run;
    const size_t count = sk.textToGlyphs(text.data(), text.size(), SkTextEncoding::kUTF8, {});
    if (count > text.size())
    {
        return run;
    }
    run.glyphs.resize(count);
    sk.textToGlyphs(text.data(), text.size(), SkTextEncoding::kUTF8, {run.glyphs.data(), count});
    std::vector<SkScalar> widths(count);
    sk.getWidths({run.glyphs.data(), count}, {widths.data(), count});
    run.x.resize(count);
    const float extra = font.kerning * font.height;
    float       pen   = 0.0f;
    for (size_t i = 0; i < count; ++i)
    {
        run.x[i] = pen;
        pen += widths[i] + extra;
    }
    run.width = count > 0 ? pen - extra : 0.0f;
    return run;
}

/** @param c  the colour
    @return   the same colour as Skia's 32-bit word */
SkColor SkColorOf(Color c)
{
    return SkColorSetARGB(c.alpha(), c.red(), c.green(), c.blue());
}

/** @param r  the rectangle
    @return   the same rectangle as Skia's */
SkRect SkRectOf(RectF r)
{
    return SkRect::MakeXYWH(r.x(), r.y(), r.width(), r.height());
}

/** @param t  the transform
    @return   the same transform as Skia's 3 x 3 matrix */
SkMatrix SkMatrixOf(const Affine &t)
{
    return SkMatrix::MakeAll(t.m00, t.m01, t.m02, t.m10, t.m11, t.m12, 0.0f, 0.0f, 1.0f);
}

/** Replay a path's operations on a Skia path, each shape through Skia's own construction of it.
    @param path  the outline
    @return      the same outline as Skia's */
SkPath SkPathOf(const Path &path)
{
    SkPath out;
    for (const auto &e : path.elements())
    {
        const auto &v = e.v;
        switch (e.op)
        {
        case Path::Op::kMoveTo:
            out.moveTo(v[0], v[1]);
            break;
        case Path::Op::kLineTo:
            out.lineTo(v[0], v[1]);
            break;
        case Path::Op::kQuadTo:
            out.quadTo(v[0], v[1], v[2], v[3]);
            break;
        case Path::Op::kCubicTo:
            out.cubicTo(v[0], v[1], v[2], v[3], v[4], v[5]);
            break;
        case Path::Op::kClose:
            out.close();
            break;
        case Path::Op::kRect:
            out.addRect(SkRect::MakeXYWH(v[0], v[1], v[2], v[3]));
            break;
        case Path::Op::kRoundedRect: {
            const float radius = std::min({v[4], v[2] * 0.5f, v[3] * 0.5f});
            out.addRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(v[0], v[1], v[2], v[3]), radius, radius));
            break;
        }
        case Path::Op::kEllipse:
            out.addOval(SkRect::MakeXYWH(v[0], v[1], v[2], v[3]));
            break;
        case Path::Op::kCentredArc: {
            // Angles clockwise from twelve o'clock in radians, Skia's from three o'clock in degrees.
            SkPath arc;
            arc.arcTo(SkRect::MakeLTRB(v[0] - v[2], v[1] - v[3], v[0] + v[2], v[1] + v[3]), v[5] * 180.0f / kPi - 90.0f,
                      (v[6] - v[5]) * 180.0f / kPi, true);
            if (v[4] < 0.0f || v[4] > 0.0f)
            {
                arc.transform(SkMatrix::RotateDeg(v[4] * 180.0f / kPi, {v[0], v[1]}));
            }
            out.addPath(arc, e.flag || out.isEmpty() ? SkPath::kAppend_AddPathMode : SkPath::kExtend_AddPathMode);
            break;
        }
        case Path::Op::kTriangle:
            out.moveTo(v[0], v[1]);
            out.lineTo(v[2], v[3]);
            out.lineTo(v[4], v[5]);
            out.close();
            break;
        case Path::Op::kTransform:
            out.transform(SkMatrix::MakeAll(v[0], v[1], v[2], v[3], v[4], v[5], 0.0f, 0.0f, 1.0f));
            break;
        }
    }
    if (path.fill_rule() == Path::FillRule::kEvenOdd)
    {
        out.setFillType(SkPathFillType::kEvenOdd); // else Skia's default, the non-zero rule
    }
    return out;
}

// ---- Images -------------------------------------------------------------------------------------------------------

/** An image's pixels as Skia holds them: a decoded or sub-image picture, or a surface drawn into and snapshotted
    when read. */
class SkiaImageData final : public Image::Data
{
  public:
    /** Hold a finished picture.
        @param image  the picture */
    explicit SkiaImageData(sk_sp<SkImage> image) : image_(std::move(image))
    {
    }

    /** Hold a surface to be drawn into.
        @param surface  the raster surface */
    explicit SkiaImageData(sk_sp<SkSurface> surface) : surface_(std::move(surface))
    {
    }

    int width() const noexcept override
    {
        return surface_ ? surface_->width() : (image_ ? image_->width() : 0);
    }

    int height() const noexcept override
    {
        return surface_ ? surface_->height() : (image_ ? image_->height() : 0);
    }

    /** @return the picture; a snapshot of the surface when it is one */
    sk_sp<SkImage> image() const
    {
        return surface_ ? surface_->makeImageSnapshot() : image_;
    }

    /** @return the surface drawn into; nullptr for a finished picture */
    SkSurface *surface() const noexcept
    {
        return surface_.get();
    }

  private:
    sk_sp<SkImage>   image_;   ///< the finished picture; null for a surface
    sk_sp<SkSurface> surface_; ///< the surface drawn into; null for a finished picture
};

/** @param image  the picture
    @return       its Skia image; nullptr for one that is not Skia's */
sk_sp<SkImage> SkImageOf(const Image &image)
{
    if (const auto *data = dynamic_cast<const SkiaImageData *>(image.data()))
    {
        return data->image();
    }
    return nullptr;
}
} // namespace

void SetSerifFaces(std::span<const std::uint8_t> regular_ttf, std::span<const std::uint8_t> bold_ttf)
{
    Faces &faces = TheFaces();
    if (!faces.manager)
    {
        faces.manager = MakeFontManager();
    }
    faces.regular      = faces.manager->makeFromData(SkData::MakeWithCopy(regular_ttf.data(), regular_ttf.size()));
    faces.bold         = faces.manager->makeFromData(SkData::MakeWithCopy(bold_ttf.data(), bold_ttf.size()));
    faces.regular_span = UnitSpan(faces.regular);
    faces.bold_span    = UnitSpan(faces.bold);
}

void SetMonospaceFaces(std::span<const std::uint8_t> regular_font, std::span<const std::uint8_t> bold_font)
{
    Faces &faces = TheFaces();
    if (!faces.manager)
    {
        faces.manager = MakeFontManager();
    }
    faces.mono_regular = faces.manager->makeFromData(SkData::MakeWithCopy(regular_font.data(), regular_font.size()));
    faces.mono_bold    = faces.manager->makeFromData(SkData::MakeWithCopy(bold_font.data(), bold_font.size()));
    faces.mono_regular_span = UnitSpan(faces.mono_regular);
    faces.mono_bold_span    = UnitSpan(faces.mono_bold);
}

Image DecodeImage(std::span<const std::uint8_t> encoded)
{
    // The PNG decoder by name - the series' images are all PNG, and the other decoders are declined at the end of this
    // file - its deferred image the one SkCodec's search makes of a PNG.
    auto codec = SkPngDecoder::Decode(SkData::MakeWithCopy(encoded.data(), encoded.size()), nullptr);
    auto image = codec ? SkCodecs::DeferredImage(std::move(codec)) : nullptr;
    if (!image)
    {
        return {};
    }
    if (auto raster = image->makeRasterImage(nullptr))
    {
        image = raster;
    }
    return Image(std::make_shared<SkiaImageData>(image));
}

Image ImageFromPixels(int width, int height, std::span<const std::uint8_t> rgba)
{
    if (width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u)
    {
        return {};
    }
    const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    auto image = SkImages::RasterFromPixmapCopy(SkPixmap(info, rgba.data(), static_cast<std::size_t>(width) * 4u));
    return image ? Image(std::make_shared<SkiaImageData>(image)) : Image();
}

Image SubImage(const Image &image, RectI area)
{
    const auto sk = SkImageOf(image);
    if (!sk)
    {
        return {};
    }
    auto part = sk->makeSubset(nullptr, SkIRect::MakeXYWH(area.x(), area.y(), area.width(), area.height()), {});
    return part ? Image(std::make_shared<SkiaImageData>(part)) : Image();
}

Image RescaleImage(const Image &image, int width, int height, Resampling quality)
{
    const auto sk = SkImageOf(image);
    if (!sk || width <= 0 || height <= 0)
    {
        return {};
    }
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface)
    {
        return {};
    }
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    const SkSamplingOptions sampling(quality == Resampling::kLow ? SkFilterMode::kNearest : SkFilterMode::kLinear);
    surface->getCanvas()->drawImageRect(sk, SkRect::MakeIWH(sk->width(), sk->height()), SkRect::MakeIWH(width, height),
                                        sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
    return Image(std::make_shared<SkiaImageData>(surface->makeImageSnapshot()));
}

Image MakeImage(int width, int height)
{
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(std::max(width, 1), std::max(height, 1)));
    if (!surface)
    {
        return {};
    }
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    return Image(std::make_shared<SkiaImageData>(surface));
}

std::unique_ptr<Canvas> MakeImageCanvas(Image &image, float scale)
{
    auto *data = dynamic_cast<SkiaImageData *>(image.data());
    if (data == nullptr || data->surface() == nullptr)
    {
        return nullptr;
    }
    SkCanvas *canvas = data->surface()->getCanvas();
    canvas->resetMatrix();
    canvas->scale(scale, scale);
    return std::make_unique<SkiaCanvas>(*canvas, 1.0f);
}

std::vector<std::uint8_t> EncodePng(const Image &image)
{
    const auto sk = SkImageOf(image);
    if (!sk)
    {
        return {};
    }
    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(sk->width(), sk->height()));
    if (!sk->readPixels(nullptr, bitmap.pixmap(), 0, 0))
    {
        return {};
    }
    SkDynamicMemoryWStream stream;
    if (!SkPngEncoder::Encode(&stream, bitmap.pixmap(), {}))
    {
        return {};
    }
    std::vector<std::uint8_t> out(stream.bytesWritten());
    stream.copyTo(out.data());
    return out;
}

// ---- The canvas ---------------------------------------------------------------------------------------------------

/** The brush, the font and the resampling in force: what Save() keeps and Restore() brings back. */
struct SkiaCanvas::State
{
    Color                   color{colors::kBlack};           ///< the solid colour; used when there is no gradient
    std::optional<Gradient> gradient;                        ///< the gradient; none for the solid colour
    float                   opacity{1.0f};                   ///< what everything drawn is multiplied by, 0 to 1
    Font                    font{};                          ///< the font text is drawn in
    Resampling              resampling{Resampling::kMedium}; ///< how images are resampled when scaled
};

/** The SkCanvas drawn on, the device scale and the state in force with the states Save() pushed. */
struct SkiaCanvas::Impl
{
    SkCanvas          &canvas;       ///< the Skia canvas drawn on
    float              device_scale; ///< the physical pixels per unit of the matrix's identity
    State              state;        ///< the state in force
    std::vector<State> saved;        ///< the states Save() pushed, the newest last

    /** @return the paint for the colour or gradient, at the state's opacity, antialiased */
    SkPaint Brush() const
    {
        SkPaint paint;
        paint.setAntiAlias(true);
        if (state.gradient)
        {
            const Gradient       &g = *state.gradient;
            std::vector<SkColor>  colors{SkColorOf(g.start_color)};
            std::vector<SkScalar> positions{0.0f};
            for (const auto &[proportion, color] : g.stops)
            {
                colors.push_back(SkColorOf(color));
                positions.push_back(std::clamp(proportion, 0.0f, 1.0f));
            }
            colors.push_back(SkColorOf(g.end_color));
            positions.push_back(1.0f);
            const auto count = static_cast<int>(colors.size());
            if (g.radial)
            {
                const float radius = std::max(g.start.DistanceTo(g.end), 0.001f);
                paint.setShader(SkGradientShader::MakeRadial({g.start.x, g.start.y}, radius, colors.data(),
                                                             positions.data(), count, SkTileMode::kClamp,
                                                             SkGradientShader::kInterpolateColorsInPremul_Flag));
            }
            else
            {
                const SkPoint points[2] = {{g.start.x, g.start.y}, {g.end.x, g.end.y}};
                paint.setShader(SkGradientShader::MakeLinear(points, colors.data(), positions.data(), count,
                                                             SkTileMode::kClamp,
                                                             SkGradientShader::kInterpolateColorsInPremul_Flag));
            }
            paint.setAlphaf(state.opacity);
        }
        else
        {
            paint.setColor(SkColorOf(state.color));
            paint.setAlphaf(state.color.AlphaFraction() * state.opacity);
        }
        return paint;
    }

    /** @return the sampling for the state's resampling quality */
    SkSamplingOptions Sampling() const
    {
        switch (state.resampling)
        {
        case Resampling::kLow:
            return SkSamplingOptions(SkFilterMode::kNearest);
        case Resampling::kMedium:
            return SkSamplingOptions(SkFilterMode::kLinear);
        case Resampling::kHigh:
            break;
        }
        return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    }

    /** Draw a shaped run of text in the brush.
        @param run       the glyphs and their positions
        @param font      the Skia font the run was shaped with
        @param x         the run's left edge
        @param baseline  the baseline's y */
    void DrawRun(const Run &run, const SkFont &font, float x, float baseline)
    {
        if (run.glyphs.empty())
        {
            return;
        }
        auto blob = SkTextBlob::MakeFromPosTextH(run.glyphs.data(), run.glyphs.size() * sizeof(SkGlyphID),
                                                 {run.x.data(), run.x.size()}, 0.0f, font, SkTextEncoding::kGlyphID);
        canvas.drawTextBlob(blob, x, baseline, Brush());
    }
};

SkiaCanvas::SkiaCanvas(SkCanvas &canvas, float device_scale)
    : impl_(std::make_unique<Impl>(Impl{canvas, device_scale, State{}, {}}))
{
}

SkiaCanvas::~SkiaCanvas() = default;

SkCanvas &SkiaCanvas::skia() noexcept
{
    return impl_->canvas;
}

void SkiaCanvas::Save()
{
    impl_->canvas.save();
    impl_->saved.push_back(impl_->state);
}

void SkiaCanvas::Restore()
{
    if (impl_->saved.empty())
    {
        return;
    }
    impl_->canvas.restore();
    impl_->state = impl_->saved.back();
    impl_->saved.pop_back();
}

void SkiaCanvas::AddTransform(const Affine &t)
{
    impl_->canvas.concat(SkMatrixOf(t));
}

bool SkiaCanvas::ClipToRect(RectI r)
{
    impl_->canvas.clipRect(SkRectOf(r.ToFloat()), SkClipOp::kIntersect, false);
    return !impl_->canvas.isClipEmpty();
}

bool SkiaCanvas::ExcludeClipRect(RectI r)
{
    impl_->canvas.clipRect(SkRectOf(r.ToFloat()), SkClipOp::kDifference, false);
    return !impl_->canvas.isClipEmpty();
}

void SkiaCanvas::ClipToPath(const Path &path, const Affine &t)
{
    SkPath p = SkPathOf(path);
    p.transform(SkMatrixOf(t));
    impl_->canvas.clipPath(p, SkClipOp::kIntersect, true);
}

RectI SkiaCanvas::ClipBounds() const
{
    // The device clip mapped back to the panel's units - not Skia's local bounds, which it outsets by a pixel for
    // antialiasing.
    SkMatrix inverse;
    if (!impl_->canvas.getLocalToDeviceAs3x3().invert(&inverse))
    {
        return {};
    }
    const SkRect r = inverse.mapRect(SkRect::Make(impl_->canvas.getDeviceClipBounds()));
    return RectI::LeftTopRightBottom(static_cast<int>(std::floor(r.left())), static_cast<int>(std::floor(r.top())),
                                     static_cast<int>(std::ceil(r.right())), static_cast<int>(std::ceil(r.bottom())));
}

float SkiaCanvas::PhysicalPixelScale() const
{
    const SkMatrix m = impl_->canvas.getLocalToDeviceAs3x3();
    return impl_->device_scale * std::sqrt(std::abs(m.getScaleX() * m.getScaleY() - m.getSkewX() * m.getSkewY()));
}

void SkiaCanvas::SetColor(Color c)
{
    impl_->state.color = c;
    impl_->state.gradient.reset();
}

void SkiaCanvas::SetGradient(const Gradient &g)
{
    impl_->state.gradient = g;
}

void SkiaCanvas::SetOpacity(float opacity)
{
    impl_->state.opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void SkiaCanvas::SetFont(const Font &font)
{
    impl_->state.font = font;
}

void SkiaCanvas::SetResampling(Resampling r)
{
    impl_->state.resampling = r;
}

const Font &SkiaCanvas::CurrentFont() const
{
    return impl_->state.font;
}

void SkiaCanvas::FillAll()
{
    impl_->canvas.drawPaint(impl_->Brush());
}

void SkiaCanvas::FillAll(Color c)
{
    SkPaint paint;
    paint.setColor(SkColorOf(c));
    impl_->canvas.drawPaint(paint);
}

void SkiaCanvas::FillRect(RectI r)
{
    impl_->canvas.drawRect(SkRectOf(r.ToFloat()), impl_->Brush());
}

void SkiaCanvas::FillRect(RectF r)
{
    impl_->canvas.drawRect(SkRectOf(r), impl_->Brush());
}

void SkiaCanvas::DrawRect(RectF r, float thickness)
{
    // The border inside the rectangle: four strips, the sides between the top and the bottom.
    const float t = std::min({thickness, r.width() * 0.5f, r.height() * 0.5f});
    FillRect(RectF{r.x(), r.y(), r.width(), t});
    FillRect(RectF{r.x(), r.bottom() - t, r.width(), t});
    FillRect(RectF{r.x(), r.y() + t, t, r.height() - 2.0f * t});
    FillRect(RectF{r.right() - t, r.y() + t, t, r.height() - 2.0f * t});
}

void SkiaCanvas::FillRoundedRect(RectF r, float corner_radius)
{
    const float radius = std::min({corner_radius, r.width() * 0.5f, r.height() * 0.5f});
    impl_->canvas.drawRRect(SkRRect::MakeRectXY(SkRectOf(r), radius, radius), impl_->Brush());
}

void SkiaCanvas::DrawRoundedRect(RectF r, float corner_radius, float thickness)
{
    SkPaint paint = impl_->Brush();
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    const float radius = std::min({corner_radius, r.width() * 0.5f, r.height() * 0.5f});
    impl_->canvas.drawRRect(SkRRect::MakeRectXY(SkRectOf(r), radius, radius), paint);
}

void SkiaCanvas::FillEllipse(RectF r)
{
    impl_->canvas.drawOval(SkRectOf(r), impl_->Brush());
}

void SkiaCanvas::DrawEllipse(RectF r, float thickness)
{
    SkPaint paint = impl_->Brush();
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    impl_->canvas.drawOval(SkRectOf(r), paint);
}

void SkiaCanvas::DrawLine(PointF from, PointF to, float thickness)
{
    SkPaint paint = impl_->Brush();
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(thickness);
    paint.setStrokeCap(SkPaint::kButt_Cap);
    impl_->canvas.drawLine(from.x, from.y, to.x, to.y, paint);
}

void SkiaCanvas::DrawHorizontalLine(int y, float left, float right)
{
    FillRect(RectF{left, static_cast<float>(y), right - left, 1.0f});
}

void SkiaCanvas::DrawVerticalLine(int x, float top, float bottom)
{
    FillRect(RectF{static_cast<float>(x), top, 1.0f, bottom - top});
}

void SkiaCanvas::FillPath(const Path &path, const Affine &t)
{
    SkPath p = SkPathOf(path);
    p.transform(SkMatrixOf(t));
    impl_->canvas.drawPath(p, impl_->Brush());
}

void SkiaCanvas::StrokePath(const Path &path, const Stroke &stroke, const Affine &t)
{
    SkPath p = SkPathOf(path);
    p.transform(SkMatrixOf(t));
    SkPaint paint = impl_->Brush();
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(stroke.thickness);
    paint.setStrokeJoin(stroke.join == Stroke::Join::kCurved     ? SkPaint::kRound_Join
                        : stroke.join == Stroke::Join::kBevelled ? SkPaint::kBevel_Join
                                                                 : SkPaint::kMiter_Join);
    paint.setStrokeCap(stroke.cap == Stroke::Cap::kRounded  ? SkPaint::kRound_Cap
                       : stroke.cap == Stroke::Cap::kSquare ? SkPaint::kSquare_Cap
                                                            : SkPaint::kButt_Cap);
    if (!stroke.dashes.empty())
    {
        paint.setPathEffect(SkDashPathEffect::Make({stroke.dashes.data(), stroke.dashes.size()}, 0.0f));
    }
    impl_->canvas.drawPath(p, paint);
}

void SkiaCanvas::DrawText(std::string_view text, RectF area, Justify justify, bool ellipsis)
{
    const Font  &font = impl_->state.font;
    const SkFont sk   = SkFontOf(font);
    Run          run  = Shape(font, sk, text);
    if (ellipsis && run.width > area.width() && !run.glyphs.empty())
    {
        // Cut glyphs from the end, each in turn, until an ellipsis put where the last one cut stood fits: the dots
        // follow the glyph before it as that glyph followed it, its kerning after it, and end inside the area.
        const Run dots = Shape(font, sk, "\xE2\x80\xA6");
        float     pen  = 0.0f;
        while (!run.glyphs.empty())
        {
            pen = run.x.back();
            run.glyphs.pop_back();
            run.x.pop_back();
            if (pen + dots.width <= area.width())
            {
                break;
            }
        }
        for (size_t i = 0; i < dots.glyphs.size(); ++i)
        {
            run.glyphs.push_back(dots.glyphs[i]);
            run.x.push_back(pen + dots.x[i]);
        }
        run.width = pen + dots.width;
    }
    float top = area.y();
    if (Has(justify, Justify::kBottom))
    {
        top = area.bottom() - font.height;
    }
    else if (Has(justify, Justify::kVerticallyCentred))
    {
        top = area.y() + (area.height() - font.height) * 0.5f;
    }
    SkFontMetrics metrics{};
    sk.getMetrics(&metrics);
    float x = area.x();
    if (Has(justify, Justify::kRight))
    {
        x = area.right() - run.width;
    }
    else if (Has(justify, Justify::kHorizontallyCentred))
    {
        x = area.x() + (area.width() - run.width) * 0.5f;
    }
    impl_->DrawRun(run, sk, x, top - metrics.fAscent);
}

void SkiaCanvas::DrawFittedText(std::string_view text, RectI area, Justify justify, int max_lines,
                                float min_horizontal_scale)
{
    const float floor_scale = min_horizontal_scale > 0.0f ? min_horizontal_scale : 0.7f;
    const Font  font        = impl_->state.font;
    const float width       = TextWidth(font, text);
    const RectF box         = area.ToFloat();
    if (width <= box.width() || max_lines <= 1)
    {
        // One line, squeezed sideways as far as the floor allows before it is cut.
        const float squeeze = width > box.width() ? std::max(floor_scale, box.width() / width) : 1.0f;
        Save();
        SetFont(font.WithHorizontalScale(font.horizontal_scale * squeeze));
        DrawText(text, box, justify, true);
        Restore();
        return;
    }
    // Words onto lines, each as long as fits, up to max_lines; the last one takes what is left.
    std::vector<std::string> lines;
    std::string              current;
    std::size_t              i = 0;
    while (i < text.size())
    {
        const std::size_t space = text.find(' ', i);
        const std::string word(text.substr(i, space == std::string_view::npos ? std::string_view::npos : space - i));
        const std::string trial = current.empty() ? word : current + " " + word;
        if (!current.empty() && TextWidth(font, trial) > box.width() && static_cast<int>(lines.size()) < max_lines - 1)
        {
            lines.push_back(current);
            current = word;
        }
        else
        {
            current = trial;
        }
        i = space == std::string_view::npos ? text.size() : space + 1;
    }
    lines.push_back(current);
    const float line_height = font.height;
    float       top         = box.y();
    const float total       = line_height * static_cast<float>(lines.size());
    if (Has(justify, Justify::kBottom))
    {
        top = box.bottom() - total;
    }
    else if (Has(justify, Justify::kVerticallyCentred))
    {
        top = box.y() + (box.height() - total) * 0.5f;
    }
    const auto horizontal = static_cast<Justify>(static_cast<int>(justify) & 7);
    for (const auto &line : lines)
    {
        const float line_width = TextWidth(font, line);
        const float squeeze    = line_width > box.width() ? std::max(floor_scale, box.width() / line_width) : 1.0f;
        Save();
        SetFont(font.WithHorizontalScale(font.horizontal_scale * squeeze));
        DrawText(line, RectF{box.x(), top, box.width(), line_height}, horizontal | Justify::kTop, true);
        Restore();
        top += line_height;
    }
}

float MeasureText(const Font &font, std::string_view text)
{
    return Shape(font, SkFontOf(font), text).width;
}

float SkiaCanvas::TextWidth(const Font &font, std::string_view text) const
{
    return MeasureText(font, text);
}

float SkiaCanvas::Ascent(const Font &font) const
{
    SkFontMetrics metrics{};
    SkFontOf(font).getMetrics(&metrics);
    return -metrics.fAscent;
}

float SkiaCanvas::Descent(const Font &font) const
{
    SkFontMetrics metrics{};
    SkFontOf(font).getMetrics(&metrics);
    return metrics.fDescent;
}

void SkiaCanvas::DrawImage(const Image &image, RectF dest, bool alpha_as_mask)
{
    const auto sk = SkImageOf(image);
    if (!sk)
    {
        return;
    }
    SkPaint paint;
    paint.setAlphaf(impl_->state.opacity);
    if (alpha_as_mask)
    {
        paint.setColorFilter(SkColorFilters::Blend(SkColorOf(impl_->state.color), SkBlendMode::kSrcIn));
    }
    impl_->canvas.drawImageRect(sk, SkRect::MakeIWH(sk->width(), sk->height()), SkRectOf(dest), impl_->Sampling(),
                                &paint, SkCanvas::kStrict_SrcRectConstraint);
}

void SkiaCanvas::DrawImageTransformed(const Image &image, const Affine &t)
{
    const auto sk = SkImageOf(image);
    if (!sk)
    {
        return;
    }
    SkPaint paint;
    paint.setAlphaf(impl_->state.opacity);
    impl_->canvas.save();
    impl_->canvas.concat(SkMatrixOf(t));
    impl_->canvas.drawImage(sk, 0.0f, 0.0f, impl_->Sampling(), &paint);
    impl_->canvas.restore();
}

void SkiaCanvas::BeginTransparencyLayer(float opacity)
{
    impl_->canvas.saveLayerAlphaf(nullptr, std::clamp(opacity, 0.0f, 1.0f));
    impl_->saved.push_back(impl_->state);
}

void SkiaCanvas::EndTransparencyLayer()
{
    Restore();
}

Offscreen SkiaCanvas::MakeOffscreen(int width, int height) const
{
    Image image  = MakeImage(width, height);
    auto  canvas = MakeImageCanvas(image, 1.0f);
    return {std::move(image), std::move(canvas)};
}
} // namespace reckless::gfx

// ---- PNG's decoder alone
// ----------------------------------------------------------------------------------------------
//
// SkCodec registers every decoder its build compiled in - in the skia-pack's, JPEG, WebP, GIF, BMP, ICO, WBMP and raw
// beside PNG - and its own object refers to each one's entry points to do it. Every codec is an SkCodec, so linking the
// PNG decoder links that object, which links every other decoder with what it is built on: libjpeg-turbo, libwebp,
// wuffs, and behind the raw decoder the DNG SDK and piex, whose static constructors would keep part of them even
// through dead stripping. The series decodes nothing but PNG. These definitions of the other decoders' entry points,
// each declining every image, are the ones the linker takes: they are in the canvas's own object, which every module
// that draws links ahead of the skia-pack's archives - before SkCodec's references are met - so those references
// resolve here and the archives' members holding the real decoders are never loaded, on any platform (a separate
// object in this library would come too late for GNU ld, which loads an archive's member only for a symbol already
// wanted). Each is a qualified definition, which must match its header's declaration, so a pack that changes a
// decoder's signature fails to compile here rather than linking that decoder back; the Skia suite holds every one
// to declining a real image of its format (test_skia_canvas.cpp).
namespace
{
/** The answer of every stub decoder below: no codec, and the result a decoder gives for a format it does not read.
    @param result  where kUnimplemented is put; may be null
    @return        nullptr, no codec */
std::unique_ptr<SkCodec> NoCodec(SkCodec::Result *result)
{
    if (result != nullptr)
    {
        *result = SkCodec::kUnimplemented;
    }
    return nullptr;
}
} // namespace

/// @cond
bool SkBmpDecoder::IsBmp(const void *, size_t)
{
    return false;
}

std::unique_ptr<SkCodec> SkBmpDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                              SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

bool SkGifDecoder::IsGif(const void *, size_t)
{
    return false;
}

std::unique_ptr<SkCodec> SkGifDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                              SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

bool SkIcoDecoder::IsIco(const void *, size_t)
{
    return false;
}

std::unique_ptr<SkCodec> SkIcoDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                              SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

bool SkJpegDecoder::IsJpeg(const void *, size_t)
{
    return false;
}

std::unique_ptr<SkCodec> SkJpegDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                               SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

std::unique_ptr<SkCodec> SkRawDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                              SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

bool SkWbmpDecoder::IsWbmp(const void *, size_t)
{
    return false;
}

std::unique_ptr<SkCodec> SkWbmpDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                               SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

bool SkWebpDecoder::IsWebp(const void *, size_t)
{
    return false;
}

std::unique_ptr<SkCodec> SkWebpDecoder::Decode(std::unique_ptr<SkStream>, SkCodec::Result *result,
                                               SkCodecs::DecodeContext)
{
    return NoCodec(result);
}

/// @endcond
