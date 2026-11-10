# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# UbiquittyTests.cmake — registers a repository's host tests with CTest.
#
# A Tests/CMakeLists.txt includes this module, finds its own test_*.cpp files,
# and hands them over rooted at the checkout that owns them. Each test is
# compiled into an executable of its own and run by ctest, and each stanza a
# shell driver lists becomes a ctest entry of its own.
#
#   ubiquitty_add_host_tests(
#       ROOT <checkout> SOURCES <path under <checkout>/Tests>...
#       [ROOT <checkout> SOURCES <path under <checkout>/Tests>...]...
#       INCLUDE_DIRECTORIES <directory>...
#       [BOUNDARY <test>...]
#       [NO_PCH <test>...]
#       [DEFERRED <test>...]
#       [UNAVAILABLE <test>...]
#       [FRONTEND_DIRECTORIES <subsystem>...])
#
#   ubiquitty_set_test_entry_properties(<ctest entry>...)
#   ubiquitty_default_shell_driver(<variable>)
#   ubiquitty_read_shell_stanzas(<variable> DRIVER <script> [LISTING <flag>])
#   ubiquitty_lock_shell_build_trees(DRIVER <script>)
#   ubiquitty_schedule_shell_stanzas(DRIVER <script> [STANZAS <stanza>...])
#   ubiquitty_finish_host_tests([DRIVER <script>])
#
# Each test is one of two kinds.
#
#   ordinary    Linked against ubiquitty-core, the aggregate of the model,
#               behaviour and bundled POSIX backend layers. Nearly every test is
#               one, and the suite compiles the Core once for all of them.
#
#   boundary    Named in BOUNDARY. Each checks a layering claim, that some
#               header compiles against a restricted set of dependencies, so its
#               link line is what is under test: its own source and whatever its
#               <test>_EXTRA_SOURCES grants it, and nothing more.
#
# A test left out of CTest is one of two kinds as well. A DEFERRED test needs a
# symbol only a frontend supplies, so it cannot link here; it is compiled into
# an OBJECT library and left to the tree's shell driver. An UNAVAILABLE test cannot
# even compile on this host, so nothing is built for it. Both are counted.
#
# Settings for one test are variables in the caller's scope, named for the test:
#
#   <test>_EXTRA_SOURCES   compiled into the test beside its own source: a core
#                          source by its path from the core checkout's root, any
#                          other by its absolute path in its owner's checkout
#   <test>_EXTRA_DEFINES   compile definitions; they keep the test off the
#                          shared precompiled header
#   <test>_EXTRA_INCLUDES  include directories a vendored source needs
#   <test>_EXTRA_LIBS      linked beside ubiquitty-core
#   <test>_SHARDS          the number of ctest entries over one binary, each
#                          passed --shard I/N
#   <test>_LAUNCHER        a command the entry runs the binary under
#
# A shell driver's stanzas take settings of their own, named for their ctest
# entries; ubiquitty_schedule_shell_stanzas lists them and says what a driver
# must answer.
#
# The functions create their targets and ctest entries in the calling
# directory, which is the test tree. ctest-covered.txt, the shell lane's stanza
# list and CTestCustom.cmake are written to the top of the build tree, where
# the tree's shell driver and the post-test census read them.
#

include_guard(GLOBAL)

if(NOT DEFINED UBIQUITTY_CORE_SOURCE_DIR)
    message(FATAL_ERROR "UbiquittyTests.cmake needs UBIQUITTY_CORE_SOURCE_DIR, the core checkout's root")
endif()

# The shared precompiled header is force-included into every ordinary test,
# which is why -DUBIQUITTY_TEST_PCH=OFF must keep working and must stay in CI. A
# test that drops an #include it still depends on compiles clean with the header
# and fails without it, so the OFF configuration is what proves every test's
# include list stands on its own.
option(UBIQUITTY_TEST_PCH "Share one precompiled header across the test targets" ON)

# A hang has to fail, and fail quickly. Some tests pass an indefinite timeout on
# purpose — Tests/PAL/test_poll.cpp does, so a backend that parks where the
# contract says it must return is caught rather than passing quietly — and
# without a bound here that test would hold a ctest slot for CTest's 1500-second
# default. Most entries finish in about a second, so this bound turns a deadlock
# red fast. Entries that legitimately need longer take their own TIMEOUT
# rather than lifting this one, because a bound raised to fit the slowest entry
# stops catching deadlocks in the rest. An instrumented run needs more than this:
# run-coverage.sh raises it, and says why there.
set(UBIQUITTY_TEST_TIMEOUT 60 CACHE STRING
    "Seconds before a single test entry is killed and reported as a failure")

# Where per-test coverage profiles land. One directory decided here rather than
# by whoever exports LLVM_PROFILE_FILE, so the writer and the reader of these
# files agree without a second place to keep in step.
set(UBIQUITTY_COVERAGE_RAW_DIR "${CMAKE_BINARY_DIR}/coverage-raw" CACHE PATH
    "Directory for per-test .profraw files under -DUBIQUITTY_COVERAGE=ON")

# -DUBIQUITTY_CTEST_SHELL_REMAINDER=OFF leaves the shell stanzas out, for a
# quick loop on the C++ tests: those take seconds, and the stanzas take minutes.
option(UBIQUITTY_CTEST_SHELL_REMAINDER
    "Schedule the shell driver's remaining stanzas as ctest entries, one each" ON)

# The build, run as a fixture. Left alone, ctest runs whatever binaries the tree
# already holds, so a compile that failed would leave the old ones to report a
# green suite, and a broken build would read exactly like a passing one. This
# entry builds before any other; when it fails, every entry requiring it is
# reported Not Run and ctest exits non-zero.
#
# The bound stops a wedged compile from holding the runner; it is not the
# deadlock detector, which is UBIQUITTY_TEST_TIMEOUT, per entry. A hosted
# ubuntu-24.04 runner with an empty compiler cache reached 1303 of 2441 build
# steps in 1800 seconds, so its cold build needs about 3400, and a build cut
# off near its end loses every entry to the build, which says nothing about the
# code under test. The bound has to fit the COLD build, because a warm one is
# never near it and a cold one is exactly the case that should still finish —
# after a cache eviction, a compiler change, or a first run on a new image.
function(_ubiquitty_add_build_fixture)
    if(TEST ubq_build)
        return()
    endif()
    add_test(NAME ubq_build
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DPYTHON=${Python3_EXECUTABLE}
            -DJOBS_SCRIPT=${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/build_jobs.py
            -P ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/run_build_fixture.cmake)
    set_tests_properties(ubq_build PROPERTIES
        FIXTURES_SETUP ubq_build_fixture
        TIMEOUT 5400)
endfunction()

# What every entry of the suite carries: it waits for the build fixture, it is
# bounded by UBIQUITTY_TEST_TIMEOUT, and it reads 77, UbiquittyTest::
# skippedExitCode (Tests/Harness.h), as Skipped, so a binary in which every
# case declined is counted out loud instead of as a pass.
#
# Under coverage every test process writes a profile, but the default name
# carries only the PID — so the merged profile answers "was this line reached"
# and cannot answer "by which test". Naming the file after the ctest entry makes
# the second question answerable, which is what lets a tool run the tests that
# reach a line instead of all of them. %p stays: a test that forks writes one
# profile per process, and the prefix still attributes every one of them to the
# entry that spawned it.
function(ubiquitty_set_test_entry_properties)
    _ubiquitty_add_build_fixture()
    foreach(_entry IN LISTS ARGN)
        set_tests_properties(${_entry} PROPERTIES
            FIXTURES_REQUIRED ubq_build_fixture
            SKIP_RETURN_CODE 77
            TIMEOUT ${UBIQUITTY_TEST_TIMEOUT})
        if(UBIQUITTY_COVERAGE)
            set_tests_properties(${_entry} PROPERTIES
                ENVIRONMENT "LLVM_PROFILE_FILE=${UBIQUITTY_COVERAGE_RAW_DIR}/${_entry}--%p.profraw")
        endif()
    endforeach()
endfunction()

# One precompiled header, built once and shared by every ordinary test through
# REUSE_FROM. A per-target precompiled header builds separately for each test
# target and costs more than it saves.
#
# Two exclusions are mandatory, and ubiquitty_add_host_tests enforces both:
#
#   Boundary tests assert that a header compiles against a restricted dependency
#   set. A force-included precompiled header supplies that set from outside the
#   header under test, so the assertion passes for a header that no longer
#   stands alone.
#
#   EXTRA_DEFINES change the preprocessor state the precompiled header was built
#   under, and clang rejects one whose flags don't match its consumer.
#
# Only stable, widely included headers belong here. Every ordinary test parses
# this set whether or not it includes these headers, so a change to any one of
# them invalidates the precompiled header for the whole suite.
function(_ubiquitty_add_test_precompiled_header)
    add_library(ubiquitty-test-pch OBJECT ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/PCHCarrier.cpp)
    target_link_libraries(ubiquitty-test-pch PRIVATE ubiquitty-core)
    target_include_directories(ubiquitty-test-pch PRIVATE ${ARGN})
    target_compile_features(ubiquitty-test-pch PRIVATE cxx_std_23)
    target_compile_options(ubiquitty-test-pch PRIVATE
        -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-multichar -Wno-comment
        ${UBIQUITTY_OWNERSHIP_WARNINGS})
    target_precompile_headers(ubiquitty-test-pch PRIVATE
        ${UBIQUITTY_CORE_SOURCE_DIR}/Tests/Harness.h
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/TerminalOperations.h
        ${UBIQUITTY_CORE_SOURCE_DIR}/Source/Core/CellGrid.h)
    # A compiler or standard library upgraded in place keeps its paths and takes
    # its package's build time, which can be older than the precompiled header,
    # so nothing rebuilt the header and every test refused it: g++ as "created by
    # a different GCC executable", clang as a standard header "modified since the
    # precompiled header was built". The source the header is compiled from
    # depends on a stamp naming the toolchain, rewritten only when the toolchain
    # changes, so an upgrade rebuilds the header. It is a dependency rather than
    # a header in the list: CMake keeps cmake_pch.hxx's time when the list
    # changes, so a new entry would leave every existing tree's header stale.
    set(_toolchain_stamp ${CMAKE_CURRENT_BINARY_DIR}/test-pch-toolchain.stamp)
    add_custom_target(ubiquitty-test-pch-toolchain
        COMMAND ${CMAKE_COMMAND} -DCOMPILER=${CMAKE_CXX_COMPILER} -DSTAMP=${_toolchain_stamp}
            -P ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/cmake/CompilerIdentity.cmake
        BYPRODUCTS ${_toolchain_stamp}
        VERBATIM)
    add_dependencies(ubiquitty-test-pch ubiquitty-test-pch-toolchain)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ubiquitty-test-pch.dir/cmake_pch.hxx.cxx
        PROPERTIES OBJECT_DEPENDS ${_toolchain_stamp})
    # g++ builds the precompiled header past ccache. A .gch the cache restored
    # has made a clean tree redefine every type the header holds, and rebuilding
    # it uncached cleared that. Moving any header it holds already makes ccache
    # miss this compile, so the cache saves nothing here.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_target_properties(ubiquitty-test-pch PROPERTIES CXX_COMPILER_LAUNCHER "")
    endif()
endfunction()

# Compiles and registers the tests each ROOT/SOURCES group names, a group per
# owning checkout. The groups are merged and ordered by the path under Tests/,
# so the checkouts' locations do not reorder the targets or the ctest entries.
function(ubiquitty_add_host_tests)
    # ROOT and SOURCES repeat, so the arguments are read in order:
    # cmake_parse_arguments would merge every group into one.
    set(_list_keywords INCLUDE_DIRECTORIES BOUNDARY NO_PCH DEFERRED UNAVAILABLE FRONTEND_DIRECTORIES)
    foreach(_keyword IN LISTS _list_keywords)
        set(_argument_${_keyword} "")
    endforeach()
    set(_keyword "")
    set(_root "")
    set(_test_paths "")
    set(_test_roots "")
    foreach(_argument IN LISTS ARGN)
        if(_argument STREQUAL "ROOT" OR _argument STREQUAL "SOURCES" OR _argument IN_LIST _list_keywords)
            set(_keyword ${_argument})
            if(_keyword STREQUAL "SOURCES" AND _root STREQUAL "")
                message(FATAL_ERROR "ubiquitty_add_host_tests: SOURCES needs the ROOT of the checkout holding them")
            endif()
        elseif(_keyword STREQUAL "ROOT")
            set(_root "${_argument}")
            set(_keyword "")
        elseif(_keyword STREQUAL "SOURCES")
            list(FIND _test_paths "${_argument}" _claimed)
            if(NOT _claimed EQUAL -1)
                list(GET _test_roots ${_claimed} _claimant)
                message(FATAL_ERROR
                    "ubiquitty_add_host_tests: Tests/${_argument} is claimed by both ${_claimant} and ${_root}")
            endif()
            list(APPEND _test_paths "${_argument}")
            list(APPEND _test_roots "${_root}")
        elseif(NOT _keyword STREQUAL "")
            list(APPEND _argument_${_keyword} "${_argument}")
        else()
            message(FATAL_ERROR "ubiquitty_add_host_tests: unexpected argument '${_argument}'")
        endif()
    endforeach()

    if(UBIQUITTY_TEST_PCH AND NOT TARGET ubiquitty-test-pch)
        _ubiquitty_add_test_precompiled_header(${_argument_INCLUDE_DIRECTORIES})
    endif()
    _ubiquitty_add_build_fixture()

    set(_ordered_paths ${_test_paths})
    list(SORT _ordered_paths)
    foreach(_test_path IN LISTS _ordered_paths)
        list(FIND _test_paths "${_test_path}" _index)
        list(GET _test_roots ${_index} _test_root)
        set(_test_source ${_test_root}/Tests/${_test_path})
        get_filename_component(_test_name ${_test_source} NAME_WE)

        # A test named for the platform it exercises compiles only there: these
        # reach Apple framework headers directly, and discovery is by glob, so
        # this is what keeps a Linux configure from building them.
        if(NOT APPLE)
            if(_test_source MATCHES "/test_macos_[^/]*\\.(cpp|mm)$")
                continue()
            endif()
            # A target whose extra sources include Objective-C++ needs a
            # compiler this host does not have, whatever the test is called,
            # and a source under an Apple platform directory reaches Apple's
            # SDK whatever its extension: Platform/Apple/Clock.cpp is C++ over
            # mach/mach_time.h. The name check above sees neither, and either
            # fails the whole configure, taking every other test with it.
            set(_candidate_extra "${${_test_name}_EXTRA_SOURCES}")
            if(_candidate_extra MATCHES "\\.mm(;|$)"
                    OR _candidate_extra MATCHES "(^|;|/)Source/Platform/(Apple|macOS|iOS)/")
                continue()
            endif()
        endif()

        if(_test_name IN_LIST _argument_UNAVAILABLE)
            set_property(DIRECTORY APPEND PROPERTY UBIQUITTY_HOST_TESTS_LEFT_OUT ${_test_name})
            continue()
        endif()

        # A test deferred by name, or one under a frontend's directory, needs
        # something only a frontend supplies; that layer stages nowhere by
        # design. The subsystem is the first directory under Tests/ of the
        # checkout the source was taken from.
        string(REGEX MATCH "^[^/]*" _subsystem "${_test_path}")
        if(_test_name IN_LIST _argument_DEFERRED OR _subsystem IN_LIST _argument_FRONTEND_DIRECTORIES)
            set_property(DIRECTORY APPEND PROPERTY UBIQUITTY_HOST_TESTS_LEFT_OUT ${_test_name})
            # A deferred test cannot LINK here — that is why it is deferred —
            # but it can compile, and compiling is what catches a change to it.
            # Without this, a deferred test is built by nothing until the shell
            # driver reaches it, and the shell driver stops at the first
            # failure, so one broken file hides the rest. An OBJECT library
            # compiles and never links, which is exactly the check a deferred
            # test can take.
            add_library(ubq-deferred-${_test_name} OBJECT ${_test_source})
            target_include_directories(ubq-deferred-${_test_name} PRIVATE ${_argument_INCLUDE_DIRECTORIES})
            target_compile_features(ubq-deferred-${_test_name} PRIVATE cxx_std_23)
            target_compile_options(ubq-deferred-${_test_name} PRIVATE
                -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-multichar -Wno-comment
                ${UBIQUITTY_OWNERSHIP_WARNINGS})
            continue()
        endif()

        # The target takes absolute paths. The same pass notes any Objective-C++
        # source, which keeps the target off the shared precompiled header.
        set(_resolved_extra_sources "")
        set(_test_has_objcxx OFF)
        foreach(_extra_source IN LISTS ${_test_name}_EXTRA_SOURCES)
            if(IS_ABSOLUTE "${_extra_source}")
                list(APPEND _resolved_extra_sources ${_extra_source})
            else()
                list(APPEND _resolved_extra_sources ${UBIQUITTY_CORE_SOURCE_DIR}/${_extra_source})
            endif()
            if(_extra_source MATCHES "\\.mm$")
                set(_test_has_objcxx ON)
            endif()
        endforeach()

        add_executable(${_test_name} ${_test_source} ${_resolved_extra_sources})

        # Every test reads the include directories it is handed. A boundary test
        # gets nothing more, neither the Core library nor a layer stage, so the
        # header it covers has to compile from its own includes; its link line
        # is the assertion.
        target_include_directories(${_test_name} PRIVATE ${_argument_INCLUDE_DIRECTORIES})
        if(_test_name IN_LIST _argument_BOUNDARY)
            # The harness's Preferences pin names Core symbols, and with no Core
            # to link they would be undefined, so this define compiles the pin
            # out.
            target_compile_definitions(${_test_name} PRIVATE UBIQUITTY_TEST_WITHOUT_CORE)
        else()
            # The session facade links Core publicly, so naming both repeats Core
            # on the link line, which Apple's linker reports as a warning.
            set(_test_core_library ubiquitty-core)
            if("ubiquitty-kit" IN_LIST ${_test_name}_EXTRA_LIBS)
                set(_test_core_library)
            endif()
            target_link_libraries(${_test_name} PRIVATE ${_test_core_library} ${${_test_name}_EXTRA_LIBS})
        endif()

        if(DEFINED ${_test_name}_EXTRA_DEFINES)
            target_compile_definitions(${_test_name} PRIVATE ${${_test_name}_EXTRA_DEFINES})
        endif()

        # A target pulling in a vendored source needs that vendor's header root,
        # which the include directories above do not carry.
        if(DEFINED ${_test_name}_EXTRA_INCLUDES)
            target_include_directories(${_test_name} PRIVATE ${${_test_name}_EXTRA_INCLUDES})
        endif()

        # The exclusions _ubiquitty_add_test_precompiled_header describes, and
        # one more: a target carrying Objective-C++ cannot reuse the header
        # either. It is precompiled with Objective-C disabled, and clang rejects
        # a .mm compiled against it — "Objective-C was disabled in precompiled
        # file". Detected from the sources where they are resolved above rather
        # than listed, so a test that gains a .mm later opts out by having one.
        if(UBIQUITTY_TEST_PCH
                AND NOT _test_has_objcxx
                AND NOT _test_name IN_LIST _argument_BOUNDARY
                AND NOT _test_name IN_LIST _argument_NO_PCH
                AND NOT DEFINED ${_test_name}_EXTRA_DEFINES)
            target_precompile_headers(${_test_name} REUSE_FROM ubiquitty-test-pch)
        endif()

        # Required here on each test rather than inherited: a boundary test
        # links no Core target that could carry the language standard to it.
        target_compile_features(${_test_name} PRIVATE cxx_std_23)
        target_compile_options(${_test_name} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wno-deprecated-declarations;-Wno-multichar;-Wno-comment>"
            ${UBIQUITTY_OWNERSHIP_WARNINGS})
        # A sharded test registers N ctest entries over one binary, so ctest can
        # run the partitions concurrently. ctest-covered.txt still gets the BASE
        # name: that file is matched against the shell driver's stanza names,
        # which do not shard.
        set(_test_command ${_test_name})
        if(DEFINED ${_test_name}_LAUNCHER)
            set(_test_command ${${_test_name}_LAUNCHER} $<TARGET_FILE:${_test_name}>)
        endif()
        if(DEFINED ${_test_name}_SHARDS)
            math(EXPR _last_shard "${${_test_name}_SHARDS} - 1")
            foreach(_shard RANGE ${_last_shard})
                add_test(NAME ${_test_name}.${_shard}
                    COMMAND ${_test_command} --shard ${_shard}/${${_test_name}_SHARDS})
                ubiquitty_set_test_entry_properties(${_test_name}.${_shard})
            endforeach()
        else()
            add_test(NAME ${_test_name} COMMAND ${_test_command})
            ubiquitty_set_test_entry_properties(${_test_name})
        endif()
        set_property(DIRECTORY APPEND PROPERTY UBIQUITTY_HOST_TESTS ${_test_name})
    endforeach()
endfunction()

# The configured checkout owns its shell tests. A checkout without a driver
# uses Core's driver, which also accepts stanzas in the configured checkout.
function(ubiquitty_default_shell_driver _variable)
    set(_driver "${CMAKE_SOURCE_DIR}/Scripts/run-tests.sh")
    if(NOT EXISTS "${_driver}")
        set(_driver "${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/run-tests.sh")
    endif()
    set(${_variable} "${_driver}" PARENT_SCOPE)
endfunction()

# Discovery and execution must name the same checkouts even when CTest is run
# later, outside the shell that configured the tree, or with other root overrides.
function(_ubiquitty_shell_driver_environment _variable)
    set(_environment
        "UBQ_TESTS_ROOT=${CMAKE_SOURCE_DIR}"
        "UBQ_CORE_ROOT=${UBIQUITTY_CORE_SOURCE_DIR}")
    set(_repository "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/Scripts/repository.txt")
        file(STRINGS "${CMAKE_SOURCE_DIR}/Scripts/repository.txt" _repository REGEX "^(core|kit|app)$")
    endif()
    if(DEFINED UBIQUITTY_KIT_SOURCE_DIR)
        list(APPEND _environment "UBQ_KIT_ROOT=${UBIQUITTY_KIT_SOURCE_DIR}")
    elseif(_repository STREQUAL "core")
        # Core has no Kit dependency. Clear both inherited forms rather than
        # advertising this checkout as a Kit source root.
        list(APPEND _environment "UBQ_KIT_ROOT=" "UBIQUITTY_KIT_SOURCE_DIR=")
    else()
        list(APPEND _environment "UBQ_KIT_ROOT=${CMAKE_SOURCE_DIR}")
    endif()
    set(${_variable} "${_environment}" PARENT_SCOPE)
endfunction()

# Sets <variable> to the lines `<DRIVER> <LISTING>` prints, one list item each;
# LISTING is --list-stanzas when none is given. A listing that exits non-zero
# stops the configure: what it would have wired is unknowable, and an empty
# list in its place schedules entries that still run, only without the order
# or the locks the listing was there to give them.
function(ubiquitty_read_shell_stanzas _variable)
    cmake_parse_arguments(PARSE_ARGV 1 _argument "" "DRIVER;LISTING" "")
    if(NOT _argument_DRIVER)
        message(FATAL_ERROR "ubiquitty_read_shell_stanzas needs the DRIVER whose stanzas it lists")
    endif()
    if(NOT _argument_LISTING)
        set(_argument_LISTING --list-stanzas)
    endif()
    _ubiquitty_shell_driver_environment(_driver_environment)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env ${_driver_environment} ${_argument_DRIVER} ${_argument_LISTING}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _ubq_listing
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _ubq_listing_result)
    if(NOT _ubq_listing_result EQUAL 0)
        message(FATAL_ERROR
            "${_argument_DRIVER} ${_argument_LISTING} failed (${_ubq_listing_result}); "
            "the shell stanzas cannot be scheduled")
    endif()
    string(REPLACE "\n" ";" _ubq_listing "${_ubq_listing}")
    set(${_variable} "${_ubq_listing}" PARENT_SCOPE)
endfunction()

# Opt-in for a driver exposing Core's --list-serial-stanzas and
# --list-host-binary-stanzas inventories. Without a shared Host build fixture,
# each Host consumer can build the daemon on demand, so the lock covers its
# entire build and run. Other build trees keep their own locks. A generic driver
# that only lists stanzas does not need these inventories.
function(ubiquitty_lock_shell_build_trees)
    cmake_parse_arguments(PARSE_ARGV 0 _argument "" "DRIVER" "")
    if(NOT _argument_DRIVER)
        message(FATAL_ERROR "ubiquitty_lock_shell_build_trees needs the DRIVER whose build trees it locks")
    endif()
    ubiquitty_read_shell_stanzas(_pairs DRIVER "${_argument_DRIVER}" LISTING --list-serial-stanzas)
    ubiquitty_read_shell_stanzas(_host_stanzas DRIVER "${_argument_DRIVER}" LISTING --list-host-binary-stanzas)
    foreach(_stanza IN LISTS _host_stanzas)
        if(NOT _stanza STREQUAL "")
            list(APPEND _pairs "${_stanza} host-tests")
        endif()
    endforeach()
    set(_properties)
    foreach(_pair IN LISTS _pairs)
        if(_pair STREQUAL "")
            continue()
        endif()
        if(NOT _pair MATCHES "^([A-Za-z0-9_-]+) ([A-Za-z0-9_-]+)$")
            message(FATAL_ERROR "Invalid shared shell build tree from ${_argument_DRIVER}: ${_pair}")
        endif()
        set(_property "shell_${CMAKE_MATCH_1}_RESOURCE_LOCK")
        list(APPEND ${_property} "ubq_build_${CMAKE_MATCH_2}")
        list(APPEND _properties "${_property}")
    endforeach()
    list(REMOVE_DUPLICATES _properties)
    foreach(_property IN LISTS _properties)
        list(REMOVE_DUPLICATES ${_property})
        set(${_property} "${${_property}}" PARENT_SCOPE)
    endforeach()
endfunction()

# The shell-only remainder: one ctest entry per stanza of <DRIVER>, so `ctest`
# is the single entry point for the whole suite. The driver still compiles what
# its stanzas run, so this unifies REPORTING, not their build.
#
# A driver is a script that runs the one stanza it is given by name, answers
# `--list-stanzas` with the names it accepts, one per line, and ends a passing
# run with `<driver's file name>: PASSED — <n> ran, <m> skipped`. That closing
# line is what tells a stanza that declined as a whole from one that passed.
# STANZAS, when given, is scheduled in place of the whole listing, for a caller
# that has read the listing itself to wire it.
#
# The names come from the driver itself. Deriving them keeps this from becoming
# a second list to maintain: a stanza added to the driver is scheduled here
# without anyone remembering to say so.
#
# Each entry runs `<DRIVER> <stanza>` with
#
#   NO_CTEST=1             the stanza alone, without handing the suite back to
#                          ctest; it is what makes these entries terminate
#   SKIP_CTEST_COVERED=1   see below
#   UBQ_OUT                the entry's own scratch directory
#   UBQ_HOST_TEST_TREE     this build tree, whose object files and targets the
#                          stanza is about; a tree derived from UBQ_OUT would be
#                          the scratch directory, which holds none
#   UBIQUITTY_QUIC_PREFIX  the QUIC dependency prefix this tree configured
#   UBQ_BUILD_JOBS         the compilers the stanza may fork
#   UBQ_TEST_TARGETS_PREBUILT=1
#                          when the entry waits for ubq_build_fixture, which has
#                          built this tree's default targets; an entry that does
#                          not wait for it is promised nothing
#
# and then the entry's own shell_<stanza>_ENVIRONMENT, and under coverage the
# profile settings below.
#
# Settings for one stanza are variables in the caller's scope, named for its
# entry. They are how a driver's own fixtures and the stanzas ordered after
# them are wired, and a stanza with none is scheduled on the defaults:
#
#   shell_<stanza>_ENVIRONMENT        VAR=value pairs the driver also runs under
#   shell_<stanza>_JOBS               the compilers it may fork and the ctest
#                                     slots it holds, in place of
#                                     UBIQUITTY_SHELL_STANZA_JOBS
#   shell_<stanza>_FIXTURES_REQUIRED  the fixtures it waits for, in place of
#                                     ubq_build_fixture
#   shell_<stanza>_FIXTURES_SETUP     the fixture it is
#   shell_<stanza>_RESOURCE_LOCK      the locks it holds while it runs
#   shell_<stanza>_SKIP_REGULAR_EXPRESSION
#                                     further output that reports it Skipped,
#                                     beside the driver's whole-stanza decline
#   shell_<stanza>_SKIP_RETURN_CODE   an exit status reported as Skipped
#   shell_<stanza>_TIMEOUT            seconds, in place of 600
#
# SKIP_CTEST_COVERED=1 decides nothing here, and it is kept rather than dropped.
# It suppresses the stanzas ctest already ran, which matters when the driver is
# invoked with no filter — and each entry below names one stanza, so the filter
# answers first and the covered list is never opened. The driver says so on its
# own last line rather than leaving the reader to derive it. What keeps it here
# is the arrangement it protects against: a registration that goes back to one
# entry for the whole remainder re-runs the suite serially, recompiling the Core
# sources per test, and that entry would need the variable already set.
#
# One ctest entry per stanza rather than one for all of them. As a single
# RUN_SERIAL entry the remainder was 340 of the suite's 369 seconds — 92% of the
# wall clock in one test, with every other core idle beside it. The stanzas are
# independent (each compiles its own link set and runs its own binary), so ctest
# can schedule them like any other test.
function(ubiquitty_schedule_shell_stanzas)
    cmake_parse_arguments(PARSE_ARGV 0 _argument "" "DRIVER" "STANZAS")
    if(NOT _argument_DRIVER)
        message(FATAL_ERROR "ubiquitty_schedule_shell_stanzas needs the DRIVER whose stanzas it schedules")
    endif()
    # The driver runs this tree's deferred tests whether or not ctest runs its
    # stanzas, and ubiquitty_finish_host_tests names it as the one that does.
    set_property(DIRECTORY PROPERTY UBIQUITTY_SHELL_DRIVER "${_argument_DRIVER}")
    if(NOT UBIQUITTY_CTEST_SHELL_REMAINDER)
        return()
    endif()
    _ubiquitty_add_build_fixture()

    if(DEFINED _argument_STANZAS OR "STANZAS" IN_LIST _argument_KEYWORDS_MISSING_VALUES)
        set(_ubq_stanzas "${_argument_STANZAS}")
    else()
        ubiquitty_read_shell_stanzas(_ubq_stanzas DRIVER ${_argument_DRIVER})
    endif()

    # How many compilers one shell stanza may fork, and simultaneously what
    # ctest charges it against `-j`. One variable because the two have to be the
    # same number: the fan-out the stanza performs is the width ctest must
    # reserve, and a stanza that under-declares is scheduled as though it were
    # idle.
    set(UBIQUITTY_SHELL_STANZA_JOBS 2 CACHE STRING
        "Compilers a single shell stanza may fork, and the ctest slots it holds")

    # The driver's closing line, which opens with the driver's file name and
    # says what the run did. Nothing ran and something declined is a stanza
    # that asserted nothing. The name is escaped so that its dot matches only a
    # dot. The dash after PASSED is three bytes, so one `.+` spans it.
    get_filename_component(_ubq_driver_name "${_argument_DRIVER}" NAME)
    string(REGEX REPLACE "([][.+*?^$|(){}\\\\])" "\\\\\\1" _ubq_driver_pattern "${_ubq_driver_name}")
    set(_ubq_stanza_decline "${_ubq_driver_pattern}: PASSED .+ 0 ran, [1-9]")
    _ubiquitty_shell_driver_environment(_driver_environment)
    foreach(_ubq_stanza IN LISTS _ubq_stanzas)
        if(_ubq_stanza STREQUAL "")
            continue()
        endif()
        set(_ubq_entry shell_${_ubq_stanza})
        set(_ubq_stanza_jobs ${UBIQUITTY_SHELL_STANZA_JOBS})
        if(DEFINED ${_ubq_entry}_JOBS)
            set(_ubq_stanza_jobs ${${_ubq_entry}_JOBS})
        endif()
        set(_ubq_stanza_fixtures ubq_build_fixture)
        if(DEFINED ${_ubq_entry}_FIXTURES_REQUIRED)
            set(_ubq_stanza_fixtures "${${_ubq_entry}_FIXTURES_REQUIRED}")
        endif()
        set(_ubq_stanza_env "")
        if("ubq_build_fixture" IN_LIST _ubq_stanza_fixtures)
            list(APPEND _ubq_stanza_env UBQ_TEST_TARGETS_PREBUILT=1)
        endif()
        list(APPEND _ubq_stanza_env ${${_ubq_entry}_ENVIRONMENT})
        # Under coverage the stanza's own compiles instrument (UBQ_COVERAGE
        # reaches the driver's compiler rider and cmake wrapper), and the
        # profile path names the entry so the covering-test index attributes
        # the stanza's lines to it — %p still separates the processes a stanza
        # forks.
        set(_ubq_stanza_coverage_env "")
        if(UBIQUITTY_COVERAGE)
            # %c: continuous mode. The stanzas' daemons die by SIGTERM/SIGKILL
            # in their drivers' teardown, and an instrumented process killed by
            # a signal writes nothing at exit — a daemon's whole run then reads
            # as unreached. Continuous mode mmaps the counters into the profile
            # as they increment, so the file is complete however the process
            # ends. The %c is a mode flag, not a substitution: the filename
            # keeps the entry--pid shape the index parses.
            set(_ubq_stanza_coverage_env
                UBQ_COVERAGE=1
                "LLVM_PROFILE_FILE=${UBIQUITTY_COVERAGE_RAW_DIR}/${_ubq_entry}--%c%p.profraw")
        endif()
        # Covered from here on: the driver's full run skips a stanza
        # ctest-covered.txt names, because ctest has run it as shell_<stanza>.
        # A stanza missing from the list runs twice, in ctest and again in the
        # loop after it.
        set_property(DIRECTORY APPEND PROPERTY UBIQUITTY_SHELL_STANZAS ${_ubq_stanza})
        add_test(NAME ${_ubq_entry}
            COMMAND ${CMAKE_COMMAND} -E env NO_CTEST=1 SKIP_CTEST_COVERED=1
                UBQ_OUT=${CMAKE_BINARY_DIR}/shell/${_ubq_stanza}
                UBQ_HOST_TEST_TREE=${CMAKE_BINARY_DIR}
                UBIQUITTY_QUIC_PREFIX=${UBIQUITTY_QUIC_PREFIX}
                UBQ_BUILD_JOBS=${_ubq_stanza_jobs}
                ${_ubq_stanza_env}
                ${_ubq_stanza_coverage_env}
                ${_driver_environment}
                ${_argument_DRIVER} ${_ubq_stanza})
        # A stanza that asserted nothing exits 0, which ctest reports as a pass
        # and a reader counts as coverage. The driver ends every run with what
        # it did, and "0 ran" is the whole-stanza decline: the tool was absent,
        # the platform was wrong, the daemon would not start. Matching that line
        # turns those into ctest's own Skipped, which the summary counts out
        # loud. A stanza that ran anything at all does not match it.
        set(_ubq_stanza_skip "${_ubq_stanza_decline}")
        if(DEFINED ${_ubq_entry}_SKIP_REGULAR_EXPRESSION)
            set(_ubq_stanza_skip "${${_ubq_entry}_SKIP_REGULAR_EXPRESSION};${_ubq_stanza_decline}")
        endif()
        # A stanza compiles its own link set before it runs, so its clock
        # includes a build the CMake-covered tests already have. Three of them
        # pass in 45-70s alone, which the ordinary per-test timeout would call a
        # hang the moment the machine is busy.
        set(_ubq_stanza_timeout 600)
        if(DEFINED ${_ubq_entry}_TIMEOUT)
            set(_ubq_stanza_timeout ${${_ubq_entry}_TIMEOUT})
        endif()
        # PROCESSORS is what tells ctest how wide the stanza is. Without it a
        # stanza counts as one process while it forks that many compilers, so
        # `-j N` schedules N stanzas and the machine gets N times this number —
        # 49 concurrent compilers measured against 15 cores at `-j12`. The same
        # variable feeds the stanza's own fan-out above, because a declared
        # width and an actual one that are written separately are two numbers
        # that can disagree, and only the smaller one is ever believed.
        set_tests_properties(${_ubq_entry} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            FIXTURES_REQUIRED "${_ubq_stanza_fixtures}"
            PROCESSORS ${_ubq_stanza_jobs}
            SKIP_REGULAR_EXPRESSION "${_ubq_stanza_skip}"
            TIMEOUT ${_ubq_stanza_timeout})
        foreach(_ubq_property IN ITEMS FIXTURES_SETUP RESOURCE_LOCK SKIP_RETURN_CODE)
            if(DEFINED ${_ubq_entry}_${_ubq_property})
                set_tests_properties(${_ubq_entry} PROPERTIES
                    ${_ubq_property} "${${_ubq_entry}_${_ubq_property}}")
            endif()
        endforeach()
    endforeach()

    list(LENGTH _ubq_stanzas _ubq_stanza_count)
    message(STATUS "Ubiquitty tests: ${_ubq_stanza_count} shell stanzas scheduled individually")

    # What the stanza lane holds, for the census that runs after every ctest
    # invocation. `ctest -E '^shell_'` is the command this repository reaches
    # for, and it runs none of these, so its summary is a true sentence about a
    # fraction of the suite that reads as one about all of it.
    #
    # CTEST_CUSTOM_POST_TEST takes a list of commands, each run with no
    # arguments, so the count travels in a file rather than beside the path.
    string(REPLACE ";" "\n" _ubq_stanza_lines "${_ubq_stanzas}")
    file(WRITE "${CMAKE_BINARY_DIR}/shell-lane-stanzas.txt" "${_ubq_stanza_lines}\n")
    file(WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
        "set(CTEST_CUSTOM_POST_TEST \"${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/ctest-shell-lane-census.sh\")\n")
endfunction()

# ctest-covered.txt tells the tree's shell driver which of its stanzas ctest
# has already run: every C++ test ubiquitty_add_host_tests registered and every
# shell stanza ubiquitty_schedule_shell_stanzas scheduled. The driver skips
# those and runs the rest, the deferred tests among them. Nobody edits the
# file, so no name can be run by both or dropped by both.
#
# DRIVER is that driver, the one ubiquitty_schedule_shell_stanzas was given
# when none is named here; a tree with neither has nothing to run what CTest
# leaves out, and stops the configure.
function(ubiquitty_finish_host_tests)
    cmake_parse_arguments(PARSE_ARGV 0 _argument "" "DRIVER" "")
    if(NOT _argument_DRIVER)
        get_property(_argument_DRIVER DIRECTORY PROPERTY UBIQUITTY_SHELL_DRIVER)
    endif()
    if(NOT _argument_DRIVER)
        message(FATAL_ERROR
            "ubiquitty_finish_host_tests needs the DRIVER that runs what CTest leaves out; "
            "name it here or schedule its stanzas with ubiquitty_schedule_shell_stanzas")
    endif()
    get_property(_tests DIRECTORY PROPERTY UBIQUITTY_HOST_TESTS)
    get_property(_stanzas DIRECTORY PROPERTY UBIQUITTY_SHELL_STANZAS)
    get_property(_left_out DIRECTORY PROPERTY UBIQUITTY_HOST_TESTS_LEFT_OUT)
    list(LENGTH _tests _registered)
    list(LENGTH _left_out _deferred)
    set(_covered ${_tests} ${_stanzas})
    list(SORT _covered)
    list(JOIN _covered "\n" _covered_text)
    file(WRITE ${CMAKE_BINARY_DIR}/ctest-covered.txt "${_covered_text}\n")
    # Named from the top of the source tree, as it is run, when it lies there.
    file(RELATIVE_PATH _driver_shown "${CMAKE_SOURCE_DIR}" "${_argument_DRIVER}")
    if(_driver_shown MATCHES "^\\.\\./")
        set(_driver_shown "${_argument_DRIVER}")
    endif()
    message(STATUS
        "Ubiquitty tests: CTest runs ${_registered} (one at a time with ctest -R <name>); "
        "${_deferred} more are left to ${_driver_shown}")
endfunction()
