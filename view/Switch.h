#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Label.h"
#include "Menu.h"
#include "ParameterControl.h"
#include "View.h"

namespace reckless::view
{
/**
    A choice parameter's plate - the series' switched setting - drawn as the releases built on JUCE drew it, a box
    with its label inside it: the plate painted over the whole (the series' legend plate, look, handed in as a
    painter, darker while a press holds it), and the current choice's name a Label (Label.h) inset from the plate's
    sides by `legend_inset`, so it clears the arrow and stays centred under the caption; its caption is a Label the
    panel places over it as a sibling. A press opens a menu of every choice with the current one ticked, under the
    plate; another choice from it is one gesture on the parameter, the current one none. Dimmed, where a setting
    ignores it, the plate and its caption fade to the dimmed alpha, its legend to half again, and it opens nothing.
*/
class Switch final : public View
{
  public:
    /** Paints the plate over `bounds`, `held` while a press holds it. The canvas's origin is the switch's top left. */
    using PlatePainter = std::function<void(gfx::Canvas &, gfx::RectI bounds, bool held)>;

    /** What a switch is dressed in. */
    struct Style
    {
        MenuStyle    menu;               ///< the menu of choices
        Label::Style legend;             ///< the current choice's name on the plate
        Label::Style caption;            ///< the caption over the plate
        int          legend_inset{18};   ///< the legend's room in from each side: the arrow and its clearance
        float        dimmed_alpha{0.4f}; ///< the opacity of the plate and its caption while dimmed, 0 to 1
    };

    /** A plate on a choice parameter.
        @param parameter  the choice parameter, owned by the caller and outliving the switch
        @param caption    the caption's text
        @param painter    paints the plate; empty for none
        @param style      the plate's dress */
    Switch(ParameterControl &parameter, std::string caption, PlatePainter painter, Style style)
        : parameter_(parameter), painter_(std::move(painter)), style_(std::move(style)), legend_(style_.legend),
          caption_(style_.caption), shown_(parameter.ChoiceIndex())
    {
        cursor = Cursor::kPointingHand;
        legend_.SetText(parameter_.ChoiceText(shown_));
        legend_.SetInterceptsMouse(false, false);
        AddChild(legend_);
        caption_.SetText(std::move(caption));
    }

    /** Once a display frame: the legend follows the parameter where the host moved it to another choice. Message
        thread. */
    void Refresh()
    {
        const int now = parameter_.ChoiceIndex();
        if (now != shown_)
        {
            shown_ = now;
            legend_.SetText(parameter_.ChoiceText(shown_));
        }
    }

    /** The plate and its caption faded and closed to the mouse, for a setting that ignores it; false brings it back.
        @param dimmed  true to dim the plate, close its menu and ignore the mouse; false to restore it */
    void SetDimmed(bool dimmed)
    {
        SetEnabled(!dimmed);
        SetAlpha(dimmed ? style_.dimmed_alpha : 1.0f);
        caption_.SetAlpha(dimmed ? style_.dimmed_alpha : 1.0f);
        cursor = dimmed ? Cursor::kArrow : Cursor::kPointingHand;
        if (dimmed)
        {
            menu_.reset();
        }
    }

    /** Whether the plate is dimmed.
        @return  true while it is dimmed and ignores the mouse */
    bool dimmed() const noexcept
    {
        return !IsEnabled();
    }

    /** Whether the menu of choices is open.
        @return  true while it is */
    bool menu_open() const noexcept
    {
        return menu_ != nullptr;
    }

    /** The legend: the current choice's name, inside the plate.
        @return  the legend label, owned by the switch */
    Label &legend() noexcept
    {
        return legend_;
    }

    /** The caption: the panel adds it beside the plate, over it.
        @return  the caption label, owned by the switch */
    Label &caption() noexcept
    {
        return caption_;
    }

    /** The plate and its caption hidden whole; true brings them back.
        @param shown  false to hide the plate and its caption */
    void SetShown(bool shown)
    {
        SetVisible(shown);
        caption_.SetVisible(shown);
    }

    /** The parameter the plate drives.
        @return  the choice parameter, owned by the caller */
    ParameterControl &parameter() noexcept
    {
        return parameter_;
    }

    /** The legend is laid out inside the plate, inset by `legend_inset` from each side. */
    void Resized() override
    {
        legend_.SetBounds({style_.legend_inset, 0, std::max(0, width() - 2 * style_.legend_inset), height()});
    }

    /** The plate, through the painter, held while a press holds it.
        @param c  the canvas, the switch's top left at its origin */
    void Paint(gfx::Canvas &c) override
    {
        if (painter_)
        {
            painter_(c, LocalBounds(), held_);
        }
    }

    /** A press opens the menu of every choice under the plate, the current one ticked; ignored while dimmed, for a
        right press and while the menu is open.
        @param e  the press */
    void MouseDown(const MouseEvent &e) override
    {
        if (!IsEnabled() || e.right || menu_ != nullptr)
        {
            return;
        }
        held_ = true;
        Repaint();
        std::vector<MenuItem> items;
        const int             count   = parameter_.ChoiceCount();
        const int             current = parameter_.ChoiceIndex();
        for (int i = 0; i < count; ++i)
        {
            MenuItem item;
            item.text   = parameter_.ChoiceText(i);
            item.id     = i + 1; // 0 is "dismissed"
            item.ticked = i == current;
            items.push_back(std::move(item));
        }
        MenuStyle style = style_.menu;
        style.min_width = std::max(style.min_width, width()); // at least as wide as the plate it hangs from
        menu_           = MenuSession::Open(*this, std::move(items), style, {0, height()}, [this](int id) {
            menu_.reset();
            if (id > 0)
            {
                Choose(id - 1);
            }
        });
    }

    /** The press ends: the plate is no longer held. */
    void MouseUp(const MouseEvent &) override
    {
        if (held_)
        {
            held_ = false;
            Repaint();
        }
    }

    /** The parameter to choice `index`, as one gesture: what a choice from the menu does. The choice the plate is on
        already is no change and no gesture: a gesture is what a host, or the series' mode starting points
        (vst-plugins' common/core/ModeDefaults.h), reads as a hand on the setting.
        @param index  the choice, 0 up to the parameter's choice count */
    void Choose(int index)
    {
        if (index == parameter_.ChoiceIndex())
        {
            Refresh();
            return;
        }
        parameter_.BeginGesture();
        parameter_.SetNormalized(parameter_.NormalizedForChoice(index));
        parameter_.EndGesture();
        Refresh();
    }

  private:
    ParameterControl            &parameter_;   ///< the choice parameter; owned by the caller
    PlatePainter                 painter_;     ///< paints the plate
    Style                        style_;       ///< the plate's dress
    Label                        legend_;      ///< the current choice's name
    Label                        caption_;     ///< the caption the panel places over the plate
    int                          shown_;       ///< the choice the legend shows
    bool                         held_{false}; ///< true while a press holds the plate
    std::unique_ptr<MenuSession> menu_;        ///< the open menu; null when closed
};
} // namespace reckless::view
