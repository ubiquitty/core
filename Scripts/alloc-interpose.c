// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Counts allocator traffic by DYLD interposition, for Scripts/count-allocations.sh.
//
// Every call is counted; nothing is sampled. That is the point of using this
// rather than a profiler: a sampled profile tells you where time went, and an
// allocation that is a sliver of the samples individually can still be the
// mechanism behind a large lane difference. A count is a fact about the run,
// and comparing two binaries' counts says whether a change removed the
// allocations it was meant to remove — evidence about the mechanism, not just
// about the clock.
//
// Interposition applies to the whole process, so the counts include the C++
// runtime's own startup traffic. The wrapper subtracts nothing: compare a run
// against another run, not against zero.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_long g_malloc, g_calloc, g_realloc, g_free, g_bytes;

typedef struct {
    const void* replacement;
    const void* replacee;
} interpose_t;
#define DYLD_INTERPOSE(_repl, _repl_ee)                            \
    __attribute__((used)) static interpose_t _interpose_##_repl_ee \
        __attribute__((section("__DATA,__interpose")))             \
        = { (const void*)&_repl, (const void*)&_repl_ee };

// Sizes are recorded as requested, not as the allocator rounded them, so
// `bytes` is what the program asked for rather than its RSS contribution.
void* ub_malloc(size_t n)
{
    atomic_fetch_add(&g_malloc, 1);
    atomic_fetch_add(&g_bytes, (long)n);
    return malloc(n);
}
void* ub_calloc(size_t count, size_t n)
{
    atomic_fetch_add(&g_calloc, 1);
    atomic_fetch_add(&g_bytes, (long)(count * n));
    return calloc(count, n);
}
void* ub_realloc(void* p, size_t n)
{
    atomic_fetch_add(&g_realloc, 1);
    atomic_fetch_add(&g_bytes, (long)n);
    return realloc(p, n);
}
void ub_free(void* p)
{
    if (p)
        atomic_fetch_add(&g_free, 1);
    free(p);
}
DYLD_INTERPOSE(ub_malloc, malloc)
DYLD_INTERPOSE(ub_calloc, calloc)
DYLD_INTERPOSE(ub_realloc, realloc)
DYLD_INTERPOSE(ub_free, free)

// stderr, because the bench writes its JSON to stdout and a harness that
// redirects stdout would otherwise swallow the counts or corrupt the JSON.
__attribute__((destructor)) static void report(void)
{
    fprintf(stderr, "ALLOC malloc=%ld calloc=%ld realloc=%ld free=%ld bytes=%ld\n",
        atomic_load(&g_malloc), atomic_load(&g_calloc), atomic_load(&g_realloc),
        atomic_load(&g_free), atomic_load(&g_bytes));
}
