# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Writes a stamp naming a compiler and the standard library it builds against,
# and touches it only when either changed.
#
#   cmake -DCOMPILER=<c++ compiler> -DSTAMP=<path> -P CompilerIdentity.cmake
#
# A build step that depends on the stamp reruns when the toolchain is upgraded
# in place, which keeps its paths and gives its files a package build time that
# can be older than the step's output. The compiler's version line names the
# compiler; the library's own version macro names the library, which a
# distribution upgrades apart from the compiler that uses it.

if(NOT COMPILER OR NOT STAMP)
    message(FATAL_ERROR "CompilerIdentity.cmake needs COMPILER and STAMP")
endif()

execute_process(COMMAND "${COMPILER}" --version
    OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(REGEX REPLACE "\n.*" "" version "${version}")

set(probe "${STAMP}.probe.cpp")
file(WRITE "${probe}" "#include <version>\n")
execute_process(COMMAND "${COMPILER}" -std=c++23 -dM -E "${probe}"
    OUTPUT_VARIABLE macros ERROR_QUIET)
file(REMOVE "${probe}")
string(REGEX MATCHALL "#define (__GLIBCXX__|_LIBCPP_VERSION|_GLIBCXX_RELEASE) [^\n]*" library "${macros}")
list(JOIN library " " library)

set(content "${version}\n${library}\n")
set(previous "")
if(EXISTS "${STAMP}")
    file(READ "${STAMP}" previous)
endif()
if(NOT previous STREQUAL content)
    file(WRITE "${STAMP}" "${content}")
endif()
