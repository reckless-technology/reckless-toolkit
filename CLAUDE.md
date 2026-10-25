# CLAUDE.md

Guidance for Claude Code working in this repository. The README is the toolkit for a reader - its options, targets,
functions and suites; this file is how work here is done.

## What this is

The Reckless Technology toolkit: the C++20 library the company's products share - JSON, text and the user's files
namespace `reckless`), JSON, text and the user's files (`core/`), the drawing layer (`gfx/`) and the series' look
(`look/`), the views (`view/`) and the series' panel (`panel/`), the child window on Skia (`window/`, `skia/`), an app's
platform layer (`app/`), the audio device layer (`audio/`), the network (`net/`), the update check every product runs
(`update/`: the check, a product's description and the one factory its checker comes from, and the Updates menu) and
the licence (`licence/`), with the artwork (`assets/`), the suites (`tests/`) and the CMake helpers (`cmake/`). Every
directory is an include directory and a header is included by its bare name (`"View.h"`), so the include namespace is
flat: a consumer names its own headers apart from the toolkit's.

Repository `github.com/reckless-technology/reckless-toolkit`, **public**, default branch `main`, free software under
the GNU General Public License version 3 or later - copyright Jonny Reckless. Three files carry that, and each holds
one thing only: `COPYRIGHT` the copyright, the grant and what the licence does not cover, `LICENSE` the Free Software
Foundation's text **byte for byte and nothing else** - a word of our own above it and GitHub stops recognising the
licence, reporting `NOASSERTION` instead of `GPL-3.0`, which is what happened to `reckless-toolkit-old` and is why
the recital lives in `COPYRIGHT` - and `NOTICE` what of this is another's and under what terms. The apps built on it are GPL too, and a GPL binary owes its user the whole of
its corresponding source: that is why this repository is public.

**The history here starts at this repository's first commit** (2026-10-24), a snapshot of the toolkit as it then
stood, taken deliberately so that no private history was published. The development history up to that date - the
toolkit's own 478 commits, reaching back through vst-plugins' `common/` and the plugins' own directories - is in
`reckless-technology/reckless-toolkit-old`, private and archived. The two share no common ancestor, so they cannot be
fetched into one another and `git log --follow` does not cross from one to the other: for anything before
2026-10-24, read the archived repository.

**The device layer is native.** Since 2026-10-25 every audio and MIDI API on every platform is a back end of the
toolkit's own, behind the seam in `audio/DeviceBackends.h` - CoreAudio and CoreMIDI, Windows Audio, ASIO and WinMM,
ALSA, its sequencer and JACK - and RtAudio and RtMidi, which the layer was first written on, are compiled into nothing.
Before touching anything under `audio/`, read **[docs/audio/native-backends.md](docs/audio/native-backends.md)**: the
decision and what it rests on, each back end and what it was measured against, how a back end is added, and the things
settled by measurement so they are not re-argued. What is left is running the hardware a CI runner lacks.

## The consumers

| Product | Repository | State |
|---|---|---|
**Every consumer carries the toolkit as a git subtree** at `external/reckless-toolkit` - no submodule anywhere any
more, the last of them converted on 2026-10-24. A pull is made by the consumer's own `tools/toolkit-pull.sh`, which
flattens it into one linear commit, because these repositories require a linear history and are rebase-only. So a
toolkit change reaches a product in two steps: merge it here, then run that script in each product.

| Product | Repository | State |
|---|---|---|
| The plugin suite | `vst-plugins` (private) | **a subtree** since pull request #503; added with `add_subdirectory`; it sets `RECKLESS_TOOLKIT_BUILD_TESTS`, `RECKLESS_TOOLKIT_GUI` and `RECKLESS_TOOLKIT_FETCH_ASIO_SDK` from its own options, and reads the pins back for its third-party notices |
| Reckless IR Capture | `vst-plugins/ir-capture/` | no longer a repository of its own: it moved into the suite and shares the suite's copy of the toolkit, with the GUI and the audio layer on and its suites off. The standalone `ir-capture` repository is archived |
| Reckless Amphitheater | `amphitheater-chess` (public) | **a subtree**, added with the GUI on and its suites and the audio layer off; its window is built on the app shell (`app/AppRun.h`, `docs/app/app-shell.md`) |
| Knect 4 | `knect4` (public) | **a subtree**, added with the GUI and the audio layer on and its suites and ASIO off; its window is built on the app shell, its landings' clicks played through the audio layer's default output |
| Reckless Sudoku | `sudoku` (public) | **a subtree**, added with the GUI on and its suites, the audio layer and ASIO off; its window is built on the app shell, under the series' header with the company mark |

**A consumer needs no key to read this repository any more**: it is public, so a consumer's CI clones it over https,
and the per-consumer read-only deploy keys that used to sit here (Settings, Deploy keys) can go with the secrets that
held their private halves in each consumer.

The other way round, this repository's CI reads each consumer for the `consumers` job below, and there a key is still
needed for a **private** consumer only: `vst-plugins`, through a read-only deploy key on it ("reckless-toolkit CI
consumer guard (read-only)") whose private half is this repository's Actions and Dependabot secret
`CONSUMER_VST_PLUGINS_DEPLOY_KEY`. `amphitheater-chess`, `knect4` and `sudoku` went public and GPL on 2026-10-24 and
are cloned over https with no key, which is what a checkout with no key can do; `tools/check-consumers.sh` chooses per
consumer. `CONSUMER_IR_CAPTURE_DEPLOY_KEY` is obsolete with the standalone `ir-capture` repository. A new consumer gets its line in `tools/check-consumers.sh`, its key and its secret (`ssh-keygen
-t ed25519`, `gh repo deploy-key add` on the consumer, `gh secret set` here for both apps, the key files deleted after).

## Commands

From the repository root. On Windows, natively (never WSL), inside the Visual Studio developer environment:
`cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo && <command>'`
(any Visual Studio with the x64 C++ tools: `vswhere -latest -products * -requires
Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`).

```
cmake --preset release && cmake --build --preset release && ctest --preset release   # build/release, every suite but the window ones
cmake --preset tests && cmake --build --preset tests && ctest --preset tests         # build/tests, nothing windowed
ctest --preset windows                                                               # the window suites (they take the focus)
tools/coverage-windows.ps1                                                           # coverage on Windows, CI's floor
python3 tools/check-docs.py                                                          # every document's links and its index line
doxygen Doxyfile                                                                     # build/doxygen/html; warnings.log must be empty
```

Build trees are directories under `build/`: `release`, `debug` (RelWithDebInfo, the debugger's), `tests`, `cov`, and
CI's `ci-toolkit`, `ci-toolkit-minimal`, `ci-toolkit-cov` and `ci-toolkit-consumers`. Run long builds detached with `-j 16` (the machines and
the runners are shared with other agents) and write their logs to the session's scratchpad, never into the repository.

`tools/setup-dev.ps1` / `tools/setup-dev.sh` (VS Code's "setup environment"; `-Check` / `--check` reports only) install
the compiler, CMake, Ninja, clang-format 22.1.8 and gcovr and set `core.hooksPath .githooks`.

## Release signing

`tools/sign-mac.sh` (macOS: `open` a keychain of the job's own, `sign <dir>` every `.vst3`, `.component` and `.app`
in it with the Developer ID Application identity and the hardened runtime, an app with `installer/app.entitlements`,
`notarize <pkg>`, `close`), `tools/esigner-sign.ps1` (Windows: one file Authenticode-signed through SSL.com's eSigner,
`-Check` a sign-in that spends no signing) and `tools/installer/app.entitlements` (the audio-input entitlement) are the
one copy every product's release signs with, run from `external/reckless-toolkit/tools/`; the README's "Release
signing" names what each reads. They find what they carry by their own directory, never a product's path. The secrets
are never here: each product holds its own copies (`MACOS_DEVELOPER_ID_*_P12`, `MACOS_SIGNING_P12_PASSWORD`,
`MACOS_TEAM_ID`, `MACOS_NOTARY_KEY_*`, `ESIGNER_*`) and its release job hands them over in the environment; nothing
prints one. Signing runs only in a release, so a change to these files is proved in a consumer's release dry run
(vst-plugins: `gh workflow run release.yml --ref main -f dry_run=true`) before the next real release, and keeps
working for every consumer: `sign`'s bundle kinds, the identity check and the exit codes are what their workflows read.

## Rules

- **A shared class never changes behaviour once a product has shipped with it.** Add a
  parameter, a template argument or a class instead.
- **Tuning numbers come from the products**: a template argument, a design struct of `static constexpr` members the
  product defines, or a function argument - never a default here.
- **Every consumer's suites are run before a consumer's submodule pointer moves.** A change here lands through a pull
  request when this CI is green - its `consumers` job has configured every product on it and built a few of each one's
  targets, so a clash of names or a moved header shows here first - and each product then moves its pointer in a pull
  request of its own, whose CI builds and tests it whole on the new toolkit.
- **Nothing here knows a product.** No include, link or path of a product; CMake paths from
  `CMAKE_CURRENT_SOURCE_DIR` / `CMAKE_CURRENT_LIST_DIR`; machine-wide settings only under `PROJECT_IS_TOP_LEVEL`;
  `reckless_warnings` only if the product has not defined it. Keep the existing target names and the bare-name include
  style: a rename breaks every consumer.
- **No libm transcendental per sample on the audio thread**: `FastTrig.h` and `FastExpLog.h`, or a recurrence.
- Double past the host boundary; `#pragma once`; full, descriptive names; every target links `reckless_warnings`, and
  the code stays warning-free on gcc, clang and MSVC (read CI's logs).
- **Documented where declared**: every class, function, member, parameter (`@param`), return (`@return`) and template
  parameter in `/** ... */` or `///<`; Doxygen's warnings log must be empty, and a new namespace gets its line in
  `docs/namespaces.dox`. The device suites' harness (`reckless::audio::test::suite`) is left out on purpose, as test
  cases are.
- Documents live under `docs/`, mirroring the code, each listed in `docs/index.md`; links stay inside the toolkit
  (`tools/check-docs.py` refuses one that leaves it).

## Formatting, commits, pull requests

- clang-format **22.1.8** on every touched C++ file before committing (`.clang-format`, vst-plugins'); the pre-commit
  hook checks the staged content and refuses anything else, and CI's `clang-format` job checks the tree.
- Every commit is authored and committed as `Jonny Reckless <jonny@recklesstechnology.com>`: set it in the clone
  (`git config user.name 'Jonny Reckless' && git config user.email jonny@recklesstechnology.com`); the hooks and CI's
  first step refuse any other. Commit messages: an imperative subject, a body that says why, and a
  `Co-Authored-By: Claude <model> <noreply@anthropic.com>` trailer.
- **`main` takes pull requests only**, enforced by the ruleset "main takes pull requests": a pull request whose `CI`
  check is green on a branch up to date with `main`, merged by rebase alone (squash and merge commits are GitHub's own
  commits, under its identity), no force push, no deletion, no bypass. The repository allows rebase merges alone and
  auto-merge. The ruleset was found missing on 2026-10-25 - lost when the repository was made anew in public - and
  restored from sudoku's, which is the same: if a `gh pr merge --auto` ever merges at once, check
  `gh api repos/reckless-technology/reckless-toolkit/rules/branches/main` first. `git switch -c <topic>`, push, `gh pr create
  --fill`, `gh pr merge --rebase --auto`; a green pull request is merged without asking (Jonny's standing permission).
  If `main` moves, rebase the branch and force-push it. The pre-push hook refuses a push to `main`.

## CI

`.github/workflows/build.yml`, on GitHub's hosted images, each pinned - `ubuntu-24.04`, `windows-2025` (MSVC through
vswhere and `VsDevCmd.bat`) and `macos-15`, a fresh machine for every job. This repository is public, and the
organisation's self-hosted pool does not serve public repositories (a job queued for it would wait for ever); the
workflow's opening comment says so. The hosted Linux kernel has no sound support, so `tools/audio-test-devices.sh`
installs the ALSA and JACK headers - the back ends compile - and the device suites are left out there
(`RECKLESS_AUDIO_DEVICES=none`): the Linux device suites run on a developer's machine and in vst-plugins' CI, on the
organisation's own runners. Jobs: `changes` (the commits' identity, `check-docs.py --tracked`,
and whether code changed), `linux` (gcc, every suite, the device suite with `RECKLESS_AUDIO_REQUIRE=ALSA,JACK` after
`tools/audio-test-devices.sh` wherever the kernel has sound, the window suites under `xvfb-run` with `RECKLESS_REQUIRE_WINDOW`), `windows` (MSVC,
`RECKLESS_AUDIO_REQUIRE=ASIO` on the SDK fetched by name and hash), `macos` (universal arm64 and x86_64, every binary
read with `lipo` and `vtool` for both slices and the 13.3 floor, `RECKLESS_AUDIO_REQUIRE=CoreAudio`,
`RECKLESS_REQUIRE_WINDOW`), `minimal` (no GUI, no audio device layer), `consumers` (Linux: `tools/check-consumers.sh`
clones vst-plugins, IR Capture, Amphitheater, Knect 4 and Sudoku at their default branches' heads through the consumer guard's
deploy keys, links this checkout in at each one's `external/reckless-toolkit`, configures each as its own CI does and
builds a few of its targets - vst-plugins' `reckless_update_shared` and `reckless_plugin_view_test`, IR Capture's
`ir_capture_ui`, Amphitheater's `amphitheater_gui`, Knect 4's `knect4_gui`, Sudoku's `sudoku_gui` - so a target, CTest or option named as a
product's, a header a product's shadows, or a moved function fails here), `coverage` (gcc, gcovr on `core/`, `gfx/` but
its Skia canvas, `look/`, `view/`, `net/`, `update/` and `licence/`, red under the floor `--fail-under-line` names),
`format`, `doxygen` and `CI`, the aggregate the ruleset requires. Documentation alone builds nothing.
`RECKLESS_UPDATE_CHECK=off` everywhere. Nothing is kept between runs: each job builds from nothing and fetches what it
needs. `.github/dependabot.yml` watches the actions' majors; its
commits must be made again under Jonny's identity before they can merge.
