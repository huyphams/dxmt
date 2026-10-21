/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Anonymous shared memory (Linux): sealed memfds.
 */

#include "directx-native/dxn_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

size_t
dxn_shm_page_size(void)
{
    return (size_t)sysconf(_SC_PAGESIZE);
}

int
dxn_shm_create(uint64_t size)
{
    if (size > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    int fd = memfd_create("dxn-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    /* udmabuf refuses a memfd that could shrink under its pages. */
    if (ftruncate(fd, (off_t)size) != 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}
