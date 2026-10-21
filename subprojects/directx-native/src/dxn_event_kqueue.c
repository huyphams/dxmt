/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Win32-style events, kqueue-backed (macOS).  See dxn_event.h for the model:
 * the kqueue trigger is the signaled state, plus a lazily-created pipe
 * mirror for the cross-process pollable fd.
 *
 *  - manual-reset registers EVFILT_USER without EV_CLEAR (the trigger stays
 *    pending, every waiter/poller sees it until an explicit clear);
 *    auto-reset registers with EV_CLEAR (retrieval consumes the trigger, and
 *    the kernel wakes exactly one blocked waiter).  kqueue provides Win32
 *    semantics directly; the kqueue is the serializer.
 *  - clearing deletes and re-adds the registration -- the only way to
 *    un-trigger EVFILT_USER.
 *  - the exported fd is a pipe: signal writes a token, clear drains it.
 *    pipe_mtx orders lazy creation against signal: a signal sets the kqueue
 *    trigger BEFORE taking the lock and creation primes the pipe under the
 *    lock AFTER peeking, so a first dup racing a signal never loses the edge
 *    (whichever critical section runs second observes the other's effect).
 *
 * The core is shared by every duplicate of a handle, so a signal through
 * any of them reaches both the kqueue and the one exported pipe.
 */

#include "directx-native/dxn_event.h"

#include "dxn_private.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#define EVT_MAGIC 0x654E5844u /* 'DXNe' */

struct dxn_evt_core {
    _Atomic uint32_t refs;
    int manual_reset; /* immutable after creation */
    int kq;           /* the signaled state; immutable after creation */
    pthread_mutex_t pipe_mtx;
    int pipe_r; /* read end handed out (dup'd) by dxn_event_dup_fd */
    int pipe_w; /* write end poked alongside the kqueue trigger */
};

/* A HANDLE: the magic tag (first, see dxn_event.h) plus the shared core. */
struct dxn_evt {
    uint32_t magic;
    struct dxn_evt_core *core;
};

static struct dxn_evt *
as_event(void *h)
{
    struct dxn_evt *e = h;
    if (!e || e->magic != EVT_MAGIC) {
        if (h)
            DXN_WARN("event: op on non-event handle %p", h);
        return NULL;
    }
    return e;
}

static int
event_register(int kq, int manual_reset)
{
    struct kevent ev;
    uint16_t flags = EV_ADD | EV_ENABLE;
    if (!manual_reset)
        flags |= EV_CLEAR;
    EV_SET(&ev, 1, EVFILT_USER, flags, 0, 0, NULL);
    return kevent(kq, &ev, 1, NULL, 0, NULL) == 0;
}

/* Close-on-exec and non-blocking (macOS has no pipe2). */
static void
set_cloexec_nonblock(int fd)
{
    int fdflags = fcntl(fd, F_GETFD);
    if (fdflags >= 0)
        fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    int stflags = fcntl(fd, F_GETFL);
    if (stflags >= 0)
        fcntl(fd, F_SETFL, stflags | O_NONBLOCK);
}

/* Peek at the signaled state.  Manual-reset: a zero-timeout read is
 * non-consuming (no EV_CLEAR).  Auto-reset: the read consumes the trigger,
 * so re-arm it with NOTE_TRIGGER -- a waiter whose wakeup this stole is
 * re-woken by the re-trigger, and the state stays signaled throughout. */
static int
event_peek_signaled(struct dxn_evt_core *c)
{
    struct kevent out;
    struct timespec zero = {0, 0};
    if (kevent(c->kq, NULL, 0, &out, 1, &zero) <= 0)
        return 0;
    if (!c->manual_reset) {
        struct kevent ev;
        EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        kevent(c->kq, &ev, 1, NULL, 0, NULL);
    }
    return 1;
}

static void
pipe_write_token(int w)
{
    if (w < 0)
        return;
    const uint8_t b = 1;
    ssize_t n;
    do {
        n = write(w, &b, 1);
    } while (n < 0 && errno == EINTR);
    /* EAGAIN: a token is already pending -- the "signaled" edge is
     * preserved, which is all a poller needs. */
}

static void
pipe_drain(int r)
{
    if (r < 0)
        return;
    uint8_t buf[64];
    ssize_t n;
    do {
        n = read(r, buf, sizeof(buf));
    } while (n > 0 || (n < 0 && errno == EINTR));
}

static int
event_ensure_pipe(struct dxn_evt_core *c)
{
    pthread_mutex_lock(&c->pipe_mtx);
    if (c->pipe_r >= 0) {
        pthread_mutex_unlock(&c->pipe_mtx);
        return 1;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        pthread_mutex_unlock(&c->pipe_mtx);
        DXN_ERROR("event: pipe() failed for exported fd: %s",
                  strerror(errno));
        return 0;
    }
    set_cloexec_nonblock(fds[0]);
    set_cloexec_nonblock(fds[1]);
    c->pipe_r = fds[0];
    c->pipe_w = fds[1];
    /* Prime the pipe if the event is already signaled so a dup taken after
     * the signal still reports readable. */
    if (event_peek_signaled(c))
        pipe_write_token(c->pipe_w);
    pthread_mutex_unlock(&c->pipe_mtx);
    return 1;
}

static void
event_signal_core(struct dxn_evt_core *c)
{
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(c->kq, &ev, 1, NULL, 0, NULL);
    /* Trigger is set; take the lock only to poke the pipe. */
    pthread_mutex_lock(&c->pipe_mtx);
    pipe_write_token(c->pipe_w);
    pthread_mutex_unlock(&c->pipe_mtx);
}

static void
event_clear_core(struct dxn_evt_core *c)
{
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_USER, EV_DELETE, 0, 0, NULL);
    kevent(c->kq, &ev, 1, NULL, 0, NULL);
    event_register(c->kq, c->manual_reset);
    pthread_mutex_lock(&c->pipe_mtx);
    pipe_drain(c->pipe_r);
    pthread_mutex_unlock(&c->pipe_mtx);
}

static void
core_unref(struct dxn_evt_core *c)
{
    if (atomic_fetch_sub_explicit(&c->refs, 1, memory_order_acq_rel) != 1)
        return;
    if (c->pipe_r >= 0)
        close(c->pipe_r);
    if (c->pipe_w >= 0)
        close(c->pipe_w);
    close(c->kq);
    pthread_mutex_destroy(&c->pipe_mtx);
    free(c);
}

static struct dxn_evt *
box_alloc(struct dxn_evt_core *c)
{
    struct dxn_evt *e = malloc(sizeof(*e));
    if (!e)
        return NULL;
    e->magic = EVT_MAGIC;
    e->core = c;
    return e;
}

void *
dxn_event_create(int manual_reset, int initial_state)
{
    int kq = kqueue();
    if (kq < 0) {
        DXN_ERROR("event: kqueue() failed: %s (fd exhaustion? raise "
                  "RLIMIT_NOFILE)", strerror(errno));
        return NULL;
    }
    int fdflags = fcntl(kq, F_GETFD);
    if (fdflags >= 0)
        fcntl(kq, F_SETFD, fdflags | FD_CLOEXEC);
    if (!event_register(kq, manual_reset)) {
        close(kq);
        return NULL;
    }

    struct dxn_evt_core *c = calloc(1, sizeof(*c));
    if (!c) {
        close(kq);
        return NULL;
    }
    atomic_store_explicit(&c->refs, 1, memory_order_relaxed);
    c->manual_reset = manual_reset != 0;
    c->kq = kq;
    pthread_mutex_init(&c->pipe_mtx, NULL);
    c->pipe_r = -1;
    c->pipe_w = -1;
    if (initial_state)
        event_signal_core(c);

    struct dxn_evt *e = box_alloc(c);
    if (!e)
        core_unref(c);
    return e;
}

void
dxn_event_signal(void *handle)
{
    struct dxn_evt *e = as_event(handle);
    if (e)
        event_signal_core(e->core);
}

void
dxn_event_clear(void *handle)
{
    struct dxn_evt *e = as_event(handle);
    if (e)
        event_clear_core(e->core);
}

void
dxn_event_close(void *handle)
{
    struct dxn_evt *e = as_event(handle);
    if (!e)
        return;
    e->magic = 0;
    core_unref(e->core);
    free(e);
}

void *
dxn_event_duplicate(void *handle)
{
    struct dxn_evt *e = as_event(handle);
    if (!e)
        return NULL;
    atomic_fetch_add_explicit(&e->core->refs, 1, memory_order_relaxed);
    struct dxn_evt *d = box_alloc(e->core);
    if (!d)
        core_unref(e->core);
    return d;
}

int
dxn_event_dup_fd(void *handle)
{
    struct dxn_evt *e = as_event(handle);
    if (!e)
        return -1;
    if (!event_ensure_pipe(e->core))
        return -1;
    pthread_mutex_lock(&e->core->pipe_mtx);
    int fd = e->core->pipe_r >= 0
                 ? fcntl(e->core->pipe_r, F_DUPFD_CLOEXEC, 0) : -1;
    pthread_mutex_unlock(&e->core->pipe_mtx);
    return fd;
}

dxn_wait_status
dxn_event_wait(void *handle, uint64_t timeout_ns)
{
    struct dxn_evt *e = as_event(handle);
    if (!e)
        return DXN_WAIT_FAILED;

    uint64_t deadline = 0;
    if (timeout_ns != DXN_WAIT_INFINITE)
        deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC) + timeout_ns;

    for (;;) {
        struct timespec ts;
        struct timespec *tsp = NULL;
        if (timeout_ns != DXN_WAIT_INFINITE) {
            uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
            uint64_t left = deadline > now ? deadline - now : 0;
            ts.tv_sec = (time_t)(left / 1000000000ull);
            ts.tv_nsec = (long)(left % 1000000000ull);
            tsp = &ts;
        }
        struct kevent out;
        int n = kevent(e->core->kq, NULL, 0, &out, 1, tsp);
        if (n > 0) {
            /* Auto-reset: this waiter consumed the single trigger, so the
             * pollable pipe must clear too.  Manual-reset stays signaled.
             * The drain can also eat the token of a second signal that
             * landed after our kevent consumed the trigger; if the event is
             * signaled again already, put a token back so the exported fd
             * stays in sync with the kqueue state. */
            if (!e->core->manual_reset) {
                pthread_mutex_lock(&e->core->pipe_mtx);
                pipe_drain(e->core->pipe_r);
                if (event_peek_signaled(e->core))
                    pipe_write_token(e->core->pipe_w);
                pthread_mutex_unlock(&e->core->pipe_mtx);
            }
            return DXN_WAIT_SIGNALED;
        }
        if (n == 0)
            return DXN_WAIT_TIMEOUT;
        if (errno != EINTR) {
            DXN_WARN("event: kevent wait failed: %s", strerror(errno));
            return DXN_WAIT_FAILED;
        }
    }
}

int
dxn_event_is_event(const void *handle)
{
    const struct dxn_evt *e = handle;
    return e && e->magic == EVT_MAGIC;
}
