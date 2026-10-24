#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "Http.h"
#include "PresetFiles.h"
#include "ProcessEnvironment.h"
#include "UpdateCheck.h"

namespace reckless::update
{
/**
    A product's update check, described once: what it calls itself, which version it is, where the website publishes
    its newest release's word and where it keeps what it found. Every product on the toolkit makes its checker from one
    of these (MakeChecker), so the request, the file and the switch that turns checks off for a process are the same
    in every one of them:

        update::Product{"RecklessPlugins", core::SuiteVersion(), std::string(update::kManifestUrl), "updates.json"}
        update::Product{"RecklessIRCapture", ToolVersion(), "https://recklesstechnology.com/updates/ir-capture.json",
                        "ir-capture-updates.json"}

    Products released apart keep files apart, so one product's answer is never read as another's; the plugins share
    one file across every plugin on the machine.
*/
struct Product
{
    /** What the check's request names itself in its User-Agent, before the version: one word, no spaces
        ("RecklessPlugins", "RecklessIRCapture"). */
    std::string name;
    /** This build's version, "MAJOR.MINOR.BUILD"; a text that is not one reads as 0.0.0, which every release is newer
        than. */
    std::string version;
    /** The small static file the website writes from the newest public release (kManifestUrl for the plugins), which
        the check reads at most once a day. */
    std::string manifest_url;
    /** The file of the product's own the check keeps what it found in, and whether checks are automatic: its name in
        the suite's folder of the user's settings, `reckless-technology/<state_file>` ("updates.json"). */
    std::string state_file;
};

/** Where a product's update check keeps what it found: `reckless-technology/<file_name>` in the user's settings
    folder.
    @param file_name  the file's name (Product::state_file)
    @return           the path; empty where the platform names no settings folder */
inline std::filesystem::path StateFile(const std::string &file_name)
{
    const std::filesystem::path folder = core::UserSettingsFolder(core::kSuiteFolder);
    return folder.empty() ? folder : folder / file_name;
}

/** Whether this process may check at all: anything but RECKLESS_UPDATE_CHECK=off, or the variable not set - the CI
    runners set it, so no suite, validator or launch's check reaches the network.
    @return  false where the environment switches checks off */
inline bool AllowedByEnvironment()
{
    return core::EnvironmentVariable("RECKLESS_UPDATE_CHECK") != std::optional<std::string>("off");
}

/** The version a product's text names, as the checker compares it.
    @param text  "MAJOR.MINOR.BUILD"
    @return      the version; 0.0.0 where the text is not one */
inline Version VersionOf(const std::string &text)
{
    return ParseVersion(text).value_or(Version{});
}

/** The checker a product runs: its file, its version and its manifest, on the platform's own HTTP stack and the wall
    clock, allowed to check unless the environment switches checks off (AllowedByEnvironment) - then it is made all the
    same and never checks, by the clock or by hand. A product holds the one it makes for as long as its windows show:
    the plugins one for the whole process, every plugin's window reading it; an app one beside its window.
    @param product  the product
    @return         the checker, read from its file */
inline std::unique_ptr<Checker> MakeChecker(const Product &product)
{
    return std::make_unique<Checker>(
        StateFile(product.state_file), VersionOf(product.version), product.manifest_url,
        [] { return net::MakePlatformTransport(); }, Checker::SystemSeconds, AllowedByEnvironment(), product.name);
}
} // namespace reckless::update
