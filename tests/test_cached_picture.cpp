// The picture a view keeps between paints (view/CachedPicture.h), on a canvas that records what is asked of it:
// drawn once at the scale the canvas lands at and drawn back from then on; drawn again when the view lets it go or the
// scale moves - twice the physical scale, never under twice the design size and each side rounded, for a grid; the
// physical scale in the smallest whole image, kept while that image's size holds, for a buffered screen - and painted
// straight onto a canvas that makes no images; and a painter that draws in the picture's own pixels handed them, the
// same picture drawn back.
#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "CachedPicture.h"

namespace
{
using reckless::gfx::Affine;
using reckless::gfx::Canvas;
using reckless::gfx::Color;
using reckless::gfx::Image;
using reckless::gfx::Offscreen;
using reckless::gfx::PointF;
using reckless::gfx::RectF;
using reckless::gfx::RectI;
using reckless::view::CachedPicture;

/** A picture of the recording canvas's own: its size alone. */
class SizeOnly final : public Image::Data
{
  public:
    SizeOnly(int width, int height) : width_(width), height_(height)
    {
    }

    int width() const noexcept override
    {
        return width_;
    }

    int height() const noexcept override
    {
        return height_;
    }

  private:
    int width_;
    int height_;
};

/** What a picture asked of the canvas it was drawn on, and of the offscreen ones it made. */
struct Log
{
    struct Made
    {
        int width;
        int height;
    };

    struct Drawn
    {
        int    width;
        int    height;
        RectF  dest;        ///< DrawImage's
        bool   transformed; ///< DrawImageTransformed's instead
        Affine transform;
        Color  colour; ///< the brush when it was drawn
    };

    std::vector<Made>  made;
    std::vector<Drawn> drawn;
    std::vector<float> painted_at; ///< the physical scale each painting landed at, offscreen or on the canvas itself
};

/** A canvas that draws nothing and records into a Log: the images it is asked to make (when `makes_images`), the
    images it draws and where, and - through the painter, which reads PhysicalPixelScale - the scale each painting
    landed at. Its physical scale is `physical` times the scales added to it. */
class RecordingCanvas final : public Canvas
{
  public:
    RecordingCanvas(Log &log, float physical, bool makes_images) : log_(log), scale_(physical), makes_(makes_images)
    {
    }

    void Save() override
    {
    }

    void Restore() override
    {
    }

    void AddTransform(const Affine &t) override
    {
        scale_ *= t.m00;
    }

    bool ClipToRect(RectI) override
    {
        return true;
    }

    void ClipToPath(const reckless::gfx::Path &, const Affine &) override
    {
    }

    RectI ClipBounds() const override
    {
        return {};
    }

    float PhysicalPixelScale() const override
    {
        return scale_;
    }

    void SetColor(Color c) override
    {
        colour_ = c;
    }

    void SetGradient(const reckless::gfx::Gradient &) override
    {
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const reckless::gfx::Font &f) override
    {
        font_ = f;
    }

    void SetResampling(reckless::gfx::Resampling) override
    {
    }

    const reckless::gfx::Font &CurrentFont() const override
    {
        return font_;
    }

    void FillAll() override
    {
    }

    void FillAll(Color) override
    {
    }

    void FillRect(RectI) override
    {
    }

    void FillRect(RectF) override
    {
    }

    void DrawRect(RectF, float) override
    {
    }

    void FillRoundedRect(RectF, float) override
    {
    }

    void DrawRoundedRect(RectF, float, float) override
    {
    }

    void FillEllipse(RectF) override
    {
    }

    void DrawEllipse(RectF, float) override
    {
    }

    void DrawLine(PointF, PointF, float) override
    {
    }

    void DrawHorizontalLine(int, float, float) override
    {
    }

    void DrawVerticalLine(int, float, float) override
    {
    }

    void FillPath(const reckless::gfx::Path &, const Affine &) override
    {
    }

    void StrokePath(const reckless::gfx::Path &, const reckless::gfx::Stroke &, const Affine &) override
    {
    }

    void DrawText(std::string_view, RectF, reckless::gfx::Justify, bool) override
    {
    }

    void DrawFittedText(std::string_view, RectI, reckless::gfx::Justify, int, float) override
    {
    }

    float TextWidth(const reckless::gfx::Font &, std::string_view text) const override
    {
        return static_cast<float>(text.size());
    }

    float Ascent(const reckless::gfx::Font &f) const override
    {
        return f.height * 0.8f;
    }

    float Descent(const reckless::gfx::Font &f) const override
    {
        return f.height * 0.2f;
    }

    void DrawImage(const Image &image, RectF dest, bool) override
    {
        log_.drawn.push_back({image.width(), image.height(), dest, false, {}, colour_});
    }

    void DrawImageTransformed(const Image &image, const Affine &t) override
    {
        log_.drawn.push_back({image.width(), image.height(), {}, true, t, colour_});
    }

    void BeginTransparencyLayer(float) override
    {
    }

    void EndTransparencyLayer() override
    {
    }

    Offscreen MakeOffscreen(int width, int height) const override
    {
        if (!makes_)
        {
            return {};
        }
        log_.made.push_back({width, height});
        return {Image(std::make_shared<SizeOnly>(width, height)), std::make_unique<RecordingCanvas>(log_, 1.0f, true)};
    }

  private:
    Log                &log_;
    float               scale_;
    bool                makes_;
    Color               colour_{};
    reckless::gfx::Font font_{};
};

/** What a view's Paint hands the picture: a painting that notes the scale it lands at. */
auto PainterFor(Log &log)
{
    return [&log](Canvas &c) { log.painted_at.push_back(c.PhysicalPixelScale()); };
}
} // namespace

TEST(CachedPicture, AGridIsDrawnOnceAtTwiceThePhysicalScaleAndStretchedBack)
{
    Log           log;
    CachedPicture grid(CachedPicture::Kind::kTwiceScale);
    const RectI   bounds{300, 101};

    RecordingCanvas at_1x(log, 1.0f, true);
    grid.Draw(at_1x, bounds, PainterFor(log));
    ASSERT_EQ(log.made.size(), 1u);
    EXPECT_EQ(log.made[0].width, 600);
    EXPECT_EQ(log.made[0].height, 202);
    ASSERT_EQ(log.painted_at.size(), 1u);
    EXPECT_FLOAT_EQ(log.painted_at[0], 2.0f) << "drawn at twice the design size";
    ASSERT_EQ(log.drawn.size(), 1u);
    EXPECT_FALSE(log.drawn[0].transformed);
    EXPECT_EQ(log.drawn[0].dest, (RectF{0.0f, 0.0f, 300.0f, 101.0f})) << "stretched back over the bounds";
    EXPECT_FLOAT_EQ(grid.scale(), 2.0f);

    grid.Draw(at_1x, bounds, PainterFor(log));
    EXPECT_EQ(log.made.size(), 1u) << "kept: the same scale draws the same picture back";
    EXPECT_EQ(log.drawn.size(), 2u);

    RecordingCanvas zoomed(log, 1.5f, true);
    grid.Draw(zoomed, bounds, PainterFor(log));
    ASSERT_EQ(log.made.size(), 2u);
    EXPECT_EQ(log.made[1].width, 900);
    EXPECT_EQ(log.made[1].height, 303) << "each side rounded at three times";
    EXPECT_FLOAT_EQ(log.painted_at.back(), 3.0f);

    RecordingCanvas nearly(log, 1.5004f, true);
    grid.Draw(nearly, bounds, PainterFor(log));
    EXPECT_EQ(log.made.size(), 2u) << "a move under a thousandth of the scale keeps the picture";

    RecordingCanvas small(log, 0.5f, true);
    grid.Draw(small, bounds, PainterFor(log));
    ASSERT_EQ(log.made.size(), 3u);
    EXPECT_EQ(log.made[2].width, 600) << "never under twice the design size";
    EXPECT_EQ(grid.renders(), 3);
}

TEST(CachedPicture, ABufferedScreenIsDrawnAtThePhysicalScaleInTheSmallestWholeImage)
{
    Log           log;
    CachedPicture screen(CachedPicture::Kind::kBuffered);

    RecordingCanvas zoomed(log, 1.5f, true);
    screen.Draw(zoomed, RectI{101, 63}, PainterFor(log));
    ASSERT_EQ(log.made.size(), 1u);
    EXPECT_EQ(log.made[0].width, 152) << "151.5 pixels held whole";
    EXPECT_EQ(log.made[0].height, 95) << "and 94.5 too: the smallest whole image that holds it, not the nearest";
    EXPECT_FLOAT_EQ(log.painted_at[0], 1.5f) << "drawn at the physical scale itself";
    ASSERT_EQ(log.drawn.size(), 1u);
    EXPECT_TRUE(log.drawn[0].transformed) << "drawn back through the inverse scale";
    EXPECT_FLOAT_EQ(log.drawn[0].transform.m00, 101.0f / 152.0f);
    EXPECT_FLOAT_EQ(log.drawn[0].transform.m11, 63.0f / 95.0f);
    EXPECT_FLOAT_EQ(log.drawn[0].transform.m02, 0.0f);
    EXPECT_EQ(log.drawn[0].colour, reckless::gfx::colors::kBlack) << "opaque";

    // At 2x, then a hair under it: the image is still 200 x 100 whole pixels, so the picture drawn at 2x is kept, as a
    // component buffered to an image keeps it.
    Log             second;
    CachedPicture   kept(CachedPicture::Kind::kBuffered);
    RecordingCanvas at_2x(second, 2.0f, true);
    kept.Draw(at_2x, RectI{100, 50}, PainterFor(second));
    RecordingCanvas under(second, 1.999f, true);
    kept.Draw(under, RectI{100, 50}, PainterFor(second));
    EXPECT_EQ(second.made.size(), 1u);
    EXPECT_FLOAT_EQ(kept.scale(), 2.0f);
    RecordingCanvas at_1x(second, 1.0f, true);
    kept.Draw(at_1x, RectI{100, 50}, PainterFor(second));
    ASSERT_EQ(second.made.size(), 2u) << "a new size, a new picture";
    EXPECT_EQ(second.made[1].width, 100);
}

TEST(CachedPicture, DroppedItIsDrawnAgain)
{
    for (const auto kind : {CachedPicture::Kind::kTwiceScale, CachedPicture::Kind::kBuffered})
    {
        Log             log;
        CachedPicture   picture(kind);
        RecordingCanvas c(log, 1.0f, true);
        picture.Draw(c, RectI{80, 40}, PainterFor(log));
        picture.Draw(c, RectI{80, 40}, PainterFor(log));
        EXPECT_EQ(log.painted_at.size(), 1u);
        picture.Drop();
        EXPECT_FALSE(picture.image().IsValid());
        picture.Draw(c, RectI{80, 40}, PainterFor(log));
        EXPECT_EQ(log.painted_at.size(), 2u) << "let go of, it is drawn again";
        EXPECT_EQ(log.drawn.size(), 3u) << "and drawn back every time";
    }
}

TEST(CachedPicture, ACanvasThatMakesNoImagesIsPaintedEveryTime)
{
    for (const auto kind : {CachedPicture::Kind::kTwiceScale, CachedPicture::Kind::kBuffered})
    {
        Log             log;
        CachedPicture   picture(kind);
        RecordingCanvas c(log, 1.0f, false);
        picture.Draw(c, RectI{80, 40}, PainterFor(log));
        picture.Draw(c, RectI{80, 40}, PainterFor(log));
        EXPECT_EQ(log.painted_at.size(), 2u) << "painted straight onto the canvas, each time";
        EXPECT_FLOAT_EQ(log.painted_at[0], 1.0f) << "at the canvas's own scale";
        EXPECT_TRUE(log.drawn.empty()) << "no picture to draw back";
        EXPECT_EQ(picture.renders(), 0);
    }
}

TEST(CachedPicture, AnEmptyViewDrawsNothing)
{
    Log             log;
    CachedPicture   picture(CachedPicture::Kind::kTwiceScale);
    RecordingCanvas c(log, 1.0f, true);
    picture.Draw(c, RectI{0, 40}, PainterFor(log));
    EXPECT_TRUE(log.made.empty());
    EXPECT_TRUE(log.painted_at.empty());
    EXPECT_TRUE(log.drawn.empty());
}

TEST(CachedPicture, APainterInThePicturesOwnPixelsIsHandedThemAndTheSamePictureDrawnBack)
{
    // A meter's face: the same picture as a grid's at the same scale - its size, when it is drawn again, how it is
    // drawn back - but its painter handed the picture's canvas at a unit a pixel, with the scale to draw at.
    Log                                  log;
    CachedPicture                        face(CachedPicture::Kind::kTwiceScale);
    RecordingCanvas                      zoomed(log, 1.5f, true);
    std::vector<std::pair<float, float>> handed; // (the canvas's own scale, the scale handed)
    const auto painter = [&handed](Canvas &on, float scale) { handed.emplace_back(on.PhysicalPixelScale(), scale); };
    face.DrawInPixels(zoomed, RectI{300, 101}, painter);
    ASSERT_EQ(log.made.size(), 1u);
    EXPECT_EQ(log.made[0].width, 900);
    EXPECT_EQ(log.made[0].height, 303);
    ASSERT_EQ(handed.size(), 1u);
    EXPECT_FLOAT_EQ(handed[0].first, 1.0f) << "the picture's canvas left at a unit a pixel";
    EXPECT_FLOAT_EQ(handed[0].second, 3.0f) << "and the painter told the picture's pixels per design pixel";
    ASSERT_EQ(log.drawn.size(), 1u);
    EXPECT_EQ(log.drawn[0].dest, (RectF{0.0f, 0.0f, 300.0f, 101.0f})) << "drawn back as a grid's is";
    face.DrawInPixels(zoomed, RectI{300, 101}, painter);
    EXPECT_EQ(handed.size(), 1u) << "kept at the same scale";

    // On a canvas that makes no images, painted straight onto it at a unit a design pixel.
    Log             direct;
    CachedPicture   unkept(CachedPicture::Kind::kTwiceScale);
    RecordingCanvas no_images(direct, 1.0f, false);
    handed.clear();
    unkept.DrawInPixels(no_images, RectI{80, 40}, painter);
    ASSERT_EQ(handed.size(), 1u);
    EXPECT_FLOAT_EQ(handed[0].second, 1.0f);
    EXPECT_TRUE(direct.drawn.empty());
}
