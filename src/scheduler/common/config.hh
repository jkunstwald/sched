#pragma once

#ifndef SCHED_CACHE_LINE_SIZE
#define SCHED_CACHE_LINE_SIZE 64
#endif

// some checks, can be omitted if you're confident there is no
// misuse of the library.
#ifndef SCHED_PERFORM_RUNTIME_CHECKS
#define SCHED_PERFORM_RUNTIME_CHECKS 1
#endif
