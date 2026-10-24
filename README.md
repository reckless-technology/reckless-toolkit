# Reckless Technology toolkit

What the company's products share, written against no one product: JSON, text and the user's files,
the drawing layer and the series' look on it, the views a panel is made of and the series' panel on them, the child
window a panel is shown in on Skia, an app's platform layer, the audio device layer, the network, the update check and
the licence - with the artwork they draw, the suites that hold them and the CMake helpers a product builds its binaries
with. C++20, every directory an include directory of its own and every header included by
its bare name (`#include "View.h"`). Free software under the GNU General Public License, version 3 or later,
copyright Jonny Reckless, published as Reckless Technology ([LICENSE](LICENSE) is the licence; [NOTICE](NOTICE)
says what of it is another's - the fonts, Steinberg's logo, the company's marks - and under what terms).

The toolkit depends on nothing of any product: nothing here may include, link or name a path of a product. It was
vst-plugins' shared code, gathered there and lifted out with its history into this repository, which is now the one
copy every product builds on.

## Who builds on it

| Product | Repository | How |
|---|---|---|
| The Reckless plugin suite | `reckless-technology/vst-plugins` | a git submodule at `external/reckless-toolkit`, added with `add_subdirectory` |
| Reckless IR Capture | `reckless-technology/ir-capture` | moving onto the submodule from its own copy of the build files |
| Reckless Amphitheater | `reckless-technology/amphitheater-chess` | a git submodule at `external/reckless-toolkit`, added with the GUI on and its suites off |
| Knect 4 | `reckless-technology/knect4` | a git submodule at `external/reckless-toolkit`, added with the GUI and the audio layer on, its suites and ASIO off |
| Reckless Sudoku | `reckless-technology/sudoku` | a git submodule at `external/reckless-toolkit`, added with the GUI on, its suites, the audio layer and ASIO off |

A product holds the toolkit as a submodule at `external/reckless-toolkit` and adds it from its root `CMakeLists.txt`,
after setting, as normal variables, whichever options follow the product's (CMP0077 lets a normal variable stand in for
an option):

```
git submodule add git@github.com:reckless-technology/reckless-toolkit.git external/reckless-toolkit
```

```cmake
set(RECKLESS_TOOLKIT_BUILD_TESTS ${MY_PRODUCT_BUILD_TESTS})
set(RECKLESS_TOOLKIT_GUI ON)
add_subdirectory(external/reckless-toolkit)
target_link_libraries(my_product PRIVATE reckless_common_core reckless_common_view reckless_window)
```

The repository is private, so a product's CI fetches the submodule with a read-only deploy key on this repository, its
private half a secret of the product's repository; a person's clone fetches it with their own credentials
(`git clone --recurse-submodules`, or `git submodule update --init` after a plain clone).

## The rules

- **A shared class never changes behaviour once a product has shipped with it.** It is in that product's sound, its
  files or its look. Add a parameter, a template argument or a new class instead.
- **Tuning numbers come from the products.** A number a shared class needs comes in from the product that uses it - a
  template argument, a small design struct of `static constexpr` members the product defines, or a function argument -
  never as a default here.
- **Every consumer's suites run before a consumer's pointer moves.** A change here is merged when this repository's CI
  is green; a product moves its submodule to it only through its own pull request, whose CI builds the product and runs
  its suites on the new toolkit.
- **The toolkit stands alone.** Every path is from `CMAKE_CURRENT_SOURCE_DIR` or `CMAKE_CURRENT_LIST_DIR`; machine-wide
  settings (C++20, the static MSVC runtime, the macOS floor) are set only when it is the top-level project; a shared
  helper target (`reckless_warnings`) is defined only when the product has not.
- One class in one place: a building block a second product needs moves here, never into a copy.

## Building it on its own

On its own it is the top-level project and builds its suites. With the presets (`CMakePresets.json`):

```
cmake --preset release && cmake --build --preset release && ctest --preset release   # every part and suite
cmake --preset tests && cmake --build --preset tests && ctest --preset tests         # nothing windowed, no Skia
ctest --preset windows                                                               # the window suites alone
```

or by hand, from the Visual Studio developer environment on Windows (`VsDevCmd.bat -arch=x64 -host_arch=x64`):

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release -LE "^window$" -E "^audio_devices$"
```

`tools/setup-dev.ps1` (Windows) and `tools/setup-dev.sh` (Linux, macOS) install what that needs - the compiler, CMake,
Ninja, clang-format 22.1.8 and gcovr - and set the git hooks (`core.hooksPath .githooks`); VS Code's "setup
environment" task runs them. `tools/coverage-windows.ps1` measures coverage natively on Windows, as CI's coverage job
does on Linux. Configured on its own it sets C++20, the static MSVC runtime and the macOS floor (13.3).

CI (`.github/workflows/build.yml`, on the organisation's own runners) builds and tests it top-level on Linux, Windows and
macOS (universal, every binary held to both slices and to the floor), again with nothing windowed and no audio device
layer, measures coverage of the parts the toolkit's own suites test, and checks the formatting, the documents and the
source documentation (`doxygen Doxyfile`, every class, function and member documented, no warning). Its last job, `CI`,
is the check `main` requires.

### Options

| Option | Default | What it does |
|---|---|---|
| `RECKLESS_TOOLKIT_BUILD_TESTS` | ON | The toolkit's suites, and GoogleTest, which a product's suites use too. |
| `RECKLESS_TOOLKIT_GUI` | ON | The windowed parts: Skia (fetched), `reckless_gfx_skia`, the child window and the app platform. Off, nothing of Skia is fetched. |
| `RECKLESS_BUILD_AUDIO` | ON | The audio device layer on RtAudio and RtMidi (fetched), and its suites. |
| `RECKLESS_ASIO`, `RECKLESS_ASIO_SDK_DIR` | ON, empty | Windows: ASIO devices on Steinberg's ASIO SDK, fetched by version and hash, or the unzipped SDK the directory names. |
| `RECKLESS_TOOLKIT_FETCH_ASIO_SDK` | OFF | Fetch the ASIO SDK on every platform, for a product's notices, compiling none of it. |
| `RECKLESS_DEMO_MODE` | OFF | The build's demo switch (`licence/DemoMode.h`), a compile definition on `reckless_demo_mode`. |

### Targets

The names are what a product links. Header-only parts are `INTERFACE` libraries; the rest are static, position
independent with their symbols hidden, as a plug-in module's code is.

- `reckless_common_core` - `core/`; `reckless_core_preset_files` - its platform part (the user's folders); `reckless_audio_decoders` and `reckless_common_audio_files` - audio files.
- `reckless_common_gfx`, `reckless_common_look`, `reckless_common_view`, `reckless_common_panel`.
- `reckless_common_net`, `reckless_update` - the update check, a product's description and factory, and its Updates menu ([docs/update/update-check.md](docs/update/update-check.md)).
- `reckless_demo_mode`, `reckless_monocypher`, `reckless_common_licence`.
- `reckless_window_headers` - the window's headers with no Skia (the shell, the licence's card); `reckless_app_headers` - the app platform's seam (`app/AppPlatform.h`) on them, with no platform compiled, for a suite's platform of its own.
- `reckless_app_shell` - a desktop app's shell on that seam, with no platform and no renderer: its window and loop (`app/AppRun.h`), its root (`app/AppRoot.h`), the window's sizes from a View menu (`app/AppWindowSizes.h`), its theme and fonts (`app/AppTheme.h`), its widgets (`app/AppWidgets.h`) and its update check (`app/AppUpdates.h`) - [docs/app/app-shell.md](docs/app/app-shell.md).
- `reckless_common_audio`; with the audio layer, `reckless_audio_devices`, `rtaudio`, `rtmidi` and on Linux with JACK's headers `reckless_jack_loader`.
- With the GUI: `reckless_skia`, `reckless_gfx_skia`, `reckless_window_api` (the window's header, for an adapter), `reckless_window` (the window compiled once), `reckless_app_platform` and `reckless_app_resources` (what an app's views draw with on Skia, `app/AppResources.h`).
- With the suites: `reckless_toolkit_test_support`, `reckless_test_host_window` (with the GUI), `GTest::gtest` and `GTest::gtest_main`, and `reckless_warnings` unless the product defined it first.

### Functions

- `reckless_embed(<target> FILES <name>=<path>...)`, `reckless_embedded_bytes(...)`, `reckless_module_assets(<target> [<name>=<path>...])` - files carried inside a binary, found by name through `window/Embedded.h` (`cmake/RecklessEmbed.cmake`).
- `reckless_monospace_font_files(<var>)` - the monospaced family's two faces, Monaspace Xenon regular and bold, as the `<name>=<path>` items the two above take: opt in, so only a product that names them carries them (`cmake/RecklessEmbed.cmake`; [docs/gfx/svg-and-monospace.md](docs/gfx/svg-and-monospace.md)).
- `reckless_bundle_icon`, `reckless_mac_bundle_files`, `reckless_windows_version_resource`, `reckless_sign_bundle` - what a bundle carries beside its module: the icon (`RECKLESS_BUNDLE_ICON`, the company's 3D logo by default), PkgInfo, the Windows version resource and an ad-hoc signature (`cmake/RecklessBundle.cmake`).
- `reckless_module_window(<target>)` - the child window compiled into one plug-in module, its view class named for the module (`skia/CMakeLists.txt`).
- `reckless_audio_device_suite(<target> <test> SOURCES ... [LIBRARIES ...])` - a suite on the device layer's test devices, its harness `tests/devices/DeviceSuite.h`, run alone.

### What it hands back

To the directory that added it: every pin it fetches by (`RECKLESS_TOOLKIT_PINS` names them; `cmake/ToolkitPins.cmake`
sets them), `RECKLESS_ASIO_SDK_DIR` (the SDK it builds on) and `RECKLESS_AUDIO_APIS`; in the cache, `RECKLESS_TOOLKIT_DIR`
(its source directory, for a product's paths to its assets and test fixtures) and `RECKLESS_AUDIO_PROMISED_TEXT`. A
product finds the fetched stb tree with `FetchContent_GetProperties(stb)`.

### Suites

`core`, `gfx`, `look`, `view`, `embedded_a`, `embedded_b`, `audio`, `audio_devices` (the real APIs, run alone, needing
each API's test device - [docs/audio/device-tests.md](docs/audio/device-tests.md); on Linux `tools/audio-test-devices.sh`
provisions ALSA's loopback card and JACK), `net`, `update`, `licence`, `app` (the queue other threads post to the main
thread through, the menu bar and its shortcuts, the suites' own platform, and an app's shell on it), and with the GUI
`skia`, `list_text_views` (the list and text views drawn on Skia, their review images written where
`RECKLESS_SCREENSHOT_DIR` names), and `child_window`, `clipboard`, `app_menu` and `app_platform` (label `window`: this
build's platform, which on Linux needs a display and on macOS takes the focus; the clipboard's round trips use the
system's clipboard only in CI or where `RECKLESS_TEST_CLIPBOARD=1` asks). The environment steers them:
`RECKLESS_AUDIO_REQUIRE` (a comma-separated list of APIs whose missing test device fails rather than skips),
`RECKLESS_REQUIRE_WINDOW` (a window suite that finds no window fails rather than skips), `RECKLESS_NET_LIVE` (the
network's suite reaches the internet) and `RECKLESS_RUN_MEASUREMENTS` (a long measurement runs under a sanitizer too).
## Release signing

Every product's release signs with the same three files, run from where the product holds the toolkit
(`external/reckless-toolkit/tools/`), so there is one copy of each:

| File | What it does | What it reads |
|---|---|---|
| `tools/sign-mac.sh` | macOS: `open` makes a keychain of the job's own from the Developer ID identities and puts it first on the search list; `sign <dir>` signs every `.vst3`, `.component` and `.app` at the top of `<dir>` with the Developer ID Application identity, the hardened runtime and a secure timestamp, each app with `installer/app.entitlements` beside the script, and verifies each as the system does; `notarize <pkg>` sends a signed installer to Apple's notary service, waits, staples the ticket and holds it to Gatekeeper; `close` deletes the keychain and restores the search list. | `MACOS_DEVELOPER_ID_APPLICATION_P12`, `MACOS_DEVELOPER_ID_INSTALLER_P12` (base64 PKCS #12), `MACOS_SIGNING_P12_PASSWORD`, `MACOS_TEAM_ID`; for `notarize`, `MACOS_NOTARY_KEY_P8`, `MACOS_NOTARY_KEY_ID`, `MACOS_NOTARY_ISSUER_ID` (an App Store Connect API key) and optionally `RECKLESS_NOTARY_TIMEOUT` (3h by default); the keychain in `RUNNER_TEMP` (or `TMPDIR`). |
| `tools/esigner-sign.ps1` | Windows: Authenticode-signs one file in place through SSL.com's CodeSignTool with the company's eSigner certificate and an RFC 3161 timestamp, then holds the signature as Windows checks it (valid, Jonathan Reckless's, timestamped); a file not named `.exe`, `.dll`, `.msi`, `.sys` or `.cab` - Inno Setup's `.tmp` uninstaller - is signed as an `.exe` copy. `-Check` signs in and lists the account's certificates, spending no signing, for a release to run before its build. Run it as an Inno Setup `SignTool` or directly: `pwsh -NoProfile -File <toolkit>/tools/esigner-sign.ps1 <file>`. | `ESIGNER_USERNAME`, `ESIGNER_PASSWORD`, `ESIGNER_TOTP_SECRET` (not for `-Check`), `ESIGNER_CREDENTIAL_ID` where the account holds more than one certificate, `RECKLESS_CODESIGNTOOL` (the folder CodeSignTool was unpacked into) and optionally `RECKLESS_SIGN_LOG` (a file the tool's words go to, the credentials blanked). |
| `tools/installer/app.entitlements` | The hardened runtime's audio-input entitlement, without which macOS refuses a signed app its microphone without asking. | - |

The toolkit holds no secret. Each product keeps its own copies of the secrets in its own repository and hands them to
these scripts through the environment of its release job; the scripts never print them.

## Layout

| Directory | What it is |
|---|---|
| `core/` | JSON (`Json.h`, `JsonFile.h`), the environment read safely (`ProcessEnvironment.h`), UTF-8, numbers to and from text in any locale (`DecimalText.h`, `ParameterText.h`), the user's preset files and folders (`PresetFiles.h`), the interface's preferences, audio files read and a WAV written (`AudioFiles.h`), the denormal flush, a meter held between threads, and the queue any thread posts calls to a UI thread through (`PostQueue.h`). |
| `gfx/` | The drawing layer: geometry, colour, paths, text in two families (the series' serif and a monospaced face), the `Canvas` every panel draws on, the meters' SVG artwork (`SvgArt.h`) and a drawing's SVG by the specification's rules (`SvgImage.h`); `gfx/skia/` the canvas on Skia. |
| `look/` | The series' look on a canvas: metrics, palettes, the chicken-head knob, plates, the header, lamps, tubes, meters' scales, the oscilloscope and the tone stack's schematic. |
| `view/` | The views: the view tree, its painting and mouse, knobs, plates, buttons, menus, dialogs, text fields, a scrolled list of many rows (`ListView.h`), many lines of text to read or edit (`TextView.h`), the zoom, scroll bars, tooltips and the panel. |
| `panel/` | The series' panel on the views (`SeriesPanel.h`) and its screens and meters, and the series' look for the views that show many lines (`ListStyles.h`). |
| `window/` | The child window a panel is shown in (an NSView, an HWND, an X11 window) and the shell round a panel (`ShellView.h`), with the licence's card. |
| `skia/` | Skia, prebuilt from a pinned skia-pack release, and the targets on it. |
| `app/` | An app's platform layer: the application, where a window goes, the audio settings' widgets, a post to the main thread from any other, the clipboard, and the menu bar with its shortcuts (`MenuBar.h`, drawn on Linux by `MenuBarView.h`) - [docs/app/platform.md](docs/app/platform.md); and a desktop app's shell on it - its window and loop, its root, theme, widgets, resources and update check ([docs/app/app-shell.md](docs/app/app-shell.md)). |
| `audio/` | The audio device layer: devices and MIDI on RtAudio and RtMidi, Windows Audio (WASAPI) as the toolkit's own back end, the settings an app keeps and an offline device. |
| `net/` | HTTP on the platform's own stack, URLs, SHA-256 and a loopback listener for a sign-in's redirect. |
| `update/` | The update check against the company's website: the check (`UpdateCheck.h`), a product's description and the one factory its checker comes from (`UpdateProduct.h`), and the Updates menu every product offers (`UpdatesMenu.h`) - [docs/update/update-check.md](docs/update/update-check.md). |
| `licence/` | A licence's Ed25519 signature checked offline, the machine's store, and the build's demo switch (`DemoMode.h`). |
| `assets/` | The fonts - the series' serif (Noto Serif) and the monospaced family (Monaspace Xenon, `fonts/monaspace/`), each regular and bold beside its licence - the chicken-head knob's film strip and its CAD model, the company's mark and icons, the meters' artwork, and the Python requirements of what the build renders. |
| `tests/` | The suites, and what every suite shares (`reckless_toolkit_test_support`), among it an app platform of the suites' own with no display (`FakeAppPlatform.h`). |
| `cmake/` | The helpers above, the pins (`ToolkitPins.cmake`) and the patches applied to fetched RtAudio. |
| `docs/` | The documents, every one listed in [docs/index.md](docs/index.md), and `docs/namespaces.dox`, every namespace documented once for Doxygen. |
| `tools/` | The setup scripts, coverage on Windows, every consumer configured on this checkout (`check-consumers.sh`, CI's `consumers` job), the Linux test devices' setup, the licence's reference signer (`licence.py`), the documents' check (`check-docs.py`), the README's filter for Doxygen, and the release signing every product shares (`sign-mac.sh`, `esigner-sign.ps1`, `installer/app.entitlements`; above, under Release signing). |
| `.githooks/` | `pre-commit` (the one identity, and staged C++ clean under clang-format 22.1.8) and `pre-push` (no push to `main`, no commit under another identity). |
