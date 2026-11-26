# xbased on macOS (Intel)

A port of xbased to Intel Macs (x86-64), work in progress. It follows
upstream's releases; its own changes are in [CHANGELOG.md](CHANGELOG.md).

Apple silicon isn't supported: the translator emits x86-64 code. An Intel
build under Rosetta 2 hasn't been tried.

## Requirements

- An Intel Mac with macOS 12 or later (MoltenVK 1.4.2's own minimum). It's
  tested on macOS 15.7 with an AMD Radeon Pro 5300M; the Intel UHD
  Graphics 630 beside it is picked with `XBASED_GPU=1`.
- Your own game images, as on the other platforms.

## Building

1. The Xcode command line tools (clang, git and python3):

   ```bash
   xcode-select --install
   ```

2. CMake (3.20 or newer) and Ninja, from either:
   - their own downloads ([cmake.org](https://cmake.org/download/),
     [ninja](https://github.com/ninja-build/ninja/releases)), or
   - pip: `python3 -m pip install --user cmake ninja` (then
     `~/Library/Python/3.x/bin` on `PATH`), or
   - a package manager (MacPorts, Homebrew).

3. The source, the dependencies and the build:

   ```bash
   git clone https://github.com/crazyriddler/xbased
   cd xbased
   bash tools/fetch_deps.sh
   cmake -S native -B ../build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build ../build/macos --target xbased
   ```

`tools/fetch_deps.sh` also fetches MoltenVK 1.4.2, Khronos' release
(checked against its SHA-256), and the build puts `libMoltenVK.dylib`
beside `../build/macos/xbased`, which runs from the build folder. Settings,
saves and states go to `~/Library/Application Support/xbased`.

On Apple silicon, `-DCMAKE_OSX_ARCHITECTURES=x86_64` builds for Intel Macs
(the result can't run there); that's how the CI builds it.

The host tests: `bash tools/native/host-tests.sh ../build/macos`
(`cputest` is Windows's).

## What differs from Linux

- **The guest's memory at address 0.** macOS's libSystem allocates in the
  low pages before `main`, so the executable reserves the guest's 4 GiB
  itself: linked without PIE, a 64 KiB `__PAGEZERO`, a `__XBGUEST` segment
  at 0x10000 (`native/core/macos_reserve.s`) and `__TEXT` at 0x200000000.
  `tools/macos/reserve_guest.py` widens `__XBGUEST` up to `__TEXT` after
  the link and signs the executable again (ad hoc). The linker warns that
  `-no_pie` and a small page zero are deprecated; Intel still honours them.
- **The POSIX layer** is compat/linux's, with macOS's shims in front
  (`native/compat/macos/include/nc_macos_posix.h`, included first): stat's
  times, monotonic condition deadlines, `clock_nanosleep`, `gettid`, thread
  CPU clocks, `eventfd` over a pipe, ucontext. nv2a's waits on an address
  use the kernel's ulocks, HleSem is a dispatch semaphore.
- **No game frame** (`native/app/win_frame.c`): a process can't hold
  another's window on macOS, so each run's window stands on its own, as
  on Linux without X11 (a state loaded or a title relaunching swaps
  windows).
- **Vulkan is MoltenVK**, loaded at run time: the one beside the
  executable first, then a loader (`libvulkan.1.dylib`), then a MoltenVK
  where Homebrew puts it. `SDL_VULKAN_LIBRARY` names another (a loader for
  the validation layers, another MoltenVK to compare). Its log is cut to
  warnings and errors unless `MVK_CONFIG_LOG_LEVEL` is set.

## MoltenVK's workarounds

Each one can be switched for an A/B test:

| What | Why | Switch |
|---|---|---|
| Swapchain images acquired with a fence (quirk `acquire_fence`) | MoltenVK 1.4.2 never signals an acquire semaphore when no image was free | `XBASED_HLE_QUIRKS=-acquire_fence` |
| The alpha test compares 8-bit values (quirk `alpha_test_8bit`) | On Metal an alpha a hair under 1.0 passed `a < 1.0`, where the NV2A's 255 fails LESS 255: Halo 2's Armory grate drew its holes grey | `XBASED_HLE_QUIRKS=-alpha_test_8bit` |
| No bindless texture set | MoltenVK makes every texture in a bound set resident at every draw: submits held Halo 2's render thread 360-850 ms of each second at Cairo Station (10-18 fps in combat), 26-30 without it | `XBASED_HLE_MVK_BINDLESS=1` keeps it |
| Triangle fans drawn as lists | Metal draws no fans | `XBASED_HLE_FANS=1` keeps them |
| Vertex attributes realigned to 4 bytes | Metal's offsets and strides are multiples of 4 | `XBASED_HLE_NO_VALIGN4=1` |
| 16-bit packed colour textures expanded to 32 bits | Metal has no R5G6B5, A1R5G5B5 or X1R5G5B5 | none |

The log's start says which are on (`vk: portability subset: ...`,
`quirks: ...`) and which MoltenVK runs (`vk: driver "MoltenVK" ...`).

## Known problems

- Halo 2: blocky, striped light on some lit metal floors (Cairo Station,
  The Armory); upstream has it on Windows too. With the light off, the
  floor of Cairo Station's weapon-rack room is blocky on macOS only.
- Loading a savestate restarts xbased, which then draws nothing.
- Now and then, a guest fault while Halo 2 loads a level.
