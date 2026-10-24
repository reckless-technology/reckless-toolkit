#pragma once

// What the list and text views' suites share: a canvas that draws nothing and records the text and the fills it is
// asked for where they land, and a host that measures text as a fixed-pitch font - every code point the same width -
// and keeps the clipboard and the keyboard as a window would.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Canvas.h"
#include "Text.h"
#include "View.h"

namespace reckless::test
{
/** A canvas that draws nothing and records each text drawn - its string, its box in the root's coordinates and its
    colour - and each rectangle filled, the transform's translation applied. Measures text as `pitch` pixels a code
    point. */
class TextRecordingCanvas final : public gfx::Canvas
{
  public:
    /** A text the canvas was asked to draw. */
    struct Drawn
    {
        std::string text;   ///< the UTF-8 text
        gfx::RectF  area;   ///< its box, in the root's coordinates
        gfx::Color  colour; ///< the brush's colour
    };

    /** A rectangle the canvas was asked to fill. */
    struct Fill
    {
        gfx::RectI area;   ///< in the root's coordinates
        gfx::Color colour; ///< the brush's colour
    };

    /** @param pitch  each code point's width, in pixels */
    explicit TextRecordingCanvas(float pitch = 7.0f) : pitch_(pitch)
    {
    }

    std::vector<Drawn> texts; ///< every text drawn, in order
    std::vector<Fill>  fills; ///< every rectangle filled, in order

    void Save() override
    {
        stack_.push_back(state_);
    }

    void Restore() override
    {
        state_ = stack_.back();
        stack_.pop_back();
    }

    void AddTransform(const gfx::Affine &t) override
    {
        state_.dx += t.m02;
        state_.dy += t.m12;
    }

    bool ClipToRect(gfx::RectI r) override
    {
        state_.clip = state_.clip.Intersection(r.Translated(Dx(), Dy()));
        return !state_.clip.IsEmpty();
    }

    void ClipToPath(const gfx::Path &, const gfx::Affine &) override
    {
    }

    gfx::RectI ClipBounds() const override
    {
        return state_.clip.Translated(-Dx(), -Dy());
    }

    float PhysicalPixelScale() const override
    {
        return 1.0f;
    }

    void SetColor(gfx::Color c) override
    {
        state_.colour = c;
    }

    void SetGradient(const gfx::Gradient &) override
    {
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const gfx::Font &f) override
    {
        state_.font = f;
    }

    void SetResampling(gfx::Resampling) override
    {
    }

    const gfx::Font &CurrentFont() const override
    {
        return state_.font;
    }

    void FillAll() override
    {
        fills.push_back({state_.clip, state_.colour});
    }

    void FillAll(gfx::Color c) override
    {
        fills.push_back({state_.clip, c});
    }

    void FillRect(gfx::RectI r) override
    {
        fills.push_back({r.Translated(Dx(), Dy()), state_.colour});
    }

    void FillRect(gfx::RectF r) override
    {
        FillRect(r.ToNearestInt());
    }

    void DrawRect(gfx::RectF, float) override
    {
    }

    void FillRoundedRect(gfx::RectF, float) override
    {
    }

    void DrawRoundedRect(gfx::RectF, float, float) override
    {
    }

    void FillEllipse(gfx::RectF) override
    {
    }

    void DrawEllipse(gfx::RectF, float) override
    {
    }

    void DrawLine(gfx::PointF, gfx::PointF, float) override
    {
    }

    void DrawHorizontalLine(int, float, float) override
    {
    }

    void DrawVerticalLine(int, float, float) override
    {
    }

    void FillPath(const gfx::Path &, const gfx::Affine &) override
    {
    }

    void StrokePath(const gfx::Path &, const gfx::Stroke &, const gfx::Affine &) override
    {
    }

    void DrawText(std::string_view text, gfx::RectF area, gfx::Justify, bool) override
    {
        texts.push_back(
            {std::string(text),
             {area.x() + static_cast<float>(Dx()), area.y() + static_cast<float>(Dy()), area.width(), area.height()},
             state_.colour});
    }

    void DrawFittedText(std::string_view text, gfx::RectI area, gfx::Justify justify, int, float) override
    {
        DrawText(text, area.ToFloat(), justify, true);
    }

    float TextWidth(const gfx::Font &, std::string_view text) const override
    {
        return pitch_ * static_cast<float>(CodePoints(text));
    }

    float Ascent(const gfx::Font &f) const override
    {
        return f.height * 0.8f;
    }

    float Descent(const gfx::Font &f) const override
    {
        return f.height * 0.2f;
    }

    void DrawImage(const gfx::Image &, gfx::RectF, bool) override
    {
    }

    void DrawImageTransformed(const gfx::Image &, const gfx::Affine &) override
    {
    }

    void BeginTransparencyLayer(float) override
    {
        Save();
    }

    void EndTransparencyLayer() override
    {
        Restore();
    }

    /** How many code points `text` holds, U+FFFD for each broken part.
        @param text  UTF-8
        @return      the count */
    static std::size_t CodePoints(std::string_view text)
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i < text.size(); ++count)
        {
            (void)gfx::NextCodePoint(text, i);
        }
        return count;
    }

  private:
    /** The canvas's state: the translation, the clip in the root's coordinates, the brush and the font. */
    struct State
    {
        float      dx{0.0f};                   ///< the translation across
        float      dy{0.0f};                   ///< the translation down
        gfx::RectI clip{0, 0, 100000, 100000}; ///< the clip, in the root's coordinates
        gfx::Color colour{};                   ///< the brush
        gfx::Font  font{};                     ///< the font
    };

    /** @return  the translation across, whole pixels */
    int Dx() const noexcept
    {
        return gfx::RoundToInt(state_.dx);
    }

    /** @return  the translation down, whole pixels */
    int Dy() const noexcept
    {
        return gfx::RoundToInt(state_.dy);
    }

    float              pitch_; ///< each code point's width, in pixels
    State              state_; ///< the state now
    std::vector<State> stack_; ///< the saved states
};

/** A host that measures text as `pitch` pixels a code point, as the canvas above does, holds a clipboard's text, and
    hands the keyboard to its router as a window does. */
class TextHost final : public view::Host
{
  public:
    /** @param pitch  each code point's width, in pixels */
    explicit TextHost(float pitch = 7.0f) : pitch_(pitch)
    {
    }

    view::MouseRouter *router{nullptr};      ///< the router keys and focus go through; null for none
    std::string        clipboard;            ///< what a paste reads
    bool               window_focused{true}; ///< whether the window holds the platform's keyboard focus

    void Invalidate(gfx::RectI) override
    {
    }

    void RequestKeyboardFocus(view::View *v) override
    {
        if (router != nullptr)
        {
            router->Focus(v);
        }
    }

    bool WindowHasKeyboardFocus() const override
    {
        return window_focused;
    }

    float TextWidth(const gfx::Font &, std::string_view text) const override
    {
        return pitch_ * static_cast<float>(TextRecordingCanvas::CodePoints(text));
    }

    std::string ClipboardText() override
    {
        return clipboard;
    }

    void ViewLeaving(const view::View &v) override
    {
        if (router != nullptr)
        {
            router->Forget(v);
        }
    }

  private:
    float pitch_; ///< each code point's width, in pixels
};
} // namespace reckless::test
