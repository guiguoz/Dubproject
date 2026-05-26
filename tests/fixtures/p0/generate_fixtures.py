#!/usr/bin/env python3
"""Generate P0 QA WAV fixtures (44100 Hz, 16-bit PCM)."""
from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

SR = 44100
N = 4096
FREQ = 440.0
AMP_L = 0.8
AMP_R = 0.0  # stereo_lr: silent right
AMP_FULL = 0.6  # stereo_full: both sides in phase


def sine_samples(n: int, amp: float, phase: float = 0.0) -> list[float]:
    out = []
    for i in range(n):
        t = i / SR
        out.append(amp * math.sin(2.0 * math.pi * FREQ * t + phase))
    return out


def write_wav(path: Path, channels: list[list[float]]) -> None:
    n = len(channels[0])
    assert all(len(c) == n for c in channels)
    num_ch = len(channels)
    interleaved = []
    for i in range(n):
        for ch in range(num_ch):
            s = max(-1.0, min(1.0, channels[ch][i]))
            interleaved.append(int(s * 32767.0))

    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(num_ch)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack(f"<{len(interleaved)}h", *interleaved))


def main() -> None:
    root = Path(__file__).resolve().parent
    mono = sine_samples(N, AMP_L)
    write_wav(root / "mono.wav", [mono])

    left = sine_samples(N, AMP_L)
    right = [0.0] * N
    write_wav(root / "stereo_lr.wav", [left, right])

    full_l = sine_samples(N, AMP_FULL)
    full_r = sine_samples(N, AMP_FULL)
    write_wav(root / "stereo_full.wav", [full_l, full_r])

    print(f"Wrote fixtures to {root}")


if __name__ == "__main__":
    main()
