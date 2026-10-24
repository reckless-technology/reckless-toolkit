#!/usr/bin/env bash
# Set a Linux machine up for the audio layer's device suite (reckless_audio_device_test, CTest audio_devices): the
# step build.yml's linux job runs before it configures, and the line to run by hand before the suite on a desktop.
# Every Linux runner needs it - the job lands on whichever is free. A product's own device suite, made by
# reckless_audio_device_suite, needs the same: vst-plugins carries this script too.
#
# What the suite's two Linux APIs need, each checked by name and provisioned where the user's sudo needs no password:
#   - the headers RtAudio's ALSA and JACK back ends compile against (libasound2-dev, libjack-jackd2-dev): without them
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
#     since the nodes are made afresh when the module loads.
# Where a step needs a sudo that wants a password, the script stops with the command for a person to run.
#
# Usage, from anywhere:  tools/audio-test-devices.sh
set -euo pipefail

[ "$(uname -s)" = Linux ] || { echo "tools/audio-test-devices.sh sets up Linux's test devices (ALSA, JACK)" >&2; exit 1; }
user=$(id -un)
can_sudo=false
if sudo -n true 2> /dev/null; then can_sudo=true; fi

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
    sudo -n modprobe snd-aloop
    command -v udevadm > /dev/null && sudo -n udevadm settle || true
    for _ in $(seq 50); do
        card=$(loopback_card)
        [ -n "$card" ] && [ -e "/dev/snd/controlC$card" ] && break
        sleep 0.1
    done
    [ -n "$card" ] || { echo "::error::snd-aloop loaded but no loopback card appeared in /proc/asound/cards" >&2; exit 1; }
fi

# This user's read and write on the card's own nodes.
nodes=(/dev/snd/controlC"$card" /dev/snd/pcmC"$card"D*)
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

echo "ALSA: the loopback card is card $card ($(sed -n "s/^ *$card \[[^]]*\]: //p" /proc/asound/cards | head -1)), its nodes open to $user: ${nodes[*]}"
echo "JACK: $(command -v jackd) ($(jackd --version 2> /dev/null | head -1 || echo 'version unknown')); the suite starts its own server"
echo "user: $user, groups: $(id -nG)"
