/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-process shared-resource descriptors.
 *
 * On a native (non-Windows) D3D implementation a shared-resource HANDLE --
 * what IDXGIResource::GetSharedHandle, IDXGIResource1/ID3D11Fence::
 * CreateSharedHandle and ID3D12Device::CreateSharedHandle return, and what
 * ID3D11Device::OpenSharedResource / OpenSharedFence and ID3D12Device::
 * OpenSharedHandle take -- is a pointer to one of the descriptors below.
 *
 * A descriptor is copy-by-value.  The ONLY live resource it references is
 * its fd, which a consumer transports out-of-band (SCM_RIGHTS over a unix
 * socket) and writes back into the receiver's copy of the descriptor before
 * opening it: the fd is the token, the struct is the layout.  The pointer
 * itself is process-local and means nothing in another process.
 *
 * What the fd is depends on the platform the exporter runs on:
 *
 *   macOS: an anonymous POSIX shm object (dxn_shm_create).  Textures are
 *     linear, one plane.
 *   Linux: a dma-buf for textures and buffers, a Vulkan opaque fd of a
 *     timeline semaphore for fences.
 *
 * The layouts are the same on both platforms (fixed-width fields only, no
 * D3D or Windows headers); fields a platform has no use for are zero.
 *
 * Ownership follows the two Windows regimes:
 *
 *   Legacy handles (IDXGIResource::GetSharedHandle): owned by the exporting
 *   resource, valid while it is alive, never closed by the caller; repeated
 *   calls return the same HANDLE.  dup() the fd to outlive the exporter.
 *
 *   NT-style handles (the CreateSharedHandle family): each call returns a
 *   new descriptor owning its own duplicate of the fd, valid after the
 *   resource is released.  The consumer closes it exactly once through the
 *   backend's DXN_SYM_SHARED_HANDLE_CLOSE export (a backend without one
 *   ties NT handles to the exporter like legacy ones).
 *
 *   Importers read the caller's descriptor synchronously during the open
 *   call and dup() the fd; the caller keeps ownership of both.
 *
 * Every importer validates with dxn_shared_check() and fails the open with
 * E_INVALIDARG on a mismatch instead of reading a layout it does not have.
 */

#ifndef DXN_SHARED_H
#define DXN_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "dxn_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Carried in every descriptor; equal to the DXN_ABI_VERSION it was
 * introduced with and bumped together with it. */
#define DXN_SHARED_VERSION 1u

#define DXN_SHARED_TEXTURE_MAGIC 0x744E5844u /* 'DXNt' */
#define DXN_SHARED_FENCE_MAGIC   0x664E5844u /* 'DXNf' */
#define DXN_SHARED_BUFFER_MAGIC  0x624E5844u /* 'DXNb' */

#define DXN_SHARED_MAX_PLANES 4u

/* Vendor MiscFlags bit (D3D11 CreateTexture2D MiscFlags / D3D12 heap flags),
 * combined with the API's SHARED flag: force a linear layout
 * (DRM_FORMAT_MOD_LINEAR on Linux), so the export can be consumed by scanout
 * paths without modifier plumbing.  Stripped from the resource's public
 * description. */
#define DXN_MISC_LINEAR_EXPORT 0x40000000u

/* The common prefix of every descriptor.  Each descriptor below begins with
 * exactly these members, so any HANDLE can be inspected through this view
 * before its type is known. */
typedef struct dxn_shared_header {
    uint32_t magic;       /* DXN_SHARED_*_MAGIC */
    uint32_t version;     /* DXN_SHARED_VERSION */
    uint32_t struct_size; /* sizeof the full descriptor */
    int32_t  fd;          /* process-local; send via SCM_RIGHTS, then patch */
} dxn_shared_header;

/* D3D-level description of a shared texture: D3D11_TEXTURE2D_DESC plus the
 * texture layout, every field a 32-bit enum or UINT. */
typedef struct dxn_texture_meta {
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint32_t array_size;
    uint32_t dxgi_format;      /* DXGI_FORMAT */
    uint32_t sample_count;     /* DXGI_SAMPLE_DESC */
    uint32_t sample_quality;
    uint32_t usage;            /* D3D11_USAGE */
    uint32_t bind_flags;
    uint32_t cpu_access_flags;
    uint32_t misc_flags;
    uint32_t texture_layout;   /* D3D11_TEXTURE_LAYOUT */
} dxn_texture_meta;

typedef struct dxn_shared_plane {
    uint64_t offset;           /* byte offset of the plane within fd */
    uint64_t pitch;            /* bytes per row, byte-exact for the consumer */
} dxn_shared_plane;

typedef struct dxn_shared_texture {
    uint32_t magic;            /* DXN_SHARED_TEXTURE_MAGIC */
    uint32_t version;          /* DXN_SHARED_VERSION */
    uint32_t struct_size;      /* sizeof(dxn_shared_texture) */
    int32_t  fd;
    dxn_texture_meta meta;
    uint64_t drm_modifier;     /* Linux; 0 (linear) on macOS */
    uint32_t plane_count;      /* 1..DXN_SHARED_MAX_PLANES; 1 on macOS */
    uint32_t reserved;         /* 0 */
    dxn_shared_plane planes[DXN_SHARED_MAX_PLANES];
    /* Linux: the exporter's whole memory allocation, which the planes lie
     *   within (import validation).
     * macOS: the logical surface bytes (pitch * rows), NOT page-padded,
     *   starting planes[0].offset into fd.  planes[0].offset is 0 for a
     *   committed surface, which owns its whole object, and nonzero for one
     *   placed in a shared heap (a window into the heap's object), not
     *   necessarily page-aligned.  A keyed-mutex state page, when the
     *   texture has one, follows at planes[0].offset +
     *   dxn_shm_page_align(size). */
    uint64_t size;
} dxn_shared_texture;

typedef struct dxn_shared_fence {
    uint32_t magic;            /* DXN_SHARED_FENCE_MAGIC */
    uint32_t version;          /* DXN_SHARED_VERSION */
    uint32_t struct_size;      /* sizeof(dxn_shared_fence) */
    /* macOS: a shm companion buffer whose uint64 value slot is at offset 0.
     * Linux: a Vulkan opaque fd of a timeline semaphore; importing binds the
     *   consumer's semaphore to the same payload (same driver on both ends). */
    int32_t  fd;
    uint32_t flags;            /* D3D11_FENCE_FLAG / D3D12_FENCE_FLAGS */
    uint32_t reserved;         /* 0 */
    uint64_t initial_value;
} dxn_shared_fence;

/* D3D buffers created with the shared flag (D3D11_RESOURCE_MISC_SHARED,
 * D3D12_HEAP_FLAG_SHARED). */
typedef struct dxn_shared_buffer {
    uint32_t magic;            /* DXN_SHARED_BUFFER_MAGIC */
    uint32_t version;          /* DXN_SHARED_VERSION */
    uint32_t struct_size;      /* sizeof(dxn_shared_buffer) */
    int32_t  fd;
    uint32_t bind_flags;
    uint32_t misc_flags;
    uint32_t cpu_access_flags;
    uint32_t reserved;         /* 0 */
    uint64_t size;             /* logical byte length */
    /* Byte offset of the buffer within fd, page-aligned: 0 for a committed
     * buffer, nonzero for one placed in a shared heap.  Map at this offset. */
    uint64_t offset;
} dxn_shared_buffer;

/* Initialise the common prefix of a descriptor for export. */
#define DXN_SHARED_INIT(desc, type_magic)                \
    do {                                                 \
        (desc)->magic = (type_magic);                    \
        (desc)->version = DXN_SHARED_VERSION;            \
        (desc)->struct_size = (uint32_t)sizeof(*(desc)); \
    } while (0)

/* Validate a HANDLE claimed to be a descriptor of the given magic and size:
 * nonzero only when the prefix matches exactly.  The fd is not checked
 * (an exporter-side descriptor may legitimately carry -1 before export). */
static inline int
dxn_shared_check(const void *handle, uint32_t magic, size_t size)
{
    const dxn_shared_header *h = (const dxn_shared_header *)handle;
    return h && h->magic == magic && h->version == DXN_SHARED_VERSION &&
           h->struct_size == (uint32_t)size;
}

/* The magic of a HANDLE that passes as some descriptor, else 0. */
static inline uint32_t
dxn_shared_type(const void *handle)
{
    const dxn_shared_header *h = (const dxn_shared_header *)handle;
    if (!h || h->version != DXN_SHARED_VERSION)
        return 0;
    switch (h->magic) {
    case DXN_SHARED_TEXTURE_MAGIC:
        return h->struct_size == sizeof(dxn_shared_texture) ? h->magic : 0;
    case DXN_SHARED_FENCE_MAGIC:
        return h->struct_size == sizeof(dxn_shared_fence) ? h->magic : 0;
    case DXN_SHARED_BUFFER_MAGIC:
        return h->struct_size == sizeof(dxn_shared_buffer) ? h->magic : 0;
    default:
        return 0;
    }
}

#ifdef __cplusplus
#define DXN_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define DXN_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

/* The layout is ABI: identical on every platform and both architectures. */
DXN_STATIC_ASSERT(sizeof(dxn_shared_header) == 16, "dxn_shared_header");
DXN_STATIC_ASSERT(sizeof(dxn_texture_meta) == 48, "dxn_texture_meta");
DXN_STATIC_ASSERT(offsetof(dxn_shared_texture, meta) == 16 &&
                  offsetof(dxn_shared_texture, drm_modifier) == 64 &&
                  offsetof(dxn_shared_texture, plane_count) == 72 &&
                  offsetof(dxn_shared_texture, planes) == 80 &&
                  offsetof(dxn_shared_texture, size) == 144 &&
                  sizeof(dxn_shared_texture) == 152, "dxn_shared_texture");
DXN_STATIC_ASSERT(offsetof(dxn_shared_fence, flags) == 16 &&
                  offsetof(dxn_shared_fence, initial_value) == 24 &&
                  sizeof(dxn_shared_fence) == 32, "dxn_shared_fence");
DXN_STATIC_ASSERT(offsetof(dxn_shared_buffer, bind_flags) == 16 &&
                  offsetof(dxn_shared_buffer, size) == 32 &&
                  offsetof(dxn_shared_buffer, offset) == 40 &&
                  sizeof(dxn_shared_buffer) == 48, "dxn_shared_buffer");
DXN_STATIC_ASSERT(offsetof(dxn_shared_texture, fd) ==
                      offsetof(dxn_shared_header, fd) &&
                  offsetof(dxn_shared_fence, fd) ==
                      offsetof(dxn_shared_header, fd) &&
                  offsetof(dxn_shared_buffer, fd) ==
                      offsetof(dxn_shared_header, fd),
                  "descriptor prefix");

#ifdef __cplusplus
}
#endif

#endif /* DXN_SHARED_H */
