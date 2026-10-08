"""Synthetic one-shots per instrument class, to check the ear pipeline end to end
(extractor -> training -> plugin loader) without downloading a dataset. A real model needs
real stems; see train_ear.py.

    python make_synthetic.py out_dir
"""
import os
import struct
import sys

import numpy as np

SR = 44100


def wav(path, x):
    x = np.clip(x, -1, 1).astype("<f4")
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + x.nbytes) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, 1, SR, SR * 4, 4, 32))
        f.write(b"data" + struct.pack("<I", x.nbytes) + x.tobytes())


def hits(rng, rate, make, seconds=6.0):
    n = int(SR * seconds)
    out = np.zeros(n, dtype=np.float32)
    period = int(SR / rate)
    for start in range(0, n - period, period):
        jitter = rng.integers(0, period // 8)
        seg = make(rng)[: n - start - jitter]
        out[start + jitter: start + jitter + len(seg)] += seg
    return out


def kick(rng):
    t = np.arange(int(SR * 0.4)) / SR
    f = 45 + 80 * np.exp(-t * 30) * rng.uniform(0.8, 1.2)
    return 0.9 * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * rng.uniform(7, 12))


def snare(rng):
    t = np.arange(int(SR * 0.3)) / SR
    return (0.5 * np.sin(2 * np.pi * rng.uniform(170, 220) * t) + 0.6 * rng.standard_normal(len(t))) * np.exp(-t * rng.uniform(15, 25))


def hat(rng):
    t = np.arange(int(SR * 0.1)) / SR
    noise = np.diff(rng.standard_normal(len(t) + 1))
    return 0.3 * noise * np.exp(-t * rng.uniform(40, 80))


def tone(rng, f0, harmonics, seconds=6.0, vibrato=0.0, noise=0.0):
    t = np.arange(int(SR * seconds)) / SR
    phase = 2 * np.pi * f0 * t + vibrato * np.sin(2 * np.pi * 5 * t)
    x = sum(a * np.sin(k * phase) for k, a in harmonics)
    return 0.3 * x * (0.7 + 0.3 * np.sin(2 * np.pi * rng.uniform(0.2, 1) * t)) + noise * rng.standard_normal(len(t))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "synthetic"
    rng = np.random.default_rng(3)
    makers = {
        "kick": lambda: hits(rng, rng.uniform(1.5, 2.5), kick),
        "snare": lambda: hits(rng, rng.uniform(1, 2), snare),
        "hihat": lambda: hits(rng, rng.uniform(4, 8), hat),
        "bass": lambda: tone(rng, rng.uniform(40, 80), [(1, 1.0), (2, 0.3), (3, 0.1)]),
        "lead_vocal": lambda: tone(rng, rng.uniform(180, 330), [(k, 1.0 / k) for k in range(1, 14)], vibrato=0.6, noise=0.01),
        "pad": lambda: tone(rng, rng.uniform(200, 400), [(1, 1.0), (1.005, 0.8), (2, 0.4), (3, 0.2)]),
    }
    for label, make in makers.items():
        os.makedirs(os.path.join(out, label), exist_ok=True)
        for i in range(12):
            wav(os.path.join(out, label, "%02d.wav" % i), make())
    print("wrote", out)


if __name__ == "__main__":
    main()
