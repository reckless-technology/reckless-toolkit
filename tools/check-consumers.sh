#!/usr/bin/env bash
# Every product built on the toolkit, configured on this checkout of it: vst-plugins, Reckless IR Capture, Reckless
# Amphitheater, Knect 4 and Reckless Sudoku, each at the head of its default branch with this toolkit in place of the
# commit it pins, configured as its own CI configures it and a few of its targets built. A toolkit change that clashes
# with a product - a target, a CTest or an option of the same name, an include a product's header shadows, a header or a
# function a product calls that moved - fails here, on the toolkit's pull request, rather than at the product's next
# submodule bump.
#
# Usage: tools/check-consumers.sh <work-dir> <fetch-dir> <python>
#   <work-dir>   where the products' clones (src/<product>) and their build trees (build/<product>) are kept between
#                runs, so a run rebuilds only what the change reaches
#   <fetch-dir>  FetchContent's downloads, a directory per product under it, kept between runs
#   <python>     a Python 3 with numpy and Pillow (assets/requirements.txt), which the products' configures look for
# Each product is cloned over SSH: with the private half of a read-only deploy key on it in the environment variable the
# table below names (CI's secrets), else with the person's own SSH identity. JOBS sets the build's parallelism (the
# machine's cores by default). Every product is tried whatever an earlier one did; the exit is 1 when any failed.
set -euo pipefail

if [ $# -ne 3 ]; then
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi
toolkit=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1" "$2"
work=$(cd "$1" && pwd)
fetch=$(cd "$2" && pwd)
python=$3
jobs=${JOBS:-$(nproc 2> /dev/null || sysctl -n hw.ncpu)}

# product | default branch | deploy key's variable | public or private | configure arguments | targets built
#
# A public consumer is cloned over https and needs no key. A private one is read through its own read-only deploy
# key, whose private half is this repository's secret - and a fork's pull request is given no secrets, so from a
# fork it cannot be read at all and is skipped rather than failed (below).
products=(
    "vst-plugins|main|CONSUMER_VST_PLUGINS_DEPLOY_KEY|private|-DCMAKE_BUILD_TYPE=Release|reckless_update_shared reckless_plugin_view_test ir_capture_ui"
    "amphitheater-chess|main|CONSUMER_AMPHITHEATER_DEPLOY_KEY|public|-DCMAKE_BUILD_TYPE=Release -DAMPHITHEATER_BUILD_TESTS=ON|amphitheater_gui"
    "knect4|main|CONSUMER_KNECT4_DEPLOY_KEY|public|-DCMAKE_BUILD_TYPE=Release -DKNECT4_BUILD_TESTS=ON|knect4_gui"
    "sudoku|main|CONSUMER_SUDOKU_DEPLOY_KEY|public|-DCMAKE_BUILD_TYPE=Release -DSUDOKU_BUILD_TESTS=ON|sudoku_gui"
)

key_dir=$(mktemp -d)
trap 'rm -rf "$key_dir"' EXIT

failed=()
summary=()
for entry in "${products[@]}"; do
    IFS='|' read -r product branch key_variable visibility configure targets <<< "$entry"
    # A private consumer with no key: from a fork's pull request, where GitHub passes no secrets. Skipped and
    # said so, rather than failed on a clone that cannot succeed; the public consumers below still run, so an
    # outside contributor's change is still held to three of the four.
    if [ "$visibility" = private ] && [ -z "${!key_variable:-}" ]; then
        echo "::notice::$product is private and this run has no key for it (a fork's pull request is given none);" \
            "skipped - the public consumers are still checked"
        summary+=("$product: skipped, no key for a private consumer")
        continue
    fi
    source_dir="$work/src/$product"
    build_dir="$work/build/$product"
    echo "::group::$product: the head of $branch, on this toolkit"
    # The product's steps in a subshell of their own, errexit on inside it: a subshell tested by || would run with
    # errexit off, so its status is read after it ends.
    set +e
    (
        set -euo pipefail
        # A private consumer is read through its own read-only deploy key, whose private half is this repository's
        # secret; a public one needs none and is cloned over https, which is what a checkout with no key can do.
        # Amphitheater, Knect 4 and Sudoku went public and GPL on 2026-10-24 and have no key any more; vst-plugins
        # is private and keeps one.
        key="${!key_variable:-}"
        if [ -n "$key" ]; then
            (umask 077; printf '%s\n' "$key" > "$key_dir/$product")
            export GIT_SSH_COMMAND="ssh -i $key_dir/$product -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"
            url="git@github.com:reckless-technology/$product.git"
        else
            url="https://github.com/reckless-technology/$product.git"
        fi
        if [ ! -d "$source_dir/.git" ]; then
            rm -rf "$source_dir"
            git clone --quiet --no-checkout "$url" "$source_dir"
        fi
        # The toolkit's place is this checkout, linked in where the product's submodule would be; the link goes before
        # the product's own files are checked out again, so git finds the submodule's empty directory as it left it.
        rm -f "$source_dir/external/reckless-toolkit" 2> /dev/null || true
        # Never into the submodule: the product's head moving its pin would have fetch reach for the toolkit's
        # repository in the clone, which is never initialised here, and stop ("Could not access submodule").
        git -C "$source_dir" fetch --quiet --force --tags --no-recurse-submodules origin "$branch"
        git -C "$source_dir" checkout --quiet --force --detach FETCH_HEAD
        git -C "$source_dir" clean -ffdxq
        rm -rf "$source_dir/external/reckless-toolkit"
        mkdir -p "$source_dir/external"
        ln -s "$toolkit" "$source_dir/external/reckless-toolkit"
        echo "$product at $(git -C "$source_dir" log -1 --format='%h %s'), pinning the toolkit at $(git -C "$source_dir" ls-tree HEAD external/reckless-toolkit | awk '{print substr($3, 1, 7)}')"
        echo "toolkit at $(git -C "$toolkit" log -1 --format='%h %s')"
        # shellcheck disable=SC2086 # the configure's arguments are words
        cmake -S "$source_dir" -B "$build_dir" -G Ninja $configure -DFETCHCONTENT_UPDATES_DISCONNECTED=ON \
            "-DFETCHCONTENT_BASE_DIR=$fetch/$product" "-DPython3_EXECUTABLE=$python"
        # shellcheck disable=SC2086 # the targets are words
        cmake --build "$build_dir" -j "$jobs" --target $targets
    )
    result=$?
    set -e
    status="ok"
    if [ $result -ne 0 ]; then status="FAILED"; fi
    echo "::endgroup::"
    summary+=("$product: $status")
    if [ "$status" != "ok" ]; then
        echo "::error::$product does not configure or build on this toolkit (its log is in the group above)"
        failed+=("$product")
    fi
done

printf '%s\n' "${summary[@]}"
if [ ${#failed[@]} -gt 0 ]; then
    echo "On this toolkit: ${failed[*]} broken. Change the toolkit so each still builds, or change the product first and" \
        "land it before this pull request." >&2
    exit 1
fi
echo "Every consumer configures and builds its targets on this toolkit."
