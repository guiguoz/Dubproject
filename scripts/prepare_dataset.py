"""Prepare a labeled dataset of audio samples for content-type classification.

Generates synthetic samples for each category:
  KICK, SNARE, HIHAT, BASS, SYNTH, PAD, PERC

Each sample is a mono WAV at 44100 Hz, ~0.5s–2s.
Output structure:
    data/dataset/
        KICK/   (≥ 30 files)
        SNARE/  (≥ 30 files)
        HIHAT/  (≥ 30 files)
        BASS/   (≥ 30 files)
        SYNTH/  (≥ 30 files)
        PAD/    (≥ 30 files)
        PERC/   (≥ 30 files)

Usage:
    python scripts/prepare_dataset.py [--output data/dataset] [--count 30]
"""

import argparse
import os
import struct
import math
import random

SR = 44100

# ── WAV writer (no dependencies) ─────────────────────────────────────────────

def write_wav(path: str, samples: list[float], sr: int = SR):
    """Write mono 16-bit WAV."""
    n = len(samples)
    data_size = n * 2
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        for s in samples:
            clamped = max(-1.0, min(1.0, s))
            f.write(struct.pack("<h", int(clamped * 32767)))

# ── Helpers ───────────────────────────────────────────────────────────────────

def sine(freq, dur, amp=0.8, phase=0.0):
    n = int(SR * dur)
    return [amp * math.sin(2 * math.pi * freq * i / SR + phase) for i in range(n)]

def noise(dur, amp=0.5):
    n = int(SR * dur)
    return [amp * (random.random() * 2 - 1) for _ in range(n)]

def envelope(samples, attack_ms, decay_ms, sustain_level=0.0, release_ms=0):
    """Apply ADSR-like amplitude envelope."""
    n = len(samples)
    attack = int(SR * attack_ms / 1000)
    decay = int(SR * decay_ms / 1000)
    release = int(SR * release_ms / 1000)
    result = []
    for i in range(n):
        if i < attack:
            env = i / max(attack, 1)
        elif i < attack + decay:
            t = (i - attack) / max(decay, 1)
            env = 1.0 - t * (1.0 - sustain_level)
        elif i < n - release:
            env = sustain_level
        else:
            t = (i - (n - release)) / max(release, 1)
            env = sustain_level * (1.0 - t)
        result.append(samples[i] * env)
    return result

def lowpass_simple(samples, cutoff_hz):
    """Simple 1st-order RC lowpass."""
    rc = 1.0 / (2 * math.pi * cutoff_hz)
    dt = 1.0 / SR
    alpha = dt / (rc + dt)
    out = [samples[0]]
    for i in range(1, len(samples)):
        out.append(out[-1] + alpha * (samples[i] - out[-1]))
    return out

def mix(a, b, level_b=1.0):
    n = max(len(a), len(b))
    result = [0.0] * n
    for i in range(n):
        va = a[i] if i < len(a) else 0.0
        vb = b[i] if i < len(b) else 0.0
        result[i] = va + vb * level_b
    return result

def normalize(samples, target=0.8):
    peak = max(abs(s) for s in samples) or 1.0
    return [s * target / peak for s in samples]

# ── Generators per category ──────────────────────────────────────────────────

def gen_kick(variation):
    """Kick: sine sweep from ~150Hz down to ~40Hz, fast attack, short."""
    dur = 0.15 + random.random() * 0.15
    freq_start = 120 + random.random() * 80
    freq_end = 35 + random.random() * 20
    n = int(SR * dur)
    samples = []
    for i in range(n):
        t = i / n
        freq = freq_start + (freq_end - freq_start) * t
        samples.append(0.9 * math.sin(2 * math.pi * freq * i / SR))
    samples = envelope(samples, 1, int(dur * 800), 0.0)
    # Add sub-click
    click = noise(0.005, 0.3)
    click = lowpass_simple(click, 2000)
    samples = mix(samples, click)
    return normalize(samples)

def gen_snare(variation):
    """Snare: body tone + filtered noise burst."""
    dur = 0.2 + random.random() * 0.15
    body_freq = 180 + random.random() * 60
    body = sine(body_freq, dur, 0.6)
    body = envelope(body, 1, int(dur * 600), 0.0)
    n_noise = noise(dur, 0.7)
    n_noise = lowpass_simple(n_noise, 5000 + random.random() * 3000)
    n_noise = envelope(n_noise, 1, int(dur * 400), 0.0)
    return normalize(mix(body, n_noise))

def gen_hihat(variation):
    """Hi-hat: high-frequency noise burst, very short."""
    dur = 0.04 + random.random() * 0.12
    n_noise = noise(dur, 0.8)
    # Highpass via subtracting lowpass
    lp = lowpass_simple(n_noise, 4000)
    hp = [a - b for a, b in zip(n_noise, lp)]
    return normalize(envelope(hp, 0.5, int(dur * 500), 0.0))

def gen_bass(variation):
    """Bass: low sine or saw, sustained."""
    dur = 1.0 + random.random() * 1.0
    freq = 40 + random.random() * 60
    n = int(SR * dur)
    # Saw-ish wave
    samples = []
    for i in range(n):
        phase = (freq * i / SR) % 1.0
        saw = 2.0 * phase - 1.0
        sin_v = math.sin(2 * math.pi * freq * i / SR)
        blend = 0.3 + random.random() * 0.4
        samples.append(0.8 * (sin_v * blend + saw * (1 - blend)))
    samples = lowpass_simple(samples, 200 + random.random() * 200)
    return normalize(envelope(samples, 10, 50, 0.7, 100))

def gen_synth(variation):
    """Synth: mid-range tone, some harmonics."""
    dur = 0.8 + random.random() * 0.8
    freq = 200 + random.random() * 400
    n = int(SR * dur)
    samples = []
    for i in range(n):
        v = 0.6 * math.sin(2 * math.pi * freq * i / SR)
        v += 0.3 * math.sin(2 * math.pi * freq * 2 * i / SR)
        v += 0.1 * math.sin(2 * math.pi * freq * 3 * i / SR)
        samples.append(v)
    samples = envelope(samples, 20, 100, 0.6, 150)
    return normalize(samples)

def gen_pad(variation):
    """Pad: slow attack, long sustain, mellow."""
    dur = 2.0 + random.random() * 1.0
    freq = 150 + random.random() * 200
    n = int(SR * dur)
    samples = []
    for i in range(n):
        v = 0.5 * math.sin(2 * math.pi * freq * i / SR)
        v += 0.3 * math.sin(2 * math.pi * (freq * 1.01) * i / SR)  # detune
        v += 0.2 * math.sin(2 * math.pi * freq * 0.5 * i / SR)
        samples.append(v)
    samples = lowpass_simple(samples, 800 + random.random() * 500)
    return normalize(envelope(samples, 400, 200, 0.6, 500))

def gen_perc(variation):
    """Percussion: short transient, mid-range noise."""
    dur = 0.1 + random.random() * 0.2
    freq = 300 + random.random() * 400
    body = sine(freq, dur, 0.5)
    n_noise = noise(dur, 0.5)
    n_noise = lowpass_simple(n_noise, 3000 + random.random() * 2000)
    mixed = mix(body, n_noise)
    return normalize(envelope(mixed, 0.5, int(dur * 600), 0.0))

GENERATORS = {
    "KICK":  gen_kick,
    "SNARE": gen_snare,
    "HIHAT": gen_hihat,
    "BASS":  gen_bass,
    "SYNTH": gen_synth,
    "PAD":   gen_pad,
    "PERC":  gen_perc,
}

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Generate labeled audio dataset")
    parser.add_argument("--output", default="data/dataset", help="Output directory")
    parser.add_argument("--count", type=int, default=30, help="Samples per category")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    args = parser.parse_args()

    random.seed(args.seed)
    total = 0

    for category, gen_fn in GENERATORS.items():
        cat_dir = os.path.join(args.output, category)
        os.makedirs(cat_dir, exist_ok=True)

        for i in range(args.count):
            samples = gen_fn(i)
            path = os.path.join(cat_dir, f"{category.lower()}_{i:03d}.wav")
            write_wav(path, samples)
            total += 1

        print(f"  {category}: {args.count} files")

    print(f"\nTotal: {total} files in {args.output}/")

if __name__ == "__main__":
    main()
