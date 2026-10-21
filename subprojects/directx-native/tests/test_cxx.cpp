/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * The headers are also consumed from C++: they must compile cleanly as C++
 * and give the same layout.
 */

#include "directx-native/directx_native.h"

DXN_DEFINE_NATIVE_ABI_VERSION(dxntest_)

static_assert(sizeof(dxn_shared_texture) == 152, "texture");
static_assert(sizeof(dxn_shared_fence) == 32, "fence");
static_assert(sizeof(dxn_shared_buffer) == 48, "buffer");

int
main()
{
    PFN_dxn_native_abi_version q = dxntest_native_abi_version;
    dxn_shared_fence f = {};
    DXN_SHARED_INIT(&f, DXN_SHARED_FENCE_MAGIC);
    return (q() == DXN_ABI_VERSION && dxn_shared_type(&f) == DXN_SHARED_FENCE_MAGIC) ? 0 : 1;
}
