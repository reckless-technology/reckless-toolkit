#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Menu.h"
#include "UpdateCheck.h"

namespace reckless::update
{
/**
    @file
    The Updates menu every product offers, made once: a line saying where the check stands ("Version 1.2.3 is the
    newest"), "Get version 1.3.0..." to the newer release's page where one is out, a rule, "Check for updates now" and
    "Check automatically", ticked while checks are made by the clock. The plugins' settings gear shows it as a submenu
    (window/ShellView.h), an app's window as a drop-down or a menu of its menu bar; each hands the item chosen back to
    ChooseUpdatesItem. The rows are made afresh each time the menu opens, from the checker as it stands, so a menu shown
    while a check runs says so.
*/

/** The ids the Updates menu's three items report when chosen: ones apart from every other item of the menu that holds
    them (the status line reports 0, which chooses nothing). */
struct UpdatesMenuIds
{
    int page{1};      ///< "Get version ...": the newer release's download page
    int check_now{2}; ///< "Check for updates now"
    int automatic{3}; ///< "Check automatically": checks by the clock on or off
};

/** What choosing an Updates item did, for a product that tells the person (IR Capture's log). */
enum class UpdatesChoice
{
    kNone,             ///< the id is none of the menu's, or the item had nothing to do
    kOpenedPage,       ///< the newer release's page was opened
    kCheckRequested,   ///< a check was asked for now (none starts while one is in flight or checks are off)
    kAutomaticChanged, ///< checks by the clock were turned on or off: Checker::automatic() says which
};

/** The status line: where the last check stands, in the menu's words.
    @param checker  the update check
    @return         "Version 1.2.3, not checked yet", "Checking for updates...", "Version 1.2.3 is the newest",
                    "Version 1.3.0 is out" or "Version 1.2.3, could not check" */
inline std::string UpdatesStatusText(const Checker &checker)
{
    const std::optional<Release> release = checker.available();
    const std::string            current = VersionText(checker.current());
    switch (checker.state())
    {
    case State::kUnchecked:
        return "Version " + current + ", not checked yet";
    case State::kChecking:
        return "Checking for updates...";
    case State::kCurrent:
        return "Version " + current + " is the newest";
    case State::kAvailable:
        return "Version " + (release ? VersionText(release->version) : current) + " is out";
    case State::kFailed:
        return "Version " + current + ", could not check";
    }
    return {};
}

/** The Updates menu's rows as the checker stands: the status line (dim, choosing nothing), "Get version ..." where a
    newer release is out, a rule, "Check for updates now" (dim while a check runs) and "Check automatically" (ticked
    while checks are made by the clock).
    @param checker  the update check
    @param ids      the ids the three items report
    @return         the rows, for a menu of their own or a submenu */
inline std::vector<view::MenuItem> UpdatesMenuItems(const Checker &checker, const UpdatesMenuIds &ids = {})
{
    const auto item = [](int id, std::string text, bool enabled, bool ticked) {
        view::MenuItem row;
        row.id      = id;
        row.text    = std::move(text);
        row.enabled = enabled;
        row.ticked  = ticked;
        return row;
    };
    std::vector<view::MenuItem> items;
    items.push_back(item(0, UpdatesStatusText(checker), false, false));
    if (const std::optional<Release> release = checker.available())
    {
        items.push_back(item(ids.page, "Get version " + VersionText(release->version) + "...", true, false));
    }
    view::MenuItem rule;
    rule.separator = true;
    items.push_back(rule);
    items.push_back(item(ids.check_now, "Check for updates now", checker.state() != State::kChecking, false));
    items.push_back(item(ids.automatic, "Check automatically", true, checker.automatic()));
    return items;
}

/** The Updates menu as one item titled "Updates" that opens its rows beside it: the plugins' settings gear's submenu.
    @param checker  the update check
    @param ids      the ids the three items report
    @return         the item */
inline view::MenuItem UpdatesSubmenu(const Checker &checker, const UpdatesMenuIds &ids = {})
{
    view::MenuItem submenu;
    submenu.text    = "Updates";
    submenu.submenu = UpdatesMenuItems(checker, ids);
    return submenu;
}

/** An Updates item chosen: the newer release's page opened, a check asked for, or checks by the clock turned over.
    @param checker   the update check
    @param ids       the ids the three items report
    @param id        the chosen item's id
    @param open_url  opens a page in the person's browser (window::OpenInBrowser); empty opens nothing
    @return          what it did; kNone for an id that is none of the three, or a page with none to open */
inline UpdatesChoice ChooseUpdatesItem(Checker &checker, const UpdatesMenuIds &ids, int id,
                                       const std::function<void(const std::string &)> &open_url)
{
    if (id == 0)
    {
        return UpdatesChoice::kNone;
    }
    if (id == ids.page)
    {
        if (const std::optional<Release> release = checker.available(); release && open_url)
        {
            open_url(release->page);
            return UpdatesChoice::kOpenedPage;
        }
        return UpdatesChoice::kNone;
    }
    if (id == ids.check_now)
    {
        checker.CheckNow();
        return UpdatesChoice::kCheckRequested;
    }
    if (id == ids.automatic)
    {
        checker.SetAutomatic(!checker.automatic());
        return UpdatesChoice::kAutomaticChanged;
    }
    return UpdatesChoice::kNone;
}
} // namespace reckless::update
