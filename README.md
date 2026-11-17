# NFS Hot Pursuit Remastered - DLAA mod

An ASI mod for **Need for Speed: Hot Pursuit Remastered** (PC, DirectX 11) that replaces the game's
TAA with **NVIDIA DLAA** (DLSS at native resolution).

[한국어 설명은 아래에 있습니다.](#한국어)

## Features
- Replaces the game's TAA pass with DLAA (NVIDIA DLSS SDK 310.9)
- All usable DLSS presets: Default, E, F, J, **K (default)**, L, M
- Adaptive sharpening after DLAA (AMD CAS style), 0-100 %
- Optional: disable the slow-motion crash (wreck) camera (same code patch as ZPatch's NoCrashCams)
- In-game menu (F8) in English or Korean, DLAA on/off hotkey (F9) for comparison
- Works with the Steam overlay and the current Ultimate ASI Loader

## Requirements (to play)
- NVIDIA GeForce RTX GPU (RTX 20 series or newer) and a recent driver
- Ultimate ASI Loader x64 (`dinput8.dll`) - https://github.com/ThirteenAG/Ultimate-ASI-Loader

Ready-to-use files (asi + ini + nvngx_dlss.dll + loader) are in the release download.

## Building
1. Visual Studio 2022 with the **Desktop development with C++** workload.
2. Download the NVIDIA DLSS SDK (https://github.com/NVIDIA/DLSS) and extract it into a folder named
   `DLSS` next to `CMakeLists.txt` (`DLSS/include/nvsdk_ngx.h` must exist).
3. Open this folder in Visual Studio (File > Open > Folder), select **x64 Release**, build.
4. Output: `NFS_DLAA.asi`. Optionally set the CMake variable `GAME_DIR` to your game folder and the asi,
   ini and `nvngx_dlss.dll` are copied there after each build.

## How it works (short)
- Hooks `IDXGISwapChain::Present` and the D3D11 immediate context (vtable hooks) to find the game's TAA
  pass, reads the jitter, motion vectors and depth it uses, and runs DLAA in its place.
- Hooking is delayed until the game window is up, so it does not conflict with the Steam overlay.
- Crash cams: three conditional jumps in the game's camera code are found by byte pattern and made
  unconditional; the original bytes are kept so the option can be toggled in game.

## Files
- `src/nfs_dlaa.cpp` - the whole mod
- `src/vtbl_index.c` - D3D11/DXGI vtable slot indices, computed from the SDK headers
- `imgui/` - Dear ImGui (MIT)
- `NFS_DLAA.ini` - default settings

## License
MIT, see `LICENSE`. Dear ImGui is MIT (`imgui/LICENSE.txt`). The NVIDIA DLSS SDK and `nvngx_dlss.dll`
are NVIDIA's and are not part of this repository. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation.
Unofficial fan mod, not affiliated with EA, Criterion or Stellar Entertainment.

Credits: crash cam patch sites from ZPatchHP2010R (NoCrashCams).

---

## 한국어

**Need for Speed: Hot Pursuit Remastered** (PC, DirectX 11) 의 TAA 를 **NVIDIA DLAA** 로 바꿔 주는 ASI 모드입니다.

### 기능
- 게임 TAA 를 DLAA (NVIDIA DLSS SDK 310.9) 로 대체
- DLSS 프리셋 선택: 기본, E, F, J, **K (기본값)**, L, M
- DLAA 뒤 적응형 선명화 (AMD CAS 방식), 0~100 %
- 선택: 슬로 모션 충돌 카메라 끄기 (ZPatch NoCrashCams 와 같은 코드 패치)
- 게임 내 메뉴 (F8, 영어/한국어), DLAA 켜기/끄기 단축키 (F9)
- Steam 오버레이, 최신 Ultimate ASI Loader 와 함께 동작

### 빌드
1. Visual Studio 2022 ("C++를 사용한 데스크톱 개발")
2. NVIDIA DLSS SDK (https://github.com/NVIDIA/DLSS) 를 받아 `CMakeLists.txt` 옆 `DLSS` 폴더에 풉니다.
3. Visual Studio 에서 이 폴더를 열고 **x64 Release** 로 빌드합니다.
4. 결과물: `NFS_DLAA.asi`. CMake 변수 `GAME_DIR` 에 게임 폴더를 지정하면 빌드 후 자동으로 복사됩니다.

### 라이선스
MIT (`LICENSE` 참고). DLSS SDK 와 `nvngx_dlss.dll` 은 NVIDIA 소유이며 이 저장소에 포함되지 않습니다.
