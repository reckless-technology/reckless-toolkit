#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Menu.h"

namespace reckless::view
{
/**
    The presets a panel's PRESETS plate shows and acts on, written against no format: the plugin's presets by name and
    the one it is on, whether anything has moved since that one was loaded or saved, and what the menu does - load one,
    load a file, save under a name, delete the current one's file, reset every parameter to its default, show the folder
    - each answering with what went wrong, or nothing, for the panel to say. The questions round them (the name to save
    under, whether to delete) and the file dialog are the panel's; the files are the format's (vst-plugins'
    common/panel's HostPresetModel, on the series' preset files). A window's owner may hand the model the window's own
    settings - the shell's View submenu, the Standalone's Audio/MIDI Settings - with ids of their own, at kExtensionBase
    and up, which the settings' gear beside the PRESETS plate lists and not the PRESETS menu (panel::SettingsPlateView);
    and a model may put fresh starts of its own first, ahead of the presets, at kLeadingBase and up (LeadingItems: the
    Pedalboard's New board), with a question to ask before one runs, and leave Reset to defaults out where they stand
    for it (OffersReset).
*/
class PresetModel
{
  public:
    /** The first id an owner's items may use; the presets and the plate's own items sit under it. */
    static constexpr int kExtensionBase = 2001;

    /** The first id a model's own fresh starts use, past the owner's items. */
    static constexpr int kLeadingBase = 3001;

    virtual ~PresetModel() = default;

    /** The plugin's presets by name, in the order the menu lists them.
        @return  the names, in the menu's order */
    virtual std::vector<std::string> Names() const = 0;

    /** The preset the plugin is on, or empty for none.
        @return  its name, or an empty string for none */
    virtual std::string Current() const = 0;

    /** Whether anything a preset holds has moved since the current one was loaded or saved.
        @return  true when the plugin differs from its preset */
    virtual bool Modified() const = 0;

    /** Names()[index] loaded - the listing the last Names() made, the menu's, whatever the folder holds since; empty,
        or what went wrong.
        @param index  the preset's place in the last Names()
        @return       an empty string for success, else what went wrong, in plain words */
    virtual std::string Load(std::size_t index) = 0;

    /** The preset file at `path` loaded; empty, or what went wrong.
        @param path  the preset file's path
        @return      an empty string for success, else what went wrong, in plain words */
    virtual std::string LoadFile(const std::string &path) = 0;

    /** The plugin's state saved as the preset `name` (Save under the current name, and Save as), the plugin put on
        it; empty, or what went wrong.
        @param name  the preset's name, without the file's extension
        @return      an empty string for success, else what went wrong, in plain words */
    virtual std::string Save(const std::string &name) = 0;

    /** The current preset's file deleted, the parameters left where they are and the plugin on no preset; empty, or
        what went wrong - a file the system would not remove, the plugin left on its preset.
        @return  an empty string for success, else what went wrong, in plain words */
    virtual std::string Delete() = 0;

    /** Every parameter back to its default, the plugin on no preset. */
    virtual void ResetToDefaults() = 0;

    /** Where the presets are: where Load from file's dialog opens, and what Show presets folder reveals.
        @return  the folder's path */
    virtual std::string Folder() const = 0;

    /** Reveal the presets folder in the system's file manager. */
    virtual void ShowFolder() = 0;

    /** The file names a preset has, for the file dialog: "*.json".
        @return  the file dialog's pattern */
    virtual std::string FilePattern() const = 0;

    /** The window's own settings, which the settings' gear lists, and what one chosen does.
        @return  the items, with ids from kExtensionBase up; none by default */
    virtual std::vector<MenuItem> ExtensionItems() const
    {
        return {};
    }

    /** Do what a window's item chosen does; nothing by default.
        @param id  the chosen item's id, as ExtensionItems gave it */
    virtual void PerformExtension(int id)
    {
        (void)id;
    }

    /** The model's own fresh starts, listed first, ahead of the presets, with ids from kLeadingBase up.
        @return  the items; none by default */
    virtual std::vector<MenuItem> LeadingItems() const
    {
        return {};
    }

    /** What to ask before a fresh start runs - its plate asks over the panel and runs it on a yes - or nothing.
        @param id  the item's id, as LeadingItems gave it
        @return    the question in plain words; empty to run it at once */
    virtual std::string LeadingQuestion(int id) const
    {
        (void)id;
        return {};
    }

    /** Do what a fresh start chosen does; nothing by default.
        @param id  the chosen item's id, as LeadingItems gave it */
    virtual void PerformLeading(int id)
    {
        (void)id;
    }

    /** Whether the menu offers Reset to defaults: a model whose fresh starts stand for it leaves it out.
        @return  true, by default */
    virtual bool OffersReset() const
    {
        return true;
    }
};
} // namespace reckless::view
