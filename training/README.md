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

## 2. 악기 인식 모델 (채널 이름 후보)

```bash
cmake --build build --target smix_ear_features smix_ear_classify
./build/core/smix_ear_features features.csv --dir stems/     # stems/kick/*.wav, stems/lead_vocal/*.wav, ...
python ear/train_ear.py features.csv ear.smxear              # numpy만 사용, CPU로 충분
./build/core/smix_ear_classify ear.smxear some_track.wav
```

특징 추출은 플러그인과 같은 C++ 코드를 써서 학습과 추론의 특징이 항상 같습니다. 데이터는 자신의 멀티트랙 스템이나 공개 스템 데이터(MUSDB18, MedleyDB, Slakh 등)를 악기별 폴더로 정리해 씁니다. `ear/make_synthetic.py`는 파이프라인 확인용 합성 신호를 만듭니다(실사용 모델이 아님).

## 3. 테스트용 초소형 GGUF

`tools/make_tiny_gguf.py`는 임의 가중치에 Qwen2 토크나이저와 채팅 템플릿을 넣은 25 MB 모델을 만듭니다. llama.cpp 저장소의 어휘 파일을 쓰므로 다운로드가 필요 없고, 단위 테스트가 빌드 중에 자동으로 생성합니다.
