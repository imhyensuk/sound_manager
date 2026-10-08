# 빌드와 설치

## 준비물

| | 필요 |
|---|---|
| 공통 | CMake ≥ 3.22, C++17 컴파일러. 첫 configure 때 JUCE 8.0.4, llama.cpp b11121, memopro, nlohmann_json, doctest를 받습니다(빌드할 때만 인터넷 사용, 플러그인은 오프라인). |
| memopro | Rust 1.85 이상(`cargo`). 없으면 대체 할당기로 자동 전환됩니다. |
| 테스트 | Python 3 + numpy, pyyaml (테스트용 초소형 GGUF 생성) |
| macOS | Xcode 15+ (AU·VST3·Standalone; Metal 가속은 `-DGGML_METAL=ON`이 기본) |
| Windows | Visual Studio 2022 (VST3·Standalone). memopro는 아직 Windows를 지원하지 않아 대체 할당기를 씁니다. |
| Linux | `libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev libxcursor-dev libxinerama-dev libxext-dev libgl1-mesa-dev` |

## 빌드

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

| 옵션 | 기본 | 설명 |
|---|---|---|
| `SMIX_BUILD_PLUGIN` | ON | JUCE 플러그인과 프로파일러 헬퍼 |
| `SMIX_BUILD_TESTS` | ON | 코어 단위 테스트 |
| `SMIX_WITH_MEMOPRO` | ON | memopro 메모리 런타임 (`SMIX_MEMOPRO_SOURCE_DIR`로 로컬 체크아웃 지정 가능) |
| `SMIX_WITH_LLAMA` | ON | 로컬 언어 모델(llama.cpp, 네트워크 기능 없이 빌드) |
| `SMIX_BUILD_INTEGRATION_TEST` | OFF | 테스트용 VST3로 실제 호스팅 전 과정 검증 (Linux는 `xvfb-run`) |
| `SMIX_AAX_SDK_PATH` | (없음) | AAX(Pro Tools) 빌드 |

## 설치

플러그인 번들을 통째로 복사합니다. 번들 안의 `SoundManagerProfiler`가 플러그인 분석에 필요합니다.

| 포맷 | macOS | Windows | Linux |
|---|---|---|---|
| VST3 | `~/Library/Audio/Plug-Ins/VST3` | `C:\Program Files\Common Files\VST3` | `~/.vst3` |
| AU | `~/Library/Audio/Plug-Ins/Components` | – | – |
| AAX | `/Library/Application Support/Avid/Audio/Plug-Ins` | `C:\Program Files\Common Files\Avid\Audio\Plug-Ins` | – |
| LV2 | – | – | `~/.lv2` |

## 데이터 위치

`SoundManagerAI` 폴더(macOS `~/Library/Application Support`, Windows `%APPDATA%`, Linux `~/.config`)에 다음이 들어갑니다.

- `models/`
  - `*.gguf`: 로컬 언어 모델. 첫 번째 파일이 쓰이며, 설정 탭에서 다른 파일을 지정할 수 있습니다.
  - `ear.smxear`: 악기 인식 모델
- `known-plugins.xml`, `catalog.json`: 스캔 결과, 허용 목록, 종류 수정
- `knowledge.jsonl`: 플러그인 지식
- `references.json`: 분석된 레퍼런스
- `SoundManagerAI.settings`: 모델 경로, 메모리 상한, 모듈 on/off

## CI

`.github/workflows/build.yml`이 Linux, macOS, Windows에서 Rust 툴체인과 함께 빌드하고 코어 테스트(초소형 GGUF 포함)를 돌립니다. Linux에서는 호스팅 통합 테스트도 돌린 뒤 플러그인을 아티팩트로 올립니다.
