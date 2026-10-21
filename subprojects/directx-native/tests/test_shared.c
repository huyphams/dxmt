/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Descriptor prefix validation and the version query.
 */

#include "directx-native/directx_native.h"

#include <string.h>

#include "dxn_test.h"

int
main(void)
{
    CHECK(dxn_abi_version() == DXN_ABI_VERSION);

    dxn_shared_texture t;
    memset(&t, 0, sizeof(t));
    DXN_SHARED_INIT(&t, DXN_SHARED_TEXTURE_MAGIC);
    t.fd = -1;
    CHECK(t.struct_size == sizeof(t));
    CHECK(dxn_shared_check(&t, DXN_SHARED_TEXTURE_MAGIC, sizeof(t)));
    CHECK(!dxn_shared_check(&t, DXN_SHARED_FENCE_MAGIC, sizeof(t)));
    CHECK(!dxn_shared_check(NULL, DXN_SHARED_TEXTURE_MAGIC, sizeof(t)));
    CHECK(dxn_shared_type(&t) == DXN_SHARED_TEXTURE_MAGIC);

    t.version = DXN_SHARED_VERSION + 1;
    CHECK(!dxn_shared_check(&t, DXN_SHARED_TEXTURE_MAGIC, sizeof(t)));
    CHECK(dxn_shared_type(&t) == 0);
    t.version = DXN_SHARED_VERSION;
    t.struct_size -= 8;
    CHECK(!dxn_shared_check(&t, DXN_SHARED_TEXTURE_MAGIC, sizeof(t)));
    CHECK(dxn_shared_type(&t) == 0);

    dxn_shared_fence f;
    memset(&f, 0, sizeof(f));
    DXN_SHARED_INIT(&f, DXN_SHARED_FENCE_MAGIC);
    CHECK(dxn_shared_type(&f) == DXN_SHARED_FENCE_MAGIC);
    /* A fence must not pass as a texture even with the right magic. */
    f.magic = DXN_SHARED_TEXTURE_MAGIC;
    CHECK(dxn_shared_type(&f) == 0);

    dxn_shared_buffer b;
    memset(&b, 0, sizeof(b));
    DXN_SHARED_INIT(&b, DXN_SHARED_BUFFER_MAGIC);
    CHECK(dxn_shared_type(&b) == DXN_SHARED_BUFFER_MAGIC);

    uint32_t junk[4] = { 0x12345678u, DXN_SHARED_VERSION, 16, 0 };
    CHECK(dxn_shared_type(junk) == 0);
    return 0;
}
