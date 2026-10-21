/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Win32 event semantics, duplicates, and the pollable fd (in-process and
 * after crossing a socket with SCM_RIGHTS).
 */

#include "directx-native/dxn_event.h"
#include "directx-native/dxn_log.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dxn_test.h"

#define MS 1000000ull

static int warnings;

static void
count_log(dxn_log_level level, const char *message, void *user)
{
    (void)message;
    (void)user;
    if (level <= DXN_LOG_WARN)
        warnings++;
}

static int
readable(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

/* Send fd across a socketpair and return the receiver's copy. */
static int
pass_fd(int fd)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    char byte = 0;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    memset(&u, 0, sizeof(u));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = u.buf,
                          .msg_controllen = sizeof(u.buf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    CHECK(sendmsg(sv[0], &msg, 0) == 1);

    memset(&u, 0, sizeof(u));
    msg.msg_controllen = sizeof(u.buf);
    CHECK(recvmsg(sv[1], &msg, 0) == 1);
    c = CMSG_FIRSTHDR(&msg);
    CHECK(c && c->cmsg_type == SCM_RIGHTS);
    int out;
    memcpy(&out, CMSG_DATA(c), sizeof(int));
    close(sv[0]);
    close(sv[1]);
    return out;
}

static void
test_manual_reset(void)
{
    void *e = dxn_event_create(1, 0);
    CHECK(e && dxn_event_is_event(e));
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_TIMEOUT);
    CHECK(dxn_event_wait(e, 5 * MS) == DXN_WAIT_TIMEOUT);
    dxn_event_signal(e);
    /* Manual-reset stays signaled for every wait until cleared. */
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_SIGNALED);
    CHECK(dxn_event_wait(e, DXN_WAIT_INFINITE) == DXN_WAIT_SIGNALED);
    dxn_event_clear(e);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_TIMEOUT);
    dxn_event_close(e);

    e = dxn_event_create(1, 1);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_SIGNALED);
    dxn_event_close(e);
}

static void
test_auto_reset(void)
{
    void *e = dxn_event_create(0, 1);
    CHECK(e);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_SIGNALED);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_TIMEOUT);
    /* Repeated signals collapse into one. */
    dxn_event_signal(e);
    dxn_event_signal(e);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_SIGNALED);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_TIMEOUT);
    dxn_event_close(e);
}

struct waiter {
    void *event;
    dxn_wait_status status;
};

static void *
wait_thread(void *arg)
{
    struct waiter *w = arg;
    w->status = dxn_event_wait(w->event, 2000 * MS);
    return NULL;
}

static void
test_auto_reset_wakes_one(void)
{
    void *e = dxn_event_create(0, 0);
    struct waiter w[2] = { { e, DXN_WAIT_FAILED }, { e, DXN_WAIT_FAILED } };
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        CHECK(pthread_create(&t[i], NULL, wait_thread, &w[i]) == 0);
    usleep(50 * 1000);
    dxn_event_signal(e);
    usleep(100 * 1000);
    dxn_event_signal(e);
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    /* Two signals, two waiters, each woken exactly once. */
    CHECK(w[0].status == DXN_WAIT_SIGNALED);
    CHECK(w[1].status == DXN_WAIT_SIGNALED);
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_TIMEOUT);
    dxn_event_close(e);
}

static void
test_duplicate(void)
{
    void *e = dxn_event_create(1, 0);
    void *d = dxn_event_duplicate(e);
    CHECK(d && d != e && dxn_event_is_event(d));
    int fd = dxn_event_dup_fd(e);
    CHECK(fd >= 0);
    /* The duplicate outlives the original and signals the same core. */
    dxn_event_close(e);
    CHECK(!readable(fd));
    dxn_event_signal(d);
    CHECK(readable(fd));
    CHECK(dxn_event_wait(d, 0) == DXN_WAIT_SIGNALED);
    dxn_event_close(d);
    /* The fd outlives every handle. */
    CHECK(readable(fd));
    close(fd);
}

static void
test_dup_fd(void)
{
    /* Manual-reset: readable exactly while signaled, also cross-process. */
    void *e = dxn_event_create(1, 0);
    int fd = dxn_event_dup_fd(e);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    int remote = pass_fd(fd);
    CHECK(!readable(fd) && !readable(remote));
    dxn_event_signal(e);
    CHECK(readable(fd) && readable(remote));
    dxn_event_clear(e);
    CHECK(!readable(fd) && !readable(remote));
    close(remote);
    close(fd);
    dxn_event_close(e);

    /* A dup taken after the signal is already readable. */
    e = dxn_event_create(0, 1);
    fd = dxn_event_dup_fd(e);
    CHECK(readable(fd));
    /* Auto-reset: the consuming wait clears the fd too. */
    CHECK(dxn_event_wait(e, 0) == DXN_WAIT_SIGNALED);
    CHECK(!readable(fd));
    dxn_event_signal(e);
    CHECK(readable(fd));
    close(fd);
    dxn_event_close(e);
}

static void
test_non_events(void)
{
    uint32_t not_an_event[4] = { 0x41414141u, 0, 0, 0 };
    dxn_set_log_callback(count_log, NULL);
    CHECK(!dxn_event_is_event(not_an_event));
    CHECK(!dxn_event_is_event(NULL));
    CHECK(dxn_event_wait(not_an_event, 0) == DXN_WAIT_FAILED);
    CHECK(dxn_event_dup_fd(not_an_event) == -1);
    CHECK(dxn_event_duplicate(not_an_event) == NULL);
    dxn_event_signal(not_an_event);
    dxn_event_close(not_an_event);
    CHECK(warnings == 5);
    /* NULL is a silent no-op. */
    dxn_event_signal(NULL);
    CHECK(warnings == 5);
    dxn_set_log_callback(NULL, NULL);
}

int
main(void)
{
    test_manual_reset();
    test_auto_reset();
    test_auto_reset_wakes_one();
    test_duplicate();
    test_dup_fd();
    test_non_events();
    return 0;
}
