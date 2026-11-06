# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# run_boundary_probe.cmake — compiles one translation unit against a single
# include directory and holds the result to the outcome its caller names.
#
#   cmake -DCXX=<compiler> -DINCLUDE_DIR=<stage> -DSRC=<source>
#         -DEXPECT=pass|fail -DOUT=<directory> -P run_boundary_probe.cmake
#
# EXPECT=fail turns the usual reading of an exit status around. Such a probe is
# there to show that a behaviour header cannot be reached from the model stage,
# so a compiler that succeeds has found the defect rather than cleared it. The
# two directions are therefore written out separately below.

foreach(_argument IN ITEMS CXX INCLUDE_DIR SRC EXPECT OUT)
    if(NOT DEFINED ${_argument})
        message(FATAL_ERROR "run_boundary_probe.cmake: -D${_argument} has no default and was not given")
    endif()
endforeach()

# A stage that was never generated would make every probe meaningless.
if(NOT EXISTS "${INCLUDE_DIR}")
    message(FATAL_ERROR
        "boundary probe: there is no ${INCLUDE_DIR} to compile against.\n"
        "Nothing generated the layer stage, so this probe could only pass vacuously.")
endif()

execute_process(
    COMMAND "${CXX}" -std=c++23 -fsyntax-only "-I${INCLUDE_DIR}" "${SRC}"
    RESULT_VARIABLE _probe_status
    OUTPUT_VARIABLE _probe_stdout
    ERROR_VARIABLE _probe_stderr)
# A status that is not a number means the compiler never ran, which counts as
# not having compiled.
if(_probe_status EQUAL 0)
    set(_probe_compiled TRUE)
else()
    set(_probe_compiled FALSE)
endif()

if(NOT EXPECT STREQUAL "pass" AND NOT EXPECT STREQUAL "fail")
    message(FATAL_ERROR "run_boundary_probe.cmake: EXPECT is '${EXPECT}'; it takes pass or fail")
elseif(EXPECT STREQUAL "pass" AND NOT _probe_compiled)
    message(FATAL_ERROR
        "boundary probe: ${SRC} did not compile against ${INCLUDE_DIR},\n"
        "and it has to. An empty or misplaced stage fails here, and left alone it\n"
        "would let the negative probe pass without testing anything.\n${_probe_stderr}")
elseif(EXPECT STREQUAL "pass")
    message(STATUS "boundary probe: ${SRC} compiled against the stage, as required")
elseif(_probe_compiled)
    message(FATAL_ERROR
        "boundary probe: ${SRC} compiled against ${INCLUDE_DIR}, which means a\n"
        "behaviour header can be reached from the model stage and the Rule-10\n"
        "boundary is not enforced at compile time. Look at the module lists\n"
        "Scripts/check-core-layer-partition.sh emits and\n"
        "Scripts/cmake/UbiquittyCoreLibrary.cmake stages: an entry as broad as a\n"
        "bare \"Core\" matches every behaviour subtree.")
else()
    message(STATUS "boundary probe: no behaviour header is reachable from the model stage")
endif()
