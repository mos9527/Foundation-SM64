# Foundation-SM64

[![Nightly Builds](https://img.shields.io/badge/FoundationSM64-builds-cyan)](https://nightly.link/mos9527/Foundation-SM64/workflows/msys2/nightly)

<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/7cb84196-62b4-4668-907a-28471dc0c054" />

Orignial repository: https://github.com/sm64pc/sm64ex

A port to [Foundation](https://github.com/mos9527/Foundation) rendering engine by [Tencent Hy3](https://github.com/Tencent-Hunyuan/Hy3).

Demonstrates using Foundation as a library. For more Foundation examples, see [Foundation/Examples](https://github.com/mos9527/Foundation/tree/dev/Examples) repository.

**Downloads are available here**: [Nightly Builds](https://nightly.link/mos9527/Foundation-SM64/workflows/msys2/nightly)
## Disclaimer

- **AI Generated Code**. Almost all porting effort is made by [Hy3](https://github.com/Tencent-Hunyuan/Hy3) itself with very little human intervention.
- This project is not officaly endorsed by Tencent.

## Building

> The build is driven by CMake (with Ninja) and targets the Foundation renderer host (`sm64_foundation`).
> The instructions below follow the official [Windows MSYS2 CI workflow](.github/workflows/msys2.yml).

### Prerequisites

- **MSYS2** (UCRT64 environment) with the following packages, installed via `pacman`
  in the **UCRT64 terminal**:
  ```sh
  pacman -S make mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-git
  ```
- **Vulkan SDK** (provides `slangc`, used to compile `.slang` shaders to `.spv`).
  Download from the [LunarG website](https://vulkan.lunarg.com/sdk/home) and ensure `slangc.exe`
  is on your `PATH` (e.g. add `$VULKAN_SDK/Bin`).
- A **SM64 US baserom** named `baserom.us.z64` in the repository root

### Steps

1. **Extract assets** from the baserom:
   ```sh
   python extract_assets.py us
   ```

2. **Configure** the CMake build:
   ```sh
   cmake -B build-cmake -G Ninja \
     -DCMAKE_BUILD_TYPE=Release \
     -DFOUNDATION_WITH_PROFILING=OFF \
     -DFOUNDATION_RHIVULKAN_VALIDATION_LAYER=OFF
   ```

3. **Build** the Foundation host executable:
   ```sh
   cmake --build build-cmake --target sm64_foundation
   ```

4. (Optional) **Package** the build into a distributable zip:
   ```sh
   python package.py build-cmake -o packages/Foundation-sm64-windows-msys2.zip
   ```

### Notes

- The build requires GCC or Clang (MinGW-w64 on Windows); MSVC is not supported.
- `SM64_NIGHTLY` (default `ON`) embeds the git hash into the title screen via the
  `NIGHTLY` / `GIT_HASH` defines.
- The classic standalone SDL2/`sm64pc` target has been removed — `sm64_foundation`
  is the only executable target.

For the original sm64ex build details, see the
[sm64ex wiki](https://github.com/sm64pc/sm64ex/wiki).

## Links
- [Foundation](https://github.com/mos9527/Foundation)
- [Hy3](https://github.com/Tencent-Hunyuan/Hy3)
- [sm64pc/sm64ex](https://github.com/sm64pc/sm64ex)
