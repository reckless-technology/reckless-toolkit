#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Button.h"
#include "Dialog.h"
#include "Knob.h"
#include "KnobLayout.h"
#include "Label.h"
#include "LookDrawing.h"
#include "LookImages.h"
#include "LookMetrics.h"
#include "LookPalette.h"
#include "Menu.h"
#include "PanelStyle.h"
#include "PanelView.h"
#include "PresetModel.h"
#include "Switch.h"

namespace reckless::panel
{
/** The format's images, from the functions its panel context carries, as the look's image backend: what the knob's
    film strip and the company mark are cut, decoded and resampled through. */
class ContextImages final : public look::ImageBackend
{
  public:
    /** The backend on a panel context's image functions; a function the format leaves out gives an empty image.
        @param context  the panel's context; only its functions are copied */
    explicit ContextImages(const view::PanelContext &context)
        : decode_(context.decode_image), clip_(context.sub_image), rescale_(context.rescale_image)
    {
    }

    gfx::Image Decode(const void *data, std::size_t size) const override
    {
        return decode_ ? decode_({static_cast<const std::uint8_t *>(data), size}) : gfx::Image();
    }

    gfx::Image Clipped(const gfx::Image &image, gfx::RectI area) const override
    {
        return clip_ ? clip_(image, area) : gfx::Image();
    }

    gfx::Image Rescaled(const gfx::Image &image, int width, int height, gfx::Resampling quality) const override
    {
        return rescale_ ? rescale_(image, width, height, quality) : image;
    }

  private:
    std::function<gfx::Image(std::span<const std::uint8_t>)>                 decode_;  ///< decodes an encoded image
    std::function<gfx::Image(const gfx::Image &, gfx::RectI)>                clip_;    ///< cuts a part out of an image
    std::function<gfx::Image(const gfx::Image &, int, int, gfx::Resampling)> rescale_; ///< resamples an image
};

/** What a panel is dressed in: its title, its palette, its header's rule, what the header's right end holds before
    PRESETS, and its face plate. */
struct Chrome
{
    std::string   title;   ///< "RECKLESS OCTAVE": the plugin's name, and nothing under it
    look::Palette palette; ///< the panel behind everything when there is no plate, the ink, the plates and the menus
    gfx::Color    rule;    ///< the header's rule on the plate's seam (the plugin's track colour)
    /** What the header's right end holds before PRESETS, from the window's edge: look::kLampStrip for a lamp,
        look::kHeaderInset for nothing (the plate then keeps clear of the corner screw), or what else the plugin puts
        there. */
    int         header_right_reserve{look::kHeaderInset};
    std::string plate; ///< the face plate's asset, exactly 2x the design size ("background.png"); empty for none
};

/** The knob bank's sizes on every panel of the series: the look's own numbers (LookMetrics.h), so a change to them
    reaches the layout as it reaches the drawing. */
inline constexpr view::KnobMetrics kKnobMetrics{look::kLabelHeight, look::kKnobBlock,  look::kKnobWidth,
                                                look::kPlateHeight, look::kFrameInset, look::kFrameOverhang};

/**
    The PRESETS plate at the header's right end - a view the plate's size holding the series' button over the whole of
    it - reading the preset the plugin is on ("Squash", with an asterisk once anything has moved since; PRESETS on none)
    and opening the presets menu under it on a click: the model's own fresh starts first where it has any (the
    Pedalboard's New board, asked about first where the model says to); the presets by name, the current one ticked;
    Save, Save as..., Delete...; Load from file..., Show presets folder; Reset to defaults, where the model offers it;
    and the window's own items last. Save as asks
    for a name and Delete asks first, over the panel; what goes wrong is said over the panel too. With no presets from
    the format, the menu is Reset to defaults alone, every control on the panel put back through its parameter.
*/
class PresetPlateView final : public view::View
{
  public:
    /** The menu's own items, after the presets (which take 1 to their count, kMaxPresets at most). */
    enum Item
    {
        kSave = 1001,    ///< Save under the current name
        kSaveAs,         ///< Save as, asking for a name
        kDelete,         ///< Delete the current preset's file, asking first
        kLoadFromFile,   ///< Load from file, through the chooser
        kShowFolder,     ///< Show presets folder
        kResetToDefaults ///< Reset to defaults
    };

    /** The most presets the menu lists, so a row's id never meets one of the plate's own: the folder's first thousand,
        and the rest through Load from file. */
    static constexpr std::size_t kMaxPresets = kSave - 1;

    /** The plate's text when the plugin is on no preset. */
    static constexpr const char *kNoPreset = "PRESETS";

    /** The plate over the whole of its bounds.
        @param model           the presets; owned by the format and outlives the plate; nullptr for none, which leaves
                               the menu Reset to defaults alone
        @param palette         the plugin's palette
        @param panel           the panel the questions and messages are laid over; outlives the plate
        @param reset_controls  puts every control on the panel back to its default; used only when there is no model */
    PresetPlateView(view::PresetModel *model, const look::Palette &palette, view::View &panel,
                    std::function<void()> reset_controls)
        : model_(model), palette_(palette), panel_(panel), reset_controls_(std::move(reset_controls)),
          button_(kNoPreset, MenuButtonPainter(palette), [this] { ShowMenu(); })
    {
        button_.tooltip = "Presets: the plugin's by name, save, save as, delete, load from a file, the folder, and "
                          "Reset to defaults.";
        AddChild(button_);
        Refresh();
    }

    /** The button that fills the plate, for the suites.
        @return  the button, owned by the plate */
    view::Button &button() noexcept
    {
        return button_;
    }

    /** The presets the plate lists, for the suites: what its leading items and Reset to defaults come from.
        @return  the model, owned by the format; nullptr for none */
    const view::PresetModel *model() const noexcept
    {
        return model_;
    }

    /** Whether the presets menu is open.
        @return  true while the menu is up */
    bool menu_open() const noexcept
    {
        return menu_ != nullptr;
    }

    /** The plate's text: the preset's name, an asterisk once it is modified, PRESETS on none.
        @return  the text the plate shows */
    std::string PlateText() const
    {
        if (model_ == nullptr || model_->Current().empty())
        {
            return kNoPreset;
        }
        return model_->Modified() ? model_->Current() + " *" : model_->Current();
    }

    /** Once a display frame: the text re-read. */
    void Refresh()
    {
        button_.SetText(PlateText());
    }

    void Resized() override
    {
        button_.SetBounds(LocalBounds());
    }

    /** The menu as the plate opens it: what the suites read its order from.
        @return  the menu's items in order; a call with a model lists the presets as Choose will load them */
    std::vector<view::MenuItem> MenuItems() const
    {
        std::vector<view::MenuItem> items;
        if (model_ == nullptr)
        {
            items.push_back(Item(kResetToDefaults, "Reset to defaults"));
            return items;
        }
        for (auto &item : model_->LeadingItems())
        {
            items.push_back(std::move(item));
        }
        if (!items.empty())
        {
            items.push_back(Separator());
        }
        const auto names   = model_->Names();
        const auto current = model_->Current();
        listed_            = true;
        for (std::size_t i = 0; i < std::min(names.size(), kMaxPresets); ++i)
        {
            view::MenuItem item = Item(static_cast<int>(i) + 1, names[i]);
            item.ticked         = names[i] == current;
            items.push_back(std::move(item));
        }
        if (!names.empty())
        {
            items.push_back(Separator());
        }
        items.push_back(Item(kSave, "Save", !current.empty()));
        items.push_back(Item(kSaveAs, "Save as..."));
        items.push_back(
            Item(kDelete, current.empty() ? "Delete..." : "Delete \"" + current + "\"...", !current.empty()));
        items.push_back(Separator());
        items.push_back(Item(kLoadFromFile, "Load from file..."));
        items.push_back(Item(kShowFolder, "Show presets folder"));
        if (model_->OffersReset())
        {
            items.push_back(Separator());
            items.push_back(Item(kResetToDefaults, "Reset to defaults"));
        }
        return items;
    }

    /** What choosing item `id` does: the same the menu's choice runs, for the suites. A preset's row is the row of the
        menu the plate listed last, loaded from that listing (PresetModel::Load) whatever the folder has gained or lost
        since - the model says if its file has gone - and the folder as it stands where the plate has listed none.
        @param id  the item's id: a preset's row from 1, one of Item, or a leading item's from
                   view::PresetModel::kLeadingBase */
    void Choose(int id)
    {
        if (model_ == nullptr)
        {
            if (id == kResetToDefaults && reset_controls_)
            {
                reset_controls_();
            }
            return;
        }
        if (id >= 1 && id < kSave)
        {
            if (!listed_)
            {
                (void)model_->Names();
                listed_ = true;
            }
            Report(model_->Load(static_cast<std::size_t>(id - 1)));
        }
        else if (id >= view::PresetModel::kLeadingBase)
        {
            if (const std::string question = model_->LeadingQuestion(id); !question.empty())
            {
                Ask(question, std::nullopt, "OK", "Cancel", [this, id](const std::string &) {
                    model_->PerformLeading(id);
                    Refresh();
                });
            }
            else
            {
                model_->PerformLeading(id);
            }
        }
        else if (id == kSave && !model_->Current().empty())
        {
            Report(model_->Save(model_->Current()));
        }
        else if (id == kSaveAs)
        {
            Ask("A name for the preset: it goes in the presets folder, in place of one of the same name.",
                model_->Current(), "Save", "Cancel", [this](const std::string &name) { Report(model_->Save(name)); });
        }
        else if (id == kDelete && !model_->Current().empty())
        {
            Ask("Delete \"" + model_->Current() + "\"? The knobs stay where they are; the file goes.", std::nullopt,
                "Delete", "Cancel", [this](const std::string &) { Report(model_->Delete()); });
        }
        else if (id == kLoadFromFile)
        {
            view::FileRequest request;
            request.title    = "Load a preset";
            request.folder   = model_->Folder();
            request.patterns = {model_->FilePattern()};
            View::ChooseFile(request, [this](const std::string &path) {
                if (!path.empty())
                {
                    Report(model_->LoadFile(path));
                }
            });
        }
        else if (id == kShowFolder)
        {
            model_->ShowFolder();
        }
        else if (id == kResetToDefaults && model_->OffersReset())
        {
            model_->ResetToDefaults();
        }
        Refresh();
    }

    /** The question or the message over the panel, while one is up: for the suites.
        @return  the dialog, owned by the plate; nullptr when none is up */
    view::Dialog *dialog() noexcept
    {
        return dialog_.get();
    }

  private:
    /** A menu row.
        @param id       the row's id
        @param text     the row's text
        @param enabled  whether the row can be chosen
        @return         the row */
    static view::MenuItem Item(int id, std::string text, bool enabled = true)
    {
        view::MenuItem item;
        item.id      = id;
        item.text    = std::move(text);
        item.enabled = enabled;
        return item;
    }

    /** A menu separator.
        @return  the separator row */
    static view::MenuItem Separator()
    {
        view::MenuItem item;
        item.separator = true;
        return item;
    }

    /** Opens the presets menu under the plate, as wide as the plate at least; nothing when it is open. */
    void ShowMenu()
    {
        if (menu_ != nullptr)
        {
            return;
        }
        view::MenuStyle style = MenuStyle(palette_);
        style.min_width       = std::max(style.min_width, width());
        menu_ = view::MenuSession::Open(button_, MenuItems(), style, {0, button_.height()}, [this](int id) {
            menu_.reset();
            if (id > 0)
            {
                Choose(id);
            }
        });
    }

    /** A question over the panel - or, with no `no`, a message - `on_yes` run with the text typed (or nothing asked)
        when it is answered yes.
        @param question  the question or the message
        @param text      the text the line starts with; nullopt for no line to type into
        @param yes       the legend of the affirmative button
        @param no        the legend of the other button; empty for a message with the one button
        @param on_yes    run with the text typed, or an empty string when none was asked */
    void Ask(std::string question, std::optional<std::string> text, std::string yes, std::string no,
             std::function<void(const std::string &)> on_yes)
    {
        dialog_ =
            std::make_unique<view::Dialog>(panel_, std::move(question), std::move(text), std::move(yes), std::move(no),
                                           DialogStyle(palette_), [this, on_yes](std::optional<std::string> answer) {
                                               dialog_.reset();
                                               if (answer.has_value() && on_yes)
                                               {
                                                   on_yes(*answer);
                                               }
                                               Refresh();
                                           });
    }

    /** What went wrong, said over the panel; nothing for nothing.
        @param problem  the model's refusal in plain words; empty for success */
    void Report(const std::string &problem)
    {
        Refresh();
        if (!problem.empty())
        {
            Ask(problem, std::nullopt, "OK", {}, nullptr);
        }
    }

    view::PresetModel                 *model_;          ///< the presets, or nullptr for none
    look::Palette                      palette_;        ///< the plugin's palette
    view::View                        &panel_;          ///< the panel the dialog is laid over
    std::function<void()>              reset_controls_; ///< Reset to defaults where there is no model
    view::Button                       button_;         ///< the button that fills the plate
    std::unique_ptr<view::MenuSession> menu_;           ///< the open menu; null when closed
    std::unique_ptr<view::Dialog>      dialog_;         ///< the question or message over the panel; null when none
    mutable bool                       listed_{false};  ///< whether the plate has listed the presets for a menu yet
};

/**
    The settings' gear in every panel's header, left of the PRESETS plate: the window's own settings - the shell's View
    submenu (scale to fit or scroll bars, and the sizes) and, in the Standalone, the Audio/MIDI Settings - in a menu
    under it, which the presets' model carries as the window's items (view::PresetModel::ExtensionItems, set by the
    format), since they belong to the window and not to any preset. Shown only where there are some: a pedal's panel in
    the board's drawer has none, the board's own gear holding them. The gear is drawn bare, the plate's height across,
    in the ink, and in the accent under the mouse and while held (look::DrawGear).
*/
class SettingsPlateView final : public view::View
{
  public:
    /** The tooltip every gear carries. */
    static constexpr const char *kTooltip = "Settings: the window's size and zoom, and in the app the audio and MIDI.";

    /** The gear over the whole of its bounds.
        @param model    the presets, whose window items the menu lists; owned by the format and outlives the gear;
                        nullptr for none, which hides it
        @param palette  the plugin's palette */
    SettingsPlateView(view::PresetModel *model, const look::Palette &palette)
        : model_(model), palette_(palette), button_("", GearPainter(palette), [this] { ShowMenu(); })
    {
        button_.tooltip = kTooltip;
        AddChild(button_);
        Refresh();
    }

    /** The button that fills the gear's bounds, for the suites.
        @return  the button, owned by the gear */
    view::Button &button() noexcept
    {
        return button_;
    }

    /** Whether the settings' menu is open.
        @return  true while the menu is up */
    bool menu_open() const noexcept
    {
        return menu_ != nullptr;
    }

    /** The menu as the gear opens it: the window's items, as the format gives them.
        @return  the items in order; none without a model */
    std::vector<view::MenuItem> MenuItems() const
    {
        return model_ != nullptr ? model_->ExtensionItems() : std::vector<view::MenuItem>();
    }

    /** What choosing item `id` does: the window's item run, as the menu's choice runs it, for the suites.
        @param id  the item's id, from view::PresetModel::kExtensionBase */
    void Choose(int id)
    {
        if (model_ != nullptr && id >= view::PresetModel::kExtensionBase)
        {
            model_->PerformExtension(id);
        }
    }

    /** Shown while the window has settings to offer: from the face's Refresh, once a display frame. */
    void Refresh()
    {
        const bool any = model_ != nullptr && !model_->ExtensionItems().empty();
        if (any != visible())
        {
            SetVisible(any);
        }
    }

    void Resized() override
    {
        button_.SetBounds(LocalBounds());
    }

  private:
    /** The gear's button: the gear alone, as large as the button is high, in the ink - in the accent under the mouse
        and while held, a shade under it while held.
        @param palette  the plugin's palette
        @return         the painter, which keeps a copy of the palette */
    static view::Button::Painter GearPainter(const look::Palette &palette)
    {
        return [palette](gfx::Canvas &c, gfx::RectI bounds, std::string_view, view::Button::Look state) {
            const float side   = static_cast<float>(std::min(bounds.width(), bounds.height()));
            const auto  colour = state.held ? palette.accent.Darker(0.25f) : state.over ? palette.accent : palette.ink;
            look::DrawGear(c, gfx::RectF(side, side).WithCentre(bounds.ToFloat().Centre()), colour);
        };
    }

    /** Opens the settings' menu under the gear; nothing when it is open. */
    void ShowMenu()
    {
        if (menu_ != nullptr)
        {
            return;
        }
        menu_ =
            view::MenuSession::Open(button_, MenuItems(), MenuStyle(palette_), {0, button_.height()}, [this](int id) {
                menu_.reset();
                if (id > 0)
                {
                    Choose(id);
                }
            });
    }

    view::PresetModel                 *model_;   ///< the presets, whose window items the menu lists; nullptr for none
    look::Palette                      palette_; ///< the plugin's palette
    view::Button                       button_;  ///< the button that fills the gear's bounds
    std::unique_ptr<view::MenuSession> menu_;    ///< the open menu; null when closed
};

/**
    What every panel in the series is dressed in and holds before it is anything of its own, on the suite's views and
    drawn by the series' look: the face plate drawn over the panel at 2x (a 2:1 reduction at 1x, the better filter when
    the window zooms it), or the panel colour without one; the header in one call with the company mark, the title and
    the version stamp and the rule on the seam; the PRESETS plate at the header's right end; the knobs and plates added
    by parameter id with their captions and tooltips, and a knob the rows leave for the panel to place itself; and the
    cream frames round the knobs and round whatever else the panel frames.

    A panel holds its face as its first member - so the PRESETS plate the face adds to it is under every view added
    after it - and asks it, in its own Resized(), Frame() and Paint(), to lay out (LayOut(), which clears the frames
    and hands back the body under the header inside the series' margins for the panel to divide by the knob bank's
    rows, grids and strips, view/KnobLayout.h), to bring its controls up to the host (Refresh()) and to paint (Paint(),
    the panel's first drawing). A panel is its face, its tooltips, where its columns fall and what its frame moves - a
    lamp (LampView.h), a knob a tempo division drives - read off whatever the format hands it beside its context (a
    plugin's core).
*/
class SeriesFace
{
  public:
    /** A plate in the switch strip is this much narrower than its knob column: the knob bank's strip. */
    static constexpr int kStripPlateTrim = 16;

    /** The word the header shows beside the version while the plugin plays in demo mode. */
    static constexpr std::string_view kDemoBadge = "DEMO";
    /** The word the header shows beside the version while a newer release is out, in the ink as DEMO is - the one
        colour that reads on every plate, the light ones too - DEMO before it. */
    static constexpr std::string_view kUpdateBadge = "UPDATE";

    /** The face of `panel`, which holds it, dressed in `chrome`, its controls on `context`'s parameters: the PRESETS
        plate added to the panel here.
        @param panel    the panel that holds the face as its first member; outlives it
        @param context  what the format hands the panel: its controls, images, files and presets; copied
        @param chrome   the title, palette, rule and plate the panel is dressed in */
    SeriesFace(view::Panel &panel, const view::PanelContext &context, Chrome chrome)
        : panel_(panel), context_(context), chrome_(std::move(chrome)), images_(context),
          version_(context.version.empty() ? std::string() : "v" + context.version),
          knob_strip_(look::KnobStrip::Shared(images_, Asset("RecklessChickenHead.png"))),
          mark_(images_, Decode("RecklessRtLogo.png")),
          plate_(chrome_.plate.empty() ? gfx::Image() : Decode(chrome_.plate)),
          presets_(context.presets, chrome_.palette, panel, [this] { ResetControls(); }),
          settings_(context.presets, chrome_.palette)
    {
        panel_.AddChild(presets_);
        panel_.AddChild(settings_);
        if (plate_.IsValid())
        {
            decoded_plates_.emplace(chrome_.plate, plate_);
        }
    }

    SeriesFace(const SeriesFace &)            = delete;
    SeriesFace &operator=(const SeriesFace &) = delete;

    /** What the panel is dressed in, with the plate SetPlate last chose.
        @return  the chrome */
    const Chrome &chrome() const noexcept
    {
        return chrome_;
    }

    /** What the face was made with: the format's controls, images and files.
        @return  the panel context the face copied */
    const view::PanelContext &context() const noexcept
    {
        return context_;
    }

    /** The series' chicken head, for a knob a panel draws on a control of its own (a drawer's, the board's).
        @return  the film strip, owned by the face */
    look::KnobStrip &knob_strip() noexcept
    {
        return knob_strip_;
    }

    /** The PRESETS plate the face added to the panel.
        @return  the plate, owned by the face */
    PresetPlateView &presets() noexcept
    {
        return presets_;
    }

    /** The settings' gear the face added to the panel, left of the PRESETS plate.
        @return  the gear, owned by the face */
    SettingsPlateView &settings() noexcept
    {
        return settings_;
    }

    // ---- The knob bank ------------------------------------------------------------------------------------------

    /** A knob of the bank, captioned and tooltipped and turning parameter `id`, added to the panel after every control
        added before it; a `suffix` on its value box, and a box wider than the series' where its readout needs it. The
        rows and grids lay it out in the order added.
        @param id         the parameter's string id
        @param caption    the caption over the knob
        @param tip        the tooltip
        @param suffix     the text after the value in its box, such as " dB"
        @param box_width  the value box's width, in design pixels
        @return           the knob, owned by the face */
    view::Knob &AddKnob(std::string_view id, std::string caption, std::string tip, std::string suffix = {},
                        int box_width = look::kValueBoxWidth)
    {
        return MakeKnob(id, std::move(caption), std::move(tip), std::move(suffix), box_width, true);
    }

    /** A knob the rows and grids leave out, for the panel to place itself, after every control added before it - the
        Mod's multi chorus MIX, laid over FEEDBACK's cell and shown in its place - with no frame of its own.
        @param id         the parameter's string id
        @param caption    the caption over the knob
        @param tip        the tooltip
        @param suffix     the text after the value in its box, such as " dB"
        @param box_width  the value box's width, in design pixels
        @return           the knob, owned by the face */
    view::Knob &AddKnobOutsideRow(std::string_view id, std::string caption, std::string tip, std::string suffix = {},
                                  int box_width = look::kValueBoxWidth)
    {
        return MakeKnob(id, std::move(caption), std::move(tip), std::move(suffix), box_width, false);
    }

    /** A legend plate of the bank, its choices the parameter's, captioned and tooltipped, added to the panel after
        every control added before it.
        @param id       the parameter's string id
        @param caption  the caption over the plate
        @param tip      the tooltip
        @return         the plate, owned by the face */
    view::Switch &AddPlate(std::string_view id, std::string caption, std::string tip)
    {
        auto plate     = std::make_unique<view::Switch>(context_.parameter(id), std::move(caption),
                                                        PlatePainter(chrome_.palette), SwitchStyle(chrome_.palette));
        plate->tooltip = std::move(tip);
        panel_.AddChild(*plate);
        panel_.AddChild(plate->caption());
        plates_.push_back(PlateEntry{std::move(plate), std::string(id)});
        return *plates_.back().plate;
    }

    /** Draws `knob`'s chicken head `side` design pixels across its frame - look::kSmallKnobSide for the series' small
        knob - in place of the size its area would give it.
        @param knob  a knob of the bank; ignored when the face does not own it
        @param side  the chicken head's side, in design pixels */
    void SetKnobSide(const view::Knob &knob, float side)
    {
        for (auto &entry : knobs_)
        {
            if (entry.knob.get() == &knob)
            {
                entry.side = side;
                entry.knob->Repaint();
            }
        }
    }

    /** The knob that turns parameter `id`, or null.
        @param id  the parameter's string id
        @return    the knob, owned by the face; nullptr when none turns it */
    view::Knob *KnobFor(std::string_view id) const noexcept
    {
        for (const auto &entry : knobs_)
        {
            if (entry.id == id)
            {
                return entry.knob.get();
            }
        }
        return nullptr;
    }

    /** The plate that switches parameter `id`, or null.
        @param id  the parameter's string id
        @return    the plate, owned by the face; nullptr when none switches it */
    view::Switch *PlateFor(std::string_view id) const noexcept
    {
        for (const auto &entry : plates_)
        {
            if (entry.id == id)
            {
                return entry.plate.get();
            }
        }
        return nullptr;
    }

    /** Every knob, in the order added: the row's and any the panel places itself (AddKnobOutsideRow).
        @return  the knobs, owned by the face */
    std::vector<view::Knob *> knobs() const
    {
        std::vector<view::Knob *> all;
        for (const auto &entry : knobs_)
        {
            all.push_back(entry.knob.get());
        }
        return all;
    }

    /** The knobs the rows and grids lay out, in the order added.
        @return  the knobs, owned by the face */
    std::vector<view::Knob *> RowKnobs() const
    {
        std::vector<view::Knob *> row;
        for (const auto &entry : knobs_)
        {
            if (entry.in_row)
            {
                row.push_back(entry.knob.get());
            }
        }
        return row;
    }

    /** Every plate, in the order added.
        @return  the plates, owned by the face */
    std::vector<view::Switch *> plates() const
    {
        std::vector<view::Switch *> all;
        for (const auto &entry : plates_)
        {
            all.push_back(entry.plate.get());
        }
        return all;
    }

    /** `f(id, knob)` for every knob, in the order added.
        @tparam F  a callable taking a `std::string_view` id and a `view::Knob &`
        @param f   the callable */
    template <typename F> void ForEachKnob(F &&f) const
    {
        for (const auto &entry : knobs_)
        {
            f(std::string_view(entry.id), *entry.knob);
        }
    }

    /** Every control on the panel back to its parameter's default, each as one gesture: Reset to defaults where the
        format brings no presets. */
    void ResetControls()
    {
        for (auto &entry : knobs_)
        {
            Restore(entry.knob->parameter());
            entry.knob->Refresh();
        }
        for (auto &entry : plates_)
        {
            Restore(entry.plate->parameter());
            entry.plate->Refresh();
        }
    }

    // ---- The layout ---------------------------------------------------------------------------------------------

    /** The header's rectangle: the top look::kHeaderHeight of the panel.
        @return  the header's bounds, in the panel's design pixels */
    gfx::RectI HeaderBounds() const noexcept
    {
        return gfx::RectI{panel_.width(), look::kHeaderHeight};
    }

    /** Resized()'s opening: the frames cleared, since every layout lays out its own (LayOutKnob and AddFrameAround add
        them), and the PRESETS plate at the header's right end, the right reserve in from the edge, with the settings'
        gear left of it; returns the body below the header inside the series' margins.
        @return  the body, in the panel's design pixels */
    gfx::RectI LayOut()
    {
        frames_.clear();
        auto header = HeaderBounds();
        header.RemoveFromRight(chrome_.header_right_reserve);
        const auto cell = header.RemoveFromRight(look::kPresetReserve).WithTrimmedRight(look::kResetGap);
        presets_.SetBounds(cell.WithSizeKeepingCentre(look::kPresetPlateWidth, look::kPlateHeight));
        LayOutSettings(presets_.bounds());
        return panel_.LocalBounds().WithTrimmedTop(look::kHeaderHeight).Reduced(look::kMarginX, look::kMarginY);
    }

    /** The settings' gear left of a PRESETS plate at `presets`, look::kResetGap from it, as wide as it is high.
        @param presets  the PRESETS plate's bounds, in the panel's design pixels */
    void LayOutSettings(gfx::RectI presets)
    {
        settings_.SetBounds(
            gfx::RectI(look::kSettingsPlateSide, look::kSettingsPlateSide)
                .WithCentre({presets.x() - look::kResetGap - look::kSettingsPlateSide / 2, presets.CentreY()}));
    }

    /** A knob in `cell`, its frame added to the frames (KnobLayout's PlaceKnob).
        @param knob  the knob to place
        @param cell  the knob's cell, in the panel's design pixels */
    void LayOutKnob(view::Knob &knob, gfx::RectI cell)
    {
        const auto place = view::PlaceKnob(cell, kKnobMetrics);
        knob.caption().SetBounds(place.caption);
        knob.SetBounds(place.dial);
        frames_.push_back(place.frame);
    }

    /** The series' cream frame round `group` - a bay of indicators, a cell of the panel's own - added to the knobs'
        frames for this layout.
        @param group  the rectangle to frame, in the panel's design pixels */
    void AddFrameAround(gfx::RectI group)
    {
        frames_.push_back(view::FrameAround(group, kKnobMetrics));
    }

    /** The knobs in a row across `area`, one equal column each of `columns`, in the order added.
        @param area     the row's area, in the panel's design pixels
        @param columns  how many equal columns the area divides into */
    void LayOutKnobRow(gfx::RectI area, int columns)
    {
        const auto row   = RowKnobs();
        const auto cells = view::RowCells(area, columns, row.size());
        for (std::size_t i = 0; i < row.size(); ++i)
        {
            LayOutKnob(*row[i], cells[i]);
        }
    }

    /** The knobs in a grid `columns` wide over `area`, row by row.
        @param area     the grid's area, in the panel's design pixels
        @param columns  the grid's width, in cells */
    void LayOutKnobGrid(gfx::RectI area, int columns)
    {
        const auto row   = RowKnobs();
        const auto cells = view::GridCells(area, columns, row.size());
        for (std::size_t i = 0; i < row.size(); ++i)
        {
            LayOutKnob(*row[i], cells[i]);
        }
    }

    /** A plate and its caption in `cell`, the plate `plate_width` wide.
        @param plate        the plate to place
        @param cell         the plate's cell, in the panel's design pixels
        @param plate_width  the plate's width, in design pixels */
    static void LayOutPlate(view::Switch &plate, gfx::RectI cell, int plate_width)
    {
        const auto place = view::PlacePlate(cell, plate_width, kKnobMetrics);
        plate.caption().SetBounds(place.caption);
        plate.SetBounds(place.plate);
    }

    /** Where a plate and its caption go in `strip` under the knobs' `area`: centred on knob column `column` (in column
        units, 2.5 between the third and fourth) of `cell_width`, `trim` narrower than the column (KnobLayout's
        PlacePlateInStrip).
        @param area        the knob area the columns start at, in design pixels
        @param strip       the switch strip, in design pixels
        @param column      the knob column the plate is centred on, counted from 0
        @param cell_width  one column's width, in design pixels
        @param trim        how much narrower than the column the plate is, in design pixels
        @return            where the caption and the plate fall */
    static view::PlatePlace PlateInStrip(gfx::RectI area, gfx::RectI strip, double column, int cell_width,
                                         int trim = kStripPlateTrim) noexcept
    {
        return view::PlacePlateInStrip(area, strip, column, cell_width, trim, kKnobMetrics);
    }

    /** `plate` and its caption in the strip, where PlateInStrip puts them.
        @param plate       the plate to place
        @param area        the knob area the columns start at, in design pixels
        @param strip       the switch strip, in design pixels
        @param column      the knob column the plate is centred on, counted from 0
        @param cell_width  one column's width, in design pixels
        @param trim        how much narrower than the column the plate is, in design pixels */
    static void LayOutPlateInStrip(view::Switch &plate, gfx::RectI area, gfx::RectI strip, double column,
                                   int cell_width, int trim = kStripPlateTrim)
    {
        const auto place = PlateInStrip(area, strip, column, cell_width, trim);
        plate.caption().SetBounds(place.caption);
        plate.SetBounds(place.plate);
    }

    /** The series' pedal layout: the switch strip off the bottom of `area`, the knobs in `columns` equal columns over
        the rest, framed, and each plate centred on its column in the strip (2.5 between the third and fourth),
        kStripPlateTrim narrower than its column.
        @param area     the pedal's area, in the panel's design pixels; the strip is taken off its bottom
        @param columns  how many equal columns the knobs fall in
        @param plates   each plate with the knob column it is centred on, counted from 0 */
    void LayOutKnobRowOverStrip(gfx::RectI area, int columns,
                                std::initializer_list<std::pair<view::Switch *, double>> plates)
    {
        const auto strip      = area.RemoveFromBottom(look::kSwitchStrip);
        const int  cell_width = area.width() / std::max(columns, 1);
        LayOutKnobRow(area, columns);
        for (const auto &[plate, column] : plates)
        {
            LayOutPlateInStrip(*plate, area, strip, column, cell_width);
        }
    }

    /** The frames this layout drew round the knobs and whatever else the panel framed.
        @return  the frames, in the panel's design pixels */
    const std::vector<gfx::RectF> &frames() const noexcept
    {
        return frames_;
    }

    // ---- The plate, the paint and the frame ---------------------------------------------------------------------

    /** The face plate drawn behind the panel from now on: `asset`, exactly 2x the design size the panel shows it at -
        a panel that changes its own size swaps its plate with it, before it takes the size (view::Panel's
        SetDesignSize), so the repaint the size brings shows the plate for it - or empty for the panel colour. Each
        plate is decoded the first time it is asked for, and kept.
        @param asset  the plate's asset name; empty for the panel colour */
    void SetPlate(std::string asset)
    {
        if (asset == chrome_.plate)
        {
            return;
        }
        if (asset.empty())
        {
            plate_ = gfx::Image();
        }
        else
        {
            auto found = decoded_plates_.find(asset);
            if (found == decoded_plates_.end())
            {
                found = decoded_plates_.emplace(asset, Decode(asset)).first;
            }
            plate_ = found->second;
        }
        chrome_.plate = std::move(asset);
        panel_.Repaint();
    }

    /** What the panel paints first: the plate (or the panel colour), the header stopping short of the right reserve
        and the PRESETS plate, the version stamp, and the frames.
        @param c  the canvas, in the panel's design pixels */
    void Paint(gfx::Canvas &c) const
    {
        if (plate_.IsValid())
        {
            // The plate is 2x the design size, so at 1x its draw is a 2:1 reduction, the pixels every review image
            // holds the chrome to; zoomed, the ratio is another and the better filter is worth it.
            const view::Host *h = panel_.host();
            if (h != nullptr && std::fabs(h->Zoom() - 1.0f) > 1.0e-3f)
            {
                c.SetResampling(gfx::Resampling::kHigh);
            }
            c.DrawImage(plate_, panel_.LocalBounds().ToFloat());
        }
        else
        {
            c.FillAll(chrome_.palette.panel);
        }
        look::DrawHeader(c, mark_, HeaderBounds(), chrome_.title, version_, chrome_.palette.ink, chrome_.rule,
                         chrome_.header_right_reserve + look::kPresetReserve + look::kSettingsReserve,
                         demo_     ? kDemoBadge
                         : update_ ? kUpdateBadge
                                   : std::string_view(),
                         chrome_.palette.ink);
        look::DrawFrames(c, frames_, chrome_.palette.ink);
    }

    /** Once a display frame: every knob, plate and the PRESETS plate brought up to what the host holds. */
    void Refresh()
    {
        for (auto &entry : knobs_)
        {
            entry.knob->Refresh();
        }
        for (auto &entry : plates_)
        {
            entry.plate->Refresh();
        }
        presets_.Refresh();
        settings_.Refresh();
        const bool demo   = context_.demo && context_.demo();
        const bool update = context_.update_available && context_.update_available();
        if (demo != demo_ || update != update_)
        {
            demo_   = demo;
            update_ = update;
            panel_.Repaint(HeaderBounds());
        }
    }

    /** Whether the header says UPDATE: the context's word at the last Refresh.
        @return  true while a newer release is out */
    bool update_available() const noexcept
    {
        return update_;
    }

    /** Whether the header says DEMO: the context's word at the last Refresh.
        @return  true while the plugin plays in demo mode */
    bool demo() const noexcept
    {
        return demo_;
    }

  private:
    /** A knob of the bank: its view, the parameter it turns, SetKnobSide's side (none for its area's own), and whether
        the rows and grids lay it out. */
    struct KnobEntry
    {
        std::unique_ptr<view::Knob> knob;         ///< the knob's view
        std::string                 id;           ///< the parameter's string id
        std::optional<float>        side;         ///< SetKnobSide's side; empty for the cell's own
        bool                        in_row{true}; ///< whether the rows and grids lay it out
    };

    /** A plate of the bank and the parameter it switches. */
    struct PlateEntry
    {
        std::unique_ptr<view::Switch> plate; ///< the plate's view
        std::string                   id;    ///< the parameter's string id
    };

    /** The one road AddKnob and AddKnobOutsideRow take: the knob made, added to the panel and entered in the bank.
        @param id         the parameter's string id
        @param caption    the caption over the knob
        @param tip        the tooltip
        @param suffix     the text after the value in its box
        @param box_width  the value box's width, in design pixels
        @param in_row     whether the rows and grids lay it out
        @return           the knob, owned by the face */
    view::Knob &MakeKnob(std::string_view id, std::string caption, std::string tip, std::string suffix, int box_width,
                         bool in_row)
    {
        const std::size_t index = knobs_.size();
        auto              knob  = std::make_unique<view::Knob>(
            context_.parameter(id), std::move(caption),
            [this, index](gfx::Canvas &c, gfx::RectI area, float angle) {
                const float side = knobs_[index].side.value_or(look::KnobStrip::SideFor(area.width(), area.height()));
                knob_strip_.Draw(c, area, angle, side);
            },
            KnobStyle(chrome_.palette, box_width), std::move(suffix));
        knob->tooltip = std::move(tip);
        panel_.AddChild(*knob);
        panel_.AddChild(knob->caption());
        knobs_.push_back(KnobEntry{std::move(knob), std::string(id), std::nullopt, in_row});
        return *knobs_.back().knob;
    }

    /** The module's file `name`, or nothing where the format carries none.
        @param name  the file's name, such as "RecklessRtLogo.png"
        @return      the file's bytes, owned by the module; empty when there is none */
    std::span<const std::uint8_t> Asset(std::string_view name) const
    {
        return context_.asset ? context_.asset(name) : std::span<const std::uint8_t>();
    }

    /** An image file of the module decoded.
        @param name  the file's name
        @return      the image; invalid when the module has no such file or it cannot be decoded */
    gfx::Image Decode(std::string_view name) const
    {
        const auto bytes = Asset(name);
        return bytes.empty() ? gfx::Image() : images_.Decode(bytes.data(), bytes.size());
    }

    /** A control put back to its default as one gesture.
        @param control  the control to reset */
    static void Restore(view::ParameterControl &control)
    {
        control.BeginGesture();
        control.SetNormalized(control.DefaultNormalized());
        control.EndGesture();
    }

    view::Panel            &panel_;          ///< the panel that holds the face
    view::PanelContext      context_;        ///< what the format handed the panel
    bool                    demo_   = false; ///< whether the header says DEMO, as of the last Refresh
    bool                    update_ = false; ///< whether the header says UPDATE, as of the last Refresh
    Chrome                  chrome_;         ///< what the panel is dressed in
    ContextImages           images_;         ///< the images, through the format's functions
    std::string             version_;        ///< "v0.14.0": the stamp beside the title
    look::KnobStrip         knob_strip_;     ///< the chicken head's film strip
    look::CompanyMark       mark_;           ///< the company mark in the header
    gfx::Image              plate_;          ///< the face plate in force; invalid for the panel colour
    PresetPlateView         presets_;        ///< the PRESETS plate
    SettingsPlateView       settings_;       ///< the settings' gear, left of PRESETS
    std::vector<KnobEntry>  knobs_;          ///< the knobs, in the order added
    std::vector<PlateEntry> plates_;         ///< the plates, in the order added
    std::vector<gfx::RectF> frames_; ///< round each knob with its caption and box, and AddFrameAround's: this layout's

    std::map<std::string, gfx::Image, std::less<>> decoded_plates_; ///< each plate SetPlate has shown, by its asset
};

/**
    A panel of the series as whatever shows one reads it - a window's shell its colours, the Pedalboard's drawer its
    PRESETS plate, the suites its knobs: a view::Panel at its design size that holds a SeriesFace and names it
    (face()), the one thing it adds; the reads beside it are the face's. Every plugin's panel is one, asking its face to
    lay out, refresh and paint in its own Resized(), Frame() and Paint().
*/
class SeriesPanel : public view::Panel
{
  public:
    /** The header's height, in design pixels. */
    static constexpr int kHeaderHeight = look::kHeaderHeight;

    /** The face the panel holds.
        @return  the face, owned by the panel */
    virtual SeriesFace &face() noexcept = 0;

    /** What the panel is dressed in.
        @return  the face's chrome */
    const Chrome &chrome() noexcept
    {
        return face().chrome();
    }

    /** The PRESETS plate.
        @return  the face's plate */
    PresetPlateView &presets() noexcept
    {
        return face().presets();
    }

    /** The settings' gear.
        @return  the face's gear */
    SettingsPlateView &settings() noexcept
    {
        return face().settings();
    }

    /** The knob that turns parameter `id`, or null.
        @param id  the parameter's string id
        @return    the knob, owned by the face; nullptr when none turns it */
    view::Knob *KnobFor(std::string_view id) noexcept
    {
        return face().KnobFor(id);
    }

    /** The plate that switches parameter `id`, or null.
        @param id  the parameter's string id
        @return    the plate, owned by the face; nullptr when none switches it */
    view::Switch *PlateFor(std::string_view id) noexcept
    {
        return face().PlateFor(id);
    }

    /** Every knob, in the order added.
        @return  the face's knobs */
    std::vector<view::Knob *> knobs()
    {
        return face().knobs();
    }

    /** Every plate, in the order added.
        @return  the face's plates */
    std::vector<view::Switch *> plates()
    {
        return face().plates();
    }

    /** The frames the last layout drew.
        @return  the face's frames, in the panel's design pixels */
    const std::vector<gfx::RectF> &frames() noexcept
    {
        return face().frames();
    }

  protected:
    /** A panel at its design size.
        @param design_width   the design width, in design pixels
        @param design_height  the design height, in design pixels */
    SeriesPanel(int design_width, int design_height) noexcept : view::Panel(design_width, design_height)
    {
    }
};
} // namespace reckless::panel
