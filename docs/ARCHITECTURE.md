# 아키텍처

## 1. 왜 "플러그인 안의 플러그인"인가

DAW 플러그인 API(VST3/AU/AAX)는 플러그인에게 **자기 입력 오디오**만 줍니다. 그래서 플러그인은 다음을 할 수 없습니다.

- 다른 트랙의 오디오를 듣기
- 다른 트랙에 꽂힌 플러그인의 값을 바꾸기
- DAW 페이더를 움직이기

그래서 Sound Manager는 두 가지 방식을 씁니다.

1. **호스팅**: 사용자가 허용한 서드파티 플러그인을 Sound Manager 인스턴스 안에 직접 불러옵니다(`HostedChain`, JUCE `AudioPluginFormatManager`). 순서, 바이패스, 모든 파라미터를 AI가 제어할 수 있습니다.
2. **연결된 인스턴스**: 모든 채널에 Sound Manager를 넣으면 같은 프로세스 안의 인스턴스들이 `SessionHub`에서 하나의 `MixSession`으로 합쳐집니다. iZotope Neutron/Relay나 Sonible 그룹 기능과 같은 방식입니다. 마스터/버스 인스턴스는 하위 채널 인스턴스의 체인과 AI 게인을 조작합니다.

## 2. 범위 규칙 (요구사항 2)

`smix::MixSession::scopeOf(rootId)`가 범위를 정합니다.

| 인스턴스 위치 | 범위 |
|---|---|
| Track | 자기 자신 |
| Bus | 자기 자신 + `parentId` 체인을 따라 이 버스에 도달하는 모든 채널 |
| Master | 세션의 모든 채널 |

`ActionExecutor`는 모든 동작에 대해 대상 채널이 요청한 인스턴스의 범위 안에 있는지 검사합니다. 예를 들어 드럼 버스 인스턴스는 보컬을 바꿀 수 없습니다. 자동 믹스는 가장 가까운 상위 인스턴스가 담당하고, 하위 인스턴스의 자동 믹스는 그동안 쉽니다. 그래서 두 인스턴스가 같은 채널을 동시에 조작하지 않습니다.

**라우팅 정보**: DAW는 "이 트랙이 어느 버스로 가는지" 플러그인에 알려주지 않습니다. 현재 방식은 다음과 같습니다.
- *Auto*: 드럼 역할 트랙은 유일한 드럼 버스 인스턴스로, 보컬은 이름에 vocal/vox/보컬이 들어간 버스로, 나머지는 마스터로 추정합니다.
- 수동: 믹스 탭의 *출력 대상(버스)*에서 지정합니다.
- 로드맵: 하위 채널 출력과 버스 입력의 상호상관으로 라우팅을 자동 확인합니다.

## 3. "귀" — 분석과 지각 (요구사항 7)

`AudioAnalyzer`는 오디오 스레드에서 동작하며 할당이나 락이 없습니다. 100 ms마다 lock-free triple buffer로 결과를 내보냅니다.

- ITU-R BS.1770 K-weighting 라우드니스(단기 3 s, 게이트 적분), 라우드니스 레인지
- 2048점 FFT 기반 10개 대역 에너지 비율: sub · bass · low_mid · mud · mid · upper_mid · presence · bite · sibilance · air
- 피크, RMS, 크레스트 팩터, 스펙트럼 중심, 평탄도
- 스펙트럼 플럭스 온셋: 초당 트랜지언트 수와 강도
- 스테레오 상관도와 폭(side/mid)

`PerceptualProfile`은 역할별 기준 곡선(킥, 보컬, 하이햇…)과의 편차로 *boomy, muddy, boxy, harsh, sibilant, bright, thin, punchy, squashed, wide, phase_issue* 점수를 냅니다(±1이면 확실히 들리는 정도). Claude는 이 수치와 묘사를 매 요청마다 `<session_snapshot>`으로 받고, 변경 후에는 `get_channel_analysis`로 "다시 듣습니다".

## 4. 파라미터 이해

- `ParamSemantics`는 이름에서 역할을 추정합니다. 예: "Band 3 Gain"은 `band_gain`(band 3), "Attack"은 컴프레서에서 `attack`, 트랜지언트 셰이퍼에서는 `transient_attack`입니다.
- `ValueMapper`는 파라미터의 `getText(v)`를 0~1 구간 33개 지점에서 샘플링해 단조 곡선을 만듭니다. 주파수·시간은 로그 보간합니다. 실제 단위 값(380 Hz, -2.5 dB, 4:1, 20 ms)을 정규화 값으로 정확히 바꿉니다.

## 5. 동작의 흐름

```
채팅/자동 믹스/버튼
   └─▶ MixAction[] (set_param, nudge_param, set_gain, nudge_gain, set_bypass, set_chain, move_slot)
         └─▶ ActionExecutor
               ├─ 범위 검사, 허용 목록 검사(set_chain), 레벨 잠금
               ├─ 단위 → 정규화 (ValueMapper), 스텝 파라미터 반올림
               ├─ 안전 한계: 파라미터 ±0.5 정규화/동작, 게인 ±6 dB/동작, 범위 -24..+12 dB
               └─▶ MixController (SessionHub) → 해당 인스턴스
                     ├─ 연속 파라미터: 300 ms 램프 (지퍼 노이즈 방지)
                     └─ 체인 교체: 비동기 로드 후 lock-free swap, 기존 플러그인 상태 유지
```

## 6. Claude 연동

- 엔드포인트: `POST /v1/messages`. 모델 기본값은 `claude-opus-5-5`이고 설정 탭에서 바꿀 수 있습니다.
- `thinking: {type: "adaptive"}`, `output_config.effort`(기본 medium)를 사용합니다.
- 거절 시 다른 모델로 넘기는 `fallbacks: "default"`(beta `server-side-fallback-2026-07-01`)를 사용합니다.
- 시스템 프롬프트는 고정 문자열이고 `cache_control`로 캐시합니다. 변하는 세션 스냅샷은 사용자 메시지에 넣습니다.
- 도구는 `apply_mix_actions`, `get_plugin_parameters`, `get_channel_analysis`, `list_allowed_plugins`, `suggest_chain`입니다.
- 대화 기록은 append-only입니다. 응답 content는 thinking 블록을 포함해 그대로 다시 보냅니다. `stop_reason: refusal`은 기록에 넣지 않습니다.
- 에이전트는 워커 스레드에서 돌고, 세션 접근은 메시지 스레드로 마샬링됩니다. 인스턴스가 삭제되면 `alive` 플래그로 안전하게 끊깁니다.
- 키가 없으면 `LocalIntentInterpreter`가 대신합니다. 한국어/영어 대상 인식("드럼의 킥" → 킥), 정도("조금", "많이"), 부정("너무 밝아" → 어둡게)을 처리하고, `RecipeEngine`의 엔지니어링 레시피를 적용합니다.

## 7. 스레드 모델

| 스레드 | 하는 일 |
|---|---|
| 오디오 | `HostedChain::process`(try-lock, swap 중에는 통과), AI 게인 스무딩, 분석기 |
| 메시지 | SessionHub 타이머(0.5 s 새로고침, 2 s 자동 믹스), 파라미터 램프, 체인 교체, UI |
| 에이전트 워커 | HTTP 요청, 도구 루프(세션 접근은 메시지 스레드로 위임) |
| 스캔 | 플러그인 디렉터리 스캔(dead-man's-pedal 파일로 크래시 플러그인 차단) |

## 8. 다중 프로세스 호스트용 IPC 계획

`SessionHub`의 인터페이스(`refresh`, `apply`, `MixController`)는 전송 계층과 분리되어 있습니다. 샌드박스 호스트를 지원하려면 다음이 필요합니다.
1. 첫 인스턴스가 `juce::InterprocessConnectionServer`(localhost)를 띄웁니다. 나머지는 접속해 `ChannelState` JSON을 주기적으로 발행합니다.
2. 원격 인스턴스로 가는 `MixAction`은 JSON으로 직렬화해 소유 인스턴스로 보내고, 소유 인스턴스는 자기 `ActionExecutor`로 실행합니다.
3. 프로젝트 식별자는 DAW 프로젝트별로 고유한 세션 토큰으로, 마스터 인스턴스 상태에 저장합니다.
