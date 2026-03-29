"""A/B test: AI classifier vs heuristic on the dataset samples.

Compares the content-type predictions of:
  A) Heuristic: spectral band energy + transient ratio (ported from C++ SmartSamplerEngine)
  B) AI:        OnnxInference + feature extraction (mirrors AiContentClassifier.cpp)

Usage:
    python scripts/ab_test_classifier.py [--dataset data/dataset] [--model models/content_classifier.onnx] [--n 50]

Output:
    Console log + docs/ab_test_classifier_results.txt
"""

import argparse
import math
import os
import struct
import random


# ── WAV reader ────────────────────────────────────────────────────────────────

def read_wav_mono(path: str) -> tuple[list[float], int]:
    """Read first channel of a WAV file as float in [-1, 1]."""
    with open(path, "rb") as f:
        assert f.read(4) == b"RIFF"
        f.read(4)
        assert f.read(4) == b"WAVE"
        sr, bits, channels = 44100, 16, 1
        samples = []
        while True:
            chunk_id = f.read(4)
            if len(chunk_id) < 4:
                break
            chunk_size = struct.unpack("<I", f.read(4))[0]
            if chunk_id == b"fmt ":
                data = f.read(chunk_size)
                channels = struct.unpack("<H", data[2:4])[0]
                sr       = struct.unpack("<I", data[4:8])[0]
                bits     = struct.unpack("<H", data[14:16])[0]
            elif chunk_id == b"data":
                bps = bits // 8
                n = chunk_size // (bps * channels)
                raw = f.read(chunk_size)
                for i in range(n):
                    idx = i * channels * bps
                    val = struct.unpack("<h", raw[idx:idx + 2])[0]
                    samples.append(val / 32768.0)
                break
            else:
                f.read(chunk_size)
    return samples, sr


# ── Heuristic classifier (ported from SmartSamplerEngine::detectContentType) ─

CLASSES = ["KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC", "OTHER"]


def heuristic_classify(pcm: list[float], sr: int) -> str:
    """Port of SmartSamplerEngine::detectContentType() in C++."""
    if not pcm:
        return "OTHER"

    # 1. Transient ratio: peak / RMS over first 20 ms
    attack_n = min(len(pcm), int(sr * 0.020))
    peak = 0.0
    sum_sq_att = 0.0
    for i in range(attack_n):
        abs_s = abs(pcm[i])
        if abs_s > peak:
            peak = abs_s
        sum_sq_att += pcm[i] * pcm[i]
    rms_att = math.sqrt(sum_sq_att / max(attack_n, 1))
    transient_ratio = peak / rms_att if rms_att > 0.001 else 1.0

    # 2. Duration in ms
    duration_ms = len(pcm) / sr * 1000.0

    # 3. Band energies via cascaded 1st-order LP
    def alpha(fc):
        t = 2 * math.pi * fc / sr
        return t / (t + 1)

    a150  = alpha(150)
    a500  = alpha(500)
    a3000 = alpha(3000)
    y150 = y500 = y3000 = 0.0
    e_sub = e_bass = e_mid = e_high = e_total = 0.0

    N = min(len(pcm), sr)  # max 1s
    for i in range(N):
        x = pcm[i]
        y150  = a150  * x + (1 - a150)  * y150
        y500  = a500  * x + (1 - a500)  * y500
        y3000 = a3000 * x + (1 - a3000) * y3000
        e_sub  += y150  * y150
        e_bass += (y500 - y150)  ** 2
        e_mid  += (y3000 - y500) ** 2
        e_high += (x - y3000)    ** 2
        e_total += x * x

    if e_total < 1e-8:
        return "OTHER"

    sub_frac  = e_sub  / e_total
    low_frac  = (e_sub + e_bass) / e_total
    mid_frac  = e_mid  / e_total
    high_frac = e_high / e_total

    if transient_ratio > 3.0 and sub_frac  > 0.28 and duration_ms < 600:
        return "KICK"
    if transient_ratio > 2.5 and high_frac > 0.40 and duration_ms < 400:
        return "HIHAT"
    if transient_ratio > 4.5 and high_frac > 0.30 and duration_ms < 500:
        return "HIHAT"
    if transient_ratio > 2.5 and mid_frac  > 0.28 and duration_ms < 700:
        return "SNARE"
    if transient_ratio < 2.0 and low_frac  > 0.50:
        return "BASS"
    if transient_ratio < 1.8 and duration_ms > 1500 and high_frac < 0.35:
        return "PAD"
    if transient_ratio > 2.5:
        return "PERC"
    if transient_ratio < 2.0 and mid_frac > 0.35:
        return "SYNTH"
    return "OTHER"


# ── AI classifier (mirrors AiContentClassifier.cpp) ──────────────────────────

def load_norm_params(norm_path: str) -> tuple[list[float], list[float]]:
    with open(norm_path, "rb") as f:
        raw = f.read(64 * 4)
        means = list(struct.unpack(f"<{64}f", raw))
        raw = f.read(64 * 4)
        stds = list(struct.unpack(f"<{64}f", raw))
    return means, stds


def pad_or_crop(pcm: list[float], target: int = 22050) -> list[float]:
    if len(pcm) >= target:
        return pcm[:target]
    return pcm + [0.0] * (target - len(pcm))


def extract_features(pcm: list[float]) -> list[float]:
    """Mirrors AiContentClassifier::extractFeatures() / train_classifier.py."""
    n = len(pcm)
    if n == 0:
        return [0.0] * 64

    peak = max(abs(s) for s in pcm)
    rms  = math.sqrt(sum(s * s for s in pcm) / n)
    crest = peak / rms if rms > 1e-8 else 1.0

    zcr = sum(1 for i in range(1, n) if pcm[i] * pcm[i - 1] < 0) / n

    peak_idx, peak_val = 0, 0.0
    for i in range(min(n, 4410)):
        if abs(pcm[i]) > peak_val:
            peak_val, peak_idx = abs(pcm[i]), i
    attack_ratio = peak_idx / 44100.0

    thresh = peak * 0.1
    above = sum(1 for s in pcm if abs(s) > thresh) / n

    def alpha(fc):
        t = 2 * math.pi * fc / 44100
        return t / (t + 1)

    bands = [150, 500, 1500, 4000, 8000]
    lp = [0.0] * len(bands)
    band_e = [0.0] * (len(bands) + 1)
    total_e = 0.0

    for s in pcm:
        prev = 0.0
        for b in range(len(bands)):
            a = alpha(bands[b])
            lp[b] = a * s + (1 - a) * lp[b]
            band_e[b] += (lp[b] - prev) ** 2
            prev = lp[b]
        band_e[-1] += (s - prev) ** 2
        total_e += s * s

    if total_e < 1e-10:
        total_e = 1e-10
    band_ratios = [e / total_e for e in band_e]

    seg_count, seg_size = 8, n // 8
    seg_rms = []
    for seg in range(seg_count):
        start = seg * seg_size
        end = start + seg_size
        seg_e = sum(pcm[i] ** 2 for i in range(start, min(end, n)))
        seg_rms.append(math.sqrt(seg_e / max(seg_size, 1)))

    features = [rms, peak, crest, zcr, attack_ratio, above] + band_ratios + seg_rms
    features = features[:64]
    while len(features) < 64:
        features.append(0.0)
    return features


def ai_classify(pcm: list[float], sr: int, session, means, stds) -> str:
    """Run ONNX inference and return class name."""
    import onnxruntime as ort
    import struct

    padded = pad_or_crop(pcm)
    features = extract_features(padded)
    features = [(features[i] - means[i]) / stds[i] for i in range(64)]

    input_name = session.get_inputs()[0].name
    import numpy as np
    inp = np.array(features, dtype=np.float32).reshape(1, 64)
    logits = session.run(None, {input_name: inp})[0][0]
    idx = int(max(range(len(logits)), key=lambda i: logits[i]))
    # Map to class name (7 classes: no OTHER in training)
    ai_classes = ["KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC"]
    return ai_classes[idx] if idx < len(ai_classes) else "OTHER"


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", default="data/dataset")
    parser.add_argument("--model",   default="models/content_classifier.onnx")
    parser.add_argument("--norm",    default="models/content_classifier_norm.bin")
    parser.add_argument("--n",       type=int, default=50,
                        help="Number of samples to test (0 = all)")
    parser.add_argument("--output",  default="docs/ab_test_classifier_results.txt")
    args = parser.parse_args()

    try:
        import onnxruntime as ort
        import numpy as np
    except ImportError:
        print("ERROR: onnxruntime not installed. Run: pip install onnxruntime numpy")
        return 1

    # Load model + norm
    session = ort.InferenceSession(args.model)
    means, stds = load_norm_params(args.norm)

    # Collect samples (label = parent directory name)
    samples_list = []
    for cls_name in ["KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC"]:
        cls_dir = os.path.join(args.dataset, cls_name)
        if not os.path.isdir(cls_dir):
            continue
        for fname in sorted(os.listdir(cls_dir)):
            if fname.endswith(".wav"):
                samples_list.append((os.path.join(cls_dir, fname), cls_name))

    if args.n > 0 and len(samples_list) > args.n:
        random.seed(42)
        samples_list = random.sample(samples_list, args.n)
        samples_list.sort()

    print(f"Testing {len(samples_list)} samples...")

    rows = []
    ai_correct = heuristic_correct = both_correct = 0

    for wav_path, ground_truth in samples_list:
        pcm, sr = read_wav_mono(wav_path)
        h_pred = heuristic_classify(pcm, sr)
        a_pred = ai_classify(pcm, sr, session, means, stds)

        h_ok = h_pred == ground_truth
        a_ok = a_pred == ground_truth
        if h_ok: heuristic_correct += 1
        if a_ok: ai_correct        += 1
        if h_ok and a_ok: both_correct += 1

        match_flag = ""
        if a_ok and not h_ok:   match_flag = "AI wins"
        elif h_ok and not a_ok: match_flag = "Heuristic wins"
        elif not a_ok and not h_ok: match_flag = "Both wrong"

        rows.append({
            "file":  os.path.basename(wav_path),
            "truth": ground_truth,
            "ai":    a_pred,
            "heur":  h_pred,
            "flag":  match_flag,
        })

    n = len(samples_list)
    ai_acc   = ai_correct   / n * 100
    heur_acc = heuristic_correct / n * 100

    # ── Report ────────────────────────────────────────────────────────────────
    lines = []
    lines.append("=" * 70)
    lines.append("A/B TEST: AI Classifier vs Heuristic")
    lines.append(f"Dataset : {args.dataset}  |  Samples tested: {n}")
    lines.append(f"Model   : {args.model}")
    lines.append("=" * 70)
    lines.append(f"{'File':<30} {'Truth':<7} {'AI':<7} {'Heur':<7} {'Note'}")
    lines.append("-" * 70)
    for r in rows:
        lines.append(f"{r['file']:<30} {r['truth']:<7} {r['ai']:<7} {r['heur']:<7} {r['flag']}")
    lines.append("=" * 70)
    lines.append(f"AI accuracy       : {ai_correct}/{n}  ({ai_acc:.1f}%)")
    lines.append(f"Heuristic accuracy: {heuristic_correct}/{n}  ({heur_acc:.1f}%)")
    lines.append(f"Both correct      : {both_correct}/{n}  ({both_correct/n*100:.1f}%)")
    verdict = "AI >= heuristic" if ai_correct >= heuristic_correct else "Heuristic > AI (needs retraining)"
    lines.append(f"Verdict           : {verdict}")
    lines.append("=" * 70)

    report = "\n".join(lines)
    print(report)

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w") as f:
        f.write(report + "\n")
    print(f"\nReport saved: {args.output}")

    return 0 if ai_correct >= heuristic_correct else 1


if __name__ == "__main__":
    raise SystemExit(main())
