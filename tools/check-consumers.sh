#!/usr/bin/env bash
# Every product built on the toolkit, configured on this checkout of it: vst-plugins, Reckless IR Capture, Reckless
# Amphitheater, Knect 4, Reckless Sudoku and Sentinel, each at the head of its default branch with this toolkit in place of the
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

# product | default branch | deploy key's variable | public or private | configure arguments | targets built |
# private dependencies
#
# A private dependency is a repository the product itself builds against and that its own checkout does not carry (for
# Sentinel, the reckless-arcade library). Each is named as
#   repository:secret variable:product's CMake variable:file that pins it:variable in that file
# and several are separated by semicolons. It is cloned into $work/deps/<product>/<repository> at the commit the
# product pins - read as the 40 hex digits of the `set(<variable> "...")` line in the file, from the product's own head,
# never the dependency's main - and handed over as -D<CMake variable>=<that directory>.
#
# **Each dependency is its own clone, in its own git call, with only its own key as the ssh identity.** Do not offer two
# deploy keys on one ssh command: GitHub authenticates a connection by the first key it recognises and then authorises it
# only against that key's repository, so the other repository answers "Repository not found" depending on the order of
# the keys. A dependency whose secret this run lacks skips its product, as a missing product key does.
#
# A public consumer is cloned over https and needs no key. A private one is read through its own read-only deploy
# key, whose private half is this repository's secret - and a fork's pull request is given no secrets, so from a
# fork it cannot be read at all and is skipped rather than failed (below).
products=(
    "vst-plugins|main|CONSUMER_VST_PLUGINS_DEPLOY_KEY|private|-DCMAKE_BUILD_TYPE=Release|reckless_update_shared reckless_plugin_view_test ir_capture_ui"
    "amphitheater-chess|main|CONSUMER_AMPHITHEATER_DEPLOY_KEY|public|-DCMAKE_BUILD_TYPE=Release -DAMPHITHEATER_BUILD_TESTS=ON|amphitheater_gui"
    "knect4|main|CONSUMER_KNECT4_DEPLOY_KEY|public|-DCMAKE_BUILD_TYPE=Release -DKNECT4_BUILD_TESTS=ON|knect4_gui"
    "sudoku|main|CONSUMER_SUDOKU_DEPLOY_KEY|public|-DCMAKE_BUILD_TYPE=Release -DSUDOKU_BUILD_TESTS=ON|sudoku_gui"
    "sentinel|main|CONSUMER_SENTINEL_DEPLOY_KEY|private|-DCMAKE_BUILD_TYPE=Release|Sentinel|reckless-arcade:ARCADE_READ_KEY:SENTINEL_ARCADE_DIR:cmake/arcade.cmake:SENTINEL_ARCADE_COMMIT"
)

key_dir=$(mktemp -d)
trap 'rm -rf "$key_dir"' EXIT

failed=()
summary=()
for entry in "${products[@]}"; do
    IFS='|' read -r product branch key_variable visibility configure targets dependencies <<< "$entry"
    # A private consumer with no key: from a fork's pull request, where GitHub passes no secrets. Skipped and
    # said so, rather than failed on a clone that cannot succeed; the public consumers below still run, so an
    # outside contributor's change is still held to three of the four.
    if [ "$visibility" = private ] && [ -z "${!key_variable:-}" ]; then
        echo "::notice::$product is private and this run has no key for it (a fork's pull request is given none);" \
            "skipped - the public consumers are still checked"
        summary+=("$product: skipped, no key for a private consumer")
        continue
    fi
    missing_dependency_key=
    if [ -n "$dependencies" ]; then
        IFS=';' read -r -a dependency_list <<< "$dependencies"
        for dependency in "${dependency_list[@]}"; do
            IFS=':' read -r _ dependency_key_variable _ <<< "$dependency"
            if [ -z "${!dependency_key_variable:-}" ]; then missing_dependency_key=$dependency_key_variable; fi
        done
    fi
    if [ -n "$missing_dependency_key" ]; then
        echo "::notice::$product needs $missing_dependency_key, which this run does not have; skipped"
        summary+=("$product: skipped, no key for a private dependency")
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
        # and Sentinel are private and keep one each.
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
        dependency_arguments=()
        if [ -n "$dependencies" ]; then
            IFS=';' read -r -a dependency_list <<< "$dependencies"
            for dependency in "${dependency_list[@]}"; do
                IFS=':' read -r dependency_repository dependency_key_variable dependency_cmake_variable                     dependency_pin_file dependency_pin_variable <<< "$dependency"
                pin=$(grep -Eo "set\($dependency_pin_variable +\"[0-9a-f]{40}\"" "$source_dir/$dependency_pin_file" |
                    grep -Eo '[0-9a-f]{40}' | head -n 1)
                if [ -z "$pin" ]; then
                    echo "::error::$product's $dependency_pin_file has no 40-digit commit on its set($dependency_pin_variable ...) line"
                    exit 1
                fi
                dependency_dir="$work/deps/$product/$dependency_repository"
                dependency_key="$key_dir/$product-$dependency_repository"
                (umask 077; printf '%s
' "${!dependency_key_variable}" > "$dependency_key")
                # Its own git calls, with this dependency's key alone (see the table's comment above).
                dependency_ssh="ssh -i $dependency_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new"
                if [ ! -d "$dependency_dir/.git" ]; then
                    rm -rf "$dependency_dir"
                    mkdir -p "$dependency_dir"
                    git -C "$dependency_dir" init --quiet
                    git -C "$dependency_dir" remote add origin "git@github.com:reckless-technology/$dependency_repository.git"
                fi
                if [ "$(git -C "$dependency_dir" rev-parse -q --verify HEAD 2> /dev/null || true)" != "$pin" ]; then
                    GIT_SSH_COMMAND="$dependency_ssh" git -C "$dependency_dir" fetch --quiet --depth 1 origin "$pin"
                    git -C "$dependency_dir" checkout --quiet --force --detach FETCH_HEAD
                fi
                echo "$dependency_repository at $(git -C "$dependency_dir" log -1 --format='%h %s'), the commit $product pins"
                dependency_arguments+=("-D$dependency_cmake_variable=$dependency_dir")
            done
        fi
        # shellcheck disable=SC2086 # the configure's arguments are words
        cmake -S "$source_dir" -B "$build_dir" -G Ninja $configure "${dependency_arguments[@]}" -DFETCHCONTENT_UPDATES_DISCONNECTED=ON \
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
