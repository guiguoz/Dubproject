"""Train an AI mix model: 8-slot audio features → per-slot EQ gains + volume.

Architecture  : MLP 48 → 64 → 32 → 32   (pure Python, no PyTorch)
Training data : synthetic 8-slot mixes derived from heuristic rules
Output        : models/mix_model.onnx + models/mix_model_norm.bin

Input tensor  : float32 [1, 48]  (8 slots × 6 features, zeros for empty slots)
Output tensor : float32 [1, 32]  (8 slots × 4 values: volume, lowGain, midGain, highGain)

EQ gains are in dB; volume is linear [0, 1].  Empty slots output all zeros.

Usage:
    python scripts/train_mix_model.py [--samples 6000] [--epochs 100]
"""

import argparse
import math
import os
import random
import struct

# ─────────────────────────────────────────────────────────────────────────────
# Feature layout
# ─────────────────────────────────────────────────────────────────────────────
# 6 features per slot (must match FeatureExtractor::extract() output order):
#   idx 0 : rms
#   idx 1 : spectralCentroid  (Hz — will be z-score normalised)
#   idx 2 : crestFactor
#   idx 3 : lowFrac
#   idx 4 : midFrac
#   idx 5 : highFrac
N_SLOTS       = 8
N_FEAT_SLOT   = 6
N_INPUT       = N_SLOTS * N_FEAT_SLOT   # 48

# 4 targets per slot:
#   idx 0 : volume      (linear, 0–1)
#   idx 1 : lowGain     (dB)
#   idx 2 : midGain     (dB)
#   idx 3 : highGain    (dB)
N_TARGET_SLOT = 4
N_OUTPUT      = N_SLOTS * N_TARGET_SLOT  # 32

CLASSES = ["KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC", "OTHER"]

# ─────────────────────────────────────────────────────────────────────────────
# Per-type feature distributions (mu, sigma) for synthetic data generation
# Order: rms, centroid(Hz), crest, lowFrac, midFrac, highFrac
# ─────────────────────────────────────────────────────────────────────────────
TYPE_FEATURE_STATS = {
    "KICK":  [(0.30, 0.07), (150,   50), (5.0, 1.5), (0.50, 0.10), (0.30, 0.08), (0.20, 0.07)],
    "SNARE": [(0.25, 0.07), (1200, 350), (4.0, 1.0), (0.20, 0.07), (0.45, 0.09), (0.35, 0.09)],
    "HIHAT": [(0.15, 0.05), (7000,1500), (3.5, 0.8), (0.05, 0.03), (0.15, 0.05), (0.80, 0.09)],
    "BASS":  [(0.35, 0.08), (150,   50), (2.0, 0.4), (0.65, 0.10), (0.25, 0.07), (0.10, 0.05)],
    "SYNTH": [(0.28, 0.07), (800,  300), (1.7, 0.4), (0.25, 0.08), (0.50, 0.09), (0.25, 0.08)],
    "PAD":   [(0.22, 0.06), (900,  400), (1.5, 0.3), (0.30, 0.08), (0.45, 0.09), (0.25, 0.08)],
    "PERC":  [(0.20, 0.06), (3000,1000), (4.0, 1.0), (0.15, 0.05), (0.35, 0.09), (0.50, 0.10)],
    "OTHER": [(0.20, 0.07), (1000, 500), (2.0, 0.5), (0.33, 0.10), (0.34, 0.09), (0.33, 0.09)],
}

# Base EQ (volume, lowGain_dB, midGain_dB, highGain_dB)
# Approximates applyRoleEQ() effect on the three bands.
BASE_EQ = {
    "KICK":  (0.75,  2.0, -1.5,  1.5),
    "SNARE": (0.60, -1.0,  1.5,  2.5),
    "HIHAT": (0.30, -3.0,  0.0,  2.0),
    "BASS":  (0.55,  2.0, -1.0, -1.5),
    "SYNTH": (0.45, -1.0, -0.5,  0.0),
    "PAD":   (0.38, -1.0, -1.0,  0.0),
    "PERC":  (0.55, -1.0,  0.0,  1.0),
    "OTHER": (0.50, -1.0,  0.0,  0.0),
}


def _clamp_pos(x: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, x))


def _gauss_clamped(mu: float, sigma: float, lo: float, hi: float) -> float:
    return _clamp_pos(random.gauss(mu, sigma), lo, hi)


def sample_slot_features(ctype: str) -> list:
    """Sample realistic audio features for a given content type."""
    stats = TYPE_FEATURE_STATS[ctype]
    rms      = _gauss_clamped(stats[0][0], stats[0][1], 0.01, 1.0)
    centroid = _gauss_clamped(stats[1][0], stats[1][1], 20.0, 22000.0)
    crest    = _gauss_clamped(stats[2][0], stats[2][1], 1.0, 12.0)
    low_f    = _gauss_clamped(stats[3][0], stats[3][1], 0.0, 1.0)
    mid_f    = _gauss_clamped(stats[4][0], stats[4][1], 0.0, 1.0)
    high_f   = _gauss_clamped(stats[5][0], stats[5][1], 0.0, 1.0)
    # Normalise band fracs to sum ≈ 1
    total = low_f + mid_f + high_f
    if total > 1e-4:
        low_f /= total; mid_f /= total; high_f /= total
    return [rms, centroid, crest, low_f, mid_f, high_f]


def compute_targets(slot_types: list) -> list:
    """Compute per-slot (volume, lowGain, midGain, highGain) from heuristic rules.

    Mirrors applyRoleEQ + applyUnmasking + targetGainForType logic.
    """
    targets = []
    kick_present = "KICK" in slot_types
    bass_indices = [i for i, t in enumerate(slot_types) if t == "BASS"]
    perc_types   = {"KICK", "SNARE", "PERC"}
    perc_indices = [i for i, t in enumerate(slot_types) if t in perc_types]

    for i, ctype in enumerate(slot_types):
        if ctype == "":   # empty slot
            targets.extend([0.0, 0.0, 0.0, 0.0])
            continue

        vol, lg, mg, hg = BASE_EQ[ctype]

        # applyUnmasking rules --------------------------------------------------

        # Rule 1: KICK present → duck sub of BASS/SYNTH
        if kick_present and ctype == "BASS":
            lg -= 3.0
        if kick_present and ctype == "SYNTH":
            lg -= 1.0

        # Rule 2: 2+ BASS → secondary BASS gets extra cuts
        if ctype == "BASS" and len(bass_indices) >= 2:
            if bass_indices.index(i) > 0:  # secondary
                lg -= 2.0
                mg -= 1.0

        # Rule 3: KICK + SNARE overlap → SNARE cut at 300 Hz (mid band)
        if kick_present and ctype == "SNARE":
            mg -= 2.0

        # Rule 4: 3+ percussive → secondary ones cut presence
        if ctype in {"PERC", "SNARE"} and len(perc_indices) >= 3:
            if perc_indices.index(i) > 0:   # secondary
                mg -= 2.0

        targets.extend([vol, lg, mg, hg])
    return targets


# ─────────────────────────────────────────────────────────────────────────────
# Synthetic dataset generation
# ─────────────────────────────────────────────────────────────────────────────

# Probability of a slot being empty (not loaded)
EMPTY_PROB = 0.25


def generate_dataset(n_samples: int) -> tuple:
    """Return (X, y) as lists of length n_samples."""
    X, y = [], []
    for _ in range(n_samples):
        # Random 8-slot configuration
        slot_types = []
        for _ in range(N_SLOTS):
            if random.random() < EMPTY_PROB:
                slot_types.append("")  # empty
            else:
                slot_types.append(random.choice(CLASSES))

        # Build feature vector (zeros for empty slots)
        feats = []
        for ctype in slot_types:
            if ctype == "":
                feats.extend([0.0] * N_FEAT_SLOT)
            else:
                feats.extend(sample_slot_features(ctype))

        targets = compute_targets(slot_types)

        assert len(feats) == N_INPUT
        assert len(targets) == N_OUTPUT

        X.append(feats)
        y.append(targets)
    return X, y


# ─────────────────────────────────────────────────────────────────────────────
# Normalisation helpers
# ─────────────────────────────────────────────────────────────────────────────

def compute_norm_params(X: list, n: int) -> tuple:
    """Z-score mean/std per feature."""
    means = [0.0] * n
    stds  = [1.0] * n
    for f in range(n):
        vals  = [X[i][f] for i in range(len(X))]
        means[f] = sum(vals) / len(vals)
        var   = sum((v - means[f]) ** 2 for v in vals) / len(vals)
        stds[f]  = math.sqrt(var) if var > 1e-10 else 1.0
    return means, stds


def apply_norm(X: list, means: list, stds: list) -> list:
    return [[(X[i][f] - means[f]) / stds[f] for f in range(len(means))]
            for i in range(len(X))]


# ─────────────────────────────────────────────────────────────────────────────
# Regression MLP — pure Python SGD, MSE loss
# ─────────────────────────────────────────────────────────────────────────────

def relu(x: float) -> float:
    return x if x > 0.0 else 0.0


class RegressionMLP:
    """3-layer MLP: input → hidden1 → hidden2 → output (regression)."""

    def __init__(self, n_in: int, n_h1: int, n_h2: int, n_out: int):
        self.n_in  = n_in
        self.n_h1  = n_h1
        self.n_h2  = n_h2
        self.n_out = n_out

        def xavier(fan_in, fan_out):
            s = math.sqrt(2.0 / fan_in)
            return [[random.gauss(0, s) for _ in range(fan_in)]
                    for _ in range(fan_out)]

        self.w1 = xavier(n_in,  n_h1)
        self.b1 = [0.0] * n_h1
        self.w2 = xavier(n_h1,  n_h2)
        self.b2 = [0.0] * n_h2
        self.w3 = xavier(n_h2, n_out)
        self.b3 = [0.0] * n_out

    # ── Forward ────────────────────────────────────────────────────────────────

    def forward(self, x: list) -> tuple:
        """Returns (h1, h2, out)."""
        h1 = [relu(self.b1[j]
                   + sum(self.w1[j][i] * x[i] for i in range(self.n_in)))
              for j in range(self.n_h1)]
        h2 = [relu(self.b2[j]
                   + sum(self.w2[j][i] * h1[i] for i in range(self.n_h1)))
              for j in range(self.n_h2)]
        out = [self.b3[j]
               + sum(self.w3[j][i] * h2[i] for i in range(self.n_h2))
               for j in range(self.n_out)]
        return h1, h2, out

    def predict(self, x: list) -> list:
        _, _, out = self.forward(x)
        return out

    # ── Training step (MSE + SGD) ──────────────────────────────────────────────

    def train_step(self, x: list, target: list, lr: float) -> float:
        h1, h2, out = self.forward(x)

        # MSE loss
        d_out = [(out[j] - target[j]) * 2.0 / self.n_out for j in range(self.n_out)]
        loss = sum((out[j] - target[j]) ** 2 for j in range(self.n_out)) / self.n_out

        # Layer 3 gradients
        d_h2 = [0.0] * self.n_h2
        for j in range(self.n_out):
            self.b3[j] -= lr * d_out[j]
            for i in range(self.n_h2):
                d_h2[i]       += self.w3[j][i] * d_out[j]
                self.w3[j][i] -= lr * d_out[j] * h2[i]

        # Layer 2 gradients (ReLU)
        d_h1 = [0.0] * self.n_h1
        for j in range(self.n_h2):
            if h2[j] <= 0.0:
                continue
            self.b2[j] -= lr * d_h2[j]
            for i in range(self.n_h1):
                d_h1[i]       += self.w2[j][i] * d_h2[j]
                self.w2[j][i] -= lr * d_h2[j] * h1[i]

        # Layer 1 gradients (ReLU)
        for j in range(self.n_h1):
            if h1[j] <= 0.0:
                continue
            self.b1[j] -= lr * d_h1[j]
            for i in range(self.n_in):
                self.w1[j][i] -= lr * d_h1[j] * x[i]

        return loss


# ─────────────────────────────────────────────────────────────────────────────
# ONNX export
# ─────────────────────────────────────────────────────────────────────────────

def export_onnx(model: RegressionMLP, output_path: str):
    """Export 3-layer regression MLP to ONNX (opset 13)."""
    import onnx
    from onnx import helper, TensorProto, numpy_helper
    import numpy as np

    def wt(weights, name):
        """Row-major weight matrix → transposed for MatMul."""
        return numpy_helper.from_array(
            np.array(weights, dtype=np.float32).T, name=name)

    def bv(biases, name):
        return numpy_helper.from_array(np.array(biases, dtype=np.float32), name=name)

    inits = [
        wt(model.w1, "w1t"), bv(model.b1, "b1"),
        wt(model.w2, "w2t"), bv(model.b2, "b2"),
        wt(model.w3, "w3t"), bv(model.b3, "b3"),
    ]

    nodes = [
        helper.make_node("MatMul", ["input", "w1t"], ["mm1"]),
        helper.make_node("Add",    ["mm1",   "b1"],  ["a1"]),
        helper.make_node("Relu",   ["a1"],            ["h1"]),
        helper.make_node("MatMul", ["h1",    "w2t"], ["mm2"]),
        helper.make_node("Add",    ["mm2",   "b2"],  ["a2"]),
        helper.make_node("Relu",   ["a2"],            ["h2"]),
        helper.make_node("MatMul", ["h2",    "w3t"], ["mm3"]),
        helper.make_node("Add",    ["mm3",   "b3"],  ["output"]),
    ]

    graph = helper.make_graph(
        nodes, "mix_model",
        [helper.make_tensor_value_info("input",  TensorProto.FLOAT, [1, model.n_in])],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, model.n_out])],
        initializer=inits,
    )

    onnx_model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    onnx_model.ir_version = 8
    onnx.checker.check_model(onnx_model)

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    onnx.save(onnx_model, output_path)
    print(f"ONNX model saved: {output_path}")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=6000,
                        help="Number of synthetic training mixes")
    parser.add_argument("--epochs",  type=int, default=100)
    parser.add_argument("--lr",      type=float, default=0.005)
    parser.add_argument("--output",  default="models/mix_model.onnx")
    parser.add_argument("--seed",    type=int,   default=42)
    args = parser.parse_args()

    random.seed(args.seed)

    # ── Generate dataset ───────────────────────────────────────────────────────
    print(f"Generating {args.samples} synthetic 8-slot mixes...")
    X_raw, y = generate_dataset(args.samples)
    print(f"  Input  : {N_INPUT} features / sample")
    print(f"  Output : {N_OUTPUT} targets  / sample")

    # ── Normalise inputs (z-score) ────────────────────────────────────────────
    means, stds = compute_norm_params(X_raw, N_INPUT)
    X = apply_norm(X_raw, means, stds)

    # ── Train / val split (80/20) ─────────────────────────────────────────────
    indices = list(range(len(X)))
    random.shuffle(indices)
    split    = int(len(indices) * 0.8)
    tr_idx   = indices[:split]
    val_idx  = indices[split:]
    print(f"  Train: {len(tr_idx)}, Val: {len(val_idx)}")

    # ── Train ─────────────────────────────────────────────────────────────────
    model = RegressionMLP(n_in=N_INPUT, n_h1=64, n_h2=32, n_out=N_OUTPUT)

    for epoch in range(args.epochs):
        random.shuffle(tr_idx)
        total_loss = 0.0
        for idx in tr_idx:
            total_loss += model.train_step(X[idx], y[idx], lr=args.lr)

        if (epoch + 1) % 20 == 0 or epoch == 0:
            # Validation MSE
            val_loss = sum(
                sum((p - t) ** 2 for p, t in zip(model.predict(X[i]), y[i]))
                / N_OUTPUT
                for i in val_idx
            ) / max(len(val_idx), 1)
            print(f"  Epoch {epoch+1:4d}: "
                  f"train_mse={total_loss/len(tr_idx):.5f}  "
                  f"val_mse={val_loss:.5f}")

    # Final validation MSE
    val_loss = sum(
        sum((p - t) ** 2 for p, t in zip(model.predict(X[i]), y[i])) / N_OUTPUT
        for i in val_idx
    ) / max(len(val_idx), 1)
    print(f"\nFinal val MSE: {val_loss:.5f}")

    # Quick sanity check: show predictions for a KICK-heavy mix
    print("\nSample prediction (8 slots: KICK SNARE HIHAT BASS SYNTH PAD PERC OTHER):")
    types_ex = ["KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC", "OTHER"]
    random.seed(0)
    feats_ex  = []
    for ct in types_ex:
        feats_ex.extend(sample_slot_features(ct))
    norm_ex   = [(feats_ex[f] - means[f]) / stds[f] for f in range(N_INPUT)]
    pred_ex   = model.predict(norm_ex)
    tgt_ex    = compute_targets(types_ex)
    print(f"  {'Slot':<6} {'Type':<6} {'Vol':>5}  {'LG':>5}  {'MG':>5}  {'HG':>5}"
          f"  | {'Vol':>5}  {'LG':>5}  {'MG':>5}  {'HG':>5}  (target)")
    for s in range(N_SLOTS):
        b = s * N_TARGET_SLOT
        print(f"  {s+1:<6} {types_ex[s]:<6} "
              f"{pred_ex[b]:.3f}  {pred_ex[b+1]:+.2f}  {pred_ex[b+2]:+.2f}  {pred_ex[b+3]:+.2f}"
              f"  | {tgt_ex[b]:.3f}  {tgt_ex[b+1]:+.2f}  {tgt_ex[b+2]:+.2f}  {tgt_ex[b+3]:+.2f}")

    # ── Export ────────────────────────────────────────────────────────────────
    export_onnx(model, args.output)

    norm_path = os.path.splitext(args.output)[0] + "_norm.bin"
    with open(norm_path, "wb") as f:
        for m in means:
            f.write(struct.pack("<f", float(m)))
        for s in stds:
            f.write(struct.pack("<f", float(s)))
    print(f"Normalisation params saved: {norm_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
