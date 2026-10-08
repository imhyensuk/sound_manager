"""악기 인식 모델(ear model) 학습: 플러그인이 채널 이름 후보를 제안할 때 씁니다.

    1. 클래스마다 폴더 하나에 WAV 스템을 넣습니다(하위 폴더 깊이 무관, 한글 폴더 이름 가능):
           stems/남성 보컬/  stems/여성 보컬/  stems/킥/  stems/스네어/  stems/메인건반/  stems/세컨건반/
           stems/서드건반/  stems/어쿠스틱 기타/  stems/일렉 기타/  stems/베이스 기타/  stems/Pad/
           stems/Tom1/  stems/Tom2/  stems/Tom3/  stems/HiHat/  stems/OverHead/
    2. 플러그인과 같은 특징 추출기로 특징을 뽑습니다(무음 구간은 자동으로 건너뜀):
           smix_ear_features features.csv --dir stems/
    3. 학습(numpy만, CPU로 충분):
           python train_ear.py features.csv ear.smxear
           python train_ear.py features.csv ear.smxear --merge "메인건반,세컨건반,서드건반=건반"
       검증은 파일 단위로 나눕니다(한 곡의 구간이 학습/검증 양쪽에 섞이지 않음). 클래스 수 불균형을
       보정하고, 혼동 행렬로 소리만으로 구분이 안 되는 클래스를 보여줍니다(그런 클래스는 --merge).
    4. ear.smxear를 SoundManagerAI/models/에 넣습니다(또는 모듈·설정 탭에서 경로 지정).

파일 형식은 core/src/ear/EarModel.cpp와 같습니다("SMXEAR01", 헤더 JSON {classes, names, roles, dims},
float32 평균/표준편차, 이어서 층마다 W[out x in], b[out]).
"""
import argparse
import json
import re
import struct

import numpy as np

# normalised folder name (lower case, no spaces/_/-) -> plugin role id (core/src/Types.cpp)
ROLE_OF = {
    "kick": "kick", "bd": "kick", "킥": "kick", "킥드럼": "kick",
    "snare": "snare", "sd": "snare", "스네어": "snare",
    "hihat": "hihat", "hats": "hihat", "hh": "hihat", "하이햇": "hihat",
    "tom": "toms", "toms": "toms", "탐": "toms",
    "overhead": "overheads", "overheads": "overheads", "oh": "overheads", "오버헤드": "overheads", "cymbals": "overheads",
    "percussion": "percussion", "perc": "percussion", "퍼커션": "percussion",
    "bass": "bass", "bassguitar": "bass", "베이스": "bass", "베이스기타": "bass",
    "vocal": "lead_vocal", "vocals": "lead_vocal", "leadvocal": "lead_vocal", "보컬": "lead_vocal",
    "malevocal": "lead_vocal", "femalevocal": "lead_vocal", "남성보컬": "lead_vocal", "여성보컬": "lead_vocal",
    "남자보컬": "lead_vocal", "여자보컬": "lead_vocal",
    "backingvocal": "backing_vocal", "bgv": "backing_vocal", "코러스": "backing_vocal",
    "guitar": "electric_guitar", "electricguitar": "electric_guitar", "일렉기타": "electric_guitar",
    "acousticguitar": "acoustic_guitar", "어쿠스틱기타": "acoustic_guitar", "통기타": "acoustic_guitar",
    "piano": "piano", "피아노": "piano",
    "keys": "keys", "건반": "keys", "메인건반": "keys", "세컨건반": "keys", "서드건반": "keys",
    "synth": "synth", "신스": "synth", "pad": "pad", "패드": "pad", "strings": "strings", "스트링": "strings",
    "brass": "brass", "fx": "fx",
}


def role_of(label):
    key = re.sub(r"[\s_\-]+", "", label.lower())
    if key in ROLE_OF:
        return ROLE_OF[key]
    key = re.sub(r"\d+$", "", key)  # Tom1, Tom2, Keys2 ...
    return ROLE_OF.get(key, key)


def load(path):
    labels, groups, rows = [], [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(",")
            labels.append(parts[0])
            groups.append(int(parts[1]))
            rows.append([float(v) for v in parts[2:]])
    return labels, np.asarray(groups), np.asarray(rows, dtype=np.float32)


def train(x, y, classes, weights, hidden=(128, 64), epochs=600, lr=3e-3, l2=1e-4, seed=0):
    rng = np.random.default_rng(seed)
    dims = [x.shape[1], *hidden, len(classes)]
    W = [rng.standard_normal((dims[i + 1], dims[i])).astype(np.float32) * np.sqrt(2.0 / dims[i]) for i in range(len(dims) - 1)]
    b = [np.zeros(dims[i + 1], dtype=np.float32) for i in range(len(dims) - 1)]
    m = [np.zeros_like(w) for w in W] + [np.zeros_like(v) for v in b]
    v = [np.zeros_like(w) for w in W] + [np.zeros_like(v) for v in b]
    onehot = np.eye(len(classes), dtype=np.float32)[y]
    sample_w = weights[y][:, None]  # balanced classes: rare instruments count as much as common ones
    for step in range(1, epochs + 1):
        acts = [x]
        for i in range(len(W)):
            z = acts[-1] @ W[i].T + b[i]
            acts.append(np.maximum(z, 0) if i + 1 < len(W) else z)
        logits = acts[-1] - acts[-1].max(axis=1, keepdims=True)
        p = np.exp(logits)
        p /= p.sum(axis=1, keepdims=True)
        loss = -np.sum(sample_w[:, 0] * np.sum(onehot * np.log(p + 1e-9), axis=1)) / sample_w.sum()
        grad = sample_w * (p - onehot) / sample_w.sum()
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
            print("step %d loss %.4f train accuracy %.3f" % (step, loss, np.mean(p.argmax(axis=1) == y)))
    return dims, W, b


def predict(x, W, b):
    a = x
    for i in range(len(W)):
        a = a @ W[i].T + b[i]
        if i + 1 < len(W):
            a = np.maximum(a, 0)
    return a.argmax(axis=1)


def write(path, classes, dims, mean, std, W, b):
    header = json.dumps({"classes": classes, "names": classes, "roles": [role_of(c) for c in classes], "dims": dims},
                        ensure_ascii=False).encode("utf-8")
    with open(path, "wb") as f:
        f.write(b"SMXEAR01")
        f.write(struct.pack("<I", len(header)))
        f.write(header)
        f.write(mean.astype("<f4").tobytes())
        f.write(std.astype("<f4").tobytes())
        for w, bias in zip(W, b):
            f.write(w.astype("<f4").tobytes())
            f.write(bias.astype("<f4").tobytes())


def confusion(y_true, y_pred, classes):
    n = len(classes)
    m = np.zeros((n, n), dtype=int)
    for t, p in zip(y_true, y_pred):
        m[t, p] += 1
    width = max(len(c) for c in classes) + 2
    print("\n혼동 행렬 (행: 정답, 열: 예측, 검증 구간 수)")
    print(" " * width + " ".join("%5d" % i for i in range(n)))
    for i, c in enumerate(classes):
        acc = m[i, i] / max(1, m[i].sum())
        print(("%-" + str(width) + "s") % ("%d %s" % (i, c)) + " ".join("%5d" % v for v in m[i]) + "   정확도 %.2f" % acc)
    # Pairs the ear mixes up most: candidates for --merge
    pairs = []
    for i in range(n):
        for j in range(n):
            if i != j and m[i].sum() > 0:
                pairs.append((m[i, j] / m[i].sum(), classes[i], classes[j]))
    pairs.sort(reverse=True)
    confused = [p for p in pairs if p[0] >= 0.25]
    if confused:
        print("\n자주 헷갈리는 쌍 (합치기를 고려하세요):")
        for rate, a, b in confused[:8]:
            print("  %s → %s  %.0f%%" % (a, b, rate * 100))


def main():
    ap = argparse.ArgumentParser(description="Sound Manager 악기 인식 모델 학습")
    ap.add_argument("features")
    ap.add_argument("out")
    ap.add_argument("--merge", action="append", default=[], help='예: "메인건반,세컨건반,서드건반=건반"')
    ap.add_argument("--val", type=float, default=0.2, help="검증용 파일 비율")
    ap.add_argument("--epochs", type=int, default=600)
    args = ap.parse_args()

    labels, groups, x = load(args.features)
    for rule in args.merge:
        sources, target = rule.split("=")
        sources = {s.strip() for s in sources.split(",")}
        labels = [target.strip() if l in sources else l for l in labels]
    classes = sorted(set(labels))
    y = np.array([classes.index(l) for l in labels])

    # Split by file, stratified per class: every class keeps files on both sides when it can.
    rng = np.random.default_rng(1)
    val_files = set()
    for c in range(len(classes)):
        files = sorted(set(groups[y == c]))
        rng.shuffle(files)
        k = int(round(len(files) * args.val))
        if len(files) >= 2:
            val_files.update(files[: max(1, k)])
    is_val = np.array([g in val_files for g in groups])
    tr, te = np.where(~is_val)[0], np.where(is_val)[0]

    counts = np.bincount(y[tr], minlength=len(classes)).astype(np.float32)
    weights = (counts.sum() / (len(classes) * np.maximum(counts, 1))).astype(np.float32)
    print("클래스 %d개, 학습 구간 %d / 검증 구간 %d (파일 단위 분리)" % (len(classes), len(tr), len(te)))
    for c, n in zip(classes, counts):
        print("  %-14s %5d 구간  역할 %s" % (c, n, role_of(c)))

    mean = x[tr].mean(axis=0)
    std = x[tr].std(axis=0) + 1e-6
    xn = (x - mean) / std
    dims, W, b = train(xn[tr], y[tr], classes, weights, epochs=args.epochs)
    if len(te):
        pred = predict(xn[te], W, b)
        print("\n검증 정확도 (처음 보는 파일): %.3f" % np.mean(pred == y[te]))
        confusion(y[te], pred, classes)
    write(args.out, classes, dims, mean, std, W, b)
    print("\n저장:", args.out)


if __name__ == "__main__":
    main()
