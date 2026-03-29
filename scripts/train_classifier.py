"""Train a small CNN classifier for audio content type detection.

Input:  data/dataset/{KICK,SNARE,HIHAT,BASS,SYNTH,PAD,PERC}/*.wav
Output: models/content_classifier.onnx

The model takes a fixed-size 1D float vector (raw PCM, 22050 samples = 0.5s)
and outputs 7 class logits.

Usage:
    python scripts/train_classifier.py [--dataset data/dataset] [--epochs 40]
"""

import argparse
import os
import struct
import math
import random

# ── WAV reader (no dependencies) ─────────────────────────────────────────────

def read_wav_mono(path: str) -> tuple[list[float], int]:
    """Read mono 16-bit WAV, return (samples_float, sample_rate)."""
    with open(path, "rb") as f:
        riff = f.read(4)
        assert riff == b"RIFF", f"Not a WAV file: {path}"
        f.read(4)  # file size
        wave = f.read(4)
        assert wave == b"WAVE"

        sr = 44100
        bits = 16
        channels = 1
        data_samples = []

        while True:
            chunk_id = f.read(4)
            if len(chunk_id) < 4:
                break
            chunk_size = struct.unpack("<I", f.read(4))[0]

            if chunk_id == b"fmt ":
                fmt_data = f.read(chunk_size)
                audio_fmt = struct.unpack("<H", fmt_data[0:2])[0]
                channels = struct.unpack("<H", fmt_data[2:4])[0]
                sr = struct.unpack("<I", fmt_data[4:8])[0]
                bits = struct.unpack("<H", fmt_data[14:16])[0]
            elif chunk_id == b"data":
                raw = f.read(chunk_size)
                n_samples = chunk_size // (bits // 8) // channels
                for i in range(n_samples):
                    idx = i * channels * (bits // 8)
                    val = struct.unpack("<h", raw[idx:idx + 2])[0]
                    data_samples.append(val / 32768.0)
            else:
                f.read(chunk_size)

    return data_samples, sr


# ── Feature extraction: simple spectral features ─────────────────────────────

TARGET_LEN = 22050  # 0.5s at 44100 Hz — fixed input size

def pad_or_crop(samples: list[float], target: int = TARGET_LEN) -> list[float]:
    if len(samples) >= target:
        return samples[:target]
    return samples + [0.0] * (target - len(samples))


CLASSES = ["KICK", "SNARE", "HIHAT", "BASS", "SYNTH", "PAD", "PERC"]
CLASS_TO_IDX = {c: i for i, c in enumerate(CLASSES)}


def load_dataset(dataset_dir: str):
    """Load all WAV files, return (X, y) as lists."""
    X, y = [], []
    for cls_name in CLASSES:
        cls_dir = os.path.join(dataset_dir, cls_name)
        if not os.path.isdir(cls_dir):
            print(f"  Warning: {cls_dir} not found, skipping")
            continue
        for fname in sorted(os.listdir(cls_dir)):
            if not fname.endswith(".wav"):
                continue
            samples, sr = read_wav_mono(os.path.join(cls_dir, fname))
            X.append(pad_or_crop(samples))
            y.append(CLASS_TO_IDX[cls_name])
    return X, y


# ── Minimal neural network (pure Python, no PyTorch) ─────────────────────────
#
# Since we want zero heavy dependencies, we implement a tiny 1-hidden-layer
# network trained with SGD.  Input: 64 hand-crafted features.  Output: 7 classes.
#
# Features extracted per sample:
#   - RMS, peak, crest factor
#   - Zero crossing rate
#   - Spectral centroid (via DFT)
#   - 4-band energy ratios (sub, bass, mid, high)
#   - Attack time (time to peak)
#   - Duration above threshold
#   ... → 64 features via 8-band breakdown + statistics

def extract_features(pcm: list[float], n_features: int = 64) -> list[float]:
    """Extract fixed-size feature vector from PCM."""
    n = len(pcm)
    if n == 0:
        return [0.0] * n_features

    # Basic stats
    peak = max(abs(s) for s in pcm)
    rms = math.sqrt(sum(s * s for s in pcm) / n)
    crest = peak / rms if rms > 1e-8 else 1.0

    # Zero crossing rate
    zcr = sum(1 for i in range(1, n) if pcm[i] * pcm[i-1] < 0) / n

    # Attack time (samples to reach peak)
    peak_idx = 0
    peak_val = 0
    for i in range(min(n, 4410)):  # first 100ms
        if abs(pcm[i]) > peak_val:
            peak_val = abs(pcm[i])
            peak_idx = i
    attack_ratio = peak_idx / 44100.0  # in seconds

    # Duration above threshold
    thresh = peak * 0.1
    above = sum(1 for s in pcm if abs(s) > thresh) / n

    # Band energies via simple 1st-order LP cascade
    def alpha(fc):
        t = 2 * math.pi * fc / 44100
        return t / (t + 1)

    bands = [150, 500, 1500, 4000, 8000]
    lp_states = [0.0] * len(bands)
    band_energy = [0.0] * (len(bands) + 1)
    total_energy = 0.0

    for s in pcm:
        prev = 0.0
        for b in range(len(bands)):
            a = alpha(bands[b])
            lp_states[b] = a * s + (1 - a) * lp_states[b]
            diff = lp_states[b] - prev
            band_energy[b] += diff * diff
            prev = lp_states[b]
        # Highest band
        diff = s - prev
        band_energy[-1] += diff * diff
        total_energy += s * s

    if total_energy < 1e-10:
        total_energy = 1e-10
    band_ratios = [e / total_energy for e in band_energy]

    # Temporal envelope: split into 8 segments, compute RMS of each
    seg_count = 8
    seg_size = n // seg_count
    seg_rms = []
    for seg in range(seg_count):
        start = seg * seg_size
        end = start + seg_size
        seg_e = sum(pcm[i] * pcm[i] for i in range(start, min(end, n)))
        seg_rms.append(math.sqrt(seg_e / max(seg_size, 1)))

    # Assemble feature vector
    features = [
        rms, peak, crest, zcr, attack_ratio, above,
    ] + band_ratios + seg_rms

    # Pad or crop to n_features
    features = features[:n_features]
    while len(features) < n_features:
        features.append(0.0)

    return features


# ── Simple 2-layer neural network ────────────────────────────────────────────

def softmax(logits):
    max_l = max(logits)
    exps = [math.exp(l - max_l) for l in logits]
    s = sum(exps)
    return [e / s for e in exps]

def relu(x):
    return max(0.0, x)

class SimpleNN:
    """2-layer fully connected: input(64) → hidden(32) → output(7)."""

    def __init__(self, n_in=64, n_hidden=32, n_out=7):
        self.n_in = n_in
        self.n_hidden = n_hidden
        self.n_out = n_out

        # Xavier init
        scale1 = math.sqrt(2.0 / n_in)
        scale2 = math.sqrt(2.0 / n_hidden)
        self.w1 = [[random.gauss(0, scale1) for _ in range(n_in)] for _ in range(n_hidden)]
        self.b1 = [0.0] * n_hidden
        self.w2 = [[random.gauss(0, scale2) for _ in range(n_hidden)] for _ in range(n_out)]
        self.b2 = [0.0] * n_out

    def forward(self, x):
        """Returns (hidden_activations, logits)."""
        # Hidden layer
        h = []
        for j in range(self.n_hidden):
            val = self.b1[j]
            for i in range(self.n_in):
                val += self.w1[j][i] * x[i]
            h.append(relu(val))

        # Output layer
        logits = []
        for j in range(self.n_out):
            val = self.b2[j]
            for i in range(self.n_hidden):
                val += self.w2[j][i] * h[i]
            logits.append(val)

        return h, logits

    def predict(self, x):
        _, logits = self.forward(x)
        return logits.index(max(logits))

    def train_step(self, x, target, lr=0.01):
        """One SGD step with cross-entropy loss. Returns loss."""
        h, logits = self.forward(x)
        probs = softmax(logits)

        loss = -math.log(max(probs[target], 1e-10))

        # Gradient of cross-entropy w.r.t. logits
        d_logits = list(probs)
        d_logits[target] -= 1.0

        # Backprop: output layer
        d_h = [0.0] * self.n_hidden
        for j in range(self.n_out):
            self.b2[j] -= lr * d_logits[j]
            for i in range(self.n_hidden):
                d_h[i] += self.w2[j][i] * d_logits[j]
                self.w2[j][i] -= lr * d_logits[j] * h[i]

        # Backprop: hidden layer (ReLU derivative)
        for j in range(self.n_hidden):
            if h[j] <= 0:
                continue  # ReLU gate closed
            self.b1[j] -= lr * d_h[j]
            for i in range(self.n_in):
                self.w1[j][i] -= lr * d_h[j] * x[i]

        return loss


# ── ONNX export (using onnx library) ─────────────────────────────────────────

def export_onnx(model: SimpleNN, output_path: str):
    """Export trained model to ONNX format."""
    import onnx
    from onnx import helper, TensorProto, numpy_helper
    import numpy as np

    # Create weight tensors
    w1 = np.array(model.w1, dtype=np.float32)  # [n_hidden, n_in]
    b1 = np.array(model.b1, dtype=np.float32)  # [n_hidden]
    w2 = np.array(model.w2, dtype=np.float32)  # [n_out, n_hidden]
    b2 = np.array(model.b2, dtype=np.float32)  # [n_out]

    # Initializers
    w1_init = numpy_helper.from_array(w1, name="w1")
    b1_init = numpy_helper.from_array(b1, name="b1")
    w2_init = numpy_helper.from_array(w2, name="w2")
    b2_init = numpy_helper.from_array(b2, name="b2")

    # Nodes: input → MatMul(w1^T) → Add(b1) → Relu → MatMul(w2^T) → Add(b2) → output
    nodes = [
        helper.make_node("MatMul", ["input", "w1t"], ["mm1"]),
        helper.make_node("Add", ["mm1", "b1"], ["add1"]),
        helper.make_node("Relu", ["add1"], ["relu1"]),
        helper.make_node("MatMul", ["relu1", "w2t"], ["mm2"]),
        helper.make_node("Add", ["mm2", "b2"], ["output"]),
    ]

    # Transpose weight matrices for MatMul: input[1,64] x w1t[64,32] = [1,32]
    w1t = np.array(model.w1, dtype=np.float32).T  # [n_in, n_hidden]
    w2t = np.array(model.w2, dtype=np.float32).T  # [n_hidden, n_out]
    w1t_init = numpy_helper.from_array(w1t, name="w1t")
    w2t_init = numpy_helper.from_array(w2t, name="w2t")

    graph = helper.make_graph(
        nodes,
        "content_classifier",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, model.n_in])],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, model.n_out])],
        initializer=[w1t_init, b1_init, w2t_init, b2_init],
    )

    onnx_model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    onnx_model.ir_version = 8
    onnx.checker.check_model(onnx_model)

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    onnx.save(onnx_model, output_path)
    print(f"ONNX model saved: {output_path}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", default="data/dataset")
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--lr", type=float, default=0.005)
    parser.add_argument("--output", default="models/content_classifier.onnx")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    random.seed(args.seed)

    print("Loading dataset...")
    X_raw, y = load_dataset(args.dataset)
    print(f"  {len(X_raw)} samples, {len(CLASSES)} classes")

    # Extract features
    print("Extracting features...")
    N_FEATURES = 64
    X = [extract_features(pcm, N_FEATURES) for pcm in X_raw]

    # Normalize features (z-score)
    means = [0.0] * N_FEATURES
    stds = [1.0] * N_FEATURES
    for f in range(N_FEATURES):
        vals = [X[i][f] for i in range(len(X))]
        means[f] = sum(vals) / len(vals)
        var = sum((v - means[f]) ** 2 for v in vals) / len(vals)
        stds[f] = math.sqrt(var) if var > 1e-10 else 1.0

    for i in range(len(X)):
        for f in range(N_FEATURES):
            X[i][f] = (X[i][f] - means[f]) / stds[f]

    # Train/val split (80/20)
    indices = list(range(len(X)))
    random.shuffle(indices)
    split = int(len(indices) * 0.8)
    train_idx = indices[:split]
    val_idx = indices[split:]

    print(f"  Train: {len(train_idx)}, Val: {len(val_idx)}")

    # Train
    model = SimpleNN(n_in=N_FEATURES, n_hidden=32, n_out=len(CLASSES))

    for epoch in range(args.epochs):
        random.shuffle(train_idx)
        total_loss = 0.0
        correct = 0

        for idx in train_idx:
            loss = model.train_step(X[idx], y[idx], lr=args.lr)
            total_loss += loss
            if model.predict(X[idx]) == y[idx]:
                correct += 1

        train_acc = correct / len(train_idx)

        # Validation
        val_correct = 0
        for idx in val_idx:
            if model.predict(X[idx]) == y[idx]:
                val_correct += 1
        val_acc = val_correct / len(val_idx) if val_idx else 0

        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}: loss={total_loss/len(train_idx):.4f}  "
                  f"train_acc={train_acc:.1%}  val_acc={val_acc:.1%}")

    # Final validation accuracy
    val_correct = sum(1 for idx in val_idx if model.predict(X[idx]) == y[idx])
    final_acc = val_correct / len(val_idx) if val_idx else 0
    print(f"\nFinal validation accuracy: {final_acc:.1%}")

    if final_acc < 0.85:
        print("WARNING: accuracy below 85% target — consider more data or epochs")

    # Export ONNX
    export_onnx(model, args.output)

    # Save normalization params for C++ inference
    norm_path = os.path.splitext(args.output)[0] + "_norm.bin"
    with open(norm_path, "wb") as f:
        for m in means:
            f.write(struct.pack("<f", m))
        for s in stds:
            f.write(struct.pack("<f", s))
    print(f"Normalization params saved: {norm_path}")

if __name__ == "__main__":
    main()
