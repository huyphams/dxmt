/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Win32-style event objects for native D3D implementations.
 *
 * Off Windows there is no NT event a HANDLE could name, so each platform
 * needs a convention for what an event HANDLE is:
 *
 *   Linux: the HANDLE is an eventfd(2), cast to a pointer; signaled ==
 *     counter > 0.  It is a kernel object with no user-space state, so any
 *     binary can create, signal, poll or close one with plain syscalls, and
 *     this library provides no functions for it.
 *   macOS: there is no kernel object with event semantics that can also be
 *     passed across processes (macOS refuses to send a kqueue fd), so the
 *     HANDLE is a pointer to user-space state, and the functions below
 *     implement it.
 *
 * A macOS event HANDLE is a small tagged box referencing a refcounted
 * core; every dxn_event_duplicate() of it is another box on the same core,
 * so a signal through any handle is seen through all of them.  The core
 * owns a kqueue whose single EVFILT_USER registration IS the signaled
 * state (manual-reset without EV_CLEAR, auto-reset with it), plus a
 * lazily-created pipe mirroring that state for dxn_event_dup_fd.  The box
 * begins with a uint32_t magic word (dxn_event_is_event() tests it), which
 * lets a backend that vends other kinds of handle (e.g. semaphores) tell
 * them apart.  Cost: one fd, three once dup_fd has been called.
 *
 * Ownership: this library is linked statically, so every binary carries
 * its own private copy of the code, and the layout of the box and core is
 * not part of the contract.  A handle may only be operated on by the copy
 * that created it.  A backend creates the events its D3D APIs accept and
 * exports create / close / dup_fd for its embedder (dxn_embedder.h); the
 * embedder treats the handle as opaque and observes it only through the fd.
 */

#ifndef DXN_EVENT_H
#define DXN_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXN_WAIT_INFINITE UINT64_MAX

typedef enum dxn_wait_status {
    DXN_WAIT_SIGNALED = 0,
    DXN_WAIT_TIMEOUT  = 1,
    DXN_WAIT_FAILED   = -1, /* not an event handle, or the wait itself failed */
} dxn_wait_status;

#ifdef __APPLE__

/* CreateEvent: manual_reset / initial_state are BOOLs (non-zero = true).
 * NULL on failure (fd exhaustion: raise RLIMIT_NOFILE). */
void *dxn_event_create(int manual_reset, int initial_state);

/* SetEvent / ResetEvent.  No-ops (with a warning) on non-event handles. */
void dxn_event_signal(void *handle);
void dxn_event_clear(void *handle);

/* CloseHandle.  The core lives until its last handle is closed; fds
 * returned by dxn_event_dup_fd survive the close. */
void dxn_event_close(void *handle);

/* DuplicateHandle: a second, independent HANDLE onto the same event.  Use it
 * to pin an event for an asynchronous signal path (a GPU completion
 * callback) that may run after the owner closed the original.  NULL on
 * allocation failure or a non-event handle. */
void *dxn_event_duplicate(void *handle);

/* A NEW fd (caller owns; close(2) it) that polls readable (POLLIN) while the
 * event is signaled, and is passable across processes.  -1 on failure or a
 * non-event handle.  Auto-reset caveat: a dxn_event_wait that consumes the
 * signal also clears the fd; a poller that only observes it does not
 * consume it. */
int dxn_event_dup_fd(void *handle);

/* WaitForSingleObject with a nanosecond timeout (DXN_WAIT_INFINITE for no
 * timeout).  Auto-reset events are consumed by the waiter that wins. */
dxn_wait_status dxn_event_wait(void *handle, uint64_t timeout_ns);

/* Nonzero when handle is a live event from this library. */
int dxn_event_is_event(const void *handle);

#endif /* __APPLE__ */

#ifdef __cplusplus
}
#endif

#endif /* DXN_EVENT_H */
