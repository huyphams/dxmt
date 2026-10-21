/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DXN_TEST_H
#define DXN_TEST_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,       \
                    __LINE__, #cond);                                     \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

#endif /* DXN_TEST_H */
