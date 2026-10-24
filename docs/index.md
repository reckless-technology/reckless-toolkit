# The toolkit's documents

Every document of the toolkit, with a line on what it is. They live under `docs/`, which mirrors the code -
`gfx/`, `audio/`, `app/`, `core/`, `update/`, `licence/` - but for the README and CLAUDE.md at the toolkit's root. When a change adds, moves,
renames or removes a document, it updates this index in the same commit; `python3 tools/check-docs.py` checks that
every relative link in every document resolves inside the toolkit and that every document is listed here.

| Document | What it is |
|---|---|
| [README.md](../README.md) | The toolkit for a reader: what it holds, its CMake interface - its targets, options, helper functions and the pins it hands back - and how to build it and its suites on its own or inside a product. |
| [CLAUDE.md](../CLAUDE.md) | How work is done here, for Claude Code: the consumers, the commands, the rules for shared code, formatting, commits and pull requests, and what CI runs. |
| [namespaces.dox](namespaces.dox) | Every namespace the toolkit opens, documented once for the source documentation (the `Doxyfile` reads it). |
| [gfx/svg-and-monospace.md](gfx/svg-and-monospace.md) | Drawing a product's own artwork and text: `gfx::SvgImage`, the SVG reader that draws icons and chess pieces by the specification's rules beside the meters' `SvgArt`, and the monospaced font family (Monaspace Xenon), how a product opts in to it and chooses it per run of text. |
| [app/platform.md](app/platform.md) | The app's platform beyond its windows: the post to the UI thread from any thread and what happens to it after the app quits, the clipboard written on each platform, and the menu bar with its shortcuts - one description, shown natively on macOS and Windows and drawn on Linux, the view with the keyboard having each key first. |
| [audio/device-tests.md](audio/device-tests.md) | The audio layer on the real APIs: what `reckless_audio_device_test` holds on CoreAudio, Windows Audio in its three modes, ASIO, JACK and ALSA, the deterministic test device on each, what each CI runner needs, and how to run it by hand. |
| [core/presets.md](core/presets.md) | The presets and the state a plugin saves: the preset files and folders (`core/PresetFiles.h`), the PRESETS plate (`panel/SeriesPanel.h`) on its model (`view/PresetModel.h`), and how vst-plugins builds its formats' presets on them. |
| [app/app-shell.md](app/app-shell.md) | A desktop app's shell on the toolkit - its window and loop with the command line's `--smoke` (`app/AppRun.h`), the root that zooms a layout down to a small screen and shows the tooltip (`app/AppRoot.h`), its colours and fonts (`app/AppTheme.h`), its widgets (`app/AppWidgets.h`), what its views draw with on Skia (`app/AppResources.h`) and its update check (`app/AppUpdates.h`) - and how a product builds on it. |
| [view/list-and-text-views.md](view/list-and-text-views.md) | The views that show many lines - the list of many rows (`view/ListView.h`) and its model, columns, selection, scrolling and log-following, and the wrapped, selectable, editable text (`view/TextView.h`) - in the series' look (`panel/ListStyles.h`), with their review images. |
| [update/update-check.md](update/update-check.md) | The update check every product runs: the checker (`update/UpdateCheck.h`), a product's description and the one factory its checker comes from (`update/UpdateProduct.h`), the Updates menu as a submenu, a drop-down or a menu bar's menu (`update/UpdatesMenu.h`), and what each product describes. |
| [licence/licence-format.md](licence/licence-format.md) | The licence: one line of text signed with Ed25519 and checked offline against the public keys a build carries (`licence/`), its payload, the verdicts, where a machine keeps it, and the reference signer and test vectors (`tools/licence.py`, `licence/tests/vectors.json`) every implementation is held to. |
