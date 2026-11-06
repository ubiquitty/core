# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# ToolchainFloor.cmake — what this tree asks of a compiler, answered at configure
# time: the oldest versions it builds with, and whether the compiler can read the
# checked-in training profile.
#
# Source/Base/ToolchainFloor.h asserts what portable Core needs from a compiler,
# as the standard's own macros. It fires on the first translation unit that
# reaches it, which is the whole build later and once per file. The floor here
# states the same requirement as the compiler versions that meet it, before
# anything is compiled, and names what to install instead.
#
#   GNU 14    The first GCC that reports 202302L for -std=c++23 and parses
#             deducing this (Base/WeakPtr.h). GCC 13, what Ubuntu 24.04's `g++`
#             installs, stamps 202100L and rejects the explicit object parameter.
#   Clang 19  The first clang whose concepts support clears libstdc++'s gate for
#             std::expected (`__cpp_concepts` 202002L). Clang 18, what Ubuntu
#             24.04's `clang` installs, reports 201907L and finds <expected>
#             empty against libstdc++. Over libc++ it would have the template,
#             and it is refused anyway: the standard library is chosen by flags
#             this check cannot see, so the floor is stated per compiler.
#
# AppleClang is not version-checked here. Apple numbers its clang independently
# of LLVM, so no LLVM floor translates to it. The macOS app names the Apple
# clang it needs, and the header above still refuses a toolchain that lacks
# C++23 or std::expected. Emscripten reports as Clang and carries its own LLVM,
# so the Clang floor applies to it as written.
#
#   ubiquitty_check_toolchain_floor(<compiler-id> <compiler-version>)
#   ubiquitty_compiler_reads_profile(<result-var> <reason-var> <profile> <cache-key>)
#

set(UBIQUITTY_GCC_FLOOR 14)
set(UBIQUITTY_CLANG_FLOOR 19)

# Stops the configure when the compiler is below the floor.
#
# - Parameters:
#   - compilerID: `CMAKE_CXX_COMPILER_ID`.
#   - compilerVersion: `CMAKE_CXX_COMPILER_VERSION`.
function(ubiquitty_check_toolchain_floor compilerID compilerVersion)
    if(compilerID STREQUAL "GNU" AND compilerVersion VERSION_LESS UBIQUITTY_GCC_FLOOR)
        message(FATAL_ERROR
            "Ubiquitty needs GCC ${UBIQUITTY_GCC_FLOOR} or newer; this is GCC ${compilerVersion}.\n"
            "  GCC ${compilerVersion} does not implement enough of C++23 for portable Core "
            "(deducing this, and a __cplusplus of 202302L).\n"
            "  On Ubuntu 24.04: sudo apt install g++-${UBIQUITTY_GCC_FLOOR}, then configure a fresh "
            "build directory with CC=gcc-${UBIQUITTY_GCC_FLOOR} CXX=g++-${UBIQUITTY_GCC_FLOOR} "
            "(for example CC=gcc-${UBIQUITTY_GCC_FLOOR} CXX=g++-${UBIQUITTY_GCC_FLOOR} ./Scripts/build-host.sh --clean).\n"
            "  Clang ${UBIQUITTY_CLANG_FLOOR} or newer also works.")
    elseif(compilerID STREQUAL "Clang" AND compilerVersion VERSION_LESS UBIQUITTY_CLANG_FLOOR)
        message(FATAL_ERROR
            "Ubiquitty needs Clang ${UBIQUITTY_CLANG_FLOOR} or newer; this is Clang ${compilerVersion}.\n"
            "  Against libstdc++, Clang ${compilerVersion} gets an empty <expected>, and portable Core "
            "uses std::expected.\n"
            "  On Ubuntu 24.04: sudo apt install clang-${UBIQUITTY_CLANG_FLOOR}, then configure a fresh "
            "build directory with CC=clang-${UBIQUITTY_CLANG_FLOOR} CXX=clang++-${UBIQUITTY_CLANG_FLOOR}; "
            "or install g++-${UBIQUITTY_GCC_FLOOR} and use CC=gcc-${UBIQUITTY_GCC_FLOOR} "
            "CXX=g++-${UBIQUITTY_GCC_FLOOR}.")
    endif()
endfunction()

# Answers whether the C++ compiler reads an LLVM profile under -fprofile-use.
#
# Being clang is necessary and not sufficient. An indexed profile carries the
# format version of the llvm-profdata that wrote it, and a clang older than that
# refuses the file with "unsupported instrumentation profile format version" on
# every translation unit it compiles. So the compiler is asked directly, by
# compiling one empty translation unit with the flag, rather than its version
# being compared against a table of which LLVM wrote which format. The compile
# is never linked or run, which keeps the probe valid for a cross build.
#
# The answer is cached under `cacheKey`, which the caller derives from the
# profile's content: a retrained profile is asked about again, and an unchanged
# one costs a single compile per build directory.
#
# - Parameters:
#   - resultVariable: set to TRUE when the compiler read the profile, FALSE
#     otherwise.
#   - reasonVariable: set to the compiler's own complaint when it did not, or
#     to an empty string.
#   - profile: path to the `.profdata`.
#   - cacheKey: a string that changes whenever the profile's content does.
function(ubiquitty_compiler_reads_profile resultVariable reasonVariable profile cacheKey)
    set(_readableVariable UBIQUITTY_PROFILE_READABLE_${cacheKey})
    set(_reasonVariable UBIQUITTY_PROFILE_REFUSAL_${cacheKey})
    if(NOT DEFINED CACHE{${_readableVariable}})
        set(_probeDirectory "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/profile-probe")
        file(WRITE "${_probeDirectory}/probe.cpp" "int ubiquittyProfileProbe() { return 0; }\n")
        set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
        try_compile(_readable "${_probeDirectory}/build" "${_probeDirectory}/probe.cpp"
            COMPILE_DEFINITIONS
                "-fprofile-use=${profile}"
                -Wno-profile-instr-unprofiled
                -Wno-profile-instr-out-of-date
                -Wno-backend-plugin
            OUTPUT_VARIABLE _output)
        set(_refusal "")
        if(NOT _readable)
            string(REGEX MATCH "[Ee]rror in reading profile[^\n]*" _refusal "${_output}")
            if(NOT _refusal)
                set(_refusal "the probe compile failed; the configure log under CMakeFiles/ holds its output")
            endif()
        endif()
        set(${_readableVariable} "${_readable}" CACHE INTERNAL
            "Whether the C++ compiler reads the training profile with this content")
        set(${_reasonVariable} "${_refusal}" CACHE INTERNAL
            "What the C++ compiler said when it could not read the training profile")
    endif()
    if($CACHE{${_readableVariable}})
        set(${resultVariable} TRUE PARENT_SCOPE)
    else()
        set(${resultVariable} FALSE PARENT_SCOPE)
    endif()
    set(${reasonVariable} "$CACHE{${_reasonVariable}}" PARENT_SCOPE)
endfunction()
