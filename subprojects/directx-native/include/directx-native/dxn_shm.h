/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Anonymous shared memory: the backing of macOS shared resources (see
 * dxn_shared.h) and of any other cross-process page a backend hands out.
 *
 *   macOS: a POSIX shm object created O_EXCL under a random name and
 *     unlinked at once, so the fd is the only name.  Under App Sandbox
 *     (APP_SANDBOX_GROUP_ID set) the name lives in the app group container
 *     as macOS requires, within the PSHMNAMLEN budget.
 *   Linux: a memfd sealed against shrinking (F_SEAL_SHRINK), which udmabuf
 *     requires to turn it into a dma-buf.
 *
 * Every fd is close-on-exec.
 */

#ifndef DXN_SHM_H
#define DXN_SHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The granularity of every shm size and mapping.  On macOS at least 16 KiB:
 * Apple Silicon kernel/GPU pages are 16 KiB, and an x86_64 process under
 * Rosetta sees getpagesize() == 4096 -- a mapping only 4 KiB-aligned there
 * leaves its final real page partly described and the GPU without a
 * mapping for the tail. */
size_t dxn_shm_page_size(void);

/* Round n up to dxn_shm_page_size(). */
uint64_t dxn_shm_page_align(uint64_t n);

/* A new anonymous shm object of `size` bytes (zero-filled).  Returns the fd,
 * or -1 with errno set. */
int dxn_shm_create(uint64_t size);

typedef struct dxn_shm_mapping {
    void  *ptr;    /* the byte at the requested offset */
    void  *base;   /* start of the mapping (page floor of the offset) */
    size_t length; /* mapped bytes from base, page-aligned */
} dxn_shm_mapping;

/* Map [offset, offset + size) of fd read-write, MAP_SHARED.  The mapping
 * starts at the page floor of offset, so offset need not be page-aligned.
 * The window is checked against the object's size first (fstat), so a short
 * fd fails here instead of faulting (SIGBUS) on first touch.  Returns 0 and
 * fills *out on success, -1 with errno set (EINVAL for a bad window). */
int dxn_shm_map(int fd, uint64_t offset, uint64_t size, dxn_shm_mapping *out);

/* Undo dxn_shm_map; a zeroed mapping is a no-op. */
void dxn_shm_unmap(dxn_shm_mapping *mapping);

#ifdef __cplusplus
}
#endif

#endif /* DXN_SHM_H */
