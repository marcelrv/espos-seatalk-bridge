/*
 * SPDX-FileCopyrightText: 2026 Marcel Verpaalen
 * SPDX-License-Identifier: Apache-2.0
 */
/* Tiny self-contained test framework for host tests. One executable per test
 * file: define tests as `static void test_x(void)`, then in main() use RUN()
 * and `return test_report();`. */
#pragma once

#include <stdio.h>
#include <string.h>

static int g_tests_run;
static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                       \
    do {                                                                         \
        const long long va_ = (long long)(a);                                    \
        const long long vb_ = (long long)(b);                                    \
        if (va_ != vb_) {                                                        \
            printf("%s:%d: CHECK_EQ_INT failed: %s == %s\n    expected %lld, actual %lld\n", \
                   __FILE__, __LINE__, #a, #b, vb_, va_);                        \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ_MEM(a, b, n)                                                    \
    do {                                                                         \
        const unsigned char *pa_ = (const unsigned char *)(a);                   \
        const unsigned char *pb_ = (const unsigned char *)(b);                   \
        size_t n_ = (size_t)(n);                                                 \
        if (memcmp(pa_, pb_, n_) != 0) {                                         \
            size_t i_;                                                           \
            printf("%s:%d: CHECK_EQ_MEM failed: %s vs %s (%zu bytes)\n    expected:", \
                   __FILE__, __LINE__, #a, #b, n_);                              \
            for (i_ = 0; i_ < n_; i_++) printf(" %02X", pb_[i_]);                \
            printf("\n    actual:  ");                                           \
            for (i_ = 0; i_ < n_; i_++) printf(" %02X", pa_[i_]);                \
            printf("\n");                                                        \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define RUN(fn)                                                                  \
    do {                                                                         \
        int before_ = g_failures;                                                \
        g_tests_run++;                                                           \
        fn();                                                                    \
        printf("[%s] %s\n", g_failures == before_ ? " OK " : "FAIL", #fn);       \
    } while (0)

static int test_report(void)
{
    printf("%d tests, %d failures\n", g_tests_run, g_failures);
    return g_failures ? 1 : 0;
}
