#pragma once

// SCHED_RUNTIME_ASSERT -----------------------------------------------------------
// used to check conditions, to test for erros
// defined only if SCHED_RUNTIME_CHECKS is evaluated to true.
#ifdef SCHED_RUNTIME_CHECKS
#include <cstdio>
#include <cstdlib>
#define SCHED_RUNTIME_ASSERT(cond, ...)                       \
    if (!(cond))                                              \
    {                                                         \
        printf("-- PX_SCHED ERROR: -------------------\n");   \
        printf("-- %s:%d\n", __FILE__, __LINE__);             \
        printf("--------------------------------------\n");   \
        printf(__VA_ARGS__);                                  \
        printf("\n--------------------------------------\n"); \
        std::abort();                                         \
    }
#else
#define SCHED_RUNTIME_ASSERT(...) /*NO CHECK*/
#endif
