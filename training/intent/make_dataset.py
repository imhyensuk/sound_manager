"""Builds the fine-tuning set for the reasoning module (requirements 5, 7, 8, 13).

Each example is exactly what the plugin sends to the local LLM (system prompt + session state +
request, see core/src/llm/IntentModel.cpp) and the JSON answer the grammar allows. Requests are
generated in Korean and English from an engineer's vocabulary: targeted changes, complaints
("너무 쏴요"), several goals at once, ambiguous requests that must be answered with a question,
graph requests, commands and feedback on the previous change.

    python make_dataset.py --out data --n 8000

Writes data/train.jsonl, data/test.jsonl ({"messages": [...]}) and data/texts.json (chat-formatted
strings for memopro.finetune once the tokenizer is known; see finetune_t4.py).
"""
import argparse
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
SYSTEM = open(os.path.join(HERE, "system_prompt.txt"), encoding="utf-8").read()

# name variants per role (role ids as in core/src/Types.cpp)
CHANNELS = {
    "kick": ["Kick In", "Kick Out", "킥", "BD", "Kick"],
    "snare": ["Snare Top", "Snare", "스네어", "SN Top"],
    "hihat": ["Hi-Hat", "HH", "하이햇"],
    "overheads": ["OH L/R", "Overheads", "오버헤드"],
    "drum_bus": ["Drum Bus", "Drums", "드럼 버스"],
    "bass": ["Bass DI", "Bass", "베이스", "808"],
    "lead_vocal": ["Lead Vox", "Vocal", "보컬", "Main Vocal", "리드 보컬"],
    "backing_vocal": ["BGV", "Backing Vox", "코러스"],
    "electric_guitar": ["Gtr L", "Guitar", "일렉 기타", "Elec Gtr"],
    "acoustic_guitar": ["AcGtr", "어쿠스틱 기타"],
    "piano": ["Piano", "피아노"],
    "synth": ["Synth Lead", "신스"],
    "pad": ["Pad", "패드", "Strings Pad"],
}
KO_ROLE = {
    "kick": ["킥", "킥 드럼"], "snare": ["스네어"], "hihat": ["하이햇", "햇"], "overheads": ["오버헤드", "심벌"],
    "drum_bus": ["드럼", "드럼 전체"], "bass": ["베이스"], "lead_vocal": ["보컬", "목소리", "리드 보컬"],
    "backing_vocal": ["코러스", "백보컬"], "electric_guitar": ["기타", "일렉 기타"], "acoustic_guitar": ["어쿠스틱 기타", "통기타"],
    "piano": ["피아노"], "synth": ["신스"], "pad": ["패드"],
}
EN_ROLE = {
    "kick": ["kick", "kick drum"], "snare": ["snare"], "hihat": ["hats", "hi-hat"], "overheads": ["overheads", "cymbals"],
    "drum_bus": ["drums"], "bass": ["bass"], "lead_vocal": ["vocal", "lead vocal", "vox"], "backing_vocal": ["backing vocals"],
    "electric_guitar": ["guitar"], "acoustic_guitar": ["acoustic guitar"], "piano": ["piano"], "synth": ["synth"], "pad": ["pad"],
}
DESCRIPTORS = [
    ("탁함(머디)", "muddy"), ("쏘고 거칠음", "harsh"), ("저음이 부밍함", "boomy"), ("얇음(바디 부족)", "thin"),
    ("어둡고 먹먹함", "bright"), ("치찰음이 강함", "sibilant"), ("어택이 약하고 흐림", "punchy"), ("과하게 눌림", "squashed"),
]

# goal -> (korean phrasings "do X", english phrasings, korean complaint that implies the goal)
GOALS = {
    "tighter": (["단단하게", "타이트하게", "탄탄하게"], ["tighter", "more solid"], []),
    "punchier": (["펀치감 있게", "더 때리게", "타격감 있게"], ["punchier", "hit harder"], ["펀치가 없어", "힘이 없어"]),
    "more_attack": (["어택이 살게", "클릭이 들리게"], ["more attack", "more click"], ["어택이 안 들려"]),
    "warmer": (["따뜻하게", "포근하게"], ["warmer"], ["너무 차가워"]),
    "brighter": (["밝게", "선명하게", "화사하게"], ["brighter", "crisper"], ["너무 어두워", "먹먹해"]),
    "darker": (["어둡게", "차분하게"], ["darker"], ["너무 밝아", "너무 쨍해"]),
    "less_muddy": (["덜 탁하게", "깔끔하게"], ["less muddy", "cleaner"], ["너무 탁해", "뭉개져", "답답해"]),
    "less_harsh": (["덜 쏘게", "부드럽게 쏘지 않게"], ["less harsh"], ["너무 쏴요", "귀가 아파", "날카로워"]),
    "less_boomy": (["덜 붕붕거리게"], ["less boomy"], ["붕붕거려", "저음이 웅웅거려"]),
    "more_body": (["두껍게", "풍성하게", "묵직하게"], ["fuller", "fatter", "thicker"], ["너무 얇아", "가벼워"]),
    "thinner": (["가볍게", "얇게"], ["thinner", "lighter"], ["너무 두꺼워"]),
    "more_air": (["공기감 있게", "시원하게"], ["airier", "more air"], []),
    "less_sibilance": (["치찰음 줄여", "스 소리 줄여"], ["less sibilant"], ["치찰음이 심해", "스 소리가 거슬려"]),
    "more_controlled": (["더 압축해서 일정하게", "눌러서 일정하게"], ["more compressed", "more consistent"], ["볼륨이 들쭉날쭉해"]),
    "more_dynamic": (["다이내믹하게", "숨 쉬게"], ["more dynamic"], ["너무 눌려 있어", "답답하게 눌렸어"]),
    "wider": (["넓게", "스테레오감 있게"], ["wider"], ["너무 좁아"]),
    "narrower": (["좁게", "가운데로"], ["narrower", "more mono"], ["너무 퍼져"]),
    "more_space": (["공간감 있게", "울림 있게"], ["more reverb", "more space"], ["너무 건조해"]),
    "drier": (["건조하게", "울림 줄여"], ["drier"], ["리버브가 과해", "너무 울려"]),
    "louder": (["크게", "키워", "올려"], ["louder"], ["안 들려", "묻혀"]),
    "quieter": (["작게", "낮춰", "줄여"], ["quieter"], ["너무 커", "시끄러워"]),
    "forward": (["앞으로 나오게", "존재감 있게", "또렷하게"], ["more upfront", "cut through"], ["뒤에 묻혀 있어"]),
    "back": (["뒤로 보내", "덜 튀게"], ["further back"], ["너무 튀어"]),
    "smoother": (["부드럽게", "매끄럽게"], ["smoother"], []),
}
AMOUNTS_KO = [("조금 ", 0.5), ("살짝 ", 0.5), ("약간 ", 0.5), ("", 1.0), ("더 ", 1.0), ("많이 ", 1.5), ("훨씬 ", 1.5)]
AMOUNTS_EN = [("a bit ", 0.5), ("slightly ", 0.5), ("", 1.0), ("a lot ", 1.5), ("much ", 1.5)]

VIEWS = {
    "waterfall": ["waterfall 보여줘", "워터폴 그래프 띄워줘", "스펙트로그램 보여줘", "show the waterfall"],
    "rta": ["RTA 보여줘", "스펙트럼 분석 그래프 보여줘", "주파수 그래프 띄워줘", "show the spectrum"],
    "meters": ["볼륨 미터 보여줘", "레벨 미터 띄워줘", "피크 미터 보여줘", "show the meters"],
    "loudness": ["라우드니스 그래프 보여줘", "LUFS 보여줘", "show loudness"],
    "correlation": ["위상 상관 보여줘", "correlation 그래프 띄워줘", "show the phase correlation"],
}
COMMANDS = {
    "undo": ["방금 거 되돌려줘", "원래대로 해줘", "취소해줘", "undo that"],
    "automix_start": ["전체 믹스 시작해줘", "믹싱해줘", "곡 전체를 믹스해줘", "mix the whole song"],
    "automix_stop": ["자동 믹스 꺼줘", "이제 그만해", "stop mixing"],
    "match_reference": ["레퍼런스 곡처럼 맞춰줘", "업로드한 곡 스타일로 맞춰줘", "match the reference"],
    "plan_chain": ["플러그인 체인 짜줘", "플러그인 순서 계획해줘", "plan the plugin chain"],
    "status": ["메모리 상태 알려줘", "모듈 상태 보여줘", "show the status"],
}
FEEDBACK = {
    "feedback_ok": ["좋아요", "딱 좋아", "완벽해", "good"],
    "feedback_more": ["조금 더", "더 해줘", "more"],
    "feedback_less": ["너무 과해요", "덜 해줘", "too much"],
}
VIEW_REPLY = {"waterfall": "워터폴 그래프를 열게요.", "rta": "RTA 스펙트럼을 열게요.", "meters": "볼륨 미터를 열게요.",
              "loudness": "라우드니스 그래프를 열게요.", "correlation": "위상 상관 그래프를 열게요."}


def session(rng):
    roles = rng.sample(sorted(CHANNELS), rng.randint(3, 8))
    chans = []
    for r in roles:
        name = rng.choice(CHANNELS[r])
        cid = "%08x" % rng.getrandbits(32)
        desc = ""
        if rng.random() < 0.6:
            d = rng.sample(DESCRIPTORS, rng.randint(1, 2))
            desc = ", ".join("%s (%s %+.2f)" % (k, e, rng.uniform(0.4, 1.4)) for k, e in d)
        chans.append({"role": r, "name": name, "id": cid, "desc": desc, "protected": rng.random() < 0.08,
                      "style": rng.choice(["", "", "", "AI 기본 스타일", "밝고 앞으로(팝)"])})
    master = {"role": "master", "name": "Master", "id": "%08x" % rng.getrandbits(32), "desc": "", "protected": False, "style": ""}
    return [master] + chans


def user_prompt(chans, text, pending=""):
    s = "[세션 채널]\n"
    for c in chans:
        s += "- %s (id %s, %s)" % (c["name"], c["id"], c["role"])
        if c["protected"]:
            s += " [보호됨]"
        if c["style"]:
            s += " 스타일: " + c["style"]
        if c["desc"]:
            s += " 들리는 상태: " + c["desc"]
        s += "\n"
    if pending:
        s += "[AI가 방금 한 질문] " + pending + "\n"
    return s + "[요청] " + text


def answer(reply="", targets=(), goals=(), question="", options=(), view="", command=""):
    return json.dumps({"reply": reply[:150], "targets": list(targets),
                       "goals": [{"goal": g, "amount": a} for g, a in goals],
                       "question": question[:150], "options": list(options)[:6], "view": view, "command": command},
                      ensure_ascii=False, separators=(",", ":"))


def example(rng, chans):
    tracks = [c for c in chans if c["role"] != "master"]
    kind = rng.choices(["goal", "complaint", "multi", "english", "ambiguous_target", "vague", "view", "command", "feedback"],
                       weights=[24, 14, 8, 10, 8, 5, 10, 11, 10])[0]
    target = rng.choice(tracks)
    word = rng.choice(KO_ROLE[target["role"]] + [target["name"]])
    eng = rng.choice(EN_ROLE[target["role"]])

    if kind == "goal":
        goal = rng.choice(sorted(GOALS))
        prefix, amount = rng.choice(AMOUNTS_KO)
        phrase = rng.choice(GOALS[goal][0])
        text = rng.choice(["%s %s%s 해줘", "%s을(를) %s%s 만들어줘", "%s 소리가 %s%s 났으면 좋겠어", "%s %s%s 바꿔줄래?"]) % (word, prefix, phrase)
        if target["role"] in ("kick", "snare", "hihat") and rng.random() < 0.3:
            text = "드럼의 " + text
        return text, answer("%s을(를) %s%s 바꿀게요." % (target["name"], prefix, phrase), [target["name"]], [(goal, amount)]), ""

    if kind == "complaint":
        goal = rng.choice([g for g in GOALS if GOALS[g][2]])
        text = "%s %s" % (word, rng.choice(GOALS[goal][2]))
        return text, answer("%s의 문제를 고쳐볼게요." % target["name"], [target["name"]], [(goal, 1.0)]), ""

    if kind == "multi":
        g1, g2 = rng.sample(sorted(GOALS), 2)
        if GOALS[g1][0] and GOALS[g2][0]:
            text = "%s 좀 %s, 그리고 %s 해줘" % (word, rng.choice(GOALS[g1][0]), rng.choice(GOALS[g2][0]))
            return text, answer("%s을(를) 두 가지 방향으로 조정할게요." % target["name"], [target["name"]], [(g1, 1.0), (g2, 1.0)]), ""

    if kind == "english":
        goal = rng.choice(sorted(GOALS))
        prefix, amount = rng.choice(AMOUNTS_EN)
        text = rng.choice(["make the %s %s%s", "can the %s sound %s%s?", "I want the %s %s%s"]) % (eng, prefix, rng.choice(GOALS[goal][1]))
        return text, answer("%s을(를) 조정할게요." % target["name"], [target["name"]], [(goal, amount)]), ""

    if kind == "ambiguous_target":
        goal = rng.choice(sorted(GOALS))
        text = rng.choice(["좀 더 %s 해줘", "%s 해줘"]) % rng.choice(GOALS[goal][0])
        return text, answer("", [], [], "어느 채널을 말씀하시는 건가요?", [c["name"] for c in tracks][:6]), ""

    if kind == "vague":
        text = rng.choice(["좀 이상해", "더 좋게 해줘", "뭔가 아쉬워", "느낌이 별로야"])
        if rng.random() < 0.5:
            text = word + " " + text
        return text, answer("", [], [], "어떤 느낌으로 바꾸고 싶으세요?", ["더 단단하게", "더 밝게", "더 따뜻하게", "덜 탁하게", "더 크게"]), ""

    if kind == "view":
        view = rng.choice(sorted(VIEWS))
        return rng.choice(VIEWS[view]), answer(VIEW_REPLY[view], view=view), ""

    if kind == "command":
        cmd = rng.choice(sorted(COMMANDS))
        return rng.choice(COMMANDS[cmd]), answer("알겠어요.", command=cmd), ""

    # feedback on the previous change
    fb = rng.choice(sorted(FEEDBACK))
    pending = "다시 들어보니 %s이(가) 단단하게 바뀌었어요. 어떠세요?" % target["name"]
    return rng.choice(FEEDBACK[fb]), answer("알겠어요.", command=fb), pending


def build(n, seed):
    rng = random.Random(seed)
    out = []
    while len(out) < n:
        chans = session(rng)
        r = example(rng, chans)
        if r is None:
            continue
        text, ans, pending = r
        out.append({"messages": [{"role": "system", "content": SYSTEM},
                                 {"role": "user", "content": user_prompt(chans, text, pending)},
                                 {"role": "assistant", "content": ans}]})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data")
    ap.add_argument("--n", type=int, default=8000)
    ap.add_argument("--test", type=int, default=300)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    data = build(a.n + a.test, a.seed)
    for name, part in (("train", data[: a.n]), ("test", data[a.n:])):
        with open(os.path.join(a.out, name + ".jsonl"), "w", encoding="utf-8") as f:
            for ex in part:
                f.write(json.dumps(ex, ensure_ascii=False) + "\n")
    print("train %d, test %d -> %s" % (a.n, a.test, a.out))


if __name__ == "__main__":
    main()
