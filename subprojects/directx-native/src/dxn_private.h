/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DXN_PRIVATE_H
#define DXN_PRIVATE_H

#include "directx-native/dxn_log.h"

#if defined(__GNUC__) || defined(__clang__)
#define DXN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define DXN_PRINTF(f, a)
#endif

void dxn_logf(dxn_log_level level, const char *fmt, ...) DXN_PRINTF(2, 3);

#define DXN_ERROR(...) dxn_logf(DXN_LOG_ERROR, __VA_ARGS__)
#define DXN_WARN(...)  dxn_logf(DXN_LOG_WARN, __VA_ARGS__)

#endif /* DXN_PRIVATE_H */
