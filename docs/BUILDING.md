# 빌드와 설치

## 준비물

| OS | 필요 |
|---|---|
| 공통 | CMake ≥ 3.22, C++17 컴파일러, 인터넷(첫 configure 시 JUCE 8.0.4 / nlohmann_json / doctest 자동 다운로드) |
| macOS | Xcode 15+ (AU·VST3·Standalone) |
| Windows | Visual Studio 2022 (VST3·Standalone, `/utf-8`는 CMake가 자동 지정) |
| Linux | `libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxrandr-dev libxcursor-dev libxinerama-dev libxext-dev libcurl4-openssl-dev libgl1-mesa-dev` |

## 빌드

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

옵션:

| 옵션 | 기본 | 설명 |
|---|---|---|
| `SMIX_BUILD_PLUGIN` | ON | JUCE 플러그인 빌드. OFF면 코어 엔진과 테스트만 빌드 |
| `SMIX_BUILD_TESTS` | ON | 코어 단위 테스트(doctest) |
| `SMIX_BUILD_INTEGRATION_TEST` | OFF | 테스트용 VST3 EQ/컴프를 빌드해 실제 호스팅·채팅·저장·자동 믹스 검증 (Linux에서는 `xvfb-run` 필요) |
| `SMIX_AAX_SDK_PATH` | (없음) | Avid AAX SDK 경로를 주면 AAX 포맷 추가 |

## 설치 위치

| 포맷 | macOS | Windows | Linux |
|---|---|---|---|
| VST3 | `~/Library/Audio/Plug-Ins/VST3` | `C:\Program Files\Common Files\VST3` | `~/.vst3` |
| AU | `~/Library/Audio/Plug-Ins/Components` | – | – |
| AAX | `/Library/Application Support/Avid/Audio/Plug-Ins` | `C:\Program Files\Common Files\Avid\Audio\Plug-Ins` | – |
| LV2 | – | – | `~/.lv2` |

DAW별 참고:
- **Logic Pro**: AU를 사용합니다. 설치 후 Plug-in Manager에서 재검사하세요. AU 안에서 다른 AU를 호스팅하므로 플러그인 탭에서 한 번 스캔해야 합니다.
- **Pro Tools**: AAX 서명(PACE iLok)이 없으면 개발용 Pro Tools Developer 빌드에서만 로드됩니다.
- **FL Studio / Ableton / Cubase / Audacity**: VST3를 사용합니다. Audacity 3.2 이상은 VST3 실시간 이펙트를 지원합니다.

## CI

`.github/workflows/build.yml`이 Linux, macOS, Windows에서 빌드하고 테스트를 돌린 뒤 플러그인 바이너리를 아티팩트로 올립니다.

## 데이터 위치

`SoundManagerAI` 폴더(macOS `~/Library/Application Support`, Windows `%APPDATA%`, Linux `~/.config`)에 다음 파일이 저장됩니다.
- `known-plugins.xml`: 스캔 결과
- `catalog.json`: 허용 목록, 종류 수정, 우선순위
- `SoundManagerAI.settings`: API 키(평문), 모델, effort
