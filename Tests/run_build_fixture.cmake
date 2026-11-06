# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# run_build_fixture.cmake — the ubq_build fixture: build the test tree with the
# job count Scripts/build_jobs.py gives this machine.
#
# Invoked as:
#   cmake -DBUILD_DIR=... -DPYTHON=... -DJOBS_SCRIPT=... -P run_build_fixture.cmake
#
# A bare `cmake --build` leaves the fan-out to Ninja, which runs a compiler per
# core whatever the machine's memory or UBQ_BUILD_JOBS says. The count is read
# when the test runs rather than when the tree is configured, because one tree
# is run both at a machine's full speed and under a caller's cap.
#

foreach(_required BUILD_DIR PYTHON JOBS_SCRIPT)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "run_build_fixture.cmake: -D${_required} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${PYTHON}" "${JOBS_SCRIPT}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _jobs
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _result EQUAL 0 OR NOT _jobs MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "build fixture: ${JOBS_SCRIPT} gave no job count: '${_jobs}'")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --parallel "${_jobs}"
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "build fixture: the build failed (above)")
endif()
