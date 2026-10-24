#!/usr/bin/env python3
"""The MP3 files the audio-file suites decode, made from synthetic signals by the LAME encoder.

The MP3s are carried, as the FLAC and Ogg Vorbis files beside them are: five short files, each a different shape of
stream for the suite's reader (core/AudioFiles.h, on dr_mp3), which tests/test_audio_files.cpp holds to
what was encoded - each tagged file trimmed to the signal's own length, its onset at the start.

    cabinet_mono_44k1_cbr128.mp3       MPEG-1 layer III, 128 kbps, with the encoder's Info frame and LAME tag: the
                                       frame count, the encoder's delay and its padding, 0.25 s
    room_stereo_48k_vbr.mp3            MPEG-1, variable bitrate, joint stereo, with a Xing frame and LAME tag, 0.75 s
    room_stereo_44k1_cbr128_tags.mp3   no Info frame, an ID3v2 tag before the audio and an ID3v1 tag after it: the
                                       frames counted by walking them, the delay kept, the tags skipped
    cabinet_mono_22k05_lsf.mp3         MPEG-2, 576 samples a frame, 0.5 s
    chords_mono_32k_v5.mp3             tones with sharp onsets, variable bitrate: short blocks at every onset, 2.6 s

The signals are noise shaped like responses - an onset, a few damped resonances, an exponential decay - from a
fixed seed, and for the last file chords struck every quarter second, so the files are the same each time the
script runs with the same encoder. They are the suite's own;
the encoder's licence does not reach what it encodes.

    python3 make_mp3_fixtures.py <path to lame>      writes the five .mp3 files beside this script
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent


def response(rate, seconds, channels, seed, decay_seconds):
    """Noise under an exponential decay, a resonance or two, one channel per column, peak 0.9."""
    rng = np.random.default_rng(seed)
    n = int(round(rate * seconds))
    t = np.arange(n) / rate
    out = np.zeros((n, channels))
    for c in range(channels):
        noise = rng.standard_normal(n)
        # a one-pole lowpass at about 4 kHz, which takes the edge off the noise as a cabinet would
        a = np.exp(-2 * np.pi * 4000.0 / rate)
        shaped = np.zeros(n)
        state = 0.0
        for i in range(n):
            state = (1 - a) * noise[i] + a * state
            shaped[i] = state
        ring = sum(np.sin(2 * np.pi * f * t + rng.uniform(0, 2 * np.pi)) * np.exp(-t / (0.3 * decay_seconds))
                   for f in (110.0 * (c + 1), 740.0, 2300.0))
        out[:, c] = (shaped * 3.0 + 0.4 * ring) * np.exp(-t / decay_seconds)
    return 0.9 * out / np.max(np.abs(out))


def chords(rate, seconds, channels, seed, decay_seconds):
    """Four tones struck together every quarter second, the root stepping up a semitone each time, peak 0.95."""
    n = int(round(rate * seconds))
    t = np.arange(n) / rate
    beat = int(rate / 4)
    out = np.zeros((n, channels))
    for c in range(channels):
        for start in range(0, n, beat):
            part = slice(start, min(n, start + beat))
            since = t[part] - t[start]
            root = 110 * 2 ** (((start // beat) % 7) / 12)
            envelope = np.exp(-since * 5) * np.minimum(1, since * 300)
            out[part, c] = sum(np.sin(2 * np.pi * root * r * t[part] + c) / r for r in (1, 1.26, 1.5, 2.0)) * envelope
    return 0.95 * out / np.max(np.abs(out))


def write_wav16(path, rate, samples):
    pcm = np.clip(np.round(samples * 32767.0), -32768, 32767).astype("<i2")
    channels = samples.shape[1]
    data = pcm.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * 2 * channels, 2 * channels, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


FIXTURES = [
    ("cabinet_mono_44k1_cbr128.mp3", 44100, 0.25, 1, 11, 0.05, ["-b", "128", "--cbr", "-m", "m"]),
    ("room_stereo_48k_vbr.mp3", 48000, 0.75, 2, 12, 0.25, ["-V", "2", "-m", "j"]),
    ("room_stereo_44k1_cbr128_tags.mp3", 44100, 0.5, 2, 13, 0.15,
     ["-b", "128", "--cbr", "-m", "s", "-t", "--add-id3v2", "--tt", "fixture", "--ta", "suite"]),
    ("cabinet_mono_22k05_lsf.mp3", 22050, 0.5, 1, 14, 0.08, ["-b", "64", "--cbr", "-m", "m"]),
    ("chords_mono_32k_v5.mp3", 32000, 2.6, 1, 0, 0.0, ["-V", "5", "-m", "m"]),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lame = sys.argv[1]
    with tempfile.TemporaryDirectory() as scratch:
        for name, rate, seconds, channels, seed, decay, options in FIXTURES:
            wav = Path(scratch) / (name + ".wav")
            signal = chords if name.startswith("chords") else response
            write_wav16(wav, rate, signal(rate, seconds, channels, seed, decay))
            subprocess.run([lame, "--silent", "--noreplaygain", *options, str(wav), str(HERE / name)], check=True)
            print(f"{name}: {(HERE / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
