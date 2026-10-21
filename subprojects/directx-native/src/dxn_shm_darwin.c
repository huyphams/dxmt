/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Anonymous POSIX shm objects (macOS).
 */

#include "directx-native/dxn_shm.h"

#include "dxn_private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* PSHMNAMLEN (the shm name limit) lives in <sys/posix_shm.h>, which only
 * the macOS SDK ships; the iOS-family SDKs share the same kernel limit. */
#if __has_include(<sys/posix_shm.h>)
#include <sys/posix_shm.h>
#endif
#ifndef PSHMNAMLEN
#define PSHMNAMLEN 31
#endif

/* Apple Silicon pages are 16 KiB; see dxn_shm.h for Rosetta. */
#define MIN_PAGE_SIZE 16384u

/* An app group prefix must leave room for "/", a slot digit and one nonce
 * digit within PSHMNAMLEN. */
#define APP_SANDBOX_GROUP_ID_MAX (PSHMNAMLEN - 3)

size_t
dxn_shm_page_size(void)
{
    size_t pg = (size_t)getpagesize();
    return pg < MIN_PAGE_SIZE ? MIN_PAGE_SIZE : pg;
}

int
dxn_shm_create(uint64_t size)
{
    if (size > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }

    /* Under App Sandbox a shm name must live in the app group container,
     * i.e. be prefixed with the group identifier instead of '/'; anything
     * else is denied.  The PSHMNAMLEN budget then leaves no room for a tag
     * or the pid, so the name is just a slot digit plus as much of the
     * nonce as fits. */
    const char *group = getenv("APP_SANDBOX_GROUP_ID");
    if (group && !group[0])
        group = NULL;
    if (group && strlen(group) > APP_SANDBOX_GROUP_ID_MAX) {
        DXN_ERROR("shm: APP_SANDBOX_GROUP_ID is %zu chars, at most %d fit",
                  strlen(group), (int)APP_SANDBOX_GROUP_ID_MAX);
        errno = EINVAL;
        return -1;
    }

    const size_t nonce_room = group ? PSHMNAMLEN - strlen(group) - 2 : 8;
    const unsigned nonce_digits = (unsigned)(nonce_room < 8 ? nonce_room : 8);
    const unsigned nonce = arc4random() >> (32 - 4 * nonce_digits);
    int fd = -1;
    /* A sandboxed slot must stay a single hex digit to fit the name. */
    const unsigned slots = group ? 16 : 32;
    for (unsigned i = 0; i < slots; i++) {
        char name[PSHMNAMLEN + 1];
        if (group)
            snprintf(name, sizeof(name), "%s/%x%0*x", group, i,
                     (int)nonce_digits, nonce);
        else
            snprintf(name, sizeof(name), "/dxn-%d-%x-%x", (int)getpid(),
                     nonce, i);
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            shm_unlink(name); /* anonymous from here on */
            break;
        }
        if (errno != EEXIST)
            return -1;
    }
    if (fd < 0)
        return -1;

    int fdflags = fcntl(fd, F_GETFD);
    if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0 ||
        ftruncate(fd, (off_t)size) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}
