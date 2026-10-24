#pragma once

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Button.h"
#include "Geometry.h"
#include "Label.h"
#include "ParameterText.h"
#include "View.h"

namespace reckless::panel
{
/**
    A button that rolls a whole design from a seed and shows the seed on it - "NAME (4133)" - with an entry for a typed
    seed: the Cab's CABRACADABRA and the Space's GOOD VERBRATIONS. A click steps the seed (after `top`, 1) and rolls
    from it; a right click (view::Button::on_secondary_click) shows the entry, a view::Label laid exactly over the
    button, its field open with the seed selected; Return, or the keyboard taken elsewhere, rolls from a changed seed
    (ParseSeed: the leading number, the roll itself rounding and clamping it), Escape just closes it, and however the
    edit ends the entry hides again (its on_editor_hide). What a roll is, and how the entry looks, are the drawer's.

    Not a view: the button and the entry are its owner's children (AddTo), so the owner's tree, its painting and its
    mouse are as they were with the two made by hand. Message thread.
*/
class SeedButton
{
  public:
    /** `name` the button's legend before its seed; `seed` reads the seed in force; `roll` rolls a design from a seed,
        which writes it back, so the next step carries on from it.
        @param name         the button's legend before its seed
        @param painter      how the button draws itself
        @param entry_style  how the typed-seed entry looks
        @param top          the seed's top; a step after it returns to 1
        @param seed         reads the seed in force
        @param roll         rolls a design from a seed and writes the seed back; given a whole seed from 1 to `top` or
                            a typed one, which it rounds and clamps */
    SeedButton(std::string name, view::Button::Painter painter, view::Label::Style entry_style, double top,
               std::function<double()> seed, std::function<void(double seed)> roll)
        : name_(std::move(name)), top_(top), seed_(std::move(seed)), roll_(std::move(roll)),
          button_(name_, std::move(painter), [this] { Step(); }), entry_(std::move(entry_style))
    {
        button_.on_secondary_click = [this] { EnterSeed(); };
        entry_.SetVisible(false); // present but hidden until asked for
        entry_.on_editor_hide = [this] { entry_.SetVisible(false); };
        entry_.on_edited      = [this](const std::string &typed) {
            if (const auto typed_seed = ParseSeed(typed))
            {
                roll_(*typed_seed);
            }
        };
    }

    SeedButton(const SeedButton &)            = delete;
    SeedButton &operator=(const SeedButton &) = delete;

    /** The button, then the entry over it, as `parent`'s children.
        @param parent  the owner's view, which must outlive this object's use of them */
    void AddTo(view::View &parent)
    {
        parent.AddChild(button_);
        parent.AddChild(entry_);
    }

    /** The button at `area`, and the entry exactly over it.
        @param area  the button's bounds, in the parent's coordinates */
    void SetBounds(gfx::RectI area)
    {
        button_.SetBounds(area);
        entry_.SetBounds(area);
    }

    /** The seed in force on the button. */
    void Refresh()
    {
        button_.SetText(name_ + " (" + Whole(seed_()) + ")");
    }

    /** A click on the button: the seed stepped (after `top`, 1) and a design rolled from it. */
    void Step()
    {
        roll_(std::fmod(seed_(), top_) + 1.0);
    }

    /** A right click on the button: the entry shown over it, its field open with the seed selected. */
    void EnterSeed()
    {
        entry_.SetText(Whole(seed_()));
        entry_.SetVisible(true);
        entry_.ShowEditor();
    }

    /** A typed seed: the leading number in `text`, or nothing if it does not start with a digit. The roll rounds and
        clamps it to the seed's range, so this only has to find a number. Pure.
        @param text  the typed text
        @return      the number at the front of the text; nothing when it does not start with a digit */
    static std::optional<double> ParseSeed(std::string_view text)
    {
        const std::string_view trimmed = core::Trim(text);
        if (trimmed.empty() || trimmed.front() < '0' || trimmed.front() > '9')
        {
            return std::nullopt;
        }
        return core::ParseNumber(trimmed);
    }

    /** The button, for its owner's layout and suites.
        @return  the button, owned by this object */
    view::Button &button() noexcept
    {
        return button_;
    }

    /** The typed-seed entry, for its owner's layout and suites.
        @return  the entry, owned by this object */
    view::Label &entry() noexcept
    {
        return entry_;
    }

  private:
    /** A seed as it is shown: the nearest whole number.
        @param seed  the seed
        @return      its text */
    static std::string Whole(double seed)
    {
        return std::to_string(core::RoundToInt(seed));
    }

    std::string                 name_;   ///< the button's legend before its seed
    double                      top_;    ///< the seed's top
    std::function<double()>     seed_;   ///< reads the seed in force
    std::function<void(double)> roll_;   ///< rolls a design from a seed
    view::Button                button_; ///< the button that steps and rolls
    view::Label                 entry_;  ///< the entry a right click opens over the button
};
} // namespace reckless::panel
