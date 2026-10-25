#!/usr/bin/env bash
# Set a Linux machine up for the audio layer's device suite (reckless_audio_device_test, CTest audio_devices): the
# step build.yml's linux job runs before it configures, and the line to run by hand before the suite on a desktop.
# Every Linux runner needs it - the job lands on whichever is free. A product's own device suite, made by
# reckless_audio_device_suite, needs the same: vst-plugins carries this script too.
#
# What the suite's two Linux APIs need, each checked by name and provisioned where the user's sudo needs no password:
#   - the headers the toolkit's ALSA and JACK back ends compile against (libasound2-dev, libjack-jackd2-dev): without them
#     the configure promises neither API, the build carries neither, and a run that requires them fails;
#   - jackd (jackd2): the suite starts a JACK server of its own on the dummy driver for each test (no hardware, under a
#     name no other server has, dying with the suite), so nothing here starts one. jackd2's package asks whether to
#     give its users real-time priority; DEBIAN_FRONTEND=noninteractive takes the default, no, and the suite's server
#     runs without it (--no-realtime);
#   - ALSA's loopback card (the kernel's snd-aloop module, which a runner's kernel has but does not load): playback on
#     its device 0 comes back as capture on its device 1, bit for bit. Loaded here, and left loaded - it stays until the
#     machine restarts, and a desktop that has one sees one more card. A card already there is left as it is;
#   - read and write on the card's nodes in /dev/snd for this user. They are root:audio 0660, and a runner's user may
#     not be in the audio group (reckless-ci-lin-0's `runner` is not): an ACL entry for the user on the loopback card's
#     own nodes - its control and its PCMs, nothing of another card - given with setfacl (the acl package), each run,
#     since the nodes are made afresh when the module loads;
#   - ALSA's sequencer for the MIDI suite: its module (snd-seq) loaded where it is not, and its node, /dev/snd/seq,
#     opened to the user the same way.
# Where a step needs a sudo that wants a password, the script stops with the command for a person to run.
#
# Usage, from anywhere:  tools/audio-test-devices.sh
set -euo pipefail

[ "$(uname -s)" = Linux ] || { echo "tools/audio-test-devices.sh sets up Linux's test devices (ALSA, JACK)" >&2; exit 1; }
user=$(id -un)
can_sudo=false
if sudo -n true 2> /dev/null; then can_sudo=true; fi

# A kernel built without sound at all can have no test device, whatever is installed: Ubuntu's azure kernels are
# built that way (Launchpad 1970586, "No sound support on Linux AWS/Azure kernels"), which is what GitHub's hosted
# runners run - zero sound modules in the tree and no CONFIG_SND_ALOOP in the build config. Nothing here can fix
# that, so the suites that need a device are left out and the job says so. This is a question about the kernel and
# not about the kind of runner: a machine whose kernel has sound and whose loopback card is missing still fails
# below, loudly, because there the provisioning is broken rather than impossible.
sound_modules=$(find "/lib/modules/$(uname -r)" -path '*sound*' -name '*.ko*' 2> /dev/null | wc -l)
if [ ! -d /proc/asound ] && [ "$sound_modules" -eq 0 ]; then
    echo "this kernel has no sound support: no /proc/asound, no sound modules, " \
        "CONFIG_SND_ALOOP $(grep -s SND_ALOOP "/boot/config-$(uname -r)" || echo absent)"
    echo "::notice::the audio device suites cannot run on this kernel and are left out; everything else runs"
    if [ -n "${GITHUB_ENV:-}" ]; then echo "RECKLESS_AUDIO_DEVICES=none" >> "$GITHUB_ENV"; fi
    # The headers all the same: compiling the toolkit's ALSA and JACK back ends needs no sound in the kernel, and
    # without them the configure promises neither API, so the back ends go uncompiled and a break in either is first
    # met by a product's build.
    headers=()
    pkg-config --exists alsa 2> /dev/null || headers+=(libasound2-dev)
    pkg-config --exists jack 2> /dev/null || headers+=(libjack-jackd2-dev)
    if [ ${#headers[@]} -gt 0 ]; then
        if ! $can_sudo; then
            echo "::error::the machine lacks ${headers[*]}: sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ${headers[*]}" >&2
            exit 1
        fi
        echo "installing ${headers[*]} with $user's passwordless sudo, for the back ends to compile"
        sudo -n apt-get update -qq
        sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${headers[@]}"
    fi
    exit 0
fi

# The packages: the two development headers, jackd and setfacl.
missing=()
pkg-config --exists alsa 2> /dev/null || missing+=(libasound2-dev)
pkg-config --exists jack 2> /dev/null || missing+=(libjack-jackd2-dev)
command -v jackd > /dev/null || missing+=(jackd2)
command -v setfacl > /dev/null || missing+=(acl)
if [ ${#missing[@]} -gt 0 ]; then
    if ! $can_sudo; then
        echo "::error::the machine lacks ${missing[*]}: sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ${missing[*]}" >&2
        exit 1
    fi
    echo "installing ${missing[*]} with $user's passwordless sudo"
    sudo -n apt-get update -qq
    sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${missing[@]}"
fi

# The loopback card, loaded if the kernel has none. A machine with no sound module loaded yet - a new runner, a
# headless machine - has no /proc/asound/cards until the module that is about to load makes it: none listed then, and
# awk's failure to open it is no failure of the script's (errexit would end it there, silently, before the load).
loopback_card() { awk '/\[Loopback *\]/ { print $1; exit }' /proc/asound/cards 2> /dev/null || true; }
card=$(loopback_card)
if [ -z "$card" ]; then
    if ! $can_sudo; then
        echo "::error::no ALSA loopback card: sudo modprobe snd-aloop" >&2
        exit 1
    fi
    # GitHub's hosted images run an Azure kernel whose base package carries no snd-aloop: the module is in
    # linux-modules-extra for that exact kernel, which the image does not install. A self-hosted machine has it
    # already and installs nothing here. Without this the step fails with "Module snd-aloop not found in
    # directory /lib/modules/<kernel>", which is what a public repository's hosted Linux job met the first time
    # this ran outside the organisation's own machines.
    if ! modinfo snd-aloop > /dev/null 2>&1; then
        extra="linux-modules-extra-$(uname -r)"
        echo "the kernel has no snd-aloop; installing $extra"
        sudo -n apt-get update -qq
        sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$extra" || true
        # The package's files land under /lib/modules/<kernel>, but modprobe reads modules.dep, which is only
        # rebuilt by depmod: without this it still reports the module as not found in a directory that now has it.
        sudo -n depmod -a || true
    fi
    if ! modinfo snd-aloop > /dev/null 2>&1; then
        # Printed as plain lines before the annotation: GitHub collapses a ::error:: to its first line, so a
        # multi-line payload inside one vanishes - and an empty search then looks exactly like a swallowed one.
        echo "after installing $extra and running depmod, modinfo still finds no snd-aloop. What is there:"
        echo "  kernels in /lib/modules: $(ls /lib/modules 2>/dev/null | tr '
' ' ')"
        echo "  anything matching aloop: $(find /lib/modules -name '*aloop*' 2>/dev/null | head -5 | tr '
' ' ')"
        echo "  the package's own files: $(dpkg -L "$extra" 2>/dev/null | grep -ci aloop) matching aloop"
        echo "  sound modules present:   $(find /lib/modules -path '*sound*' -name '*.ko*' 2>/dev/null | wc -l)"
        # The kernel's own build configuration, which settles it: a module the kernel was not built with cannot be
        # installed by any package. Launchpad 1970586 is this exact thing - snd-aloop is built for Ubuntu's GCP
        # kernels and not for its AWS and Azure ones.
        echo "  CONFIG_SND_ALOOP:        $(grep -s SND_ALOOP "/boot/config-$(uname -r)" || echo 'absent from this kernel config')"
        echo "::error::this kernel has no snd-aloop and $extra did not bring one (the lines above say what is there)"
        exit 1
    fi
    sudo -n modprobe snd-aloop
    command -v udevadm > /dev/null && sudo -n udevadm settle || true
    for _ in $(seq 50); do
        card=$(loopback_card)
        [ -n "$card" ] && [ -e "/dev/snd/controlC$card" ] && break
        sleep 0.1
    done
    [ -n "$card" ] || { echo "::error::snd-aloop loaded but no loopback card appeared in /proc/asound/cards" >&2; exit 1; }
fi

# ALSA's sequencer, which the MIDI suite's own port and the back end's input share: its module loaded where it is not
# (a machine whose desktop never asked for MIDI may not have it), and its node opened to this user beside the card's.
if [ ! -e /dev/snd/seq ]; then
    if ! $can_sudo; then
        echo "::error::no ALSA sequencer (/dev/snd/seq): sudo modprobe snd-seq" >&2
        exit 1
    fi
    sudo -n modprobe snd-seq
    command -v udevadm > /dev/null && sudo -n udevadm settle || true
    for _ in $(seq 50); do [ -e /dev/snd/seq ] && break; sleep 0.1; done
    [ -e /dev/snd/seq ] || { echo "::error::snd-seq loaded but /dev/snd/seq did not appear" >&2; exit 1; }
fi

# This user's read and write on the card's own nodes, and on the sequencer's.
nodes=(/dev/snd/controlC"$card" /dev/snd/pcmC"$card"D* /dev/snd/seq)
for node in "${nodes[@]}"; do
    [ -e "$node" ] || continue
    if [ ! -r "$node" ] || [ ! -w "$node" ]; then
        if ! $can_sudo; then
            echo "::error::$user cannot open $node: sudo setfacl -m u:$user:rw ${nodes[*]} (or add $user to the audio group)" >&2
            exit 1
        fi
        sudo -n setfacl -m "u:$user:rw" "$node"
    fi
done
for node in "${nodes[@]}"; do
    [ -r "$node" ] && [ -w "$node" ] || { echo "::error::$user still cannot open $node" >&2; exit 1; }
done

echo "ALSA: the loopback card is card $card ($(sed -n "s/^ *$card \[[^]]*\]: //p" /proc/asound/cards | head -1)), its nodes and the sequencer's open to $user: ${nodes[*]}"
echo "JACK: $(command -v jackd) ($(jackd --version 2> /dev/null | head -1 || echo 'version unknown')); the suite starts its own server"
echo "user: $user, groups: $(id -nG)"
