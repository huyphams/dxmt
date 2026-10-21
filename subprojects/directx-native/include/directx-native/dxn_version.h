/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * directx-native versioning.
 *
 * DXN_ABI_VERSION is the one number every party to the host <-> backend
 * contract agrees on: the shared-resource descriptors (dxn_shared.h), the
 * embedder symbols and their signatures (dxn_embedder.h), and the event
 * HANDLE conventions (dxn_event.h).  It is bumped on ANY change to a struct
 * layout, a field's meaning, an enum value, a symbol name or a function
 * signature in those headers -- there is no compatibility window: every
 * consumer is rebuilt against the same revision, and a backend that
 * reports another version is refused at load (see
 * DXN_SYM_NATIVE_ABI_VERSION).
 *
 * DXN_SHARED_VERSION, carried inside every descriptor, moves with it: a
 * descriptor is only ever read by a peer built from the same revision.
 */

#ifndef DXN_VERSION_H
#define DXN_VERSION_H

#include <stdint.h>

#define DXN_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

/* DXN_ABI_VERSION as compiled into this copy of the library. */
uint32_t dxn_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DXN_VERSION_H */
