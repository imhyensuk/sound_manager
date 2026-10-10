# 모델 학습 (Colab T4)

플러그인은 모델 없이도 규칙 기반으로 동작합니다. 아래 두 모델을 넣으면 더 잘 이해하고 더 잘 듣습니다. 두 모델 모두 사용자 컴퓨터에서만 실행됩니다.

## 1. 요구·문맥 추론 모듈 (로컬 LLM → GGUF)

| 단계 | 파일 |
|---|---|
| 데이터 | `intent/make_dataset.py`: 플러그인이 모델에 보내는 것과 **똑같은** 형식의 예제를 생성합니다. 시스템 프롬프트(`system_prompt.txt`)와 출력 문법(`grammar.gbnf`)은 C++에서 내보낸 것이고, 테스트가 일치를 검사합니다. 예제 종류는 대상 지정 요청, 불만 표현, 다중 목표, 영어, 모호한 대상(되묻기), 막연한 요청(되묻기), 그래프, 명령, 피드백입니다. |
| 학습 | `intent/finetune_t4.py`, `finetune_t4.ipynb`: `memopro.finetune`이 16비트 가중치를 파일에서 스트리밍하며 LoRA를 학습합니다. T4(15 GB)에서 `--budget 4GiB`로 1.5B~3B 모델을 학습할 수 있습니다. `--no-memopro`면 일반 PEFT로 학습합니다. |
| 변환 | 같은 스크립트가 어댑터를 병합하고, llama.cpp(플러그인과 같은 b11121)로 GGUF 변환과 `Q4_K_M` 양자화를 합니다. |
| 평가 | `smix_intent_eval out/smix-intent.gguf data/test.jsonl`: 플러그인과 같은 프롬프트, 문법, 탐욕적 디코딩으로 대상/목표/그래프/명령 정확도와 속도를 측정합니다. |
| 설치 | `smix-intent.gguf`를 `SoundManagerAI/models/`에 넣습니다. |

권장 기반 모델은 `Qwen/Qwen2.5-1.5B-Instruct`(Q4_K_M 약 1 GB, CPU에서 실시간에 가까움)이고, 품질이 더 필요하면 3B를 씁니다.

## 2. 장르 프로필 (내 곡의 멀티트랙 + 완성 믹스 → 장르 믹싱 기준)

> 원본과 처리된 트랙을 함께 주면 악기별 **처리 방식**(게이트, EQ, 컴프 등)까지 배웁니다. 폴더 구조와 Logic에서 내보내는 방법은 [DATA.md](DATA.md)를 보세요. 아래는 원본 + 믹스만 쓰는 간단 모드 설명입니다.

믹싱 전 멀티트랙과 완성 믹스가 함께 있는 곡이 있으면, 그 장르(예: CCM)에서 **악기별 볼륨 밸런스**, **믹스 안에서의 악기별 톤 곡선**, **마스터 사운드**를 학습할 수 있습니다. 데이터가 작아서 Colab이 필요 없고 PC에서 몇 분이면 끝납니다. 8곡 이상을 권장합니다.

```
songs/곡1/  01_Kick.wav  02_Snare.wav  여성 보컬.wav  메인건반.wav ...  곡1_Mix.wav
songs/곡2/  ...
```

- 곡마다 폴더 하나에 넣습니다. 파일 이름에 `mix`/`master`/`final`/`믹스`/`마스터`/`완성`이 들어간 WAV가 완성 믹스이고, 나머지는 원본 트랙입니다. 다른 단어를 쓰려면 `--mix-word 단어`를 붙이세요.
- 원본 트랙과 완성 믹스는 **곡 처음부터 같은 위치로 정렬**되어 있어야 합니다. DAW에서 0마디부터 같은 길이로 내보내면 됩니다. 샘플레이트도 같아야 합니다.
- 클릭, 가이드, 큐, 토크백 트랙은 이름으로 자동 제외됩니다.

```bash
cmake --build build --target smix_learn_mix smix_ear_features
./build/core/smix_learn_mix --genre CCM --out CCM.smxgenre songs/
```

1. 처음 실행하면 `tracks.csv`(파일,라벨)가 생깁니다. 라벨은 파일 이름에서 추정합니다. 틀린 라벨을 고치고(빼려는 트랙은 `skip`) 다시 실행하면 수정한 라벨이 적용됩니다. 이때 다음 라벨 초안은 `tracks.csv.new`에 저장됩니다.
2. 출력에서 곡마다 **reconstruction fit**을 확인하세요. 원본 트랙들로 완성 믹스를 얼마나 설명했는지를 나타냅니다. 낮다면 리버브 리턴이나 버스 처리처럼 원본에 없는 소리가 크다는 뜻입니다. 정렬이 어긋나거나 믹스 파일을 잘못 고른 곡도 낮게 나오니 그런 곡은 빼세요.
3. 만든 `CCM.smxgenre`를 `SoundManagerAI/models/`에 넣습니다. 플러그인의 **모듈·설정** 탭에서 장르 프로필을 고를 수 있습니다(기본은 첫 번째 프로필).
4. 같은 실행에서 `ear-tracks.csv`(라벨,경로)도 만들어집니다. 이 파일을 쓰면 곡 폴더를 악기별로 다시 정리하지 않고 3장의 악기 인식 모델을 학습할 수 있습니다.
   ```bash
   ./build/core/smix_ear_features features.csv --list ear-tracks.csv
   python training/ear/train_ear.py features.csv ear.smxear
   ```

장르 프로필은 이렇게 쓰입니다.
- 볼륨 밸런스: 자동 믹스의 악기별 목표 레벨이 리드 보컬 대비 LU로 바뀝니다.
- 톤 곡선: "쏜다", "탁하다" 같은 판단의 기준이 바뀝니다.
- 마스터: 레퍼런스를 고르지 않았을 때 기본 레퍼런스로 쓰입니다.

트랙이 2개 미만인 역할은 기본 규칙을 그대로 씁니다.

## 3. 악기 인식 모델 (채널 이름 후보)

가지고 계신 스템을 **클래스마다 폴더 하나**로 정리합니다. 폴더 이름이 그대로 플러그인의 이름 후보 버튼에 표시되므로 한글 이름을 써도 됩니다.

```
stems/남성 보컬/곡1.wav, 곡2.wav ...   stems/여성 보컬/   stems/킥/   stems/스네어/
stems/메인건반/   stems/세컨건반/   stems/서드건반/   stems/어쿠스틱 기타/   stems/일렉 기타/
stems/베이스 기타/   stems/Pad/   stems/Tom1/   stems/Tom2/   stems/Tom3/   stems/HiHat/   stems/OverHead/
```

```bash
cmake --build build --target smix_ear_features smix_ear_classify
./build/core/smix_ear_features features.csv --dir stems/       # 무음 구간은 자동 제외
python training/ear/train_ear.py features.csv ear.smxear       # 검증 정확도 + 혼동 행렬 출력
./build/core/smix_ear_classify ear.smxear 새곡_보컬.wav         # 확인
```

- **WAV만 읽습니다.** FLAC·AIFF 등은 먼저 변환하세요: `ffmpeg -i in.flac out.wav`.
- **무음 제외:** 3초 구간 중 -50 dBFS보다 작거나 소리가 나는 비율이 15% 미만인 구간은 학습에서 빠집니다. 기준은 `--silence-db`로 바꿉니다.
- **파일 단위 검증:** 같은 곡의 구간이 학습과 검증에 동시에 들어가지 않아서, 정확도가 실제 새 곡에서의 성능에 가깝습니다. 클래스마다 파일이 2개 이상이어야 검증에 쓰입니다.
- **클래스 균형:** 구간 수가 적은 클래스(예: Tom)에 가중치를 줍니다.
- **소리로 구분할 수 없는 클래스:** 메인/세컨/서드 건반은 편곡상의 역할이라 같은 악기면 소리만으로는 구분이 어렵습니다. 혼동 행렬의 "자주 헷갈리는 쌍"을 보고 합치세요. 남성/여성 보컬, Tom1/2/3는 음역이 달라 대개 구분됩니다.
  ```bash
  python training/ear/train_ear.py features.csv ear.smxear --merge "메인건반,세컨건반,서드건반=건반"
  ```
- **역할 연결:** 남성/여성 보컬은 리드 보컬, Tom1~3은 탐, 건반류는 건반, 베이스 기타는 베이스로 연결되어 믹싱 레시피가 적용됩니다. 화면에는 폴더 이름이 그대로 보입니다.
- **설치:** 만든 `ear.smxear`를 `SoundManagerAI/models/`에 넣습니다.

`ear/make_synthetic.py`는 파이프라인 확인용 합성 신호를 만듭니다(실사용 모델 아님).

## 4. 테스트용 초소형 GGUF

`tools/make_tiny_gguf.py`는 임의 가중치에 Qwen2 토크나이저와 채팅 템플릿을 넣은 25 MB 모델을 만듭니다. llama.cpp 저장소의 어휘 파일을 쓰므로 다운로드가 필요 없고, 단위 테스트가 빌드 중에 자동으로 생성합니다.
