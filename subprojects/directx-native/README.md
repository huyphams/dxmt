# directx-native

A small C library holding the contract between a native (non-Windows)
implementation of the D3D APIs -- a *backend* -- and the process that loads
it and drives it -- the *embedder*: the layout of shared-resource handles,
the plain-C functions an embedder resolves beside the D3D entry points, and
the Win32-style events and anonymous shared memory both sides need.

Each backend exports the embedder functions under a symbol prefix of its own
choosing (`<prefix><suffix>`); this library defines only the suffixes and
signatures.

## Contents

| Header | What |
| --- | --- |
| `dxn_version.h` | `DXN_ABI_VERSION`, the one version every party agrees on. |
| `dxn_shared.h` | Shared-resource descriptors (`dxn_shared_texture`, `_fence`, `_buffer`) that a native shared `HANDLE` points to, plus `dxn_shared_check()`. Identical layout on both platforms. |
| `dxn_embedder.h` | The dlsym'd embedder API: symbol suffixes and `PFN_dxn_*` typedefs, plus `<prefix>native_abi_version`. |
| `dxn_event.h` | Win32-style events. On Linux a HANDLE is an eventfd, used directly with syscalls. On macOS it is a kqueue-backed object implemented here, with a pollable, `SCM_RIGHTS`-passable fd, and owned by the binary that created it. |
| `dxn_shm.h` | Anonymous shared memory (POSIX shm on macOS, sealed memfd on Linux) and windowed mapping. |
| `dxn_log.h` | One log callback for the library's diagnostics. |

`directx_native.h` includes all of them.

## Versioning

`DXN_ABI_VERSION` is bumped on **any** change to a struct layout, a field's
meaning, an enum value, a symbol name or a function signature in these
headers. There is no compatibility window. Every consumer pins one revision
of this repository, and:

* every descriptor carries `DXN_SHARED_VERSION`, and importers reject a
  descriptor whose magic, version or `struct_size` differs;
* every backend exports `<prefix>native_abi_version()`
  (`DXN_DEFINE_NATIVE_ABI_VERSION`), and the embedder refuses a backend
  whose answer differs from its own `DXN_ABI_VERSION`.

The project version (`meson.build`) follows semver over the same surface.

## Building and consuming

The library is built **only as a static archive** (`static_library()`, PIC,
hidden visibility, nothing installed) and is consumed as a meson
subproject. Add `subprojects/directx-native.wrap`:

```ini
[wrap-git]
url = <this repository>
revision = <commit>
depth = 1

[provide]
dependency_names = directx-native, directx-native-headers
```

then:

```meson
directx_native_dep = dependency('directx-native')          # code + headers
directx_native_headers_dep = dependency('directx-native-headers')  # headers only
```

Standalone:

```sh
meson setup build && meson test -C build
```

Supported hosts: macOS and Linux.

## License

Apache-2.0, see `LICENSE`.
