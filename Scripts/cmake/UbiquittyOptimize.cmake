# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# What an optimized build of ubiquitty-core is compiled with: link-time
# optimization, the training profile and the release path map. Every one of
# them is directory-scoped, so a consumer includes this file at its top
# directory, ahead of its own targets, and they are compiled the way Core's
# are: Core inlines into them across translation units, and the profile's
# counts for Core's inline functions reach them too.
#
# Read once per build. A second -fprofile-use and a second UBIQUITTY_PGO_STAMP
# on one command line is "macro redefined" under -Werror the day the two
# disagree, and two profiles in one link are what ThinLTO refuses.
#
#   UBIQUITTY_MULTI_CONFIGURATION, UBIQUITTY_BUILDS_OPTIMIZED
#   UBIQUITTY_PGO (ON, OFF or REQUIRE), UBIQUITTY_PGO_PROFILE
include_guard(GLOBAL)

# UbiquittyToolchain.cmake defaults the build type and chooses the QUIC prefix
# the path map names, so it is read first: an empty build type read here is an
# unoptimized build that prints nothing about it.
include("${CMAKE_CURRENT_LIST_DIR}/UbiquittyToolchain.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ToolchainFloor.cmake")
ubiquitty_require_top_directory(UbiquittyOptimize.cmake)

# Which configurations get what an optimized build gets: link-time
# optimization, the training profile and the release path map below.
#
# A single-configuration build is one configuration, named by CMAKE_BUILD_TYPE.
# A multi-configuration generator writes them all into one project and leaves
# CMAKE_BUILD_TYPE empty, so there each setting is made for the optimized
# configurations by name. The Xcode project's Release configuration is what an
# archive builds, and a TestFlight build is the build that ships, so it gets
# what the Ninja Release build gets.
get_property(UBIQUITTY_MULTI_CONFIGURATION GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(UBIQUITTY_MULTI_CONFIGURATION OR CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
    set(UBIQUITTY_BUILDS_OPTIMIZED ON)
else()
    set(UBIQUITTY_BUILDS_OPTIMIZED OFF)
endif()

# ----------------------------------------------------------------------
# Link-time optimization for optimized native builds.
#
# Cross-TU inlining folds the per-byte ingest helpers (utf8Feed, cellAt,
# VT100Parser::transition, Unicode::Width::forCodePoint, SGR::apply) into their
# hot-loop callers — they otherwise stay out-of-line because they live in
# separate translation units. Gated to optimized configs (Debug stays fast to
# build).
# ----------------------------------------------------------------------
if(UBIQUITTY_BUILDS_OPTIMIZED)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ubiquitty_ipo_ok OUTPUT _ubiquitty_ipo_msg)
    if(_ubiquitty_ipo_ok AND UBIQUITTY_MULTI_CONFIGURATION)
        # Per configuration. The Xcode generator writes LLVM_LTO into each
        # configuration it is on for.
        foreach(_ubiquitty_ipo_configuration IN ITEMS RELEASE RELWITHDEBINFO MINSIZEREL)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${_ubiquitty_ipo_configuration} ON)
        endforeach()
    elseif(_ubiquitty_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
    if(_ubiquitty_ipo_ok)
        # GCC's LTO compares every type by name across translation units and
        # warns [-Wodr] where the definitions differ. Wuffs makes them differ
        # on purpose: a decoder struct carries a 123 MB `dead_weight` member in
        # every TU except the one that defines WUFFS_IMPLEMENTATION, so that no
        # consumer can stack-allocate at a sizeof upstream does not promise.
        # The consumers here (Source/Core/Codec/ZlibInflate.cpp, PNGDecoder.cpp)
        # hold pointers into storage the implementation TU sizes and never the
        # struct by value, and every -Wodr line in the tree's Linux logs names
        # a wuffs type. Clang's LTO has no such check. The accidental kind of
        # collision is caught at the source by Scripts/check-name-collisions.py,
        # where a fix is possible; here the only choice is whether to be told
        # about upstream's design on every link.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_link_options(-Wno-odr)
        endif()
        message(STATUS "LTO/IPO: enabled for the optimized configurations")
    else()
        message(STATUS "LTO/IPO: unsupported (${_ubiquitty_ipo_msg})")
    endif()
endif()

# ----------------------------------------------------------------------
# PGO — the training profile reaches every arm whose compiler can read it.
#
# Every arm, not only the macOS app: the host daemon and the GTK frontend run
# the wire/parse workload the profile is trained on. The policy belongs beside
# the IPO decision above, which is the other thing every Release build gets.
#
# Clang only, and this is not a preference. The profile is LLVM's IR
# instrumentation format; GCC's -fprofile-use takes a DIRECTORY of .gcda files,
# so handed a .profdata path it looks for `<path>/<object>.gcda`, finds none,
# warns once, and COMPILES ANYWAY. The result is a plain -O3 binary that the
# build log calls profile-guided. A build that cannot use the profile reports
# that it did not, so the log distinguishes the two cases.
#
# UBIQUITTY_PGO=OFF is the A/B escape hatch and must keep producing the pre-PGO
# flag set exactly. UBIQUITTY_PGO=REQUIRE stops a configure that would build
# without the profile, as Scripts/pgo-flags.sh does for the builds it serves:
# a release that ships a profiled binary asks for it. A caller that already put
# -fprofile-use in the C++ flags places the profile itself, for instance to leave
# some translation units out or to give Swift a profile of its own, and is left
# alone.
#
# Both the plain flags and the per-configuration ones, because a caller placing
# them is as likely to use either and this has to see them wherever they land.
# For a caller that writes CMAKE_CXX_FLAGS_RELWITHDEBINFO, a check of
# CMAKE_CXX_FLAGS alone reads empty, adds a second -fprofile-use and a second
# UBIQUITTY_PGO_STAMP, and the two stamps disagree the moment the profile is
# retrained after that cache was written -- which -Werror turns into
# "'UBIQUITTY_PGO_STAMP' macro redefined" on every C and C++ file. The stamp
# exists to make a retrain invalidate cached objects, so a duplicate of it that
# goes stale is the failure it was invented to prevent.
#
# Under a multi-configuration generator the flags are conditioned on the
# optimized configurations, and every one of those configurations' flags is
# read for a caller's own -fprofile-use.
if(UBIQUITTY_BUILDS_OPTIMIZED)
    set(UBIQUITTY_PGO "ON" CACHE STRING "Use the checked-in training profile (ON/OFF/REQUIRE)")
    if(NOT DEFINED UBIQUITTY_PGO_PROFILE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/pgo_localize.py
                --default-profile "${UBIQUITTY_CORE_SOURCE_DIR}" "${CMAKE_SYSTEM_NAME}" "${CMAKE_SYSTEM_PROCESSOR}"
            OUTPUT_VARIABLE _ubiquitty_pgo_default
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        set(UBIQUITTY_PGO_PROFILE "${_ubiquitty_pgo_default}"
            CACHE FILEPATH "The training profile -fprofile-use reads")
    endif()
    if(UBIQUITTY_MULTI_CONFIGURATION)
        string(JOIN " " _ubiquitty_pgo_caller_flags "${CMAKE_CXX_FLAGS}" "${CMAKE_CXX_FLAGS_RELEASE}"
            "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}" "${CMAKE_CXX_FLAGS_MINSIZEREL}")
        set(_ubiquitty_pgo_compiles
            "$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>")
    else()
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _ubiquitty_pgo_config)
        set(_ubiquitty_pgo_caller_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_ubiquitty_pgo_config}}")
        set(_ubiquitty_pgo_compiles "$<COMPILE_LANGUAGE:C,CXX>")
    endif()
    if(NOT UBIQUITTY_PGO STREQUAL "OFF" AND NOT _ubiquitty_pgo_caller_flags MATCHES "-fprofile-use")
        unset(_ubiquitty_pgo_unapplied)
        set(_ubiquitty_pgo_toolchain_status 0)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            execute_process(
                COMMAND ${Python3_EXECUTABLE} ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/pgo_toolchain.py
                    verify "${UBIQUITTY_PGO_PROFILE}" "${CMAKE_CXX_COMPILER}"
                RESULT_VARIABLE _ubiquitty_pgo_toolchain_status
                ERROR_VARIABLE _ubiquitty_pgo_toolchain_refusal
                ERROR_STRIP_TRAILING_WHITESPACE)
        endif()
        if(NOT EXISTS "${UBIQUITTY_PGO_PROFILE}")
            message(STATUS "PGO: no profile at ${UBIQUITTY_PGO_PROFILE} — building without one")
            set(_ubiquitty_pgo_unapplied "there is no profile at ${UBIQUITTY_PGO_PROFILE}")
        elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(STATUS
                "PGO: NOT APPLIED — ${CMAKE_CXX_COMPILER_ID} does not read LLVM's profile format. "
                "Support/pgo carries a .profdata (clang, IR instrumentation); GCC's -fprofile-use "
                "wants a directory of .gcda and would compile clean without it. This build is "
                "plain -O3, and says so rather than claiming a profile it did not use.")
            set(_ubiquitty_pgo_unapplied "${CMAKE_CXX_COMPILER_ID} does not read LLVM's profile format")
        elseif(NOT _ubiquitty_pgo_toolchain_status EQUAL 0)
            message(STATUS "PGO: NOT APPLIED — ${_ubiquitty_pgo_toolchain_refusal}; building without PGO")
            set(_ubiquitty_pgo_unapplied "${_ubiquitty_pgo_toolchain_refusal}")
        else()
            # The profile's CONTENT rides along as an inert define. A build
            # system rebuilds an object when its command line changes, and
            # -fprofile-use names only the profile's PATH: after a retrain the
            # cached objects still carry the old counts, and ThinLTO then
            # refuses to link the mix on conflicting ProfileSummary flags.
            file(SHA256 "${UBIQUITTY_PGO_PROFILE}" _ubiquitty_pgo_hash)
            string(SUBSTRING "${_ubiquitty_pgo_hash}" 0 16 _ubiquitty_pgo_stamp)
            # A clang older than the llvm-profdata that wrote the profile cannot
            # read it and fails every translation unit on it, so the compiler is
            # asked before it is handed the file. Scripts/cmake/ToolchainFloor.cmake
            # says why the question is put to the compiler and not its version.
            ubiquitty_compiler_reads_profile(_ubiquitty_pgo_readable _ubiquitty_pgo_refusal
                "${UBIQUITTY_PGO_PROFILE}" "${_ubiquitty_pgo_stamp}")
            if(NOT _ubiquitty_pgo_readable)
                message(STATUS
                    "PGO: NOT APPLIED — ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} cannot read "
                    "${UBIQUITTY_PGO_PROFILE}: ${_ubiquitty_pgo_refusal}. This build is plain -O3. "
                    "A clang at least as new as the LLVM that wrote the profile applies it, and "
                    "-DUBIQUITTY_PGO=OFF leaves it out without asking.")
                set(_ubiquitty_pgo_unapplied
                    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} cannot read the profile")
            else()
                # The three -Wno flags are the tolerance contract: TUs the
                # training battery never executes, functions that drifted since
                # the retrain, and same-name CFG mismatches compile at plain -O3
                # placement instead of erroring under this tree's -Werror.
                # The committed profile keys internal-linkage functions by the
                # trainer's relative paths, and this build compiles absolute
                # ones, so it reads a copy rooted at the Core checkout the
                # profiled sources compile from; Scripts/pgo_localize.py says
                # why no flag can do it. The copy is derived per profile and
                # root, so a retrain reconfigures.
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${UBIQUITTY_PGO_PROFILE}")
                find_package(Python3 COMPONENTS Interpreter REQUIRED)
                execute_process(
                    COMMAND ${Python3_EXECUTABLE} ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/pgo_localize.py
                        "${UBIQUITTY_PGO_PROFILE}" "${UBIQUITTY_CORE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/pgo"
                    OUTPUT_VARIABLE _ubiquitty_pgo_local
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
                add_compile_options(
                    $<${_ubiquitty_pgo_compiles}:-fprofile-use=${_ubiquitty_pgo_local}>
                    $<${_ubiquitty_pgo_compiles}:-DUBIQUITTY_PGO_STAMP=${_ubiquitty_pgo_stamp}>
                    $<${_ubiquitty_pgo_compiles}:-Wno-profile-instr-unprofiled>
                    $<${_ubiquitty_pgo_compiles}:-Wno-profile-instr-out-of-date>
                    $<${_ubiquitty_pgo_compiles}:-Wno-backend-plugin>)
                message(STATUS "PGO: using ${UBIQUITTY_PGO_PROFILE} as ${_ubiquitty_pgo_local}")
            endif()
        endif()
        if(DEFINED _ubiquitty_pgo_unapplied AND UBIQUITTY_PGO STREQUAL "REQUIRE")
            message(FATAL_ERROR
                "PGO: UBIQUITTY_PGO=REQUIRE, and this build would have no profile: "
                "${_ubiquitty_pgo_unapplied}. Configure with a clang that reads it, or with "
                "UBIQUITTY_PGO=ON to accept a plain -O3 build.")
        endif()
    endif()
endif()

# A release build keeps the builder's paths out of what it writes. re2 under
# Source/ThirdParty passes __FILE__ to its logging, and an absolute checkout or
# build root in a shipped binary names the machine it was built on and makes two
# builds of one commit at different paths differ. Release and MinSizeRel remap
# every path the compiler records. RelWithDebInfo remaps only __FILE__, because
# its debug information is how Instruments and lldb find the sources; the
# vendored QUIC stack is remapped where it is built (Scripts/fetch-quic-deps.sh).
# A multi-configuration project carries both, each conditioned on its
# configurations.
#
# - Parameters:
#   - option: The prefix-map option.
#   - condition: The generator expression the option is added under.
function(ubiquitty_add_path_map option condition)
    # Every source checkout maps to ".", once per distinct root: the project's
    # own tree first, then the Kit's, then Core's. A checkout nested inside
    # another is matched by both maps. Clang's __FILE__ takes the longer prefix
    # whatever the order; GCC, and clang's debug information, take the map named
    # last. Core's checkout may sit inside the Kit's, as the Kit's submodule, and
    # either inside the project's tree, never the reverse, so each checkout is
    # named after every checkout that holds it and its files map as its own
    # under every one of those rules, as the profile keys them.
    set(sourceRoots "${CMAKE_SOURCE_DIR}" "${UBIQUITTY_KIT_SOURCE_DIR}" "${UBIQUITTY_CORE_SOURCE_DIR}")
    list(REMOVE_DUPLICATES sourceRoots)
    list(TRANSFORM sourceRoots APPEND "=." OUTPUT_VARIABLE sourcePaths)
    # An empty prefix would match every path, so each is named only when set.
    foreach(mappedPath IN LISTS sourcePaths ITEMS
            "${CMAKE_BINARY_DIR}=build" "${UBIQUITTY_QUIC_PREFIX}=quic-deps")
        if(NOT mappedPath MATCHES "^=")
            add_compile_options("$<${condition}:${option}=${mappedPath}>")
        endif()
    endforeach()
endfunction()
# A compile line spells every source absolute, so a root is mapped as an
# absolute path or it maps nothing. Core's is made absolute where it is
# defaulted; the Kit's is made absolute here, against this top directory.
if(NOT "${UBIQUITTY_KIT_SOURCE_DIR}" STREQUAL "")
    get_filename_component(UBIQUITTY_KIT_SOURCE_DIR "${UBIQUITTY_KIT_SOURCE_DIR}" ABSOLUTE)
endif()
set(_ubiquitty_path_map_compiles "$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>")
if(UBIQUITTY_MULTI_CONFIGURATION)
    ubiquitty_add_path_map("-ffile-prefix-map"
        "$<AND:${_ubiquitty_path_map_compiles},$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>>")
    ubiquitty_add_path_map("-fmacro-prefix-map"
        "$<AND:${_ubiquitty_path_map_compiles},$<CONFIG:RelWithDebInfo>>")
elseif(CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    ubiquitty_add_path_map("-ffile-prefix-map" "${_ubiquitty_path_map_compiles}")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    ubiquitty_add_path_map("-fmacro-prefix-map" "${_ubiquitty_path_map_compiles}")
endif()

# The map names the roots set when this module is read. A Kit root named later
# is a checkout the map leaves out, whose release objects then carry its
# absolute paths, so the root read here is held against the one the top
# directory ends with, both absolute against the top directory.
set_property(GLOBAL PROPERTY UBIQUITTY_PATH_MAPPED_KIT_SOURCE_DIR "${UBIQUITTY_KIT_SOURCE_DIR}")
function(ubiquitty_require_path_mapped_kit_root)
    get_property(mappedRoot GLOBAL PROPERTY UBIQUITTY_PATH_MAPPED_KIT_SOURCE_DIR)
    set(kitRoot "${UBIQUITTY_KIT_SOURCE_DIR}")
    if(NOT kitRoot STREQUAL "")
        get_filename_component(kitRoot "${kitRoot}" ABSOLUTE)
    endif()
    if(NOT kitRoot STREQUAL mappedRoot)
        message(FATAL_ERROR
            "UBIQUITTY_KIT_SOURCE_DIR is \"${kitRoot}\", and UbiquittyOptimize.cmake "
            "mapped \"${mappedRoot}\" when it was read. Set it before that module, in the order "
            "Scripts/cmake/UbiquittyCoreRoot.cmake lays out.")
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL ubiquitty_require_path_mapped_kit_root)
