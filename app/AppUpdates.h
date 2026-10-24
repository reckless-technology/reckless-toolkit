#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Menu.h"
#include "MenuBar.h"
#include "ProcessEnvironment.h"
#include "UpdateCheck.h"
#include "UpdateProduct.h"
#include "UpdatesMenu.h"

namespace reckless::app
{
/** An environment variable's value, read safely on every runtime: core::EnvironmentVariable, by the name an app's code
    has always called it by.
    @param name  the variable's name
    @return      its value, empty when it is set to nothing, or none when it is not set */
inline std::optional<std::string> EnvironmentVariable(const char *name)
{
    return core::EnvironmentVariable(name);
}

/** The update check an app's window runs, on the platform's HTTP stack (update::MakeChecker): none where the
    environment says RECKLESS_UPDATE_CHECK=off (the CI runners do), so the window offers no update menu that could
    only say it has not checked. Its menu is the toolkit's (update/UpdatesMenu.h).
    @param product  the app: its name, its version, its manifest and its file
    @return         the checker, or null with checks switched off */
inline std::unique_ptr<update::Checker> MakeUpdateChecker(const update::Product &product)
{
    if (!update::AllowedByEnvironment())
    {
        return nullptr;
    }
    return update::MakeChecker(product);
}

/** The Updates menu as a menu of an app's menu bar (app/MenuBar.h): the toolkit's rows (update/UpdatesMenu.h) made the
    bar's entries - each row's text its title, a dim row disabled, a tick checked, a rule a separator - each item's
    action ChooseUpdatesItem on the checker, so no id of the app's is taken. The entries say what the checker said as
    they were made: an app sets its bar again whenever Checker::revision() moves, so the status line follows the check.
    @param checker   the update check, outliving the bar
    @param open_url  opens the newer release's page in the person's browser (window::OpenInBrowser); empty opens nothing
    @param chosen    told what each choice did, for an app that says so in its own words; may be empty
    @param title     the menu's title in the bar
    @return          the menu */
inline MenuEntry UpdatesMenuEntry(update::Checker &checker, std::function<void(const std::string &)> open_url,
                                  std::function<void(update::UpdatesChoice)> chosen = {}, std::string title = "Updates")
{
    const update::UpdatesMenuIds ids;
    std::vector<MenuEntry>       entries;
    for (const view::MenuItem &row : update::UpdatesMenuItems(checker, ids))
    {
        if (row.separator)
        {
            entries.push_back(MenuEntry::Separator());
            continue;
        }
        MenuEntry entry = MenuEntry::Item(row.text, 0);
        entry.enabled   = row.enabled;
        entry.checked   = row.ticked;
        if (row.id != 0)
        {
            entry.action = [&checker, ids, id = row.id, open_url, chosen] {
                const update::UpdatesChoice choice = update::ChooseUpdatesItem(checker, ids, id, open_url);
                if (chosen)
                {
                    chosen(choice);
                }
            };
        }
        entries.push_back(std::move(entry));
    }
    return MenuEntry::Menu(std::move(title), std::move(entries));
}
} // namespace reckless::app
