#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Button.h"
#include "Canvas.h"
#include "TextField.h"
#include "View.h"

namespace reckless::view
{
/**
    A question asked over a whole panel: a veil over everything that takes every press, and a card in its middle
    with the question, a line to type into when it asks for text (a preset's name), and its two buttons - the
    affirmative on the right - or the one, for a message with no choice in it (an empty negative). Return or the
    affirmative button answers with the text (or with an empty string for a question with no text); Escape, the other
    button or a press on the veil answers with nothing. The answer runs once, after the event that gave it
    (Host::CallSoon), with the dialog already hidden and the keyboard given up; it stays in the tree until its owner
    destroys it, which the answer is the place for. Its look - the card's colours, the font and the buttons' painter -
    is the panel's.
*/
class Dialog final : public View
{
  public:
    /** The dialog's look: the card's colours, the font, the sizes and the buttons' painter. */
    struct Style
    {
        gfx::Color       veil{gfx::Color::FromArgb(140, 0, 0, 0)};   ///< the translucent fill over the whole panel
        gfx::Color       card{gfx::Color::FromRgb(36, 34, 30)};      ///< the card's fill
        gfx::Color       outline{gfx::Color::FromRgb(200, 160, 60)}; ///< the card's outline
        gfx::Color       text{gfx::Color::FromRgb(240, 230, 200)};   ///< the question's colour
        gfx::Font        font{15.0f};                                ///< the question's font
        int              width{320};                                 ///< the card's width at most, in design pixels
        int              padding{16};                                ///< the card's margin, in design pixels
        int              button_width{90};                           ///< each button's width, in design pixels
        int              button_height{28};                          ///< each button's height, in design pixels
        TextField::Style field{};                                    ///< the line of text's look
        /** Draws the buttons' plates; the default draws nothing. */
        Button::Painter button_painter{[](gfx::Canvas &, gfx::RectI, std::string_view, Button::Look) {}};
    };

    /** What the dialog answers: the text typed, or an empty string for a question with no text, when the affirmative
        answered; nothing otherwise.
        @param answer  the answer; an empty optional for the negative, Escape or a press on the veil */
    using Answer = std::function<void(std::optional<std::string>)>;

    /** A question over `root` (the panel's root view), sized to it; `text` given asks for a line of text with it
        filled in and selected.
        @param root         the panel's root view; the dialog is added to it and covers it, and must not outlive it
        @param question     the question, at most two lines
        @param text         the line of text to ask for, filled in; empty optional for a question with no text
        @param affirmative  the right-hand button's text
        @param negative     the other button's text; empty for a message with no choice, which hides that button
        @param style        the dialog's look
        @param answer       runs once with the answer, after the event that gave it; may be empty for none */
    Dialog(View &root, std::string question, std::optional<std::string> text, std::string affirmative,
           std::string negative, Style style, Answer answer)
        : root_(root), question_(std::move(question)), style_(std::move(style)), answer_(std::move(answer)),
          field_(style_.field), yes_(std::move(affirmative), style_.button_painter, [this] { Reply(true); }),
          no_(std::move(negative), style_.button_painter, [this] { Reply(false); })
    {
        if (text.has_value())
        {
            asks_text_ = true;
            field_.SetText(*text);
            field_.commit_on_focus_lost    = false; // only Return answers from the field
            field_.select_all_when_focused = true;
            field_.on_commit               = [this](const std::string &) { Reply(true); };
            field_.on_cancel               = [this] { Reply(false); };
            AddChild(field_);
        }
        no_.SetVisible(!no_.text().empty());
        AddChild(no_);
        AddChild(yes_);
        SetBounds(root.LocalBounds());
        root.AddChild(*this);
        if (asks_text_)
        {
            field_.GrabKeyboardFocus();
        }
        else
        {
            GrabKeyboardFocus();
        }
    }

    /** The dialog removed from the root it covers, if it is still in it. */
    ~Dialog() override
    {
        if (parent() != nullptr)
        {
            parent()->RemoveChild(*this);
        }
    }

    /** Lays the field and the two buttons out in the card. */
    void Resized() override
    {
        const gfx::RectI card = Card();
        int              y    = card.y() + style_.padding + static_cast<int>(style_.font.height * 2.2f);
        if (asks_text_)
        {
            field_.SetBounds({card.x() + style_.padding, y, card.width() - 2 * style_.padding, 24});
            y += 24 + style_.padding;
        }
        const int right = card.right() - style_.padding;
        yes_.SetBounds({right - style_.button_width, y, style_.button_width, style_.button_height});
        no_.SetBounds(
            {right - 2 * style_.button_width - style_.padding / 2, y, style_.button_width, style_.button_height});
    }

    /** Draws the veil, the card and the question.
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
        c.SetColor(style_.text);
        c.SetFont(style_.font);
        c.DrawFittedText(question_,
                         gfx::RectF{static_cast<float>(card.x() + style_.padding),
                                    static_cast<float>(card.y() + style_.padding),
                                    static_cast<float>(card.width() - 2 * style_.padding), style_.font.height * 2.2f}
                             .ToNearestInt(),
                         gfx::Justify::kCentredLeft, 2);
    }

    /** A press on the veil, outside the card: no answer. Inside the card, nothing.
        @param e  the press: its position in the dialog's local coordinates */
    void MouseDown(const MouseEvent &e) override
    {
        if (!Card().ToFloat().Contains(e.position))
        {
            Reply(false);
        }
    }

    /** @return  true for a question with no text, which keeps the keyboard itself for Return and Escape */
    bool WantsKeyboardFocus() const override
    {
        return !asks_text_; // a question with no text keeps the keyboard itself, for Return and Escape
    }

    /** Return answers with the text, Escape with nothing; no other key passes the dialog.
        @param key  the key pressed
        @return     always true: the dialog is modal */
    bool KeyPressed(const KeyEvent &key) override
    {
        if (key.key == Key::kReturn)
        {
            Reply(true);
            return true;
        }
        if (key.key == Key::kEscape)
        {
            Reply(false);
            return true;
        }
        return true; // modal: no key passes it
    }

    /** @return  the line of text as typed, or an empty string for a question with no text */
    std::string text() const
    {
        return asks_text_ ? field_.text() : std::string();
    }

    /** @return  the question, valid for the dialog's life */
    const std::string &question() const noexcept
    {
        return question_;
    }

  private:
    /** @return  the card's rectangle, in the dialog's local coordinates: centred, as tall as its contents need */
    gfx::RectI Card() const
    {
        const int height = style_.padding * 3 + static_cast<int>(style_.font.height * 2.2f) + style_.button_height +
                           (asks_text_ ? 24 + style_.padding : 0);
        return LocalBounds().WithSizeKeepingCentre(std::min(style_.width, width() - 16), height);
    }

    /** The answer, once: the dialog hides and gives up the keyboard - it leaves the tree when its owner destroys it -
        and the answer, where there is one, runs soon.
        @param affirmative  true for the affirmative button, Return or the field's commit */
    void Reply(bool affirmative)
    {
        if (answered_)
        {
            return;
        }
        answered_                         = true;
        std::optional<std::string> result = affirmative ? std::optional<std::string>(text()) : std::nullopt;
        Host                      *host   = root_.host();
        ReleaseKeyboardFocus();
        SetVisible(false);
        const Answer answer = answer_;
        if (!answer)
        {
            return;
        }
        if (host != nullptr)
        {
            host->CallSoon([answer, result] { answer(result); });
        }
        else
        {
            answer(result);
        }
    }

    View       &root_;             ///< the panel's root view, whose host runs the answer
    std::string question_;         ///< the question
    Style       style_;            ///< the look
    Answer      answer_;           ///< what the answer is handed to
    TextField   field_;            ///< the line to type into, in the tree only when the dialog asks for text
    Button      yes_;              ///< the affirmative button
    Button      no_;               ///< the other button, hidden when its text is empty
    bool        asks_text_{false}; ///< the dialog asks for a line of text
    bool        answered_{false};  ///< the answer has run: later ones are dropped
};
} // namespace reckless::view
