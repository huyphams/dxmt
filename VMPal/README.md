# DXMT in VMPal

This branch is the DXMT that [VMPal](https://vmpal.com) ships, as `dxmt-native.framework`: DXMT
([3Shain/dxmt](https://github.com/3Shain/dxmt)) at 0df8610, with the commits after it.

From UTM's fork ([utmapp/dxmt](https://github.com/utmapp/dxmt)), and the last from DXMT's `fix/coherent-event` branch,
each by its author:

- `native: build all D3D modules into a single libdxmt-native.dylib`
- `native: cross-process shared textures and Win32-style events`
- `dxmt: skip nil-pipeline draws at command-build time`
- `d3d11: retry failed PSO with integer-attachment writes dropped`
- `winemetal: out-errors replace what an earlier call left`
- `native: typed shader-output variants for integer RTVs`
- `d3d11: name shm objects for App Sandbox`
- `build: key dxmt_crossbuild off the target system`
- `build: lower the deployment target floor to macOS 13.0`
- `airconv: target Metal 3.0 where nothing newer is needed`
- `native: target iOS, tvOS and visionOS`
- `tests: add a native offscreen D3D11 smoke suite`
- `dxmt: emulate BC textures on devices without hardware support`
- `airconv: emit AIR that pre-Metal-3.1 AGX compilers accept`
- `native: shared-texture handle version 2 with a placement offset`
- `native: build without Metal SPI on every platform but macOS`
- `dxmt: complete an occlusion query that ends in a draw-less chunk`
- `native: let an embedder's shared memory back DYNAMIC buffer renames`
- `d3d11: UpdateSubresource into STAGING buffers and textures`
- `native: take the embedder contract, events and shm from directx-native`
- `native: drop the private symbols App Review flags on macOS too`
- `native: gate logic ops on their own DXMT_NO_LOGIC_OP, keep them everywhere`
- `feat(d3d11): add dummy stream-output queries`

VMPal's changes:

- `d3d11: don't let a new texture's contents be wiped or read too early`
- `d3d11: report forced samples as coverage in target-independent rasterization`
- `native: build Direct3D 12 into dxmt-native`
- `native, d3d12: Direct3D 12 under an embedder (Neptune's Windows guests)`
- `d3d12, airconv: this series' pixel data in geometry and tessellation pipelines`
- `airconv, d3d12: DXIL from Windows' own shaders, and shader model 6.2`
- `airconv, d3d12: pipelines in the shader cache, and what Unity and GLOn12 need`
- `airconv, d3d12: 3DMark's Steel Nomad Light, and no 4 KB placements`
- `d3d12: fewer allocations and waits, and heaps at aligned addresses`
- `airconv, d3d12: shader model 6.6 shaders (Unreal Engine 5's Nanite)`
- `d3d12: Black Myth: Wukong's benchmark runs through`
- `d3d12: occlusion queries, counted with Metal's visibility results`
- `d3d12: a BC texture written through a plain-format UAV, by way of a twin`
- `native: say how many threads wait for a pipeline to compile`
- `native: order work across devices that share textures`

The last commit before this README, `VMPal: directx-native 7258be1, which DXMT links (Apache 2.0)`, puts the subproject
DXMT's `subprojects/directx-native.wrap` pins in the tree, so a build needs no network.

DXMT is under the GNU Lesser General Public License 2.1 or later (`LICENSE`, `COPYING.LIB`). Its submodules are at the
commits this tree pins. directx-native ([osy/directx-native](https://github.com/osy/directx-native) at 7258be1), the
contract between DXMT and the program that loads it, is under the Apache License 2.0
(`subprojects/directx-native/LICENSE`).

## Building

On an Apple silicon Mac with Xcode (and its Metal toolchain), CMake, Ninja, Meson and Python 3:

```sh
git submodule update --init --depth 1
VMPal/build.sh
```

It builds LLVM 15.0.7, which DXMT's shader compiler links, then DXMT with Direct3D 12 and `subprojects/directx-native`
(`--force-fallback-for=directx-native`), and prints the library's path. VMPal puts `libdxmt-native.dylib` in a
framework, `dxmt-native.framework`, with the install name `@rpath/dxmt-native.framework/Versions/A/dxmt-native`.
