#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Button.h"
#include "Canvas.h"
#include "DemoMode.h"
#include "Dialog.h"
#include "JsonFile.h"
#include "Licence.h"
#include "LicenceStore.h"
#include "TextField.h"
#include "View.h"

namespace reckless::window
{
/**
    The licence over a window: a veil over everything and a card that says what licence the machine holds - the
    licensee's name and address, the serial and the major version it opens, or, without one, that the plugins play in
    demo mode and why the licence on file does not check - with a line to paste a licence into and the buttons that
    act on it: Install (the pasted text, or Return), From file... (a licence file, through the platform's dialog),
   Buy... (the shop, in the person's browser), Remove (the user's licence off the machine) and Close (or Escape, or a
   press on the veil). A licence that does not check is never written: the card says why, in the words a person reads
    (licence::VerdictText). Every action goes through the machine's store, so every plugin in the process, and every
    other process at its next poll, runs under what the card shows.

    Its look is the panel's dialog's (view::Dialog::Style). It stays in the tree until its owner destroys it, which
    the `closed` callback is the place for; it runs once, after the event that closed the card.
*/
class LicenceDialog final : public view::View
{
  public:
    /** Where Buy... goes: the suite's page in the shop. */
    static constexpr std::string_view kBuyUrl = "https://recklesstechnology.com/plugins/buy";

    /** The card's width at most, in shell units. */
    static constexpr int kCardWidth = 460;

    /** A card over `root`, sized to it, reading and writing `store`.
        @param root      the window's root view; the dialog is added to it, covers it and must not outlive it
        @param store     the machine's licence, which outlives the dialog
        @param style     the look, the panel's dialog's
        @param open_url  opens a page in the person's browser (Buy...); null hides the button
        @param closed    runs once, after the event that closed the card; its owner destroys the dialog there */
    LicenceDialog(view::View &root, licence::Store &store, view::Dialog::Style style,
                  std::function<void(const std::string &)> open_url, std::function<void()> closed)
        : root_(root), store_(store), style_(std::move(style)), open_url_(std::move(open_url)),
          closed_(std::move(closed)), field_(FieldStyle(style_.field)),
          install_("Install", style_.button_painter, [this] { InstallText(field_.text()); }),
          file_("From file...", style_.button_painter, [this] { ChooseLicenceFile(); }),
          buy_("Buy...", style_.button_painter, [this] { Buy(); }),
          remove_("Remove", style_.button_painter, [this] { Remove(); }),
          close_("Close", style_.button_painter, [this] { Close(); })
    {
        field_.commit_on_focus_lost = false; // Return installs; leaving the field does not
        field_.on_commit            = [this](const std::string &text) { InstallText(text); };
        field_.on_cancel            = [this] { Close(); };
        AddChild(field_);
        for (view::Button *button : {&install_, &file_, &buy_, &remove_, &close_})
        {
            AddChild(*button);
        }
        buy_.SetVisible(static_cast<bool>(open_url_));
        Read();
        SetBounds(root.LocalBounds());
        root.AddChild(*this);
        field_.GrabKeyboardFocus();
    }

    /** The dialog removed from the root it covers, if it is still in it. */
    ~LicenceDialog() override
    {
        if (parent() != nullptr)
        {
            parent()->RemoveChild(*this);
        }
    }

    /** Lays the field and the buttons out in the card. */
    void Resized() override
    {
        const gfx::RectI card    = Card();
        const int        inner_x = card.x() + style_.padding;
        const int        inner_w = card.width() - 2 * style_.padding;
        int              y       = card.y() + style_.padding + LinesHeight();
        field_.SetBounds({inner_x, y, inner_w, 24});
        y += 24 + style_.padding / 2 + LineHeight() + style_.padding / 2;

        std::vector<view::Button *> shown;
        for (view::Button *button : {&install_, &file_, &buy_, &remove_, &close_})
        {
            if (button->visible())
            {
                shown.push_back(button);
            }
        }
        const int count = static_cast<int>(shown.size());
        const int gap   = style_.padding / 2;
        const int each  = count > 0 ? (inner_w - gap * (count - 1)) / count : 0;
        for (int i = 0; i < count; ++i)
        {
            shown[static_cast<std::size_t>(i)]->SetBounds({inner_x + i * (each + gap), y, each, style_.button_height});
        }
    }

    /** Draws the veil, the card, what the machine's licence is and the last action's word.
        @param c  the canvas, in the dialog's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        c.SetColor(style_.veil);
        c.FillAll();
        const gfx::RectI card = Card();
        c.SetColor(style_.card);
        c.FillRoundedRect(card.ToFloat(), 6.0f);
        c.SetColor(style_.outline);
        c.DrawRoundedRect(card.ToFloat(), 6.0f, 1.0f);

        const int x = card.x() + style_.padding;
        const int w = card.width() - 2 * style_.padding;
        int       y = card.y() + style_.padding;
        c.SetColor(style_.text);
        c.SetFont(style_.font.Bold());
        c.DrawText(lines_.front(), gfx::RectI{x, y, w, LineHeight()}.ToFloat(), gfx::Justify::kCentredLeft, true);
        c.SetFont(style_.font);
        for (std::size_t i = 1; i < lines_.size(); ++i)
        {
            y += LineHeight();
            c.DrawText(lines_[i], gfx::RectI{x, y, w, LineHeight()}.ToFloat(), gfx::Justify::kCentredLeft, true);
        }

        // The last action's word, under the field: why a licence did not check, or that it did.
        const gfx::RectI field = field_.bounds();
        c.SetColor(note_is_error_ ? style_.outline : style_.text);
        c.DrawText(note_, gfx::RectI{x, field.bottom() + style_.padding / 2, w, LineHeight()}.ToFloat(),
                   gfx::Justify::kCentredLeft, true);
    }

    /** A press on the veil, outside the card, closes it. Inside the card, nothing.
        @param e  the press: its position in the dialog's local coordinates */
    void MouseDown(const view::MouseEvent &e) override
    {
        if (!Card().ToFloat().Contains(e.position))
        {
            Close();
        }
    }

    /** Escape closes; no other key passes the card.
        @param key  the key pressed
        @return     always true: the card is modal */
    bool KeyPressed(const view::KeyEvent &key) override
    {
        if (key.key == view::Key::kEscape)
        {
            Close();
        }
        return true;
    }

    /** `text` checked and, valid, installed as the user's licence; the card says either way.
        @param text  a licence as the person has it, white space and all
        @return      the check */
    licence::Checked InstallText(std::string_view text)
    {
        const licence::Checked checked = store_.Install(text);
        if (checked.verdict == licence::Verdict::kValid)
        {
            field_.SetText({});
            SetNote("Installed. Thank you.", false);
        }
        else
        {
            SetNote(std::string(licence::VerdictText(checked.verdict)), true);
        }
        Read();
        return checked;
    }

    /** The lines the card shows about the machine's licence, the first its heading.
        @return  the lines */
    const std::vector<std::string> &lines() const noexcept
    {
        return lines_;
    }

    /** The last action's word.
        @return  the note, empty before any action */
    const std::string &note() const noexcept
    {
        return note_;
    }

    /** The line a licence is pasted into, for the suites.
        @return  the field */
    view::TextField &field() noexcept
    {
        return field_;
    }

    /** The button that takes the user's licence off the machine, shown while one is installed.
        @return  the button */
    view::Button &remove_button() noexcept
    {
        return remove_;
    }

  private:
    /** The panel's field look, admitting only what a licence is made of: the format's prefix, base64url and the dots.
        A wrapped licence pasted from an email loses its line breaks on the way in.
        @param style  the panel's field look
        @return       that look, restricted */
    static view::TextField::Style FieldStyle(view::TextField::Style style)
    {
        style.allowed    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";
        style.max_length = licence::kMaxTextLength;
        return style;
    }

    /** The machine's licence read into the card's lines, and Remove shown only while the user has one to remove. */
    void Read()
    {
        const licence::Status status = store_.status();
        lines_.clear();
        if (status.checked.verdict == licence::Verdict::kValid && status.checked.licence)
        {
            const licence::Licence &held = *status.checked.licence;
            lines_.push_back("Licensed to " + held.name);
            lines_.push_back(held.email);
            lines_.push_back("Serial " + held.serial + ", for version " + std::to_string(held.major) + " and earlier" +
                             (status.source == licence::Source::kMachine ? ", for every user" : ""));
        }
        else
        {
            lines_.push_back("Not licensed");
            lines_.push_back(core::kDemoModeBuild ? "The plugins are in demo mode: two seconds of silence in every ten."
                                                  : "No licence is installed on this computer.");
            if (status.source != licence::Source::kNone)
            {
                lines_.push_back(std::string(licence::VerdictText(status.checked.verdict)));
            }
        }
        lines_.push_back("Paste a licence below, or load the file it came in:");
        remove_.SetVisible(status.source == licence::Source::kUser);
        Resized();
        Repaint();
    }

    /** The platform's dialog for a licence file; the file chosen read and installed. */
    void ChooseLicenceFile()
    {
        view::Host *host = root_.host();
        if (host == nullptr)
        {
            return;
        }
        view::FileRequest request;
        request.title    = "Choose your Reckless Technology licence";
        request.patterns = {"*.rtlicence", "*.txt", "*.json"};
        host->ChooseFile(request, [this](const std::string &path) {
            if (path.empty())
            {
                return;
            }
            const std::optional<std::string> bytes = core::ReadBytes(path);
            if (!bytes)
            {
                SetNote("That file could not be read.", true);
                return;
            }
            // A licence file holds the line; a licence.json copied from another machine holds it as `licence`.
            core::json::ParseResult parsed = core::json::Parse(*bytes);
            InstallText(parsed.IsOk() && parsed.value.IsObject() ? parsed.value.StringOr("licence", "") : *bytes);
        });
    }

    /** The shop, in the person's browser. */
    void Buy()
    {
        if (open_url_)
        {
            open_url_(std::string(kBuyUrl));
        }
    }

    /** The user's licence taken off the machine. */
    void Remove()
    {
        store_.Remove();
        SetNote("The licence was removed from this computer.", false);
        Read();
    }

    /** The card hides and gives up the keyboard, and its owner hears, once, after the event that closed it. */
    void Close()
    {
        if (closing_)
        {
            return;
        }
        closing_ = true;
        ReleaseKeyboardFocus();
        SetVisible(false);
        if (!closed_)
        {
            return;
        }
        const auto  closed = closed_;
        view::Host *host   = root_.host();
        if (host != nullptr)
        {
            host->CallSoon(closed);
        }
        else
        {
            closed();
        }
    }

    /** The word under the field.
        @param note      the word
        @param is_error  true for a refusal, drawn in the outline's colour */
    void SetNote(std::string note, bool is_error)
    {
        note_          = std::move(note);
        note_is_error_ = is_error;
        Repaint();
    }

    /** @return  one line's height, in shell units */
    int LineHeight() const noexcept
    {
        return static_cast<int>(std::ceil(style_.font.height * 1.4f));
    }

    /** @return  the height of the card's lines above the field */
    int LinesHeight() const noexcept
    {
        return LineHeight() * static_cast<int>(lines_.size()) + style_.padding / 2;
    }

    /** @return  the card's rectangle, in the dialog's local coordinates: centred, as tall as its contents need */
    gfx::RectI Card() const
    {
        const int height = style_.padding * 2 + LinesHeight() + 24 + style_.padding + LineHeight() +
                           style_.padding / 2 + style_.button_height;
        return LocalBounds().WithSizeKeepingCentre(std::min(kCardWidth, width() - 16), height);
    }

    view::View                              &root_;                 ///< the window's root view
    licence::Store                          &store_;                ///< the machine's licence
    view::Dialog::Style                      style_;                ///< the look
    std::function<void(const std::string &)> open_url_;             ///< opens the shop; null for none
    std::function<void()>                    closed_;               ///< told once the card has closed
    view::TextField                          field_;                ///< where a licence is pasted
    view::Button                             install_;              ///< installs the pasted text
    view::Button                             file_;                 ///< installs a licence file
    view::Button                             buy_;                  ///< opens the shop
    view::Button                             remove_;               ///< takes the user's licence off the machine
    view::Button                             close_;                ///< closes the card
    std::vector<std::string>                 lines_;                ///< what the card says of the machine's licence
    std::string                              note_;                 ///< the last action's word
    bool                                     note_is_error_{false}; ///< whether the note is a refusal
    bool                                     closing_{false};       ///< the card has closed: later closes are dropped
};
} // namespace reckless::window
