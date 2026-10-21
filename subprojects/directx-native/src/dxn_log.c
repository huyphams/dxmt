/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dxn_private.h"

#include <stdarg.h>
#include <stdio.h>

static dxn_log_fn log_fn;
static void *log_user;

void
dxn_set_log_callback(dxn_log_fn fn, void *user)
{
    log_user = user;
    log_fn = fn;
}

void
dxn_logf(dxn_log_level level, const char *fmt, ...)
{
    dxn_log_fn fn = log_fn;
    if (!fn)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fn(level, buf, log_user);
}
