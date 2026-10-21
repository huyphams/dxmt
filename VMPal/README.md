# DXMT in VMPal

This branch is the DXMT that [VMPal](https://vmpal.com) ships, as `dxmt-native.framework`: UTM's DXMT
([utmapp/dxmt](https://github.com/utmapp/dxmt)) at 8b9d334, with VMPal's changes as the commits after it:

- `d3d11: don't let a new texture's contents be wiped or read too early`
- `d3d11: report forced samples as coverage in target-independent rasterization`
- `airconv, d3d11: fixes from DXMT's own main that UTM's fork lacks`
- `VMPal: directx-native 7258be1, which DXMT links (Apache 2.0)`: the subproject DXMT's
  `subprojects/directx-native.wrap` pins, in the tree, so a build needs no network

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

It builds LLVM 15.0.7, which DXMT's shader compiler links, then DXMT with `subprojects/directx-native`
(`--force-fallback-for=directx-native`), and prints the library's path. VMPal puts
`libdxmt-native.dylib` in a framework, `dxmt-native.framework`, with the install name
`@rpath/dxmt-native.framework/Versions/A/dxmt-native`.
