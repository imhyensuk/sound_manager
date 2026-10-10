# Logic Pro에서 쓰기

## 1. 설치

1. GitHub 저장소의 **Actions** 탭에서 가장 최근의 초록색(성공) `build` 실행을 엽니다.
2. 아래쪽 **Artifacts**에서 `SoundManagerAI-macOS-LogicPro`를 내려받아 압축을 풉니다.
   - 폴더 안에는 `Sound Manager AI.component`(AU), `.vst3`, Standalone 앱, `install.command`, 학습 도구(`smix_learn_mix` 등)가 들어 있습니다.
3. `install.command`를 **오른쪽 클릭 › 열기**로 실행합니다.
   - 개발자 서명이 없는 파일이라 처음에는 오른쪽 클릭으로 열어야 합니다.
   - 실행하면 이렇게 됩니다.
     - AU를 `~/Library/Audio/Plug-Ins/Components/`에 복사합니다.
     - 다운로드 격리 표시를 지우고, 이 Mac에서 서명(ad-hoc)합니다.
     - Apple 검증(`auval`)을 실행합니다.
     - 모델 폴더(`~/Library/Application Support/SoundManagerAI/models`)를 만들고 Finder로 엽니다.
4. Logic Pro를 엽니다. 이미 열려 있었다면 **설정 › 플러그인 관리자**에서 Sound Manager AI를 선택하고 **재설정 및 다시 스캔**합니다.

요구 사항은 macOS 12 이상, Apple Silicon(arm64)이에요. Intel Mac용은 현재 CI에서 만들지 않습니다.

## 2. 채널에 넣기

| 넣을 곳 | 역할 |
|---|---|
| 각 트랙(오디오/소프트웨어 악기)의 Audio FX 슬롯 | 트랙: 자기 채널만 |
| Aux 채널(버스), 트랙 스택(Summing Stack) | 버스: 아래로 연결된 채널들 |
| **Stereo Out** | 마스터: 전체 |

- 플러그인 위치: Audio FX › Audio Units › Sound Manager › **Sound Manager AI**
- 트랙 이름은 Logic이 플러그인에 알려 줍니다. "Kick", "여성 보컬"처럼 지어 두면 역할을 바로 알아요. "Audio 3" 같은 이름이면 플러그인이 이름을 물어봅니다.
- 모노 트랙은 "모노" 또는 "모노 → 스테레오"로 넣을 수 있습니다.
- 버스 연결은 이름으로 추정합니다(드럼 → Drum Bus, 보컬 → Vocal Bus). 다르면 믹스 탭에서 직접 고릅니다.

## 3. Logic에서 AI가 바꿀 수 있는 것

Logic의 기본 플러그인(Channel EQ, Compressor 등)은 Logic 내부 플러그인이라 다른 플러그인이 불러오거나 조작할 수 없습니다. 그래서 Sound Manager는 **자체 내장 처리기**를 씁니다. 이 처리기는 Sound Manager AI 안에서 동작합니다.

| 내장 처리기 | 하는 일 |
|---|---|
| SM Gain | 레벨, 팬, 스테레오 폭, 위상 반전 |
| SM Gate | 번짐·잡음 제거 (스레숄드, 레인지, 어택, 홀드, 릴리즈, 키 필터) |
| SM EQ | 로우컷, 로우/하이 셸프, 피크 4밴드, 하이컷 |
| SM De-Esser | 치찰음 대역만 순간적으로 줄임 |
| SM Compressor | 스레숄드, 비율, 어택, 릴리즈, 니, 메이크업, 믹스(병렬) |
| SM Saturation | 테이프/튜브/클립, 드라이브, 톤, 믹스 |
| SM Enveloper | 트랜지언트 셰이퍼: 어택·서스테인 따로 |
| SM Reverb | 프리딜레이, 길이, 크기, 댐핑, 로우컷, 믹스, 폭 |
| SM Limiter | 인풋 게인, 실링, 릴리즈 |

- 직접 산 서드파티 AU 플러그인(FabFilter, Waves 등)도 Sound Manager 안에 호스팅해서 쓸 수 있습니다. 플러그인 탭에서 체크하면 그 종류의 내장 처리기보다 먼저 쓰입니다.
- AI가 바꾼 값은 체인 탭에서 확인하고 직접 고칠 수 있습니다. 보호를 켜면 AI가 손대지 않아요.
- 내장 처리기 창을 열면 JUCE 기본 편집기(슬라이더)가 표시됩니다.

## 4. 처음 쓰는 순서

1. 학습한 `CCM.smxgenre`(와 있으면 `ear.smxear`, `smix-intent.gguf`)를 모델 폴더에 넣습니다. 만드는 방법은 [training/DATA.md](../training/DATA.md)를 보세요.
2. 모든 트랙, 버스, Stereo Out에 Sound Manager AI를 넣고 곡을 재생합니다.
3. Stereo Out 인스턴스의 **모듈·설정** 탭에서 장르 프로필이 "CCM"인지, 악기별 학습 결과가 보이는지 확인합니다.
4. 채팅에서 "전체 믹스 시작해줘"라고 합니다.
   - AI가 장르 프로필에 맞춰 악기마다 처리기를 넣고, 학습된 설정으로 시작합니다.
   - 그다음 들으면서 조정합니다.

## 5. 여러 프로세스 연결

Logic이 일부 플러그인을 별도 프로세스에서 실행하는 경우가 있어요(예: Rosetta로 실행되는 플러그인, AUv3). 이때도 인스턴스끼리 서로를 봅니다.

- 이 Mac 안에서만(127.0.0.1) 통신하며, 인터넷은 쓰지 않습니다.
- 끄려면 **모듈·설정** 탭에서 "다른 프로세스의 Sound Manager와 연결"을 해제합니다.
- 상태(연결된 프로세스 수, 원격 채널 수)는 같은 탭에 표시됩니다.

## 6. 문제 해결

| 증상 | 해결 |
|---|---|
| Logic 플러그인 목록에 없음 | 플러그인 관리자에서 다시 스캔합니다. 터미널에서 `auval -v aufx Smx1 Smgr`를 실행해 결과를 확인합니다. |
| "손상되었거나 확인되지 않은 개발자" | `install.command`를 다시 실행하거나 `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Sound Manager AI.component"` |
| 장르 프로필이 "없음" | 파일 확장자가 `.smxgenre`인지, 모델 폴더 위치가 맞는지 확인합니다. |
| 분석 도우미를 찾을 수 없음 | `.component` 안의 `Contents/MacOS/SoundManagerProfiler`가 그대로 있는지 확인합니다(설치 스크립트로 다시 설치). |
