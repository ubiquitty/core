# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# CheckMemcmpFold.cmake — does this compiler fold memcmp at a runtime offset?
#
# GCC folds `memcmp(&constantArray[runtimeIndex], constant, n)` as though the
# index were zero, so a search over a constant array never matches. Reproduced on
# 13.3 and 15.2, aarch64 and x86_64, at every optimization level including -O0;
# the emitted function is `mov w0, -1; ret`. Clang emits a real load at the offset
# and is correct. `-fno-builtin-memcmp` fixes it, but at a cost on a 32-byte
# compare — the shape of cellEqual — so the flag is not applied globally.
#
# Wider than PR 95189, which needs a zero byte in one operand: an array with no
# zero bytes against a needle with no terminator misfolds identically.
#
# The defect is avoided in source instead (Scripts/check-memcmp-fold.py keeps it
# out), and this probe exists so a compiler that has the bug says so at configure
# time rather than through one silently failing test.
#
# Sets UBIQUITTY_MEMCMP_FOLD_BROKEN.
#

function(ubiquitty_check_memcmp_fold)
    if(DEFINED CACHE{UBIQUITTY_MEMCMP_FOLD_BROKEN})
        return()
    endif()

    # The probe is a RUN, and a cross build has nothing to run it on. try_run
    # then asks the operator to supply the exit status through a cache variable,
    # and an unset one reads as non-zero — which is this probe's spelling of
    # "this compiler misfolds". So the question is refused rather than answered
    # wrong: every cross arm here is compiled by the same clang as its host, and
    # that host's own configure answers it.
    if(CMAKE_CROSSCOMPILING)
        set(UBIQUITTY_MEMCMP_FOLD_BROKEN "unknown" CACHE INTERNAL
            "memcmp fold probe cannot run when cross-compiling")
        message(STATUS
            "Ubiquitty: the memcmp-fold probe has to RUN its binary and this "
            "build is cross-compiling for ${CMAKE_SYSTEM_NAME} — result unknown")
        return()
    endif()

    set(_source "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/memcmp-fold-probe.cpp")
    file(WRITE "${_source}" "
#include <cstring>
#include <cstdint>
static const uint8_t data[] = { 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78 };
int main()
{
    // The literal sits at index 4. A compiler that folds against index 0
    // reports a mismatch at every index, so the search finds nothing.
    for (unsigned i = 0; i + 4 < sizeof(data); ++i) {
        if (memcmp(&data[i], \"IDAT\", 4) == 0)
            return 0; // found it: folding is correct
    }
    return 1;
}
")

    try_run(_run_result _compile_result
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/memcmp-fold-probe"
        "${_source}"
        CXX_STANDARD 23)

    if(NOT _compile_result)
        # A probe that will not build tells us nothing; do not read that as a pass.
        set(UBIQUITTY_MEMCMP_FOLD_BROKEN "unknown" CACHE INTERNAL "memcmp fold probe did not build")
        message(STATUS "Ubiquitty: memcmp-fold probe did not build — result unknown")
        return()
    endif()

    if(_run_result EQUAL 0)
        set(UBIQUITTY_MEMCMP_FOLD_BROKEN OFF CACHE INTERNAL "memcmp folds correctly at a runtime offset")
        message(STATUS "Ubiquitty: memcmp folds correctly at a runtime offset")
    else()
        set(UBIQUITTY_MEMCMP_FOLD_BROKEN ON CACHE INTERNAL "memcmp misfolds at a runtime offset")
        # A status line, not a warning: the tree already avoids the shape and
        # check-memcmp-fold.py is what rejects it, so nothing here asks anyone
        # to act. GCC folds this way on every Linux job; a warning there was
        # the same sentence on every configure, telling no one anything new.
        message(STATUS
            "Ubiquitty: this compiler folds memcmp(&constantArray[runtimeIndex], \"literal\", n) "
            "as though the index were zero.\n"
            "  Source must not use that shape — Scripts/check-memcmp-fold.py rejects it.\n"
            "  A translation unit that genuinely needs it can carry -fno-builtin-memcmp via\n"
            "  set_source_files_properties(<file> PROPERTIES COMPILE_OPTIONS -fno-builtin-memcmp);\n"
            "  the flag is not global: it measured 45% slower on a 32-byte compare.")
    endif()
endfunction()
