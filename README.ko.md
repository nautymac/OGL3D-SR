# OGL3D

[English](README.md) · **한국어**

OGL3D는 **OpenGL** 게임을 입체 3D로 보여 줍니다. geo-11이 DirectX 11 게임에 하는 일을 OpenGL 게임에 하는 셈입니다. 게임 exe 옆에 두는 `opengl32.dll` 프록시로, 게임이 매 프레임을 양쪽 눈용으로 그리게 만든 다음 SR SDK로 두 영상을 위빙해 무안경 렌티큘러 SR 패널(Acer SpatialLabs, Leia SR 디스플레이)에 맞춥니다. SR 패널이 없으면 다른 3D 디스플레이용으로 SBS(좌우)나 TAB(상하)로 출력합니다. 64비트와 32비트 게임을 모두 지원합니다.

지금까지 테스트한 게임은 게임별 셰이더 수정이 필요 없었습니다:

- **셰이더 주입:** 게임이 정점 셰이더를 컴파일할 때 바꿔 씁니다. 고정 기능 렌더링, 즉시 모드, 디스플레이 리스트는 대신 눈별 투영 행렬을 씁니다.
- **오른쪽 눈 대상:** 창과 화면비가 같은 렌더 타깃은 오른쪽 눈용 사본을 만듭니다.
- **2D는 평면 그대로:** HUD 같은 2D 요소는 화면 깊이에 그대로 둡니다.
- **자동 컨버전스(기본값):** 양쪽 눈 영상을 측정해 가장 가까운 물체를 화면 면에 맞추고, 카메라 컷에도 따라갑니다.

DirectX 게임은 자매 프로젝트 [geo-11 SR](https://github.com/nautymac/geo-11_SR)이 맡습니다. geo-11 SR v0.3.0부터는 그 설치기로 OGL3D도 설치할 수 있습니다(종류 **OpenGL**).

## 내려받기와 설치

[Releases](../../releases)에서 **`OGL3D-v1.0.0.zip`**을 내려받습니다:

```
OGL3D\
  x64\opengl32.dll     64비트 게임용
  x86\opengl32.dll     32비트 게임용
  OGL3D.ini            설정 (모든 옵션 설명이 파일 안에 있음)
  README.md, README.ko.md, LICENSE
```

1. 게임 exe가 있는 폴더를 찾습니다. 32비트 게임은 작업 관리자에서 이름 뒤에 "(32비트)"가 붙습니다.
2. 64비트 게임이면 `x64`, 32비트 게임이면 `x86`의 `opengl32.dll`을 `OGL3D.ini`와 함께 그 폴더에 복사합니다.
3. 게임을 평소처럼 실행합니다. exe 옆에 `OGL3D.log`가 생깁니다.

제거하려면 `opengl32.dll`, `OGL3D.ini`, `OGL3D.log`를 지웁니다.

[geo-11 SR 설치기](https://github.com/nautymac/geo-11_SR)(v0.3.0 이상)를 쓰면 이 과정을 대신 해 줍니다. OpenGL 게임을 감지하고, 32/64비트를 고르고, 업스케일을 켜고 끌 수 있습니다.

OpenGL 게임인지는 exe가 `opengl32.dll`을 가져오는지, 또는 게임 설정에 OpenGL 렌더러가 있는지로 압니다. 설치기는 자동으로 확인합니다. `OGL3D.log`가 끝내 생기지 않으면 그 게임은 OpenGL을 쓰지 않는 것입니다.

## 필요한 것

| 항목 | 필요 여부 |
|---|---|
| OpenGL 게임 | OpenGL 3.x/4.x. 호환 프로필로 테스트(확인된 게임 모두); 코어 프로필도 처리하지만 테스트는 안 됨. DirectX 게임은 geo-11 SR을 쓰세요. |
| GPU | NVIDIA에서 테스트(RTX 4080 Laptop, 드라이버 610.88). 프레임버퍼 오브젝트와 블릿을 씁니다. AMD, Intel은 테스트하지 않았습니다. |
| SR 패널 + SR 런타임 | SR 위빙에만 필요합니다. Acer SpatialLabs 기기에서는 **SpatialLabs Experience Center**만 설치하면 됩니다. 64비트 런타임(`C:\Program Files\Acer\SpatialLabs\Platform\bin`)과 32비트 런타임(`C:\Program Files (x86)\Simulated Reality\Platform\bin`)을 함께 설치합니다. 런타임이 없으면 좌우 절반 SBS로 출력합니다. |
| Visual C++ 재배포 패키지 | 필요 없음(정적 링크). |
| Visual Studio, CMake, Python | 소스에서 빌드할 때만. |

게임은 패널 기본 해상도(예: 3840×2160)로 전체 화면이나 테두리 없는 창으로 실행하세요. 그렇지 않으면 렌티큘러 패턴이 맞지 않습니다. DPI를 인식하지 못하는 게임은 OGL3D가 DPI 인식으로 바꿔 기본 해상도로 그리게 합니다. 그러면 UI가 너무 작아질 수 있는데, 그때는 아래의 **업스케일** 모드를 쓰세요.

## 단축키

| 키 | 동작 |
|---|---|
| Ctrl+F3 / Ctrl+F4 | 세퍼레이션 − / + |
| Ctrl+F5 / Ctrl+F6 | 컨버전스 − / + (자동 컨버전스가 꺼짐) |
| Ctrl+F2 | 자동 컨버전스 켜기/끄기 |
| Ctrl+F7 | 세퍼레이션, 컨버전스, 셰이더 선택을 `OGL3D.ini`에 저장 |
| Ctrl+F1 | 값을 화면에 계속 표시(평소에는 조정할 때만 보임) |
| Ctrl+T | 3D 켜기/끄기 |
| Ctrl+Alt+W | SR 위빙 켜기/끄기(끄면 좌우 SBS) |
| Ctrl+F12 | 양쪽 눈을 BMP로 저장하고 깊이 보고를 로그에 기록 |
| Ctrl+F9, Ctrl+F10 / F11, Ctrl+F8 | 헌팅 모드: 화면의 셰이더를 하나씩 골라 stereo, mono(화면 깊이), skip으로 지정 |

모든 키는 `OGL3D.ini`의 `[Keys]`에서 바꿀 수 있습니다.

## 설정

`OGL3D.ini`의 주요 옵션입니다. 모든 옵션의 설명은 파일 안에 있습니다.

| 옵션 | 기본값 | 의미 |
|---|---|---|
| `[Stereo] separation` | 50 | 입체감 세기(%) |
| `[Stereo] convergence` | auto | `auto`는 장면을 따라감, 숫자는 화면 면에 놓일 깊이 |
| `[Stereo] auto_popout` | 0 | auto: 가장 가까운 물체가 화면 밖으로 튀어나와도 되는 정도(세퍼레이션의 %) |
| `[Stereo] auto_speed` | 1 | auto: 장면 변화를 따라가는 속도(2 = 빠르게, 0.5 = 차분하게) |
| `[Stereo] output` | auto | `auto`(SR 위빙, 없으면 SBS), `sr`, `sbs`, `tab`, `left`, `right` |
| `[Stereo] swap_eyes` | 0 | 왼쪽/오른쪽 바꾸기 |
| `[Display] upscale` | 0 | 게임 해상도로 그리고, 위빙은 기본 해상도로 표시 |
| `[Display] dpi_aware` | 1 | DPI를 인식하지 못하는 게임을 기본 해상도로 그리게 함 |
| `[Display] virtual_backbuffer` | 1 | 게임은 화면 밖 버퍼에 그리고, 창에는 완성된 프레임만 보냄(깜빡임 방지) |
| `[Overlay] show_values` | 0 | 값을 항상 화면에 표시 |

### 업스케일 모드

3840×2160에서는 메뉴와 글씨가 아주 작아지는 게임이 많습니다. `upscale = 1`로 하고 게임 자체는 1920×1080으로 설정하세요. 게임은 양쪽 눈을 그 해상도로 그리고, OGL3D는 게임 위의 자체 창에 패널 기본 해상도로 위빙해 보여 줍니다. 마우스 입력은 게임으로 그대로 전달되고, 해상도 변경은 막아 패널이 기본 해상도를 유지합니다.

## 확인된 게임 (2026-10-14)

| 게임 | 참고 |
|---|---|
| Curse of the Dead Gods (64비트) | 실제 깊이감, HUD는 화면 깊이. 수동 값이 잘 맞음(예: 세퍼레이션 18, 컨버전스 21.7). |
| Amnesia: The Dark Descent (32비트) | 디퍼드 렌더러, 고정 기능, 즉시 모드 모두 입체로 나옴. 권장: `upscale = 1`, 게임은 1920×1080. |
| Darkest Dungeon (64비트, `_windows\win64`) | 던전과 마을 레이어에 실제 깊이감. 인트로 영상은 평면. |
| Broken Age (32비트) | 장면마다 깊이 스케일이 다른 2.5D 레이어. 자동 컨버전스가 컷을 따라감. |

테스트 중 고친 문제: Windows가 다 그려지지 않은 백 버퍼를 보여 주던 깜빡임(가상 백 버퍼로 해결), 게임이 프레임 끝에 곱하기/더하기 블렌드를 켜 둔 채로 두면 화면이 검게 또는 하얗게 되던 문제(OGL3D 자신의 그리기를 항상 중립 상태에서 시작).

## 동작 방식

1. **프록시.** `opengl32.dll`이 System32의 진짜 DLL을 불러와 모든 함수를 전달합니다. 컨텍스트 생성, `SwapBuffers`, 그리기 호출, 클리어, 프레임버퍼와 텍스처 함수는 가로챕니다.
2. **셰이더.** 정점 셰이더마다 `main()`의 이름을 바꾸고, 새 `main()`이 `gl_Position.x`에 `눈 × 세퍼레이션 × (w − 컨버전스)`를 더합니다. `w = 1`인 형상(2D, HUD, 전체 화면 패스)은 화면 깊이에 남습니다. 주입된 셰이더가 없는 프로그램은 눈별 투영 행렬을 씁니다.
3. **오른쪽 눈.** 입체 대상(기본 프레임버퍼, 또는 창과 화면비가 같은 렌더 타깃)에 그리는 모든 호출을 오른쪽 눈 사본에 한 번 더 그립니다. 이때 읽는 텍스처도 오른쪽 눈 사본을 씁니다.
4. **합성.** `SwapBuffers` 때 양쪽 눈을 나란히 SR SDK의 OpenGL 위버에 넘깁니다(업스케일 모드에서는 DirectX 11 위버). SR이 없으면 SBS나 TAB으로 내보냅니다.
5. **자동 컨버전스.** 양쪽 눈을 GPU에서 축소해 멈춤 없이 읽어 오고, 작업 스레드에서 블록 매칭합니다. 잘 맞은 것 중 가장 가까운 물체가 컨버전스를 정합니다.

## 문제 해결

- **로그:** exe 옆의 `OGL3D.log`에 GL 버전, 주입한 셰이더, 렌더 타깃, SR 상태, 자동 컨버전스 결정, 프레임 통계, 크래시(모듈 + 오프셋 + 스택), 멈춤(`HANG?`)이 기록됩니다. 시작 직후 `DimencoWeaving*.dll`(또는 모듈 없는 주소)의 `EXCEPTION` 줄은 SR 런타임이 CPU 기능을 시험하는 것으로, 런타임이 스스로 처리하므로 그 뒤 게임이 꺼질 때만 의미가 있습니다.
- **프레임 덤프:** **Ctrl+F12**는 `OGL3D_frameNNNNN.bmp`(양쪽 눈)와 `_srinput.bmp`(위버가 받은 영상)를 저장하고, 로그에 시차 격자를 남깁니다.
- **한쪽 눈이 이상하거나 효과의 깊이가 틀릴 때:** 헌팅 모드(Ctrl+F9)를 쓰세요. 해당 셰이더까지 넘기고(선택된 것은 사라짐), Ctrl+F8로 mono나 skip으로 지정한 뒤 Ctrl+F7로 저장합니다.
- **깜빡임 추적:** `[Debug] flicker_probe = 1`. `bin\tools\flickercap.exe <초> <폴더>`는 디스플레이에 실제로 나간 화면을 기록하고(DXGI 데스크톱 복제), 한 프레임짜리 번쩍임을 이미지로 저장합니다.

## 빌드

- **준비:** C++와 CMake가 포함된 Visual Studio 2022 이상, Python 3. `tools\gen_exports.py`가 시스템 `opengl32.dll`에서 전달용 스텁을 만들고, `build.ps1`이 이를 실행합니다.
- **SR SDK:** 포함되어 있지 않습니다. `SR_SDK` 옵션이 아키텍처에 맞게 압축을 푼 SR SDK(1.34.x)를 가리키게 하세요: `build.ps1 -Arch x64 -DSR_SDK=<win64 SDK>`, `build.ps1 -Arch x86 -DSR_SDK=<win32 SDK>`. `-DSR_SDK`를 주지 않으면 `CMakeLists.txt`의 기본값을 씁니다.
- **빌드:** `build.ps1 -Arch all` → `bin\x64\opengl32.dll`, `bin\x86\opengl32.dll`.
- **패키지:** `package.ps1` → `dist\OGL3D-v<버전>.zip`.
- **flickercap:** `cmake -S tools\flickercap -B build-flickercap -A x64`, `cmake --build build-flickercap --config Release` → `bin\tools\flickercap.exe`.

## 크레딧과 라이선스

- **helifax의 [OGL-3DVision-Wrapper](https://github.com/helifax/OGL-3DVision-Wrapper)**: OpenGL 3D Vision 래퍼. OpenGL 입체 주입 방식과 게임 프로필을 참고했습니다. 코드는 가져오지 않았습니다.
- **SR SDK / SR Platform:** Leia Inc. / Acer SpatialLabs. 포함되어 있지 않으며, 설치된 SR 런타임에서 실행 시 불러옵니다.
- **[geo-11 SR](https://github.com/nautymac/geo-11_SR):** DirectX 게임용 자매 프로젝트. 그 설치기로 OGL3D도 설치할 수 있습니다.

이 저장소의 코드는 MIT 라이선스입니다. `LICENSE`를 보세요.

커뮤니티: [3D Vision Discord](https://discord.gg/zTnnMrA4GE) · [HelixMod](https://helixmod.blogspot.com/)
