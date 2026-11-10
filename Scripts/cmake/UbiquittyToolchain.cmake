# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# What every target of a build that compiles ubiquitty-core is compiled under:
# the compiler floor, the archive recipe, the build type, the version header,
# the warning and instrumentation switches, the build root and the QUIC
# dependencies. A consumer includes this file at its top directory, right after
# project(): the settings below are directory-scoped, and a directory below the
# consumer's top would keep them from the consumer's own targets.
#
#   UBIQUITTY_WERROR, UBIQUITTY_QUIC, UBIQUITTY_BUILD_TESTS,
#   UBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE, UBIQUITTY_TEST_BOUNDS_CHECKS,
#   UBIQUITTY_COVERAGE, UBIQUITTY_SANITIZERS, UBIQUITTY_OWNERSHIP_ANALYSIS
#   UBIQUITTY_NARROWING_WARNING, UBIQUITTY_OWNERSHIP_WARNINGS, UBIQUITTY_BUILD_VERSION_HEADER,
#   UBIQUITTY_BUILD_ROOT, UBIQUITTY_QUIC_PREFIX
#   ubiquitty-build-version, the step that refreshes the version header
#   ubiquitty_ensure_quic_deps([<sdk>])
#   ubiquitty_order_after_build_version(<directory>)
include_guard(GLOBAL)

# The version, the floors and the launcher this module builds on are decided
# before project(), which this module comes after.
get_property(_ubq_pre_project_read GLOBAL PROPERTY UBIQUITTY_PRE_PROJECT_READ)
if(NOT _ubq_pre_project_read)
    message(FATAL_ERROR
        "UbiquittyToolchain.cmake is read without UbiquittyPreProject.cmake. Include that "
        "before project(), in the order Scripts/cmake/UbiquittyCoreRoot.cmake lays out.")
endif()
ubiquitty_require_top_directory(UbiquittyToolchain.cmake)

# The compiler is known from here on, and nothing has been compiled yet: a
# compiler below the floor is refused now, with the remedy, rather than once per
# translation unit by Source/Base/ToolchainFloor.h.
include("${CMAKE_CURRENT_LIST_DIR}/ToolchainFloor.cmake")
ubiquitty_check_toolchain_floor("${CMAKE_CXX_COMPILER_ID}" "${CMAKE_CXX_COMPILER_VERSION}")

# A translation unit whose whole body sits behind a build flag — PerfCounters.cpp
# under UBIQUITTY_PERF_DIAG=OFF — compiles to an object with no symbols, and
# Apple's ranlib says so for every archive that holds one: "has no symbols",
# once per library per configure. The object is right; the symbol table is what
# ar builds and ranlib rebuilds, so ar is told to skip it (S) and ranlib to keep
# quiet about an empty member. GNU tools have no such warning and keep CMake's
# default recipe.
if(APPLE)
    foreach(_ubq_lang C CXX)
        set(CMAKE_${_ubq_lang}_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_${_ubq_lang}_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    endforeach()
endif()

# An empty CMAKE_BUILD_TYPE is not a neutral default: CMake passes no
# optimisation flag at all, so a hand-configured build directory produces a -O0
# binary that looks and runs like any other. In a terminal whose whole claim is
# throughput that is a trap -- an unoptimised host serialises its scrollback
# byte at a time under libc++ container annotations, burns a core, and every
# number measured against it is wrong in a way nothing reports.
#
# The build scripts always pass one. This is for `cmake -B <dir> -D...` by hand,
# which is how benchmarks and bisects get configured, and where the mistake
# costs the most.
#
# A test tree defaults to Debug instead: Release carries NDEBUG, and the gate
# below rejects that combination outright. NDEBUG compiles out the assertions the
# suite exists to trip -- a Release test tree reports every case passing while a
# checked-pointer violation goes unnoticed.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    if(UBIQUITTY_BUILD_TESTS)
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    else()
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
    message(WARNING
        "CMAKE_BUILD_TYPE was empty, which means NO optimisation, not a default.\n"
        "  Defaulting to Release. Pass -DCMAKE_BUILD_TYPE=Debug if that is what you meant.")
endif()
message(STATUS "Ubiquitty build type: ${CMAKE_BUILD_TYPE}")

# The version reaches a binary through one generated header, which one
# translation unit includes (Source/Core/BuildVersion.cpp). It is
# never a compile definition: a definition rides every translation unit's
# command line, and a version that moves with each commit would then recompile
# the tree on every pull. The header lives in the build directory, so nothing
# that hashes Source/ sees it change.
#
# The configure writes it, and a step that runs in every build rewrites it when
# its text would change: a commit, or a first edit to a tracked file, which the
# build identity names. The include directory is the same on every command line
# in every build, so it invalidates nothing.
set(UBIQUITTY_BUILD_VERSION_HEADER "${UBIQUITTY_BUILD_VERSION_DIR}/UbiquittyBuildVersion.h")
ubiquitty_write_build_version_header(
    "${UBIQUITTY_BUILD_VERSION_HEADER}" "${UBIQUITTY_VERSION}" "${UBIQUITTY_BUILD_IDENTITY}")
include_directories("${UBIQUITTY_BUILD_VERSION_DIR}")
# What this configure derived, for the build step to hold the tree against. A
# file rather than an argument below, so the build graph is the same text at
# every commit.
file(WRITE "${UBIQUITTY_BUILD_VERSION_DIR}/configured-version.txt" "${UBIQUITTY_VERSION}\n")
add_custom_target(ubiquitty-build-version
    COMMAND "${CMAKE_COMMAND}"
        "-DUBIQUITTY_VERSION_SOURCE_DIR=${UBIQUITTY_VERSION_SOURCE_DIR}"
        "-DUBIQUITTY_VERSION_HEADER=${UBIQUITTY_BUILD_VERSION_HEADER}"
        "-DUBIQUITTY_VERSION_CONFIGURED_FILE=${UBIQUITTY_BUILD_VERSION_DIR}/configured-version.txt"
        "-DUBIQUITTY_VERSION_RECONFIGURE_STAMP=${UBIQUITTY_BUILD_VERSION_RECONFIGURE_STAMP}"
        -P "${CMAKE_CURRENT_LIST_DIR}/BuildVersion.cmake"
    BYPRODUCTS "${UBIQUITTY_BUILD_VERSION_HEADER}"
    COMMENT "Checking the build version"
    VERBATIM)

# Treat compiler warnings as errors in every first-party compile: -Werror for
# C, C++ and Objective-C (set per target, beside each target's warning list),
# -warnings-as-errors for Swift (set once here, so every Swift target in this
# directory and below inherits it, and the Xcode generator carries it into
# OTHER_SWIFT_FLAGS). ON by default to keep the tree warning-clean; flip OFF
# (-DUBIQUITTY_WERROR=OFF) if a new SDK/compiler version introduces warnings in
# code you can't touch yet, so a toolchain bump never hard-blocks a build.
option(UBIQUITTY_WERROR "Treat compiler warnings as errors" ON)
add_compile_options(
    "$<$<AND:$<COMPILE_LANGUAGE:Swift>,$<BOOL:${UBIQUITTY_WERROR}>>:-warnings-as-errors>")

# Xcode enables -Wshorten-64-to-32 by default and Ninja builds do not, so an
# implicit 64-to-32-bit narrowing used to surface only in an Xcode build. Every
# clang compile of first-party C and C++ code asks for it. GCC has
# no such flag, so the expression is empty there. Vendored sources carry -w.
set(UBIQUITTY_NARROWING_WARNING
    "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wshorten-64-to-32>"
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wshorten-64-to-32>")

# Clang's thread-safety analysis proves that model state is touched only under
# the model lock (Source/Base/ThreadSafety.h, Source/Core/ModelLock.h). It is
# an error whatever UBIQUITTY_WERROR says: a finding is a data race or a lock
# the analysis cannot credit, and a build that demotes it to a warning has
# stopped checking. GCC has no such analysis, so the expression is empty there.
# Every first-party C++ target takes UBIQUITTY_OWNERSHIP_WARNINGS beside
# UBIQUITTY_NARROWING_WARNING, and the host tests take it too, because they are
# the only Core targets that compile the header-only TerminalSession.h.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_ubq_ownership_analysis_default ON)
else()
    set(_ubq_ownership_analysis_default OFF)
endif()
option(UBIQUITTY_OWNERSHIP_ANALYSIS
    "Check model-lock ownership with Clang's thread-safety analysis, as errors"
    ${_ubq_ownership_analysis_default})
set(UBIQUITTY_OWNERSHIP_WARNINGS
    "$<$<AND:$<BOOL:${UBIQUITTY_OWNERSHIP_ANALYSIS}>,$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>>:-Wthread-safety;-Werror=thread-safety>")

# ----------------------------------------------------------------------
# QUIC transport (ngtcp2 + AWS-LC) — the resilient always-on datagram transport
# (roaming / NAT / loss survival). ON for every shipping frontend (the macOS app,
# the native host, and the static-musl Linux cross-build); the pinned stack is
# auto-fetched below if missing. OFF applies only to libUbiquittyCore, the
# dependency-free standalone Core. The link step finds the static libs + headers
# under UBIQUITTY_QUIC_PREFIX.
# ----------------------------------------------------------------------
option(UBIQUITTY_QUIC "Build the QUIC transport (ngtcp2 + AWS-LC)" ON)
# The host tests link against the portable Core layer chain, which only the
# Core frontend defines; Tests/CMakeLists.txt finds and registers them.
option(UBIQUITTY_BUILD_TESTS "Configure the host tests and register each one with CTest" OFF)
option(UBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE
    "Build the isolated real SSH/QUIC acceptance executable (Host, QUIC, Debug only)" OFF)
if(UBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE AND
        (NOT UBIQUITTY_FRONTEND STREQUAL "Host" OR NOT UBIQUITTY_QUIC))
    message(FATAL_ERROR
        "Sync transport acceptance requires UBIQUITTY_FRONTEND=Host and UBIQUITTY_QUIC=ON")
endif()

# NDEBUG deletes the assertions the suite exists to trip, and with them the paint
# bookkeeping CellGrid and PaintSession keep for the tests (the paint-leak flag,
# the walk counters). A tree where some translation units have it is not a weaker
# test run either: an inline function with an assert() is a different definition
# on each side, an ODR violation that links, runs, and prints its pass line.
#
# Fatal rather than a warning for that reason: there is no runtime signal to notice
# later. The FLAGS are read rather than the build type, so NDEBUG arriving through
# CXXFLAGS or a toolchain file is caught too, and the config is uppercased because
# that is how CMake looks the per-config flags up — `-DCMAKE_BUILD_TYPE=debug` is
# legal and must not trip this.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _ubq_config_upper)
if((UBIQUITTY_BUILD_TESTS OR UBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE) AND
        "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_ubq_config_upper}}"
        MATCHES "NDEBUG")
    message(FATAL_ERROR
        "UBIQUITTY_BUILD_TESTS=ON with NDEBUG in the compile flags "
        "(CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}).\n"
        "  NDEBUG deletes the assertions and the paint bookkeeping the tests check, "
        "so every case would pass without checking them.\n"
        "  Configure the test tree with -DCMAKE_BUILD_TYPE=Debug.")
endif()

# A standard library that checks its subscripts, for the test tree only.
#
# Both spellings, because the two libraries this tree is compiled against
# disagree about the name and each ignores the other's: libc++ reads the
# hardening mode, libstdc++ reads the assertions switch. Fast mode is the tier
# that checks element access and iterator validity without the allocator and
# complexity checks the debug tier adds, so the cost is a compare and a branch
# at each subscript rather than a different data structure.
#
# The reason it is set here rather than left to whoever configures a build: an
# unchecked `operator[]` past the end of a container cannot report a defect, so
# an arm compiled without this is not weak evidence about out-of-bounds reads —
# it is no evidence. Agreement is evidence only among arms that could have
# disagreed.
#
# The whole tree compiles clean under it, and its cost is small beside what a
# sweep of cases spends.
#
# Scoped to the test configuration, so the shipping frontends keep the unchecked
# subscript: this is an instrument for the tier that runs cases rather than a
# guard for the tier that draws frames, and a bounds check in buildCellInstances
# would sit in the hottest loop in the renderer.
#
# The switch exists so the question "was this case failing before the checks, or
# because of them" can be answered by building both ways. Answering it any other
# way means reasoning about a trap from its absence, which is the reasoning this
# whole block exists to remove.
option(UBIQUITTY_TEST_BOUNDS_CHECKS "Compile the test tree against a checking standard library" ON)
if(UBIQUITTY_BUILD_TESTS AND UBIQUITTY_TEST_BOUNDS_CHECKS)
    # C and C++ only: a Swift compile takes -D as a bare condition and warns
    # on the value, and the test tree's Swift targets are warning-clean.
    add_compile_definitions(
        "$<$<COMPILE_LANGUAGE:C,CXX>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:_GLIBCXX_ASSERTIONS=1>")
endif()

# Compressed debug information in the Linux test tree's Debug builds. ELF
# carries DWARF in the binary itself, about four fifths of a test binary's 100
# MB, and a daemon a test drives reads its whole executable when it pins the
# files a local adopt may come from: on a loaded build machine the hashing
# alone took a handoff case past ctest's limit. Compressed, the binaries and
# the build trees are about half the size. zlib rather than zstd: Fedora's gdb
# reads zlib-compressed sections and refuses zstd ones ("BFD is not built with
# zstd support"), while the sanitizers' symbolizer reads both. None where the
# compiler, assembler and linker do not take it. Mach-O keeps its DWARF beside
# the objects, so macOS has nothing here to compress.
if(UBIQUITTY_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckCXXSourceCompiles)
    set(_ubq_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "-g -gz=zlib")
    check_cxx_source_compiles("int main() { return 0; }" UBIQUITTY_LINKS_ZLIB_DEBUG_SECTIONS)
    set(CMAKE_REQUIRED_FLAGS "${_ubq_saved_required_flags}")
    if(UBIQUITTY_LINKS_ZLIB_DEBUG_SECTIONS)
        add_compile_options("$<$<AND:$<CONFIG:Debug>,$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>>:-gz=zlib>")
        add_link_options("$<$<CONFIG:Debug>:-gz=zlib>")
    endif()
endif()

# Source-based coverage. Set at the top level rather than per target so the Core
# libraries and the tests that exercise them are instrumented by one switch —
# llvm-cov attributes a Core line to the test binary that ran it, and a
# half-instrumented tree reports that line as unexecuted.
option(UBIQUITTY_COVERAGE "Instrument for llvm-cov source-based coverage" OFF)
if(UBIQUITTY_COVERAGE)
    # -runtime-counter-relocation is what makes CONTINUOUS mode work, and the
    # shell tier asks for continuous mode by putting %c in LLVM_PROFILE_FILE
    # (Scripts/cmake/UbiquittyTests.cmake) so a daemon killed by a signal still
    # leaves a complete profile. On ELF the counters are not writable in place,
    # so the runtime needs the bias symbol this flag emits; without it every
    # instrumented process prints
    #
    #     LLVM Profile Error: __llvm_profile_counter_bias is undefined
    #
    # on stderr, continuous mode does not happen, and the profile is whatever
    # the exit hook managed to write — which for a signalled daemon is nothing.
    # The message is not fatal, which is why this survived: the process exits 0
    # and the run looks well. It is the message itself that fails a test, by
    # landing in a stream the test reads. Mach-O relocates counters without the
    # flag and accepts it as a no-op, so it is unconditional rather than
    # per-platform — one spelling cannot drift from the other.
    #
    # Clang's languages only. These are clang's spellings, and a Swift target
    # in the tree (the pasteboard library a test links) hands them to swiftc,
    # which stops at `unknown argument: '-fprofile-instr-generate'` and takes
    # the whole instrumented build with it. The index is of C++ sources, so a
    # Swift target is built plain.
    foreach(_ubq_coverage_option -fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic "SHELL:-mllvm -runtime-counter-relocation")
        add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:${_ubq_coverage_option}>")
    endforeach()
    # A test whose link swiftc drives still carries instrumented C++ objects,
    # which need the profile runtime; `-profile-generate` is swiftc's spelling
    # of the link half.
    add_link_options("$<IF:$<LINK_LANGUAGE:Swift>,-profile-generate,-fprofile-instr-generate>")
    # The profile runtime's writer is linked only here, and a declaration of it
    # in any other build leaves an undefined symbol. A test that forks and lets
    # the child die needs it: the child never reaches the exit hook that writes
    # its counters, so the branch it went there to exercise reports unexecuted.
    add_compile_definitions("$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:UBIQUITTY_COVERAGE_INSTRUMENTED=1>")
endif()

# Runtime sanitizers. Set at the top level for the same reason coverage is: the
# Core and the tests that exercise it must carry one instrumentation, since an
# allocation made by an uninstrumented Core is invisible to the checker in the
# test that frees it. The value is the -fsanitize= list verbatim
# ("address", "address,undefined"); empty builds nothing sanitized.
set(UBIQUITTY_SANITIZERS "" CACHE STRING
    "Comma-separated -fsanitize= list applied to every target (empty: none)")
if(UBIQUITTY_SANITIZERS)
    # The vptr check reads type information, which a tree built without RTTI
    # has none of: Clang leaves the check out there by itself, and GCC keeps it
    # and stops at the link for want of every typeinfo. Named off for both.
    add_compile_options(-fsanitize=${UBIQUITTY_SANITIZERS} -fno-sanitize=vptr -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${UBIQUITTY_SANITIZERS} -fno-sanitize=vptr)
    # wuffs marks its generated code no_sanitize("unsigned-integer-overflow"),
    # a check Clang alone has. GCC answers the feature test the mark sits
    # behind, ignores the name and warns that it did, which under -Werror ends
    # a sanitized build at a third-party file. The mark is right where the
    # check exists, so the warning stays a warning here.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-error=attributes)
    endif()
endif()
# The build root of the project being configured (Scripts/build_root.py), outside
# its checkout: UBIQUITTY_BUILD_ROOT. The QUIC dependencies live there beside
# every other build product, so each checkout builds AWS-LC and ngtcp2 once; an
# explicit -DUBIQUITTY_QUIC_PREFIX is left alone.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/build_root.py" "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE UBIQUITTY_BUILD_ROOT
    RESULT_VARIABLE _ubiquitty_build_root_status
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _ubiquitty_build_root_status EQUAL 0 OR UBIQUITTY_BUILD_ROOT STREQUAL "")
    message(FATAL_ERROR "Scripts/build_root.py could not name this checkout's build root "
        "(status ${_ubiquitty_build_root_status}); UBQ_BUILD_ROOT in the environment names one outright")
endif()
set(UBIQUITTY_QUIC_PREFIX "${UBIQUITTY_BUILD_ROOT}/quic-deps" CACHE PATH
    "Prefix holding the built ngtcp2 + AWS-LC (from Scripts/fetch-quic-deps.sh)")

# Ensure the pinned QUIC TLS deps (ngtcp2 + its crypto backend) in
# UBIQUITTY_QUIC_PREFIX are complete and native. Only frontends that actually link the
# QUIC transport (Host, macOS) call this, so a QUIC-free consumer (Core, Agent,
# Linux) never triggers a TLS-stack download or a network fetch at configure time.
# ensure-quic-deps.sh returns immediately for a warm cache; a cold or incompatible
# prefix delegates once to the pinned builder. The gate covers every static archive
# the QUIC targets link.
#
# - Parameter ARGV0: On iOS, the SDK to ensure (`iphoneos` or `iphonesimulator`).
#   Omitted, it is read from CMAKE_OSX_SYSROOT. A tree that serves both SDKs —
#   the Xcode project, where the destination picks one at build time — calls
#   once per SDK, since the sysroot it was configured with names only one.
set_property(GLOBAL PROPERTY UBIQUITTY_ENSURE_QUIC_DEPS_SCRIPT
    "${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/ensure-quic-deps.sh")
function(ubiquitty_ensure_quic_deps)
    # Which platform the archives are for, and where they live.
    #
    # The host keeps the bare prefix rather than gaining a subdirectory of its own, so
    # every already-built tree stays built and no existing consumer re-downloads a TLS
    # stack it has.
    set(_ubiquitty_quic_sdk "")
    set(_ubiquitty_quic_deployment_target "")
    set(_ubiquitty_quic_prefix "${UBIQUITTY_QUIC_PREFIX}")
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # The simulator and the device are DIFFERENT builds — arm64-apple-ios-simulator
        # against arm64-apple-ios — so they are different prefixes, and a device slice
        # is not evidence for a simulator one or the other way round.
        if(ARGC GREATER 0)
            set(_ubiquitty_quic_sdk "${ARGV0}")
        elseif(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            set(_ubiquitty_quic_sdk "iphonesimulator")
        else()
            set(_ubiquitty_quic_sdk "iphoneos")
        endif()
        # Passed rather than read by the script: UBIQUITTY_IOS_MIN lives once, in
        # UbiquittyPreProject.cmake.
        set(_ubiquitty_quic_deployment_target "${UBIQUITTY_IOS_MIN}")
        set(_ubiquitty_quic_prefix "${UBIQUITTY_QUIC_PREFIX}/${_ubiquitty_quic_sdk}")
    endif()
    # What the caller links against. Reads the same as UBIQUITTY_QUIC_PREFIX on the host,
    # so the targets that already name that keep working unchanged.
    set(UBIQUITTY_QUIC_PLATFORM_PREFIX "${_ubiquitty_quic_prefix}" PARENT_SCOPE)

    set(_ubiquitty_quic_required_libraries
        "${_ubiquitty_quic_prefix}/lib/libngtcp2_crypto_boringssl.a"
        "${_ubiquitty_quic_prefix}/lib/libngtcp2.a"
        "${_ubiquitty_quic_prefix}/lib/libssl.a"
        "${_ubiquitty_quic_prefix}/lib/libcrypto.a")

    # UBQ_CMAKE rather than letting the script find one: an Xcode run-script phase runs
    # with a PATH that excludes /opt/homebrew/bin, so a bare `cmake` in the script is not
    # found even though cmake is what is running this line. Handing over CMAKE_COMMAND also
    # guarantees the dependencies are built by the same cmake as the rest of the tree.
    get_property(_ubiquitty_quic_script GLOBAL PROPERTY UBIQUITTY_ENSURE_QUIC_DEPS_SCRIPT)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "UBQ_CMAKE=${CMAKE_COMMAND}"
                "${_ubiquitty_quic_script}"
                "${_ubiquitty_quic_prefix}"
                "${_ubiquitty_quic_sdk}"
                "${_ubiquitty_quic_deployment_target}"
        RESULT_VARIABLE _ubiquitty_quic_fetch_rc)

    set(_ubiquitty_quic_missing_libraries)
    foreach(_ubiquitty_quic_library IN LISTS _ubiquitty_quic_required_libraries)
        if(NOT EXISTS "${_ubiquitty_quic_library}")
            list(APPEND _ubiquitty_quic_missing_libraries "${_ubiquitty_quic_library}")
        endif()
    endforeach()

    if(_ubiquitty_quic_missing_libraries)
        string(REPLACE ";" ", " _ubiquitty_quic_missing_libraries_text
            "${_ubiquitty_quic_missing_libraries}")
    endif()

    # RESULT_VARIABLE carries an exit status or a string saying why the process
    # could not be run at all, and the two want different advice. A missing Go or
    # Perl makes the script exit non-zero after running; a non-numeric result
    # means it never started, and naming a dependency then sends the reader to
    # install something they already have.
    if(NOT _ubiquitty_quic_fetch_rc MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "QUIC deps: could not run Scripts/ensure-quic-deps.sh — ${_ubiquitty_quic_fetch_rc}. "
            "The script did not start, so nothing about the QUIC stack has been established. "
            "Check that it is executable and that its shebang is on line 1: a `#!` below the "
            "first line is a comment, and execve then fails with ENOEXEC where no shell retries it.")
    endif()

    # 127 is specifically "command not found", and the script has already said which on its
    # stderr. Advising Go and Perl there sends a reader to install two things they have, and
    # describes the most likely PAST cause rather than the one that just happened.
    if(_ubiquitty_quic_fetch_rc EQUAL 127)
        message(FATAL_ERROR
            "QUIC deps: Scripts/ensure-quic-deps.sh could not find a tool it needs (exit 127) — "
            "its output above names which. An Xcode run-script phase has a restricted PATH, "
            "which is the usual cause. "
            "Configure with -DUBIQUITTY_QUIC=OFF to build without the QUIC transport.")
    endif()

    if(NOT _ubiquitty_quic_fetch_rc EQUAL 0)
        message(FATAL_ERROR
            "QUIC deps: Scripts/ensure-quic-deps.sh exited ${_ubiquitty_quic_fetch_rc}. "
            "AWS-LC needs Go and Perl on PATH. "
            "Configure with -DUBIQUITTY_QUIC=OFF to build without the QUIC transport.")
    endif()

    if(_ubiquitty_quic_missing_libraries)
        message(FATAL_ERROR
            "QUIC deps: Scripts/ensure-quic-deps.sh reported success and these archives are "
            "still absent: ${_ubiquitty_quic_missing_libraries_text}. "
            "The prefix (${_ubiquitty_quic_prefix}) and what the script installs disagree.")
    endif()
endfunction()

# A compiler that misfolds memcmp at a runtime offset says so here rather than
# through one silently failing test. See the module for why the cure is not a
# global flag.
include("${CMAKE_CURRENT_LIST_DIR}/CheckMemcmpFold.cmake")
ubiquitty_check_memcmp_fold()

# Every compiled target is built after the step that refreshes the generated
# version header, so the one translation unit that includes it never compiles
# against the previous commit's text and then again against this one's. The
# consumer calls it last, on its top directory, once every target exists.
#
# - Parameter directory: The directory whose targets, and whose subdirectories'
#   targets, are ordered after the step.
function(ubiquitty_order_after_build_version directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            add_dependencies(${_target} ubiquitty-build-version)
        endif()
    endforeach()
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        ubiquitty_order_after_build_version("${_subdirectory}")
    endforeach()
endfunction()
