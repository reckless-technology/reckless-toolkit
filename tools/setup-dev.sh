#!/usr/bin/env bash
# Set up a Linux or macOS machine to build, test, debug and measure the Reckless Technology toolkit: the toolchain, the
# tools and the editor. Each component is checked first and installed only when it is missing or not the version the
# repo wants, so the script is safe to run again at any time. The tools go into the user's own profile (~/.local); the
# system packages come from apt (Debian, Ubuntu, Mint: sudo asks for a password) or Homebrew (macOS).
# tools/setup-dev.ps1 is the same for Windows. VS Code's "setup environment" and "check environment" tasks run the one
# for the platform.
#
# What it sets up:
#   - the compiler, CMake and Ninja (build-essential, cmake, ninja-build, gdb on Linux, with what the windowed parts
#     and the audio device layer link - FreeType, fontconfig, X11, ALSA and JACK's headers - and Xvfb; Xcode's
#     command-line tools and Homebrew's cmake and ninja on macOS)
#   - uv, Astral's tool installer, and with it clang-format and gcovr at the versions CI pins (build.yml)
#   - the repo's git hooks (core.hooksPath .githooks) and the VS Code extensions .vscode/extensions.json recommends
#
#   tools/setup-dev.sh                # set up
#   tools/setup-dev.sh --check        # report only; exit 1 if anything is missing
#   tools/setup-dev.sh --no-persist   # add nothing to the shell profile's PATH (a test run in a scratch HOME)
set -euo pipefail
cd "$(dirname "$0")/.."

check=0
persist=1
for arg in "$@"; do
    case "$arg" in
        --check) check=1 ;;
        --no-persist) persist=0 ;;
        -h | --help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------------------------------------------------------------
# Pins. The versions CI uses are read from where CI reads them, so the two cannot drift.
pin() { sed -nE "s/.*$2.*/\\1/p" "$1" | head -1; }
clang_format_version=$(pin .github/workflows/build.yml "clang-format==([0-9.]+)")
gcovr_version=$(pin .github/workflows/build.yml 'gcovr==([0-9.]+)')
for value in "$clang_format_version" "$gcovr_version"; do
    [ -n "$value" ] || { echo "a pin is missing from .github/workflows/build.yml" >&2; exit 1; }
done
# Astral's installer for this uv release, which carries the hash of every uv archive it may fetch.
uv_version=0.12.7
uv_installer_sha256=92e8554321e2bde08c9b1445dae47a65360f885274f31df51cdc2f9faa84e001

# ---------------------------------------------------------------------------------------------------------------------
# Helpers (the same in every reckless-technology repo's setup script).
missing=0
ok() { printf '  \033[32mok\033[0m       %-24s %s\n' "$1" "$2"; }
miss() { missing=$((missing + 1)); printf '  \033[33mmissing\033[0m  %-24s %s\n' "$1" "$2"; }
installing() { printf '  \033[36minstall\033[0m  %-24s %s\n' "$1" "$2"; }
have() { command -v "$1" >/dev/null 2>&1; }
os=$(uname -s)

# Put a directory on this shell's PATH and, unless --no-persist, on every new shell's (the profiles a login shell,
# bash and zsh read).
add_to_path() {
    case ":$PATH:" in *":$1:"*) ;; *) PATH="$1:$PATH"; export PATH ;; esac
    [ "$persist" = 1 ] && [ "$check" = 0 ] || return 0
    local line="export PATH=\"$1:\$PATH\""
    local profiles=("$HOME/.profile" "$HOME/.bashrc")
    [ "$os" = Darwin ] && profiles+=("$HOME/.zprofile")
    for f in "${profiles[@]}"; do
        grep -qxF "$line" "$f" 2>/dev/null || printf '\n%s\n' "$line" >>"$f"
    done
}

sha256_of() { if have sha256sum; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# Download a file and refuse it unless its SHA-256 is the one given.
fetch_verified() {
    curl -fsSL "$1" -o "$3"
    local got
    got=$(sha256_of "$3")
    if [ "$got" != "$2" ]; then rm -f "$3"; echo "SHA-256 mismatch for $1 (got $got, want $2)" >&2; exit 1; fi
}

sudo_cmd() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }

# apt packages that are not installed yet, installed together (Debian, Ubuntu, Mint).
apt_packages() {
    local need=()
    for p in "$@"; do
        grep -q 'install ok installed' <<<"$(dpkg-query -W -f='${Status}' "$p" 2>/dev/null || true)" || need+=("$p")
    done
    if [ ${#need[@]} = 0 ]; then ok 'apt packages' "$*"; return; fi
    if [ "$check" = 1 ]; then miss 'apt packages' "${need[*]}"; return; fi
    installing 'apt packages' "${need[*]}"
    sudo_cmd apt-get update -q
    sudo_cmd env DEBIAN_FRONTEND=noninteractive apt-get install -y -q "${need[@]}"
}

# Homebrew formulae that are not installed yet.
brew_packages() {
    local need=()
    for p in "$@"; do brew list --formula "$p" >/dev/null 2>&1 || need+=("$p"); done
    if [ ${#need[@]} = 0 ]; then ok 'Homebrew packages' "$*"; return; fi
    if [ "$check" = 1 ]; then miss 'Homebrew packages' "${need[*]}"; return; fi
    installing 'Homebrew packages' "${need[*]}"
    brew install "${need[@]}"
}

# macOS: Xcode's command-line tools (the compilers, git) and Homebrew.
macos_base() {
    if xcode-select -p >/dev/null 2>&1; then ok 'Xcode command-line tools' "$(xcode-select -p)"
    elif [ "$check" = 1 ]; then miss 'Xcode command-line tools' 'xcode-select --install'
    else
        installing 'Xcode command-line tools' 'a dialog opens: install, then run this script again'
        xcode-select --install || true
        exit 0
    fi
    if ! have brew; then
        for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && eval "$("$b" shellenv)" && break; done
    fi
    if have brew; then ok 'Homebrew' "$(brew --version | head -1)"
    elif [ "$check" = 1 ]; then miss 'Homebrew' 'https://brew.sh'
    else
        installing 'Homebrew' 'the official installer (asks for your password)'
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && eval "$("$b" shellenv)" && break; done
    fi
}

linux_base() {
    if have apt-get; then apt_packages "$@"
    else
        echo "  This is not a Debian-family Linux: install the equivalents of these packages, then run again: $*" >&2
        if [ "$check" = 1 ]; then missing=$((missing + 1)); fi
    fi
}

set_git_hooks() {
    [ -d .githooks ] || return 0
    if [ "$(git config core.hooksPath || true)" = .githooks ]; then ok 'git hooks' 'core.hooksPath .githooks'
    elif [ "$check" = 1 ]; then miss 'git hooks' 'core.hooksPath is not .githooks'
    else git config core.hooksPath .githooks; installing 'git hooks' 'core.hooksPath .githooks'; fi
}

# The extensions .vscode/extensions.json recommends, when VS Code's command line is on the PATH.
install_vscode_extensions() {
    have code && [ -f .vscode/extensions.json ] || return 0
    local installed
    installed=$(code --list-extensions 2>/dev/null || true)
    for ext in $(sed -e 's://.*$::' .vscode/extensions.json | tr -d '\n' | sed -e 's/.*"recommendations"[^[]*\[//' -e 's/\].*//' | tr -d '" ' | tr ',' ' '); do
        if grep -qixF "$ext" <<<"$installed"; then ok 'VS Code extension' "$ext"
        elif [ "$check" = 1 ]; then miss 'VS Code extension' "$ext"
        else installing 'VS Code extension' "$ext"; code --install-extension "$ext" --force >/dev/null; fi
    done
}

# The first line a program prints for its version flag, or nothing.
tool_version() { have "$1" && { "$1" "${2:---version}" 2>&1 | head -1; } || true; }

# Whether a version line names the version wanted: exactly, or with "at-least" that or later.
version_ok() {
    local found
    found=$(printf '%s\n' "$1" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)
    [ -n "$found" ] || return 1
    if [ "$3" = exact ]; then [ "$found" = "$2" ]; return; fi
    [ "$(printf '%s\n%s\n' "$2" "$found" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$2" ]
}

# A uv tool at a version (exact, or at-least), its executable the first of its name on the PATH: installed, or
# reinstalled at the version, with uv tool install into ~/.local/bin.
uv_tool() {
    local name=$1 version=$2 mode=$3 why=$4 line want
    want=$version; [ "$mode" = exact ] || want="$version or later"
    line=$(tool_version "$name")
    if version_ok "$line" "$version" "$mode"; then ok "$name" "$line ($(command -v "$name"))"; return; fi
    local found="none on the PATH"
    [ -z "$line" ] || found="found $line at $(command -v "$name")"
    if [ "$check" = 1 ] || ! have uv; then miss "$name" "$want, $why ($found)"; return; fi
    installing "$name" "$version through uv tool install"
    uv tool install --force "$name==$version"
    line=$(tool_version "$name")
    if ! version_ok "$line" "$version" "$mode"; then
        miss "$name" "$version is installed in $local_bin, but $(command -v "$name") comes first on the PATH: move it after"
    fi
}

# ---------------------------------------------------------------------------------------------------------------------
echo "Reckless Technology toolkit: $([ "$check" = 1 ] && echo checking || echo 'setting up') the $os development environment"

local_bin="$HOME/.local/bin"
case "$os" in
    # The compiler and the build, then what the windowed parts link (FreeType, fontconfig, X11, found by pkg-config),
    # the audio device layer's ALSA and JACK headers, Xvfb for the window suites on a display, and Python for the
    # documents' check.
    Linux)
        linux_base build-essential cmake ninja-build pkg-config gdb curl git python3 libfreetype-dev \
            libfontconfig1-dev libx11-dev libasound2-dev libjack-jackd2-dev xvfb
        ;;
    Darwin) macos_base; have brew && brew_packages cmake ninja ;;
    *) echo "unsupported system: $os (tools/setup-dev.ps1 is the Windows script)" >&2; exit 2 ;;
esac

# CMake 3.25 or later, the presets' floor.
cmake_line=$(tool_version cmake)
if version_ok "$cmake_line" 3.25 at-least; then ok 'CMake' "$cmake_line"; else miss 'CMake' "3.25 or later (found: ${cmake_line:-none})"; fi

# uv, which installs clang-format and gcovr. Its executables go to ~/.local/bin, spelled out from HOME so a scratch HOME
# is honoured.
export UV_TOOL_BIN_DIR="$local_bin"
[ -x "$local_bin/uv" ] && add_to_path "$local_bin"
if have uv; then ok 'uv' "$(uv --version) ($(command -v uv))"
elif [ "$check" = 1 ]; then miss 'uv' 'which installs clang-format and gcovr'
else
    installing 'uv' "Astral's installer for uv $uv_version"
    scratch=$(mktemp -d)
    fetch_verified "https://astral.sh/uv/$uv_version/install.sh" "$uv_installer_sha256" "$scratch/uv-installer.sh"
    UV_INSTALL_DIR="$local_bin" UV_NO_MODIFY_PATH=1 sh "$scratch/uv-installer.sh"
    rm -rf "$scratch"
    add_to_path "$local_bin"
fi

# clang-format exactly at CI's version (build.yml): another release restyles code this one leaves alone (Homebrew's and
# apt's among them). gcovr at CI's version or later, whose reports agree: a machine with a newer one keeps it.
uv_tool clang-format "$clang_format_version" exact 'which the pre-commit hook and CI format with'
uv_tool gcovr "$gcovr_version" at-least "CI's coverage report"

set_git_hooks
install_vscode_extensions

if [ "$check" = 1 ]; then
    if [ "$missing" -gt 0 ]; then echo "$missing missing: run tools/setup-dev.sh (VS Code: the 'setup environment' task)."; exit 1; fi
    echo 'Everything is in place.'
elif [ "$missing" -gt 0 ]; then
    echo "$missing still missing (above)."
    exit 1
else
    echo 'Done. Open a new terminal (or restart VS Code) for PATH changes to reach it, then:'
    echo 'cmake --preset release && cmake --build --preset release && ctest --preset release'
fi
