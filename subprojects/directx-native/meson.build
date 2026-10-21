# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: Apache-2.0
#
# directx-native is consumed as a meson subproject and only ever linked
# statically: static_library() (not library()) makes it immune to
# default_library, and the archive is never installed.  The objects are PIC with hidden
# visibility so they can go into a consumer's shared library without
# leaking dxn_* into its exported ABI.  Only the headers may be installed
# (option install_headers), for consumers whose public headers include them.

project(
  'directx-native',
  'c',
  version: '1.0.0',
  license: 'Apache-2.0',
  meson_version: '>=0.58.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')
system = host_machine.system()

inc = include_directories('include')

dxn_sources = files(
  'src/dxn_log.c',
  'src/dxn_shm.c',
  'src/dxn_version.c',
)

dxn_deps = []

if system == 'darwin'
  dxn_sources += files(
    'src/dxn_event_kqueue.c',
    'src/dxn_shm_darwin.c',
  )
  # arc4random, clock_gettime_nsec_np and PSHMNAMLEN are Darwin extensions
  # hidden by a strict -std=c11.
  add_project_arguments('-D_DARWIN_C_SOURCE', language: 'c')
elif system == 'linux'
  # memfd_create, F_ADD_SEALS and the POSIX calls the tests use are hidden
  # by a strict -std=c11.
  add_project_arguments('-D_GNU_SOURCE', language: 'c')
  dxn_sources += files('src/dxn_shm_linux.c')
else
  error('directx-native supports macOS and Linux, not ' + system)
endif

dxn_deps += dependency('threads')

dxn_lib = static_library(
  'directx-native',
  dxn_sources,
  include_directories: inc,
  dependencies: dxn_deps,
  pic: true,
  gnu_symbol_visibility: 'hidden',
  install: false,
)

# The full library: headers + the static archive.
directx_native_dep = declare_dependency(
  include_directories: inc,
  link_with: dxn_lib,
  dependencies: dxn_deps,
  version: meson.project_version(),
)
meson.override_dependency('directx-native', directx_native_dep)

# Headers only, for embedders that consume the descriptors and the embedder
# API but none of the code (no link, no static copy of the globals).
directx_native_headers_dep = declare_dependency(
  include_directories: inc,
  version: meson.project_version(),
)
meson.override_dependency('directx-native-headers', directx_native_headers_dep)

if get_option('install_headers')
  install_subdir('include/directx-native', install_dir: get_option('includedir'))
endif

if get_option('tests') and not meson.is_subproject()
  subdir('tests')
endif
