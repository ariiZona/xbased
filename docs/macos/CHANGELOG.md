# xbased on macOS (Intel): changelog

The port's own changes, on top of upstream's releases (its changelog is
[CHANGELOG.md](../../CHANGELOG.md)).

## Unreleased (on 1.2.11)

### New
- xbased builds and runs on Intel Macs (macOS 12 or later), with MoltenVK
  as its Vulkan driver. The guest's 4 GiB live at address 0 as on Windows
  and Linux: the executable reserves them at exec time (README.md).
- MoltenVK 1.4.2 comes with the build: `tools/fetch_deps.sh` fetches
  Khronos' release, checked against its hash, and the build puts it beside
  the executable, which loads it before any other.
- The host tests (`tools/native/host-tests.sh`) run on macOS, with the
  same compatibility headers as the app.
- The build for Intel Macs can be made on Apple silicon
  (`-DCMAKE_OSX_ARCHITECTURES=x86_64`).

### MoltenVK's workarounds
- Halo 2's combat at Cairo Station no longer drops to 10-18 fps: MoltenVK
  made every texture of the bindless set resident at every draw, and the
  submits held the render thread 360-850 ms of each second (26-30 now).
  The bindless set is off on portability drivers.
- Swapchain images are acquired with a fence: MoltenVK 1.4.2 never
  signals an acquire semaphore when no image was free at the acquire, and
  a pass waiting on it would hold Metal's queue for good.
- Triangle fans go as lists, vertex attributes are realigned to 4 bytes,
  and the 16-bit packed colour textures (R5G6B5, A1R5G5B5, X1R5G5B5) are
  expanded to 32 bits: Metal has none of them. A 16-bit target the GPU
  can't draw into isn't rendered (it crashed in Metal's validation).
