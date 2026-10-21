/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Anonymous shm creation, page granularity and windowed mapping.
 */

#include "directx-native/dxn_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dxn_test.h"

int
main(void)
{
    const size_t page = dxn_shm_page_size();
    CHECK(page >= 4096 && (page & (page - 1)) == 0);
#ifdef __APPLE__
    CHECK(page >= 16384);
#endif
    CHECK(dxn_shm_page_align(0) == 0);
    CHECK(dxn_shm_page_align(1) == page);
    CHECK(dxn_shm_page_align(page) == page);
    CHECK(dxn_shm_page_align(page + 1) == 2 * page);

    const uint64_t size = 3 * page;
    int fd = dxn_shm_create(size);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && (uint64_t)st.st_size == size);

    /* Two creates never collide. */
    int fd2 = dxn_shm_create(page);
    CHECK(fd2 >= 0 && fd2 != fd);

    dxn_shm_mapping whole, window;
    CHECK(dxn_shm_map(fd, 0, size, &whole) == 0);
    CHECK(whole.ptr == whole.base && whole.length == size);
    for (uint64_t i = 0; i < size; i++)
        CHECK(((uint8_t *)whole.ptr)[i] == 0);
    memset(whole.ptr, 0xab, size);
    ((uint8_t *)whole.ptr)[page + 100] = 0x5a;

    /* An unaligned window maps from its page floor. */
    CHECK(dxn_shm_map(fd, page + 100, 16, &window) == 0);
    CHECK((uint8_t *)window.ptr - (uint8_t *)window.base == 100);
    CHECK(window.length == page);
    CHECK(*(uint8_t *)window.ptr == 0x5a);
    *(uint8_t *)window.ptr = 0x33;
    CHECK(((uint8_t *)whole.ptr)[page + 100] == 0x33);
    dxn_shm_unmap(&window);
    CHECK(window.base == NULL);
    dxn_shm_unmap(&window); /* no-op */

    /* Windows past the object fail instead of faulting later. */
    errno = 0;
    CHECK(dxn_shm_map(fd, 2 * page, page + 1, &window) == -1 &&
          errno == EINVAL);
    CHECK(dxn_shm_map(fd, 0, 0, &window) == -1);
    CHECK(dxn_shm_map(fd, UINT64_MAX - 1, 4, &window) == -1);
    CHECK(dxn_shm_map(-1, 0, 4, &window) == -1);

#ifdef __linux__
    /* The memfd is sealed against shrinking (udmabuf requirement). */
    CHECK(ftruncate(fd, page) != 0);
#endif

    dxn_shm_unmap(&whole);
    close(fd2);
    close(fd);
    return 0;
}
