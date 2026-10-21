/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Diagnostics from the library are routed through one callback so each
 * embedding backend can feed them to its own logger.  With no callback set
 * the library is silent.
 */

#ifndef DXN_LOG_H
#define DXN_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dxn_log_level {
    DXN_LOG_ERROR = 0,
    DXN_LOG_WARN  = 1,
    DXN_LOG_INFO  = 2,
    DXN_LOG_DEBUG = 3,
} dxn_log_level;

/* `message` is a complete line without a trailing newline. */
typedef void (*dxn_log_fn)(dxn_log_level level, const char *message,
                           void *user);

/* Install (or with NULL remove) the callback.  Set it once during start-up,
 * before other threads call into the library. */
void dxn_set_log_callback(dxn_log_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* DXN_LOG_H */
