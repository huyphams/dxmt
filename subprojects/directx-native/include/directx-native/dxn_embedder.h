/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * The embedder API: the plain-C functions a native D3D backend exports
 * beside D3D11CreateDevice / D3D12CreateDevice / CreateDXGIFactory*, which
 * an embedder resolves with dlsym.
 *
 * Every symbol is <prefix><suffix>: this header defines the suffixes, and
 * the prefix is the backend's own, agreed between it and its embedder.  A
 * backend may export all of them from one library or split them across its
 * D3D11 and D3D12 libraries, and need export only those it supports.
 *
 * Anything a backend hands out behind a pointer (an event HANDLE, a shared
 * resource descriptor) belongs to the backend's own copy of this library:
 * the embedder reaches it only through these exports, or through a kernel
 * fd the backend gave it, never through its own copy of the code.
 *
 * Every library that exports any of these also exports
 * <prefix>native_abi_version, returning the DXN_ABI_VERSION it was built
 * with; an embedder refuses the library's embedder API unless that equals
 * its own DXN_ABI_VERSION.  Define it with DXN_DEFINE_NATIVE_ABI_VERSION.
 *
 * All of these use the platform's plain C calling convention (SysV), NOT
 * the MS ABI of the D3D entry points and COM methods.  HRESULTs are int32_t,
 * COM pointers are void *, IIDs point at the 16-byte GUID.
 */

#ifndef DXN_EMBEDDER_H
#define DXN_EMBEDDER_H

#include <stddef.h>
#include <stdint.h>

#include "dxn_event.h"
#include "dxn_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -- Every backend ------------------------------------------------------ */

#define DXN_SYM_NATIVE_ABI_VERSION "native_abi_version"
typedef uint32_t (*PFN_dxn_native_abi_version)(void);

/* Defines the exported <prefix>native_abi_version() (with its prototype, so
 * -Wmissing-prototypes builds stay clean).  In C++, prefix it with
 * extern "C". */
#define DXN_DEFINE_NATIVE_ABI_VERSION(prefix)                               \
    __attribute__((visibility("default"))) uint32_t                        \
    prefix##native_abi_version(void);                                      \
    __attribute__((visibility("default"))) uint32_t                        \
    prefix##native_abi_version(void) { return DXN_ABI_VERSION; }

/* -- Events (macOS) ---------------------------------------------------- */
/* The backend's event HANDLEs (dxn_event.h): the embedder creates them
 * through the backend's exports, passes them wherever the backend's D3D APIs
 * take an event HANDLE (SetEventOnCompletion, ...), and observes them only
 * through dup_fd.  It never signals, waits on or otherwise touches one with
 * its own copy of this library, and never mixes handles of two backends.
 * On Linux an event HANDLE is an eventfd and there is nothing to export. */

#define DXN_SYM_EVENT_CREATE "event_create"
#define DXN_SYM_EVENT_CLOSE  "event_close"
#define DXN_SYM_EVENT_DUP_FD "event_dup_fd"

typedef void *(*PFN_dxn_event_create)(int manual_reset, int initial_state);
typedef void (*PFN_dxn_event_close)(void *handle);
typedef int (*PFN_dxn_event_dup_fd)(void *handle);

/* -- Shared handles ----------------------------------------------------- */

/* CloseHandle for an NT-style shared handle (the CreateSharedHandle family,
 * dxn_shared.h): closes the descriptor's fd and frees it.  Returns 0, or a
 * negative error for NULL, a legacy GetSharedHandle value, an
 * already-closed handle or anything the backend did not vend (then nothing
 * is touched).  Optional: a backend without it ties NT handles to the
 * exporting resource. */
#define DXN_SYM_SHARED_HANDLE_CLOSE "shared_handle_close"
typedef int (*PFN_dxn_shared_handle_close)(void *handle);

/* -- Imported placed-buffer heaps (D3D12) ------------------------------- */
/* Import a window of a shared-memory object as an ID3D12Heap on `device` (an
 * ID3D12Device *): buffers later placed on it via CreatePlacedResource alias
 * those pages exactly (heap offset preserved), so a CPU writer of the object
 * and the GPU see one allocation.  `fd` is borrowed.  On success *heap is an
 * AddRef'd interface `iid` (normally IID_ID3D12Heap). */

/* macOS: [offset, offset + size) of a shm fd; offset page-aligned, size
 * nonzero and at most 4 GiB, the window inside the object.  heap_type /
 * heap_flags are the app's D3D12_HEAP_TYPE / D3D12_HEAP_FLAGS (advisory). */
#define DXN_SYM_OPEN_EXISTING_HEAP_FROM_FD "open_existing_heap_from_fd"
typedef int32_t (*PFN_dxn_open_existing_heap_from_fd)(
    void *device, int fd, uint64_t offset, uint64_t size, uint32_t heap_type,
    uint32_t heap_flags, const void *iid, void **heap);

/* Linux: a whole dma-buf of `size` bytes. */
#define DXN_SYM_OPEN_EXISTING_HEAP_FROM_DMABUF "open_existing_heap_from_dmabuf"
typedef int32_t (*PFN_dxn_open_existing_heap_from_dmabuf)(
    void *device, int dmabuf_fd, uint64_t size, const void *iid, void **heap);

/* -- Externally-backed D3D11 DYNAMIC buffers ---------------------------- */
/* Make the embedder's memory fd (macOS: its shm object; Linux: a dma-buf of
 * it) the storage of one rename allocation of a DYNAMIC ID3D11Buffer, so a
 * Map writes the bytes the GPU reads with no copy.  backing_length must
 * cover the buffer and be page aligned.  The fd is dup'ed; the caller keeps
 * its own.  The allocation is tagged with `cookie` and queued for a later
 * Map(WRITE_DISCARD) rename.  Returns an HRESULT. */
#define DXN_SYM_D3D11_BUFFER_BIND_EXTERNAL_FD "d3d11_buffer_bind_external_fd"
typedef int32_t (*PFN_dxn_d3d11_buffer_bind_external_fd)(
    void *d3d11_buffer, int fd, uint64_t backing_length, uint32_t cookie);

/* 0 and *out_cookie if mapped_ptr (from ID3D11DeviceContext::Map) is an
 * external allocation of that buffer, -1 if it is ordinary device memory. */
#define DXN_SYM_D3D11_BUFFER_EXTERNAL_COOKIE "d3d11_buffer_external_cookie"
typedef int (*PFN_dxn_d3d11_buffer_external_cookie)(
    void *d3d11_buffer, const void *mapped_ptr, uint32_t *out_cookie);

#ifdef __cplusplus
}
#endif

#endif /* DXN_EMBEDDER_H */
