"""Trains the instrument recogniser (ear model) the plugin uses for its naming question.

    1. Collect labelled audio: one folder per instrument (kick/, snare/, lead_vocal/, ...) with WAV
       stems or one-shots (your own multitracks, MUSDB18/MedleyDB/Slakh stems, NSynth, ...).
    2. Features with the plugin's own extractor (guarantees identical features at inference):
           smix_ear_features features.csv --dir stems/
    3. Train (numpy only, CPU is enough; works the same on Colab):
           python train_ear.py features.csv ear.smxear
    4. Put ear.smxear in SoundManagerAI/models/ (or set the path in the 모듈·설정 tab).

The file layout matches core/src/ear/EarModel.cpp ("SMXEAR01", header JSON, float32 mean/std,
then per layer W[out x in] and b[out]).
"""
import json
import struct
import sys

import numpy as np

# folder name -> plugin role id (core/src/Types.cpp)
ROLE_OF = {
    "kick": "kick", "bd": "kick", "snare": "snare", "sd": "snare", "hihat": "hihat", "hats": "hihat", "hh": "hihat",
    "toms": "toms", "tom": "toms", "overheads": "overheads", "oh": "overheads", "cymbals": "overheads",
    "percussion": "percussion", "perc": "percussion", "bass": "bass", "vocals": "lead_vocal", "vocal": "lead_vocal",
    "lead_vocal": "lead_vocal", "backing_vocal": "backing_vocal", "bgv": "backing_vocal", "guitar": "electric_guitar",
    "electric_guitar": "electric_guitar", "acoustic_guitar": "acoustic_guitar", "piano": "piano", "keys": "keys",
    "synth": "synth", "pad": "pad", "strings": "strings", "brass": "brass", "fx": "fx",
}


def load(path):
    labels, rows = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(",")
            labels.append(parts[0])
            rows.append([float(v) for v in parts[1:]])
    return labels, np.asarray(rows, dtype=np.float32)


def train(x, y, classes, hidden=(128, 64), epochs=400, lr=3e-3, l2=1e-4, seed=0):
    rng = np.random.default_rng(seed)
    dims = [x.shape[1], *hidden, len(classes)]
    W = [rng.standard_normal((dims[i + 1], dims[i])).astype(np.float32) * np.sqrt(2.0 / dims[i]) for i in range(len(dims) - 1)]
    b = [np.zeros(dims[i + 1], dtype=np.float32) for i in range(len(dims) - 1)]
    m = [np.zeros_like(w) for w in W] + [np.zeros_like(v) for v in b]
    v = [np.zeros_like(w) for w in W] + [np.zeros_like(v) for v in b]
    onehot = np.eye(len(classes), dtype=np.float32)[y]
    for step in range(1, epochs + 1):
        acts = [x]
        for i in range(len(W)):
            z = acts[-1] @ W[i].T + b[i]
            acts.append(np.maximum(z, 0) if i + 1 < len(W) else z)
        logits = acts[-1] - acts[-1].max(axis=1, keepdims=True)
        p = np.exp(logits)
        p /= p.sum(axis=1, keepdims=True)
        loss = -np.mean(np.sum(onehot * np.log(p + 1e-9), axis=1))
        grad = (p - onehot) / len(x)
        gW, gb = [None] * len(W), [None] * len(W)
        for i in reversed(range(len(W))):
            gW[i] = grad.T @ acts[i] + l2 * W[i]
            gb[i] = grad.sum(axis=0)
            if i > 0:
                grad = (grad @ W[i]) * (acts[i] > 0)
        for k, (param, g) in enumerate(zip(W + b, gW + gb)):  # Adam
            m[k] = 0.9 * m[k] + 0.1 * g
            v[k] = 0.999 * v[k] + 0.001 * g * g
            param -= lr * (m[k] / (1 - 0.9 ** step)) / (np.sqrt(v[k] / (1 - 0.999 ** step)) + 1e-8)
        if step % 100 == 0:
            print("step %d loss %.4f accuracy %.3f" % (step, loss, np.mean(p.argmax(axis=1) == y)))
    return dims, W, b


def predict(x, W, b):
    a = x
    for i in range(len(W)):
        a = a @ W[i].T + b[i]
        if i + 1 < len(W):
            a = np.maximum(a, 0)
    return a.argmax(axis=1)


def write(path, classes, dims, mean, std, W, b):
    header = json.dumps({"classes": classes, "roles": [ROLE_OF.get(c.lower(), c.lower()) for c in classes], "dims": dims}).encode()
    with open(path, "wb") as f:
        f.write(b"SMXEAR01")
        f.write(struct.pack("<I", len(header)))
        f.write(header)
        f.write(mean.astype("<f4").tobytes())
        f.write(std.astype("<f4").tobytes())
        for w, bias in zip(W, b):
            f.write(w.astype("<f4").tobytes())
            f.write(bias.astype("<f4").tobytes())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    labels, x = load(sys.argv[1])
    classes = sorted(set(labels))
    y = np.array([classes.index(l) for l in labels])
    rng = np.random.default_rng(1)
    order = rng.permutation(len(y))
    split = max(1, int(0.8 * len(y)))
    tr, te = order[:split], order[split:]
    mean = x[tr].mean(axis=0)
    std = x[tr].std(axis=0) + 1e-6
    xn = (x - mean) / std
    dims, W, b = train(xn[tr], y[tr], classes)
    if len(te):
        print("held-out accuracy: %.3f (%d windows)" % (np.mean(predict(xn[te], W, b) == y[te]), len(te)))
    write(sys.argv[2], classes, dims, mean, std, W, b)
    print("wrote", sys.argv[2], "classes", classes)


if __name__ == "__main__":
    main()
