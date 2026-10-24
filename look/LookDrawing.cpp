#include "LookDrawing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <numbers>

#include "LookGeometry.h"

namespace reckless::look
{
// ---- The chicken head ------------------------------------------------------------------------------------------

/** The decoded film strip and the frames made from it at each size, shared by every KnobStrip made from one asset. */
struct KnobStrip::Film
{
    /** @param s  the decoded film strip */
    explicit Film(gfx::Image s) : strip(std::move(s))
    {
    }

    /** The frames `pixels` across, made through `images` unless a strip holds them already (FramesAtSize's halving). A
        size no strip holds any more is let go; the lock is held while a size is made, so it is made once.
        @param images  the toolkit's image operations
        @param pixels  the frame's side, in physical pixels
        @return        every frame at that size, in angle order, kept alive by the caller's reference */
    std::shared_ptr<const Frames> At(const ImageBackend &images, int pixels)
    {
        const std::lock_guard<std::mutex> held(lock);
        std::erase_if(sizes, [](const auto &size) { return size.second.expired(); });
        std::weak_ptr<const Frames> &slot = sizes[pixels];
        if (auto frames = slot.lock())
        {
            return frames;
        }

        // The strip is one frame wide and as many frames tall as it holds.
        const int side  = strip.width();
        const int count = std::max(1, strip.height() / std::max(1, side));
        Frames    frames;
        frames.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            gfx::Image frame = images.Clipped(strip, {0, i * side, side, side});
            while (frame.width() >= 4 * pixels)
            {
                frame = images.Rescaled(frame, frame.width() / 2, frame.height() / 2, gfx::Resampling::kHigh);
            }
            frames.push_back(images.Rescaled(frame, pixels, pixels, gfx::Resampling::kHigh));
        }
        auto made = std::make_shared<const Frames>(std::move(frames));
        slot      = made;
        return made;
    }

    const gfx::Image                           strip; ///< the decoded film strip.
    std::mutex                                 lock;  ///< held while a size is looked up or made.
    std::map<int, std::weak_ptr<const Frames>> sizes; ///< each size while a strip holds it
};

KnobStrip::KnobStrip(const ImageBackend &images, gfx::Image strip)
    : KnobStrip(images, std::make_shared<Film>(std::move(strip)))
{
}

KnobStrip::KnobStrip(const ImageBackend &images, std::shared_ptr<Film> film) : images_(images), film_(std::move(film))
{
}

KnobStrip KnobStrip::Shared(const ImageBackend &images, std::span<const std::uint8_t> asset)
{
    if (asset.empty())
    {
        return KnobStrip(images, gfx::Image());
    }
    // The module's own: this library is linked into each module with its symbols hidden, so two plug-ins in one host
    // hold a strip each. Each film is held by the strips alone - a weak entry here - so none outlives the last panel,
    // and the module unloads with nothing of the toolkit's left to free.
    static std::mutex                                                                  lock;
    static std::map<std::pair<const std::uint8_t *, std::size_t>, std::weak_ptr<Film>> films;
    const std::lock_guard<std::mutex>                                                  held(lock);
    std::erase_if(films, [](const auto &film) { return film.second.expired(); });
    std::weak_ptr<Film>  &slot = films[std::make_pair(asset.data(), asset.size())];
    std::shared_ptr<Film> film = slot.lock();
    if (film == nullptr)
    {
        film = std::make_shared<Film>(images.Decode(asset.data(), asset.size()));
        slot = film;
    }
    return KnobStrip(images, std::move(film));
}

float KnobStrip::SideFor(int width, int height) noexcept
{
    const float reach = std::max(0.0f, static_cast<float>(std::min(width, height)) - 8.0f) * 0.5f;
    return 2.0f * kKnobMargin * reach;
}

const gfx::Image &KnobStrip::strip() const noexcept
{
    return film_->strip;
}

const std::vector<gfx::Image> &KnobStrip::FramesAtSize(int pixels)
{
    for (const auto &[size, frames] : held_)
    {
        if (size == pixels)
        {
            return *frames;
        }
    }
    held_.emplace_back(pixels, film_->At(images_, pixels));
    return *held_.back().second;
}

void KnobStrip::Draw(gfx::Canvas &c, gfx::RectI area, float angle, float side)
{
    const auto centre = area.ToFloat().Reduced(4.0f).Centre();

    // The frames are scaled once per physical size, so a HiDPI display gets the detail it can show; the sizes drawn at
    // another scale - the window's last zoom - are let go, a zoom moving the scale by far more than its rounding.
    const float scale = c.PhysicalPixelScale();
    if (std::fabs(scale - scale_) > 1.0e-4f)
    {
        held_.clear();
        scale_ = scale;
    }
    const auto &frames = FramesAtSize(std::max(8, gfx::RoundToInt(side * scale)));

    // The frame nearest the angle, clockwise from twelve; the strip's frame i is at 2 pi i / count. Nearest rather
    // than a crossfade of two: at 128 frames the nose moves under 3 px between them and a blend would soften it.
    const int   count = static_cast<int>(frames.size());
    const float turns = angle / static_cast<float>(2.0 * std::numbers::pi);
    const int   index = gfx::RoundToInt((turns - std::floor(turns)) * static_cast<float>(count)) % count;

    c.SetResampling(gfx::Resampling::kHigh);
    c.SetOpacity(1.0f);
    c.DrawImage(frames[static_cast<std::size_t>(index)], gfx::RectF(side, side).WithCentre(centre));
}

// ---- The company mark ------------------------------------------------------------------------------------------

CompanyMark::CompanyMark(const ImageBackend &images, gfx::Image mark) : images_(images), mark_(std::move(mark))
{
}

const gfx::Image &CompanyMark::AtSize(int width, int height) const
{
    const auto key = std::make_pair(width, height);
    if (!at_size_.has_value() || at_size_->first != key)
    {
        at_size_.emplace(key, ShrunkImage(images_, mark_, width, height));
    }
    return at_size_->second;
}

void CompanyMark::Draw(gfx::Canvas &c, gfx::RectF cell) const
{
    const auto placed =
        CentredFit(gfx::RectF(static_cast<float>(mark_.width()), static_cast<float>(mark_.height())), cell);
    const float scale = c.PhysicalPixelScale();
    const auto &mark  = AtSize(gfx::RoundToInt(placed.width() * scale), gfx::RoundToInt(placed.height() * scale));
    c.SetResampling(gfx::Resampling::kHigh);
    c.DrawImage(mark, OnPhysicalPixels(placed, mark.width(), mark.height(), scale));
}

// ---- The legend plate and the button ---------------------------------------------------------------------------

void FillDropShadow(gfx::Canvas &c, gfx::RectI target, gfx::Color colour, int radius, gfx::PointI offset)
{
    gfx::Gradient ramp{colour, {}, colour.WithAlpha(0.0f), {}, false, {}};
    for (float i = 0.05f; i < 1.0f; i += 0.1f)
    {
        ramp.stops.emplace_back(static_cast<float>(1.0 - i), colour.WithMultipliedAlpha(i * i));
    }

    const float radius_inset    = static_cast<float>(radius) / 2.0f;
    const float expanded_radius = static_cast<float>(radius) + radius_inset;
    const auto  area =
        target.ToFloat().Reduced(radius_inset).Translated(static_cast<float>(offset.x), static_cast<float>(offset.y));

    // One piece: the ramp from `centre` to `edge`, each given as shares of the piece's width and height.
    const auto piece = [&](gfx::RectF section, bool corner, float centre_x, float centre_y, float edge_x,
                           float edge_y) {
        ramp.start  = {section.x() + section.width() * centre_x, section.y() + section.height() * centre_y};
        ramp.end    = {section.x() + section.width() * edge_x, section.y() + section.height() * edge_y};
        ramp.radial = corner;
        c.SetGradient(ramp);
        c.FillRect(section);
    };
    auto r      = area.Expanded(expanded_radius);
    auto top    = r.RemoveFromTop(expanded_radius);
    auto bottom = r.RemoveFromBottom(expanded_radius);
    piece(top.RemoveFromLeft(expanded_radius), true, 1.0f, 1.0f, 0.0f, 1.0f);
    piece(top.RemoveFromRight(expanded_radius), true, 0.0f, 1.0f, 1.0f, 1.0f);
    piece(top, false, 0.0f, 1.0f, 0.0f, 0.0f);
    piece(bottom.RemoveFromLeft(expanded_radius), true, 1.0f, 0.0f, 0.0f, 0.0f);
    piece(bottom.RemoveFromRight(expanded_radius), true, 0.0f, 0.0f, 1.0f, 0.0f);
    piece(bottom, false, 0.0f, 0.0f, 0.0f, 1.0f);
    piece(r.RemoveFromLeft(expanded_radius), false, 1.0f, 0.0f, 0.0f, 0.0f);
    piece(r.RemoveFromRight(expanded_radius), false, 0.0f, 0.0f, 1.0f, 0.0f);

    c.SetColor(colour);
    c.FillRect(area);
}

void DrawLegendPlate(gfx::Canvas &c, int width, int height, bool is_button_down, gfx::Color plate,
                     gfx::Color pressed_plate, gfx::Color edge, gfx::Color arrow)
{
    const auto rect = gfx::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).Reduced(1.0f);
    FillDropShadow(c, rect.ToNearestInt(), gfx::colors::kBlack.WithAlpha(0.45f), 6, {2, 3});
    c.SetColor(is_button_down ? pressed_plate : plate);
    c.FillRoundedRect(rect, 4.0f);
    c.SetColor(edge);
    c.DrawRoundedRect(rect, 4.0f, 1.2f);

    DrawPlateArrow(c, gfx::RectI(width, height), arrow);
}

void DrawSwitchPlate(gfx::Canvas &c, const Palette &palette, int width, int height, bool is_button_down)
{
    DrawLegendPlate(c, width, height, is_button_down, palette.plate, palette.plate.Darker(0.4f), palette.plate_edge,
                    palette.ink.WithAlpha(0.8f));
}

void DrawButtonPlate(gfx::Canvas &c, const Palette &palette, gfx::RectI bounds, bool on, bool is_mouse_over,
                     bool is_button_down)
{
    // The same plate as a switch, without the switch's shadow, lit a shade under the mouse and a shade more
    // while held; the edge comes up with it and further while the button is on (a toggled CABINET MAKER).
    const auto plate = bounds.ToFloat().Reduced(1.0f);
    const bool lit   = is_mouse_over || is_button_down || on;
    const auto fill  = palette.plate;
    c.SetColor(is_button_down ? fill.Brighter(kPlatePressedLift) : lit ? fill.Brighter(kPlateLitLift) : fill);
    c.FillRoundedRect(plate, 4.0f);
    c.SetColor(palette.plate_edge.WithMultipliedAlpha(on ? 1.8f : lit ? 1.6f : 1.0f));
    c.DrawRoundedRect(plate, 4.0f, 1.2f);
}

void DrawPlateArrow(gfx::Canvas &c, gfx::RectI bounds, gfx::Color arrow)
{
    const auto  rect = bounds.ToFloat().Reduced(1.0f);
    gfx::Path   triangle;
    const float ax = rect.right() - static_cast<float>(kArrowInset);
    const float ay = rect.CentreY();
    const float hw = static_cast<float>(kArrowHalfWidth);
    triangle.AddTriangle({ax - hw, ay - 2.5f}, {ax + hw, ay - 2.5f}, {ax, ay + 3.0f});
    c.SetColor(arrow);
    c.FillPath(triangle);
}

void DrawPlateLamp(gfx::Canvas &c, gfx::RectI bounds, bool lit)
{
    const gfx::Color dark{0xff2a2825}; // the unlit lens: an indicator's dark glass
    const auto       rect = bounds.ToFloat().Reduced(1.0f);
    const auto       lens = gfx::RectF(kPlateLampDiameter, kPlateLampDiameter)
                                .WithCentre({rect.x() + static_cast<float>(kArrowInset), rect.CentreY()});
    if (lit)
    {
        // The glow on the plate round the lens, as a lit indicator throws it: two rings, fading out; then the lens
        // hot at its centre and amber to its rim, a lamp seen through coloured glass.
        c.SetColor(colors::kAmber.WithAlpha(0.22f));
        c.FillEllipse(lens.Expanded(0.6f * kPlateLampDiameter));
        c.SetColor(colors::kAmber.WithAlpha(0.45f));
        c.FillEllipse(lens.Expanded(0.25f * kPlateLampDiameter));
        c.SetGradient(gfx::Gradient{
            gfx::Color(0xffffe2a0), lens.Centre(), colors::kAmber, {lens.right(), lens.CentreY()}, true, {}});
    }
    else
    {
        c.SetGradient(gfx::Gradient{dark.Brighter(0.6f),
                                    {lens.x() + 0.3f * lens.width(), lens.y() + 0.3f * lens.height()},
                                    dark.Darker(0.4f),
                                    {lens.right(), lens.bottom()},
                                    true,
                                    {}});
    }
    c.FillEllipse(lens);
    c.SetColor(colors::kLensRim);
    c.DrawEllipse(lens.Expanded(0.5f), 1.0f);
}

void DrawGear(gfx::Canvas &c, gfx::RectF area, gfx::Color colour)
{
    // Eight teeth, each a trapezoid narrower at its tip than at its root, the root circle between them and a bore of
    // a third of the gear's radius; the bore's outline runs the other way round, so the non-zero rule leaves it open.
    constexpr int   kTeeth     = 8;
    constexpr float kRoot      = 0.74f; // the root circle, as a share of the gear's radius
    constexpr float kBore      = 0.34f; // the bore
    constexpr float kRootWidth = 0.62f; // a tooth's width at its root, as a share of its pitch
    constexpr float kTipWidth  = 0.40f; // and at its tip
    constexpr int   kBoreSteps = 24;
    const float     r          = 0.5f * std::min(area.width(), area.height());
    const auto      centre     = area.Centre();
    const float     pitch      = 2.0f * std::numbers::pi_v<float> / static_cast<float>(kTeeth);
    const auto      at         = [&](float angle, float radius) {
        return gfx::PointF{centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle)};
    };
    gfx::Path gear;
    for (int i = 0; i < kTeeth; ++i)
    {
        const float mid   = pitch * static_cast<float>(i);
        const float root  = 0.5f * kRootWidth * pitch;
        const float tip   = 0.5f * kTipWidth * pitch;
        const auto  start = at(mid - root, kRoot * r);
        if (i == 0)
        {
            gear.MoveTo(start);
        }
        else
        {
            gear.LineTo(start);
        }
        gear.LineTo(at(mid - tip, r));
        gear.LineTo(at(mid + tip, r));
        gear.LineTo(at(mid + root, kRoot * r));
        gear.LineTo(at(mid + 0.5f * pitch, kRoot * r)); // the root circle, halfway to the next tooth
    }
    gear.Close();
    gear.MoveTo(at(0.0f, kBore * r));
    for (int i = 1; i <= kBoreSteps; ++i)
    {
        gear.LineTo(
            at(-2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kBoreSteps), kBore * r));
    }
    gear.Close();
    c.SetColor(colour);
    c.FillPath(gear);
}

// ---- The header ---// ---- The header
// ------------------------------------------------------------------------------------------------

void DrawTitle(gfx::Canvas &c, std::string_view title, gfx::RectI header_content, gfx::Color ink,
               std::string_view version, std::string_view badge, gfx::Color badge_colour)
{
    // One row, as tall as the title font, centred on the header's height and flush with the header content's
    // left. The title takes the whole row, with the ellipsis off, so nothing measures it and nothing can trim it.
    const auto title_font = gfx::Font(kTitleFontSize * kTextScale).Bold();
    const int  title_rows = gfx::RoundToInt(title_font.height);
    const auto row        = header_content.WithSizeKeepingCentre(header_content.width(), title_rows);

    c.SetColor(ink);
    c.SetFont(title_font);
    c.DrawText(title, row.ToFloat(), gfx::Justify::kCentredLeft, false);

    if (version.empty())
    {
        return;
    }

    // The stamp follows the title on its row. The two fonts are centred on the same row, so their baselines
    // differ by half the difference of their (ascent - descent); the stamp's row is shifted by that so the
    // figures stand on the title's line rather than floating above it.
    const auto  version_font = gfx::Font(kVersionFontSize * kTextScale);
    const int   title_width  = static_cast<int>(std::ceil(c.TextWidth(title_font, title)));
    const float baseline_shift =
        0.5f * ((c.Ascent(title_font) - c.Descent(title_font)) - (c.Ascent(version_font) - c.Descent(version_font)));
    const auto stamp_row =
        row.WithTrimmedLeft(title_width + kVersionGap).Translated(0, gfx::RoundToInt(baseline_shift));
    c.SetColor(ink.WithAlpha(0.35f));
    c.SetFont(version_font);
    c.DrawText(version, stamp_row.ToFloat(), gfx::Justify::kCentredLeft, false);

    if (badge.empty())
    {
        return;
    }
    // The badge follows the stamp on the same line, bold and at full strength, where the eye that reads the version
    // finds it.
    const auto badge_font    = version_font.Bold();
    const int  version_width = static_cast<int>(std::ceil(c.TextWidth(version_font, version)));
    c.SetColor(badge_colour);
    c.SetFont(badge_font);
    c.DrawText(badge, stamp_row.WithTrimmedLeft(version_width + kVersionGap).ToFloat(), gfx::Justify::kCentredLeft,
               false);
}

void DrawHeader(gfx::Canvas &c, const CompanyMark &mark, gfx::RectI header, std::string_view title,
                std::string_view version, gfx::Color ink, gfx::Color rule, int right_reserve, std::string_view badge,
                gfx::Color badge_colour)
{
    // Everything starts at x = kHeaderInset, clear of the top-left corner screw baked into the face plate, and
    // stops short of what the right end holds; the reserve is measured from the window's edge, as the inset is,
    // so a lamp strip narrower than the inset takes nothing more off. The mark leads, then the title.
    auto content = header.Reduced(kHeaderInset, 0).WithTrimmedRight(std::max(0, right_reserve - kHeaderInset));
    // A lamp or a meter repainting its own strip clips the paint to it: when none of the content lies inside the
    // clip, the mark and the three glyph layouts are skipped and only the rule below is redrawn, rather than laid
    // out on every lamp frame at up to the display's rate.
    if (c.ClipBounds().Intersects(content))
    {
        if (mark.IsValid())
        {
            mark.Draw(c, content.RemoveFromLeft(kLogoWidth).ToFloat().Reduced(0.0f, static_cast<float>(kLogoInset)));
            content.RemoveFromLeft(kLogoGap);
        }
        DrawTitle(c, title, content, ink, version, badge, badge_colour);
    }

    // The rule under the header, on the plate's seam.
    c.SetColor(rule);
    c.DrawHorizontalLine(header.bottom(), static_cast<float>(header.x()), static_cast<float>(header.right()));
}

gfx::RectI LampStrip(gfx::RectI header) noexcept
{
    return header.RemoveFromRight(kLampStrip);
}

void DrawLamp(gfx::Canvas &c, gfx::RectI header, double glow, gfx::Color accent, gfx::Color rim)
{
    // Trimmed off the right edge so the glow stays clear of the corner screw on the face plate.
    const auto  lamp = LampStrip(header).WithTrimmedRight(28).ToFloat().Reduced(18.0f, 16.0f);
    const float lit  = static_cast<float>(std::clamp(glow, 0.0, 1.0));

    c.SetColor(accent.WithAlpha(0.10f + 0.35f * lit));
    c.FillEllipse(lamp.Expanded(9.0f * lit + 3.0f));
    c.SetColor(accent.WithAlpha(0.25f + 0.75f * lit));
    c.FillEllipse(lamp);
    c.SetColor(rim);
    c.DrawEllipse(lamp, 1.0f);
}

// ---- The indicators --------------------------------------------------------------------------------------------

float IndicatorFloor(gfx::RectI cell, int tube_height) noexcept
{
    return static_cast<float>(cell.y()) + 3.0f + static_cast<float>(tube_height);
}

void DrawLedCell(gfx::Canvas &c, gfx::RectI cell, std::string_view caption, gfx::Color lit, double glow,
                 int tube_height)
{
    const gfx::Color dark{0xff2a2825}; // the unlit lens: dark glass
    const float      lit_amount = static_cast<float>(std::clamp(glow, 0.0, 1.0));
    // The lens sits on the indicators' floor, its bezel a pixel above it, where a tube in the next cell stands: so
    // its caption falls on the tubes' row and a header's LEDs and tubes read as one line of captions.
    const float floor = IndicatorFloor(cell, tube_height);
    const auto  led =
        gfx::RectF(kLensSize, kLensSize).WithCentre({cell.ToFloat().CentreX(), floor - 1.0f - 0.5f * kLensSize});
    // Unlit, the lens is the dark glass alone; the first hint of glow tints it and the rest brightens it.
    const auto colour = lit_amount > 0.0f ? dark.InterpolatedWith(lit, 0.3f + 0.7f * lit_amount) : dark;
    if (lit_amount > 0.0f)
    {
        c.SetColor(lit.WithAlpha(0.35f * lit_amount));
        c.FillEllipse(led.Expanded(3.0f + 3.0f * lit_amount));
    }
    c.SetGradient(gfx::Gradient{colour.Brighter(0.6f),
                                {led.x() + 3.0f, led.y() + 3.0f},
                                colour.Darker(0.4f),
                                {led.right(), led.bottom()},
                                true,
                                {}});
    c.FillEllipse(led);
    c.SetColor(colors::kLensRim);
    c.DrawEllipse(led.Expanded(1.0f), 1.5f);
    DrawIndicatorCaption(c, cell, static_cast<int>(floor) + kIndicatorCaptionGap, caption);
}

void DrawTubeCell(gfx::Canvas &c, gfx::RectI cell, std::string_view caption, TubeGlyph::Kind tube, gfx::Color lit,
                  double glow, int tube_height)
{
    const auto &glyph      = TubeGlyph::Of(tube);
    const auto  part       = glyph.bounds(); // in millimetres
    const float lit_amount = static_cast<float>(std::clamp(glow, 0.0, 1.0));

    // The tube stands on the indicators' floor, tube_height under the cell's top, centred: the EL34 the full
    // height, the 12AX7 - half its size in life - at kSmallTubeShare of it, so a preamp tube reads as the smaller
    // beside a power tube and both stay legible at 36 px; its width follows the part's proportions. The caption's
    // row is under the floor, the same row an LED's caption takes (DrawLedCell).
    const float full      = static_cast<float>(tube_height);
    const float height    = tube == TubeGlyph::Kind::kEL34 ? full : kSmallTubeShare * full;
    const float width     = height * part.width() / part.height();
    const float floor     = IndicatorFloor(cell, tube_height);
    const auto  tube_area = gfx::RectF(width, height).WithCentre({cell.ToFloat().CentreX(), floor - 0.5f * height});

    // One fit for the halo and the tube: the light comes from the heater inside the plates.
    const auto fit    = glyph.FitTo(tube_area);
    const auto heater = fit.heater;

    // The halo behind the envelope grows with the glow; unlit there is none. It is light, not a sticker: a radial
    // gradient from the heater, the accent at the centre and nothing left by a little over half a tube's height
    // out (more as the glow grows), so it fades into the panel the way a lamp's light does.
    if (lit_amount > 0.0f)
    {
        const float reach = (0.55f + 0.25f * lit_amount) * full;
        c.SetGradient(gfx::Gradient{lit.WithAlpha(0.5f * lit_amount),
                                    heater,
                                    lit.WithAlpha(0.0f),
                                    {heater.x, heater.y + reach},
                                    true,
                                    {{0.4f, lit.WithAlpha(0.2f * lit_amount)}}});
        c.FillEllipse(gfx::RectF(2.0f * reach, 2.0f * reach).WithCentre(heater));
    }

    glyph.Draw(c, fit, colors::kInk, lit, glow);
    DrawIndicatorCaption(c, cell, static_cast<int>(floor) + kIndicatorCaptionGap, caption);
}

void DrawIndicatorCaption(gfx::Canvas &c, gfx::RectI cell, int top, std::string_view text)
{
    c.SetColor(colors::kInk.WithAlpha(0.45f));
    c.SetFont(gfx::Font(kFinePrintFontSize * kTextScale));
    c.DrawText(text, cell.WithTop(top).WithHeight(kIndicatorCaptionHeight).ToFloat(), gfx::Justify::kCentred);
}

// ---- The knob bank's frames ------------------------------------------------------------------------------------

void DrawFrames(gfx::Canvas &c, const std::vector<gfx::RectF> &frames, gfx::Color ink)
{
    c.SetColor(ink.WithAlpha(0.6f));
    for (const auto &frame : frames)
    {
        c.DrawRoundedRect(frame, 6.0f, 1.2f);
    }
}
} // namespace reckless::look
