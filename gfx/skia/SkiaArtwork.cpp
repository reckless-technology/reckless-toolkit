#include "SkiaArtwork.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include "SkiaCanvas.h"
#include "SvgArt.h"

namespace reckless::gfx
{
namespace
{
/** One pass of the three-tap box along `num` samples `delta` apart, rounded as JUCE's software blur rounds it: each
    sample the mean of itself and its two neighbours, an end the mean of itself and its one.
    @param d      the first sample; the pass is made in place
    @param num    the samples in the line, at least 2
    @param delta  the distance between samples, in bytes */
void BlurTriplets(std::uint8_t *d, int num, std::ptrdiff_t delta) noexcept
{
    std::uint32_t last = d[0];
    d[0]               = static_cast<std::uint8_t>((d[0] + d[delta] + 1) / 3);
    d += delta;
    num -= 2;
    do
    {
        const std::uint32_t now = d[0];
        d[0]                    = static_cast<std::uint8_t>((last + d[0] + d[delta] + 1) / 3);
        d += delta;
        last = now;
    } while (--num > 0);
    d[0] = static_cast<std::uint8_t>((last + d[0] + 1) / 3);
}

/** The shape's soft shadow on `target`, `clip` its clip in the canvas's units: the mask drawn at its offset into an
    alpha picture the size of the shape and the blur's reach within the clip's, as juce::DropShadow sizes it, the blur
    along every row and then every column, and the mask painted in the shadow's colour.
    @param target  the Skia canvas drawn on
    @param shadow  the outline, colour, blur radius and offset
    @param clip    the target's clip, in its units */
void DrawShadow(SkCanvas &target, const Shadow &shadow, RectI clip)
{
    const RectI shape = shadow.path.Bounds().SmallestIntegerContainer().Translated(shadow.offset);
    const RectI area  = shape.Expanded(shadow.radius + 1).Intersection(clip.Expanded(shadow.radius + 1));
    if (area.width() <= 2 || area.height() <= 2 || shadow.radius <= 0)
    {
        return;
    }
    auto mask = SkSurfaces::Raster(SkImageInfo::MakeA8(area.width(), area.height()));
    if (!mask)
    {
        return;
    }
    mask->getCanvas()->clear(SK_ColorTRANSPARENT);
    {
        SkiaCanvas drawing(*mask->getCanvas());
        drawing.SetColor(colors::kWhite);
        drawing.FillPath(shadow.path, Affine::Translation(static_cast<float>(shadow.offset.x - area.x()),
                                                          static_cast<float>(shadow.offset.y - area.y())));
    }
    SkPixmap pixels;
    if (!mask->peekPixels(&pixels))
    {
        return;
    }
    auto      *data        = static_cast<std::uint8_t *>(pixels.writable_addr());
    const auto row_bytes   = static_cast<std::ptrdiff_t>(pixels.rowBytes());
    const int  repetitions = 2 * shadow.radius;
    for (int y = 0; y < area.height(); ++y)
    {
        for (int i = 0; i < repetitions; ++i)
        {
            BlurTriplets(data + row_bytes * y, area.width(), 1);
        }
    }
    for (int x = 0; x < area.width(); ++x)
    {
        for (int i = 0; i < repetitions; ++i)
        {
            BlurTriplets(data + x, area.height(), row_bytes);
        }
    }
    SkPaint paint;
    paint.setColor(static_cast<SkColor>(shadow.color.argb()));
    target.drawImage(mask->makeImageSnapshot(), static_cast<float>(area.x()), static_cast<float>(area.y()),
                     SkSamplingOptions(), &paint);
}

/** The document read, the first time it is asked for, and kept: none for one the reader refuses. A document is one of
    the module's embedded files, so its bytes outlive every panel.
    @param svg  the document's bytes, which outlive the process's panels
    @return     the parsed document, owned by the cache; nullptr when the reader refuses it */
const SvgArt *DocumentFor(std::span<const std::uint8_t> svg)
{
    static std::mutex                                                                    lock;
    static std::map<std::pair<const std::uint8_t *, std::size_t>, std::optional<SvgArt>> documents;
    const std::lock_guard<std::mutex>                                                    held(lock);
    const auto key = std::make_pair(svg.data(), svg.size());
    auto       it  = documents.find(key);
    if (it == documents.end())
    {
        it = documents
                 .emplace(key, SvgArt::Parse(std::string_view(reinterpret_cast<const char *>(svg.data()), svg.size())))
                 .first;
    }
    return it->second ? &*it->second : nullptr;
}
} // namespace

void DrawArtwork(Canvas &canvas, const ArtworkLayer &layer)
{
    if (layer.shadow)
    {
        if (auto *skia = dynamic_cast<SkiaCanvas *>(&canvas))
        {
            DrawShadow(skia->skia(), *layer.shadow, canvas.ClipBounds());
        }
    }
    if (!layer.svg.empty())
    {
        if (const SvgArt *document = DocumentFor(layer.svg))
        {
            document->Draw(canvas, layer.transform);
        }
    }
}
} // namespace reckless::gfx
