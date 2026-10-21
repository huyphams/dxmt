/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform-neutral half of dxn_shm.h: alignment and mapping.
 */

#include "directx-native/dxn_shm.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

uint64_t
dxn_shm_page_align(uint64_t n)
{
    const uint64_t page = (uint64_t)dxn_shm_page_size();
    return (n + page - 1) & ~(page - 1);
}

int
dxn_shm_map(int fd, uint64_t offset, uint64_t size, dxn_shm_mapping *out)
{
    memset(out, 0, sizeof(*out));
    if (fd < 0 || size == 0 || offset + size < offset) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    if (st.st_size < 0 || offset + size > (uint64_t)st.st_size) {
        errno = EINVAL;
        return -1;
    }

    const uint64_t page = (uint64_t)dxn_shm_page_size();
    const uint64_t floor = offset & ~(page - 1);
    const uint64_t length = dxn_shm_page_align(offset - floor + size);
    if (length > (uint64_t)SIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    void *base = mmap(NULL, (size_t)length, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, (off_t)floor);
    if (base == MAP_FAILED)
        return -1;
    out->base = base;
    out->length = (size_t)length;
    out->ptr = (uint8_t *)base + (offset - floor);
    return 0;
}

void
dxn_shm_unmap(dxn_shm_mapping *mapping)
{
    if (mapping->base)
        munmap(mapping->base, mapping->length);
    memset(mapping, 0, sizeof(*mapping));
}
