# DXMT in VMPal

This branch is the DXMT that [VMPal](https://vmpal.com) ships, as `dxmt-native.framework`: UTM's DXMT
([utmapp/dxmt](https://github.com/utmapp/dxmt)) at f05e963, with VMPal's changes as the commits after it:

- `d3d11: keep shared textures away from the resource initializer`
- `d3d11: track immutable resources that come without their contents`
- `d3d11: update staging resources with UpdateSubresource`

DXMT is under the GNU Lesser General Public License 2.1 or later (`LICENSE`, `COPYING.LIB`). Its submodules are at the
commits this tree pins.

## Building

On an Apple silicon Mac with Xcode (and its Metal toolchain), CMake, Ninja, Meson and Python 3:

```sh
git submodule update --init --depth 1
VMPal/build.sh
```

It builds LLVM 15.0.7, which DXMT's shader compiler links, then DXMT, and prints the library's path. VMPal puts
`libdxmt-native.dylib` in a framework, `dxmt-native.framework`, with the install name
`@rpath/dxmt-native.framework/Versions/A/dxmt-native`.
