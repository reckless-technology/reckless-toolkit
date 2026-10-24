# Presets

How a plugin's settings, and a whole board, are saved to a file and loaded back, in every plugin and every format of the
suite the same way. The files are the toolkit's core/: `Json.h` (the JSON text), `JsonFile.h` (a JSON file read and
written whole), vst-plugins' `HostSession.h` (a plugin's state as a document) and `PresetFiles.h` with `PresetFiles.cpp` (the folder, the
names, the listing, the save, the load and the delete). The menu is panel/'s: the PRESETS plate (`SeriesPanel.h`'s
`PresetPlateView`) on its model, `view::PresetModel` (view/PresetModel.h), which every format makes a
`panel::HostPresetModel` (`HostPresetModel.h`) on the session it keeps round the core. The core suite (CTest `core`)
holds the JSON, the document and the files, the view suite (`view`) the model, and each plugin's `<x>_session` CTest its
own document round its own core (The proof, below).

## What a preset is

A preset is the plugin's state document in a JSON file. Every format saves and restores a plugin's state through one
road - `core::HostSession` writes the document from the parameters and the core and restores it - and a preset takes
that same road: `HostSession::PresetDocument` is what a preset holds, and a load is `HostSession::Load` of the document
a file holds. A session's document:

```json
{
  "format": 1,
  "plugin": "Reckless Comp",
  "parameters": {
    "threshold_db": -18.0,
    "attack_ms": 5.0,
    "element": 2,
    "...": "..."
  },
  "state": {
    "link_follows_layout": true
  },
  "ui": {
    "scale": 1.25
  },
  "preset": "Squash"
}
```

- `format` is the document's version, 1; a document of another is refused, so a later change can read this one and move
  it on.
- `plugin` is the plugin's name (its core's `kName`) and the guard a restore checks: a Comp's document does not load
  into a Gate, whatever folder it came from. It is also the folder a preset lives in.
- `parameters` is every parameter by its id at the value the core reads, in the parameter's own units: a float
  parameter's as the shortest decimal that reads back to the double it holds (0.3), so a restore lands on that double
  exactly, and a choice's index and a toggle's 0 or 1 as whole numbers.
- `state` is what the core saves beside its parameters (`PluginCore::SaveState`), left out when it saves nothing: the
  EQ's analyser switch, the Comp's link flag, the Wah's pedal, the Cab's response and recipe, the NAM's capture, the
  Space's response and recipe, the board's settings and its pedals. A loaded response's samples are an object of their
  rate, the file's length and each channel's samples as little-endian doubles in base-64 (`core::ResponseDocument`), so
  a document carries the response whole and a restore builds the kernel a fresh load of the file builds, bit for bit.
- `ui` is the window's - its scale, or its size in scroll mode, which the window's shell (window/ShellView.h)
  writes, and a setting of the window a core keeps beside it (`PluginCore::SaveUi`: the Amp's view, SIMPLE or WIZARD) -
  and `preset` the name of the preset the session is on. Both are the session's alone, each left out when empty.

A preset is the document without `ui` and `preset`: a project reopens with its windows as they were left, and a preset
never carries a window. `HostPresetModel` drops whatever `ui` and `preset` a file carries and puts the instance's own
`ui` and the file's name in their places, so a load leaves the window where it is; and the plate's modified test
compares preset documents, so a resized window is not a changed preset.

A restore takes a document the session reads - `HostSession::Refusal` says why one is not, and a refused one changes
nothing: every parameter it names by id is set to its value, one it lacks or that is not a number to its default, and an
id no parameter has is ignored; then the core's `state` goes to `LoadState` and the `ui` to `LoadUi`, each an empty
object where the document has none. A value lands on its parameter as the parameter keeps it - a float's the double the
document carries, a stepped dial on its step, a choice's index, a toggle's 0 or 1 - so a session comes back to the bit
and saves the same document again.

Why JSON: a pretty-printed document of a few dozen `"key": value` lines is one a person can read, edit and diff, and the
suite's own writer and reader (`Json.h`: two-space indents, LF, reals in the shortest form that reads back to the same
double, a strict RFC 8259 reader) add no dependency to twenty-two plugins.

## Where presets live

`Documents/reckless-technology/presets/<plugin>/<preset name>.json` - `<plugin>` the plugin's folder name
(`core::PluginFolderName`: the product name lower case with "Reckless " dropped, `amp` for Reckless Amp, so no path has
a space in it), the user's documents found by the platform's own call (`presets::DocumentsDirectory`: the system's
documents directory on macOS, the Documents known folder on Windows, the XDG documents directory on Linux), one folder
per plugin. A name is trimmed of white space at either end and refused where a file system cannot hold it
(`presets::NameRefusal`): nothing at all, a path separator or a character Windows keeps (`/ \ : * ? " < > |`) or a
control character, a dot first or last, a name Windows keeps for a device (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to
LPT9, alone or before a dot), or more than 200 bytes. The listing is every file in the folder whose name ends in `.json`
in any case, not a hidden one, sorted by name ignoring case. Presets are documents people share and copy between
machines; the settings folder the Cab and the Space keep their last-opened folder in (`reckless-technology/<name>.json`)
is for settings nobody looks at. The plugin's folder is where the plate lists from and where its file dialog opens; a
file loaded from anywhere else is checked by its `plugin` field, not its folder. A file is written whole or not at all,
to a hidden temporary beside it and renamed over it. Tests point the root at a scratch folder
(`presets::SetRootForTests`) so a run never touches the user's presets.

### Factory presets

A plugin may ship presets of its own: the JSON files in its `plugins/<x>/Presets/` folder (today the Pedalboard's
boards), committed LF (`.gitattributes`) so every platform installs the same bytes. A release stages them as
`presets/<x>/` (`tools/stage-release.sh`), every zip carries that folder, and every installer puts it in a folder of the
machine's, beside nothing of the user's (`presets::InstalledFactoryRoot`, `PresetFiles.cpp`):

| platform | installer | factory presets |
|---|---|---|
| Windows | `tools/installer/RecklessPlugins.iss`, in no component so every install takes them | `C:\ProgramData\reckless-technology\presets\<x>\` |
| macOS | `tools/make-installer-mac.sh`, a hidden part every install takes | `/Library/Application Support/reckless-technology/presets/<x>/` |
| Linux | `tools/make-deb.sh` | `/usr/share/reckless-technology/presets/<x>/` - the first directory of `$XDG_DATA_DIRS` holding `reckless-technology/presets`, `/usr/share` where none does |

Each installer's uninstaller takes them off again (the macOS uninstaller leaving `/Library/Application
Support/reckless-technology` itself while a machine-wide licence is in it), and each installer's test holds its
payload to the staged files byte for byte (`tools/test-installer-windows.ps1`, `tools/test-installer-mac.sh`,
`tools/test-deb.sh`), as `tools/check-release-zip.py` holds each zip's `presets/` to the plugins' `Presets` folders.

The listing (`presets::List`) is the user's folder and the plugin's factory folder (`presets::FactoryFolderFor`)
together, sorted by name ignoring case as before, with no mark between them: a factory preset loads as any other. The
factory folder is read only. A user's preset of a factory preset's name, ignoring case, takes its place in the
listing, so Save on a factory preset writes the user's copy of it into the user's folder and that copy is the preset
from then on; Delete on a preset that is the factory's alone (`presets::IsFactoryOnly`) is refused in words ("\"Lead\"
is a factory preset, which cannot be deleted."), and Delete on the user's copy takes the copy and brings the factory
preset back. Show presets folder and Load from file's dialog stay the user's folder. A factory preset is a preset like
any other - the plugin's state document - and the files the plugins' own PRESETS plates wrote, with any path to a file
on the machine they were made on taken out (a NAM's `model_path`: its capture rides whole in `model_json`, which is all
a restore needs).

A suite never sees the machine's factory presets: while `presets::SetRootForTests` has a root set there are none
unless `presets::SetFactoryRootForTests` names a scratch folder of them, so an installed release cannot change what a
suite on the same machine lists. The core suite's `test_preset_files.cpp` holds the listing, the shadowing and the
installed folder on each platform (on Linux every `$XDG_DATA_DIRS` case), the view suite's `test_host_preset_model.cpp`
a factory preset loaded, saved over as the user's copy, refused a delete and back once the copy goes, and the board's
`pedalboard_format` every committed factory preset listed through the plate's model and loaded onto a board of cores,
every pedal it names made.

## Interchangeability

A pedal on the Pedalboard is the plugin's own core, made through its door (`Source/hosted/<X>Door.cpp`), and the drawer
shows the plugin's own panel with its own PRESETS plate: its model a `HostPresetModel` on the pedal's session under the
plugin's own name (`PedalPanelHost`, `plugins/pedalboard/Source/panel/PedalPanelHost.h`), reading the same folder and
writing the same files as the plugin on a track, a load heard as the pedal's restore (`CorePedal::SessionRestored`). So
a pedal's preset *is* the plugin's preset, and there is no conversion to keep in step.

A board preset is the Pedalboard's own preset. Its document is the board's parameters (the footswitches, INPUT, LEVEL
and the macros) and, in its `state`, the four settings (the tuner's reference, the tuner and AUTO MUTE switched on or
off, the level strip open or folded) and the board - every pedal in its place with its kind, its key and its state, the
splits with their modes, levels and pans, the macro links. A pedal's state is its plugin's session document, nested:
the block for a pedal in a board file is the document the pedal's plugin saves, but for its `ui` and its preset's name.
No preset carries a window's size or a panel's view, and a board preset is no exception: the board's own `ui`, its
window's size, is the session's, and the core takes every pedal's `ui` (the Amp's view in the drawer) and preset name
out of the preset's state as it is written (`PluginCore::PresetOfState`, which `HostSession::PresetDocument` asks: the
board's `BoardState::DropPedalUis`). A load takes out whatever `ui` a pedal's document in the file carries and gives the
pedal of the same kind in the same place along the signal path the `ui` the instance's pedal there has now
(`PluginCore::StateOfPreset`, which `HostPresetModel` asks before the restore: the board's `BoardState::KeepPedalUis`),
so each pedal's view stays where it was and a pedal with none of its kind in its place comes fresh. A board session
carries each pedal's whole document, its `ui` among it.

## The plate

Every header carries the PRESETS plate (`panel::PresetPlateView`, placed by the panel's `panel::SeriesFace` at the
header's right end, `look::kPresetPlateWidth`, 96 px, wide to carry a name; the Pedalboard puts it left of its TUNER,
INIT and CLEAR). It reads the preset the plugin is on - `Squash`, `Squash *` once anything has moved since the load or
the save, `PRESETS` on none - and opens the menu: the plugin's presets by name (the first thousand; the rest are reached through Load from file) with the current one
ticked; Save (under
the current name), Save as..., Delete...; Load from file..., Show presets folder; and Reset to defaults at the bottom.
Whatever the window adds (`view::PresetModel::ExtensionItems`, ids from `kExtensionBase`, 2001, above the presets' and
the plate's own, 1001 to 1006; the format sets them with `HostPresetModel::SetExtension`) is not in that menu but in
the settings' gear's, left of the plate (`panel::SettingsPlateView`, drawn bare, a plate's height across): in the
Standalone the Audio/MIDI Settings first, then the shell's View submenu (`window::ShellView`): Scale to fit or Scroll
bars, the mode the suite's `interface.json` holds ticked (`core::InterfaceSettings`, core/InterfaceSettings.h),
and the sizes 75, 100, 125, 150 and 200 % - the menu the panel has for things that are not the sound. The gear shows
only where the window has such items, so a pedal's panel in the Pedalboard's drawer, whose board's gear holds them,
shows none. Save as asks for a name and Delete asks first, over the panel, and what goes wrong is said over the
panel too; Load from file opens the window's own file dialog at the plugin's folder. Reset to defaults is in the menu
rather than on a button of its own because the narrow plugins' headers have room for one plate. With no presets from the
format - a panel rendered with no host - the menu is Reset to defaults alone, every control put back through its
parameter.

The model is `view::PresetModel`: the presets by name, the current one, whether it is modified, and each action
answering with what went wrong, or nothing. Every format makes it a `panel::HostPresetModel` on the session it keeps
round the core (`core::HostSession` on its `core::HostParameters`) - the VST3 (vst-plugins' common/vst3/Vst3Effect.h), the Audio
Unit (common/au/AuEffect.h), the Standalone (`StandaloneHost::MakePresets`, common/app/StandaloneHost.h) and each pedal in
the board's drawer - and the board's window asks its own of whether it is modified (below):

- The presets are the plugin's folder's and its factory folder's, by name (Factory presets, above); the one the session is on is its `preset` (HostSession's `preset` and
  `SetPreset`), which the session saves, so a session remembers which preset it was on and the plate reads it back. A
  load sets it to the file's name, a save to the name given, Delete and Reset to defaults clear it; it is never a
  parameter.
- Save writes the session's preset document under the name, refused where the name cannot be a file's. A load is
  `presets::LoadFrom` - the file read as JSON - then the session's refusal of a document that is not this plugin's
  state, said in plain words ("Gate.json cannot be loaded: it is a state of Reckless Gate, not of Reckless Toy."), then
  the session's own restore of it with the instance's `ui` in place of any the file carries, the session put on the
  file's name, and the host told as of a restore: the VST3's controller set to every parameter's position, then
  `restartComponent(kParamValuesChanged)` and `setDirty`; the Audio Unit's listeners told the change of each parameter
  the load moved, then the `ClassInfo` property changed, the property its state is saved on.
- The asterisk: on a preset, whether the session's preset document differs from the one taken at the last load or save -
  so a knob turned back takes the asterisk off again. The copy is taken again when the window opens and whenever the
  host restores the session (HostSession's `restores` count). The plate asks once a display frame and a document copies
  the whole state - a Space's thirty-second stereo response among it - so the document is made and compared only when
  the session's revision (HostSession's `Revision`) has moved since the last comparison, the answer kept otherwise: the
  same answer, made once a change. The revision counts every write of the window's - the preset's name, the `ui` - every
  restore, every move of a parameter that reaches the core (HostParameters' `Revision`) and every word from
  the core that what its `SaveState` writes has moved (`PluginCore::SavedStateChanged`, counted in `SavedStateRevision`:
  the EQ's analyser switch, the Wah's pedal on a controller, the Comp's link flag, a core's own store - the Cab's, the
  NAM's, the Space's `core::SessionProperties` - the Pedalboard's settings and its board, `BoardState::revision`), and a
  write behind the parameters' back that touches the session (`HostSession::Touch`: a board's macro turning a pedal's
  knob).
- The board's plate asks a test of its own (`BoardPresets`, `plugins/pedalboard/Source/panel/BoardPanel.h`) and never
  the format's, which would make every pedal's document - a Cab's or a Space's response among them - once a frame: on a
  preset, modified by an edit the board's parameters do not show since the last load, save or restore (the board's
  `BoardChanged`, and its settings flag, `BoardState::TakeSettingsChanged`: a pedal's knob or a merger's), or by a board
  parameter that is not where it stood then, read off the format's controls. Its Reset to defaults is the format's,
  a fresh board: no pedal, every setting at its default.
- Delete takes the current file and leaves the session on no preset (or, when the system will not remove the file or
  the preset is the factory's alone, says so and leaves the session on the preset); Reset to defaults is the plugin as made: the document of no
  parameters and no state - `format` and `plugin` alone - loaded by the road a preset takes, so every parameter lands
  at its default, the core is handed an empty state, which every core reads as a fresh core's (`PluginCore::LoadState`:
  the Cab's synthesised cabinet, the Space's built-in cathedral, the NAM with no capture, the Comp's link following
  the layout, the board empty; each plugin's `<x>_session` holds it, `TheDocumentOfNothingIsThePluginAsMade`), the
  instance's `ui` is kept - the window's size and the Amp's view - and the host is told as of a restore; the session
  is left on no preset; Show presets folder makes the folder and shows it in the platform's file manager
  (`window::RevealInFileManager`); Load from file opens the window's own file dialog; Reset to defaults comes last, and
  the shell's View submenu is the settings' gear's.

## The proof

The core suite's `test_json.cpp` holds the writer's text - the indents, the reals in their shortest form with a whole
one kept a real, the escapes, a byte that is not UTF-8 repaired - the strict reader's grammar and every text it refuses
with where, a document through its text and back the same value, and the bytes as base-64. Its `test_host_session.cpp`
holds the document: what a save writes, a restore by id with the rest at their defaults, every document the session
cannot read refused with nothing changed, a document through its text to the bit, the window's `ui` and the preset's
name the session's and no preset's, the revision, and a core's own store saved and restored whole. Its
`test_preset_files.cpp` holds the names refused, the files saved, listed, loaded, refused and deleted in a scratch
folder, a file written whole, and the documents and music folders the platform's; `test_response_files.cpp` a
response's document every sample to the bit and anything else refused. The view suite's `test_host_preset_model.cpp`
holds the model on the adapters' toy core (vst-plugins' common/tests/NativeToyCore.h) with no format - a save, the asterisk and a
knob turned back, a load keeping the window, a host's restore, Delete, Reset to defaults as the plugin as made with the
window kept, the problems said in plain words, the folder made before it is shown, the window's items set and taken off
for the gear - and the document made to compare only when something moved, once a change, on a core that counts its
saves; the Space's panel suite (`space_panel`) the plate on a session carrying thirty seconds of a response, with what a
frame costs. Each plugin's `<x>_window` (vst-plugins' common/vst3/tests/test_native_window.cpp) holds the plate on the plugin's own
VST3 editor, its whole menu ending at Reset to defaults, the View submenu behind the gear left of it, and a preset saved
and loaded through it; the adapters' suites (`vst3_effect`, `au_effect`) a
load told to the host as a restore; and the app's (`standalone_app`) the plate on the Standalone's own window.

Each plugin's `<x>_session` CTest (vst-plugins' common/tests/test_session_document.cpp, built by the recipe for every plugin whose
core's class it records) holds its own document round its own core: every parameter moved, saved, written as text, read
back and restored into a fresh core to the bit, the same document saved again, the core's own state whole; a parameter
the document lacks at its default; another plugin's document, another format's and a malformed one refused with nothing
changed. The Cab's and the Space's suites hold a session's response restored to the kernel a fresh load of the file
builds, bit for bit. The board's `pedalboard_format` (plugins/pedalboard/Tests/test_board_format.cpp) holds every kind
on a full board of cores saved and reopened the same, the Cab, the NAM and the Space carrying what they play through a
board, and the board's presets through the plate's own model: neither the board's window nor an Amp's view in a board
preset nor taken from one, a file carrying them included, the Amp left in the view the instance had it in, and the
board's session carrying the view and restoring it.

## What is deliberately not here

No factory presets built into a binary - a release's factory presets are files an installer puts on the machine, so a
build a developer deploys by hand (`tools/deploy-vst3.*`) or a plugin from a zip lists none until they are copied into
the machine's folder. No mark in the menu between a factory preset and the user's. No preset browser beyond the menu. No per-pedal preset menu on the board itself: a pedal's presets are reached through its drawer,
where its own panel is. No reading of the state the releases up to v0.15 saved: a session or a preset from them does not
load.
