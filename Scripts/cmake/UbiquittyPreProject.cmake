# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# What a build of ubiquitty-core decides before project(): the compiler
# launcher, the deployment floors, the macOS SDK and the version. Each is read
# when a language is enabled or a project is declared, so a consumer includes
# this file at its top directory after UbiquittyCoreRoot.cmake and ahead of its
# project() call.
#
# UBIQUITTY_VERSION_SOURCE_DIR names the tree whose commit and VERSION file
# give the version; it is Core's checkout unless the consumer names its own.
include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/UbiquittyCoreRoot.cmake")
ubiquitty_require_top_directory(UbiquittyPreProject.cmake)

# Everything below is read when a language is enabled, so a project whose
# languages are already enabled would keep none of it and say nothing.
get_property(_ubq_enabled_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
if(_ubq_enabled_languages)
    list(JOIN _ubq_enabled_languages ", " _ubq_enabled_languages)
    message(FATAL_ERROR
        "UbiquittyPreProject.cmake is read after the project enabled ${_ubq_enabled_languages}. "
        "Include it before project(), in the order Scripts/cmake/UbiquittyCoreRoot.cmake lays out.")
endif()
set_property(GLOBAL PROPERTY UBIQUITTY_PRE_PROJECT_READ TRUE)

if(NOT DEFINED UBIQUITTY_VERSION_SOURCE_DIR)
    set(UBIQUITTY_VERSION_SOURCE_DIR "${UBIQUITTY_CORE_SOURCE_DIR}")
endif()

# Route compilation through ccache when it is installed. Vendored ThirdParty
# never changes, so compiling it again on every clean configure is pure repeat
# cost. Silently absent is fine: an unset launcher is the
# stock build, so nobody has to install anything for the tree to work.
#
# Ahead of the project() call because CMAKE_<LANG>_COMPILER_LAUNCHER is read
# when a language is enabled, not when a target is defined.
#
# The sloppiness setting is carried here rather than left to the operator's
# environment, because without it the cache misses much of the tree and
# silently so. ccache will not reuse a compilation that consumed a precompiled
# header unless pch_defines lets it skip verifying the PCH's own macro state,
# and the test suite shares one PCH across nearly every target.
#
# time_macros covers __DATE__/__TIME__, which nothing here depends on.
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(UBIQUITTY_CCACHE ccache)
    if(UBIQUITTY_CCACHE)
        set(_ubq_ccache_launcher
            ${CMAKE_COMMAND} -E env CCACHE_SLOPPINESS=pch_defines,time_macros
            ${UBIQUITTY_CCACHE})
        set(CMAKE_C_COMPILER_LAUNCHER ${_ubq_ccache_launcher})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${_ubq_ccache_launcher})
        message(STATUS "Ubiquitty: compiling through ccache (${UBIQUITTY_CCACHE})")
    endif()
endif()

# Minimum supported macOS. Must be set before project() so the compiler /
# SDK probes pick it up. Ignored on non-Apple hosts. Override the C++ floor with
# -DCMAKE_OSX_DEPLOYMENT_TARGET=...
#
# Scripts/macos-deployment-target.txt holds two floors and says why each is the
# number it is. `cxx` becomes CMAKE_OSX_DEPLOYMENT_TARGET, so every C-family
# target inherits it: the portable Core, UbiquittyKit, the bundled
# ubiquitty-host and ubq helpers, and the Metal shader library; the
# vendored QUIC builds and the generated Xcode workspace read the same line.
# `app` is higher and is the floor a user meets: the app bundle's
# LSMinimumSystemVersion and the Swift frontend's target. Every consumer reads
# its number from that file rather than restating it.
file(STRINGS "${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/macos-deployment-target.txt" _ubqFloors
     REGEX "^(cxx|app)=")
foreach(_ubqLine IN LISTS _ubqFloors)
    if(_ubqLine MATCHES "^cxx=(.+)$")
        set(UBIQUITTY_MACOS_MIN "${CMAKE_MATCH_1}")
    elseif(_ubqLine MATCHES "^app=(.+)$")
        set(UBIQUITTY_MACOS_APP_MIN "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT UBIQUITTY_MACOS_MIN OR NOT UBIQUITTY_MACOS_APP_MIN)
    message(FATAL_ERROR "Scripts/macos-deployment-target.txt must define cxx= and app=")
endif()

# An iOS configure reads the same file and must take neither number.
# CMAKE_OSX_DEPLOYMENT_TARGET names the deployment target of whichever Apple
# platform is being built, so the macOS floor written into an iOS build declares
# iOS 13.3 — a version no SDK in this toolchain still builds for, and one no
# diagnostic attributes to a file named for macOS. The iOS floor is its own
# number, set first so the macOS reconciliation below is inert for this build
# rather than merely harmless.
#
# Every iOS product shares the same deployment floor: the first release of the
# previous major, which is what a phone that has not taken this year's update
# runs. What the shell reaches for above it — the bar items' overflow
# priorities (27.0) and the arrangement view that lays a Duo's pane pair clear
# of the fold (27.1) — is gated on availability, and an older device lays the
# same items and the same pair out without them.
set(UBIQUITTY_IOS_MIN "26.0")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${UBIQUITTY_IOS_MIN}" CACHE STRING
            "Minimum iOS version the binaries support")
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "${UBIQUITTY_IOS_MIN}")
        message(STATUS "Ubiquitty: iOS deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET} is below this tree's ${UBIQUITTY_IOS_MIN}; raising it")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${UBIQUITTY_IOS_MIN}" CACHE STRING
            "Minimum iOS version the binaries support" FORCE)
    endif()
endif()

# A directory configured before that file changed keeps the OLD value:
# CMAKE_OSX_DEPLOYMENT_TARGET is a cache variable, `if(NOT DEFINED)` never
# revisits it, and CMake says nothing. The compiler then rejects a header nobody
# edited, naming neither the cache nor the change.
#
# So the value this tree last chose is remembered beside it. When they differ, a
# cached target equal to the PREVIOUS choice came from here rather than from the
# person building, and is updated. One equal to neither was passed with -D and is
# left alone — which is what makes this safe to run on every configure.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # This build's floor was set above. Both arms below compare a target against
    # the macOS number, and an iOS target is not on that scale.
elseif(DEFINED CACHE{UBIQUITTY_MACOS_MIN_CHOSEN})
    if(NOT "$CACHE{UBIQUITTY_MACOS_MIN_CHOSEN}" STREQUAL "${UBIQUITTY_MACOS_MIN}"
       AND "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "$CACHE{UBIQUITTY_MACOS_MIN_CHOSEN}")
        message(STATUS "Ubiquitty: deployment target $CACHE{UBIQUITTY_MACOS_MIN_CHOSEN} -> ${UBIQUITTY_MACOS_MIN}; this directory predates the change")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${UBIQUITTY_MACOS_MIN}" CACHE STRING
            "Minimum macOS version the binaries support" FORCE)
    endif()
elseif(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET
       AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "${UBIQUITTY_MACOS_MIN}")
    # A directory configured before this marker existed carries no record of
    # what chose its target, so the two cases cannot be told apart and the
    # comparison has to stand in for the record: BELOW the tree's floor is the
    # one value that cannot compile, whoever set it. Raising it is the only
    # action that leaves a buildable directory either way.
    message(STATUS "Ubiquitty: deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET} is below this tree's ${UBIQUITTY_MACOS_MIN}; raising it")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${UBIQUITTY_MACOS_MIN}" CACHE STRING
        "Minimum macOS version the binaries support" FORCE)
endif()
set(UBIQUITTY_MACOS_MIN_CHOSEN "${UBIQUITTY_MACOS_MIN}" CACHE INTERNAL
    "The target this tree last chose, so a moved default is distinguishable from an override")

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${UBIQUITTY_MACOS_MIN}" CACHE STRING
        "Minimum macOS version the binaries support")
endif()

# Pin the macOS SDK so the compiler gets a real -isysroot. Without it CMake
# resolves system frameworks under a bare -F (a USER framework path), which
# demotes Apple's own headers to non-system and floods the build with
# deprecation / unused-parameter warnings from inside AppKit / Foundation /
# ApplicationServices — noise that would also block -Werror. A resolved
# -isysroot restores their system-header status. Host-side only; the Linux
# frontend builds on Linux where this is a no-op.
#
# An iOS build is excluded by name rather than by the CMAKE_OSX_SYSROOT test
# alone: `xcrun --show-sdk-path` answers with the macOS SDK whatever is being
# built, so a configure that reached here would pin an iOS target to macOS
# headers and fail on the first UIKit import with a message about a missing
# framework.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_SYSROOT
        AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE UBIQUITTY_MACOS_SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(UBIQUITTY_MACOS_SDK AND IS_DIRECTORY "${UBIQUITTY_MACOS_SDK}")
        set(CMAKE_OSX_SYSROOT "${UBIQUITTY_MACOS_SDK}" CACHE PATH
            "macOS SDK sysroot (resolved via xcrun)" FORCE)
    endif()
endif()

# The version of this build, derived from the commit it is built from: the
# nearest `vMAJOR.MINOR.PATCH` tag and the commits past it, or the VERSION file
# where the tree cannot say. The rule, its bounds and its refusals are in
# Scripts/cmake/BuildVersion.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/BuildVersion.cmake")
ubiquitty_resolve_build_version("${UBIQUITTY_VERSION_SOURCE_DIR}")
message(STATUS "Ubiquitty version: ${UBIQUITTY_VERSION} (${UBIQUITTY_BUILD_IDENTITY})")
set(UBIQUITTY_BUILD_VERSION_DIR "${CMAKE_BINARY_DIR}/generated/version")
set(UBIQUITTY_BUILD_VERSION_RECONFIGURE_STAMP "${UBIQUITTY_BUILD_VERSION_DIR}/reconfigure.stamp")
file(MAKE_DIRECTORY "${UBIQUITTY_BUILD_VERSION_DIR}")
if(NOT EXISTS "${UBIQUITTY_BUILD_VERSION_RECONFIGURE_STAMP}")
    file(WRITE "${UBIQUITTY_BUILD_VERSION_RECONFIGURE_STAMP}" "")
endif()
# The version is baked into bundle metadata when the build is configured, so
# whatever moves it re-runs the configure: VERSION and the rule themselves, the
# worktree's HEAD and its log, and the stamp the build step writes when it
# finds the tree on another version than the one configured.
ubiquitty_build_version_configure_inputs("${UBIQUITTY_VERSION_SOURCE_DIR}" _ubq_version_configure_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${UBIQUITTY_VERSION_SOURCE_DIR}/VERSION"
    "${CMAKE_CURRENT_LIST_DIR}/BuildVersion.cmake"
    "${UBIQUITTY_BUILD_VERSION_RECONFIGURE_STAMP}"
    ${_ubq_version_configure_inputs})
