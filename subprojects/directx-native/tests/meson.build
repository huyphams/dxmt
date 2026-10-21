# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: Apache-2.0

c_tests = ['shared', 'shm']
if host_machine.system() == 'darwin'
  c_tests += 'event'
endif

foreach t : c_tests
  test(t, executable('test_' + t, 'test_' + t + '.c',
                     dependencies: directx_native_dep))
endforeach

if add_languages('cpp', required: false, native: false)
  test('cxx', executable('test_cxx', 'test_cxx.cpp',
                         dependencies: directx_native_dep,
                         override_options: ['cpp_std=c++17']))
endif
