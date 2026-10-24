#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "Canvas.h"
#include "TextField.h"
#include "View.h"

namespace reckless::view
{
/**
    A line of text on a panel - a knob's caption, its value box - drawn as JUCE's label draws one, so a panel lands on
    the pixels the releases built on JUCE drew: the background filled, the text fitted inside the border on as many
    lines as the height holds at the font's size (at least one), squeezed sideways to the minimum scale before it is
    cut, at half strength while the label is disabled, and the outline drawn round its edge.

    An editable label opens a TextField over itself on a click, as the JUCE label does on a single click: the field
    sized up from a small square, filled with the label's text, given the keyboard and all of the text selected,
    then laid over the whole label - the steps the JUCE label takes, so its text lands where the JUCE editor's did.
    Return, or the keyboard taken elsewhere, ends the edit and hands the text to `on_edited` when it changed;
    Escape ends it with nothing. While it edits the label draws its background and its outline alone.
*/
class Label : public View
{
  public:
    /** The label's look: its font, its border and its colours. */
    struct Style
    {
        gfx::Font        font{15.0f};                         ///< the text's font
        gfx::Justify     justify{gfx::Justify::kCentredLeft}; ///< where the text sits in its box
        int              border_top{1};                       ///< the text's room inside the top edge, in design pixels
        int              border_left{5};                      ///< and inside the left edge, in design pixels
        int              border_bottom{1};                    ///< and inside the bottom edge, in design pixels
        int              border_right{5};                     ///< and inside the right edge, in design pixels
        float            min_horizontal_scale{0.0f}; ///< how far a long text is squeezed; 0 for the backend's floor
        gfx::Color       text{gfx::colors::kBlack};  ///< the text's colour
        gfx::Color       background{};               ///< transparent unless given
        gfx::Color       outline{};                  ///< the outline round the label's edge
        TextField::Style editor{};                   ///< the field an edit opens, its font the label's
    };

    /** @param style  the label's look */
    explicit Label(Style style) : style_(std::move(style))
    {
    }

    /** The inline editor taken out of the label, if it is still the label's child. */
    ~Label() override
    {
        if (editor_ != nullptr && editor_->parent() == this)
        {
            RemoveChild(*editor_);
        }
    }

    /** What an edit that changed the text hands on, after the field has closed. */
    std::function<void(const std::string &)> on_edited;

    /** What the label's owner hears as an edit ends, kept or not, once the field has closed and before on_edited: the
        JUCE label's onEditorHide. A label shown only while it is typed into - a seed typed over the button that rolls
        one - hides itself here. */
    std::function<void()> on_editor_hide;

    /** @return  the label's look, valid until the next SetStyle */
    const Style &style() const noexcept
    {
        return style_;
    }

    /** Replace the look and repaint.
        @param style  the new look */
    void SetStyle(Style style)
    {
        style_ = std::move(style);
        Repaint();
    }

    /** Change the text; repaints when it differs. An edit in progress is not touched.
        @param text  the new text */
    void SetText(std::string text)
    {
        if (text != text_)
        {
            text_ = std::move(text);
            Repaint();
        }
    }

    /** @return  the label's text, valid until the next SetText or edit */
    const std::string &text() const noexcept
    {
        return text_;
    }

    /** Whether a click opens the label to type into.
        @param editable  true to let a click open the editor */
    void SetEditable(bool editable) noexcept
    {
        editable_ = editable;
    }

    /** @return  true when a click opens the label to type into */
    bool editable() const noexcept
    {
        return editable_;
    }

    /** @return  true while the editor is open over the label */
    bool editing() const noexcept
    {
        return editor_ != nullptr && editor_->parent() == this;
    }

    /** The field while the label is being edited; null otherwise.
        @return  the field, owned by the label, or nullptr when not editing */
    TextField *editor() noexcept
    {
        return editing() ? editor_.get() : nullptr;
    }

    /** The field opened over the label, as the JUCE label opens its editor. */
    void ShowEditor()
    {
        if (editing())
        {
            return;
        }
        TextField::Style field = style_.editor;
        field.font             = style_.font;
        editor_                = std::make_unique<TextField>(field);
        editor_->on_commit     = [this](const std::string &typed) { EndEdit(typed, true); };
        editor_->on_cancel     = [this] { EndEdit({}, false); };
        editor_->SetBounds({0, 0, 10, 10});
        AddChild(*editor_);
        editor_->SetText(text_);
        editor_->GrabKeyboardFocus();
        editor_->SetHighlightedRegion(0, CodePointsOf(text_).size());
        editor_->SetBounds(LocalBounds());
        Repaint();
    }

    /** The edit ended: the text it holds kept when `keep`, and the field closed either way.
        @param keep  true to keep the typed text and hand it to on_edited when it changed */
    void HideEditor(bool keep)
    {
        if (editing())
        {
            EndEdit(editor_->text(), keep);
        }
    }

    /** Keeps an open editor over the whole label. */
    void Resized() override
    {
        if (editing())
        {
            editor_->SetBounds(LocalBounds());
        }
    }

    /** Draws the background, the fitted text and the outline; while editing, the background and outline alone.
        @param c  the canvas, in the label's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        c.FillAll(style_.background);
        if (!editing())
        {
            const float alpha = IsEnabled() ? 1.0f : 0.5f;
            c.SetColor(style_.text.WithMultipliedAlpha(alpha));
            c.SetFont(style_.font);
            const gfx::RectI area = TextArea();
            const int lines = std::max(1, static_cast<int>(static_cast<float>(area.height()) / style_.font.height));
            c.DrawFittedText(text_, area, style_.justify, lines, style_.min_horizontal_scale);
            c.SetColor(style_.outline.WithMultipliedAlpha(alpha));
        }
        else if (IsEnabled())
        {
            c.SetColor(style_.outline);
        }
        c.DrawRect(LocalBounds().ToFloat(), 1.0f);
    }

    /** A left release that has not wandered more than a few pixels from its press opens the editor, when editable.
        @param e  the release: its position and where its press began */
    void MouseUp(const MouseEvent &e) override
    {
        const float dx = e.position.x - e.down.x;
        const float dy = e.position.y - e.down.y;
        if (editable_ && IsEnabled() && LocalBounds().ToFloat().Contains(e.position) && !e.right &&
            dx * dx + dy * dy <= kDragDistance * kDragDistance)
        {
            ShowEditor();
        }
    }

    /** The text's box: the label inside its border.
        @return  the box, in the label's local coordinates */
    gfx::RectI TextArea() const
    {
        const gfx::RectI all = LocalBounds();
        return gfx::RectI{all.x() + style_.border_left, all.y() + style_.border_top,
                          std::max(0, all.width() - style_.border_left - style_.border_right),
                          std::max(0, all.height() - style_.border_top - style_.border_bottom)};
    }

  private:
    /** How far a press may wander and still be a click. */
    static constexpr float kDragDistance = 4.0f;

    /** Close the field and, when the text changed, tell on_editor_hide and then on_edited.
        @param typed  the text the field held
        @param keep   true to keep `typed`; false for Escape or a cancel */
    void EndEdit(const std::string &typed, bool keep)
    {
        if (!editing())
        {
            return;
        }
        const bool changed = keep && typed != text_;
        if (changed)
        {
            text_ = typed;
        }
        // The field leaves the tree here and goes at the next edit, since this may be its own callback; it is no
        // longer the edit first, so the keyboard handed back raises nothing that ends it again.
        TextField &field = *editor_;
        retired_         = std::move(editor_);
        field.on_commit  = nullptr;
        field.on_cancel  = nullptr;
        if (field.has_focus())
        {
            field.ReleaseKeyboardFocus();
        }
        RemoveChild(field);
        Repaint();
        if (on_editor_hide)
        {
            const auto hidden = on_editor_hide; // a copy: the owner may change the label
            hidden();
        }
        if (changed && on_edited)
        {
            const auto edited = on_edited; // a copy: the owner may change the label
            edited(text_);
        }
    }

    Style                      style_;           ///< the look
    std::string                text_;            ///< the text
    bool                       editable_{false}; ///< a click opens the editor
    std::unique_ptr<TextField> editor_;          ///< the field while an edit is open; null otherwise
    std::unique_ptr<TextField> retired_; ///< the last edit's field, kept until the next: it may still be on the stack
};
} // namespace reckless::view
