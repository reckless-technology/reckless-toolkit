# The update check

Every product on the toolkit tells the person when a newer release of it is out, the same way: once a day at most, a
plain GET of a small static file on the company's website, only while one of the product's windows is on a screen, and
never a download or an install - only a menu that points at the release's page. Three headers in `update/`, all in
`reckless_update`, make it a service a product describes rather than writes.

| Header | What it is |
|---|---|
| `update/UpdateCheck.h` | `Checker`: the check itself - the manifest read strictly, the once a day after an answer and once an hour after none, a check on a thread of its own cancelled as the checker goes, the answer and "Check automatically" kept in a file every process of the product shares and read again when another moves it. `kManifestUrl`, the plugins' manifest; `kTrustedPagePrefix`, the only site a manifest may send the person to. |
| `update/UpdateProduct.h` | `Product`, a product's description; `MakeChecker`, the one factory every product's checker comes from; `StateFile`, `AllowedByEnvironment` and `VersionOf` beneath it. |
| `update/UpdatesMenu.h` | The Updates menu every product offers, as the views' menu rows: `UpdatesMenuItems`, `UpdatesSubmenu`, `UpdatesStatusText`, and `ChooseUpdatesItem` for the item chosen. |

## A product's description

```cpp
const reckless::update::Product product{
    "RecklessIRCapture",                                    // name: the User-Agent's product, before the version
    ToolVersion(),                                          // version: "MAJOR.MINOR.BUILD"
    "https://recklesstechnology.com/updates/ir-capture.json", // manifest_url: what the website writes
    "ir-capture-updates.json"};                             // state_file: reckless-technology/<it> in the user's settings
const std::unique_ptr<reckless::update::Checker> checker = reckless::update::MakeChecker(product);
```

`MakeChecker` puts the file at `reckless-technology/<state_file>` in the user's settings folder (`StateFile`), reads the
version (`VersionOf`: a text that is not a version reads as 0.0.0, which every release is newer than), and runs each
check on the platform's own HTTP stack (`net::MakePlatformTransport`) and the wall clock. Its request carries
`User-Agent: <name>/<version>` and `Cache-Control: no-cache`, and nothing about the person. Where the environment says
`RECKLESS_UPDATE_CHECK=off` - every CI runner sets it, so no suite, validator or launch's check reaches the network -
the checker is made all the same and never checks, by the clock or by hand (`AllowedByEnvironment`).

What each product describes:

| Product | name | manifest_url | state_file | How it holds it |
|---|---|---|---|---|
| The plugins (vst-plugins) | `RecklessPlugins` | `https://recklesstechnology.com/updates/latest.json` (`kManifestUrl`) | `updates.json`, shared by every plugin on the machine | one for the whole process (`update::Shared()`, its `common/update/`), made on first use, every plugin window's shell reading it; switched off by the environment it still shows its menu, which says it has not checked |
| Reckless IR Capture | `RecklessIRCapture` | `https://recklesstechnology.com/updates/ir-capture.json` | `ir-capture-updates.json` | one beside its window (`app::MakeUpdateChecker`, none at all where the environment switches checks off, so the window shows no UPDATES button) |

A product released apart from the others keeps a file of its own, so one product's answer is never read as another's.
The website writes each manifest from the product's newest public release as the release is promoted: `{"version":
"1.2.0", "page": "https://recklesstechnology.com/...", "released": "2026-11-02", "summary": "..."}`.

## Polling it

A checker does nothing on its own: the window's frame calls `Poll(shown)` about once a second, `shown` whether a window
of the product is on a screen a person can see, and reads `revision()` to know when what the menu or a header says may
have changed. A check is started by `Poll` when one is due and the window shows and checks are automatic, or by
`CheckNow`. `available()` is the newer release, where one is out - what a header's UPDATE badge reads.

## The Updates menu

The menu is made afresh each time it opens, from the checker as it stands:

| Row | Text | Id | Enabled | Ticked |
|---|---|---|---|---|
| status | `Version 1.2.3, not checked yet` / `Checking for updates...` / `Version 1.2.3 is the newest` / `Version 1.3.0 is out` / `Version 1.2.3, could not check` | 0 | no | no |
| page, only where a newer release is out | `Get version 1.3.0...` | `ids.page` | yes | no |
| a rule | | | | |
| check now | `Check for updates now` | `ids.check_now` | not while a check runs | no |
| automatic | `Check automatically` | `ids.automatic` | yes | while checks are made by the clock |

The three ids are the owner's (`UpdatesMenuIds`, 1, 2 and 3 by default), apart from every other item of the menu that
holds the rows. `ChooseUpdatesItem(checker, ids, id, open_url)` does what the chosen item says - the page opened through
`open_url` (`window::OpenInBrowser`), a check asked for, checks by the clock turned over - and says what it did
(`UpdatesChoice`), for a product that tells the person in its own words.

- **As a submenu**: `UpdatesSubmenu(checker, ids)` is one item titled `Updates` that opens the rows beside it - the
  plugins' settings gear (`window/ShellView.h`: `SetUpdates`, its ids `kUpdatePageItem`, `kCheckNowItem` and
  `kCheckDailyItem`, 2210 to 2212).
- **As a drop-down**: `UpdatesMenuItems(checker, ids)` are the rows of a menu of their own - IR Capture's UPDATES
  button opens them with `view::MenuSession::Open`.
- **As a menu of a menu bar**: `app::UpdatesMenuEntry(checker, open_url, chosen)` (`app/AppUpdates.h`) is the same
  rows as one menu of the bar (`app/MenuBar.h`) - each row's text its title, a dim row disabled, a tick checked, a rule
  a separator - each item choosing through its action, so it takes none of the app's ids, and `chosen` hearing what it
  did. The entries say what the checker said as they were made: the app sets its bar again whenever `revision()`
  moves, so the status line follows the check.

## Its suites

`update` (`reckless_update_test`): the checker on a scripted transport and a clock of the suite's own
(`update/tests/test_update_check.cpp`), and a product's description and the menu in every state the check can stand in,
its ids and what choosing each row does (`update/tests/test_updates_menu.cpp`). The settings gear's submenu is held in
`view`'s `test_shell_view.cpp`, and an app's `MakeUpdateChecker` in `app`'s `test_app_shell.cpp`.
