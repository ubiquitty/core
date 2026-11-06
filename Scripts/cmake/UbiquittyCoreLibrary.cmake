# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The libraries a build of ubiquitty-core defines, and the package it installs.
# A consumer includes this file at its top directory after project(), and it
# reads UbiquittyOptimize.cmake (which reads UbiquittyToolchain.cmake) and
# UbiquittyCoreSources.cmake first. Every function creates its targets in the
# directory that calls it, so a consumer calls them from its top directory,
# where UbiquittyToolchain.cmake and UbiquittyOptimize.cmake set the flags every
# translation unit, Core's and the consumer's own, is compiled with.
#
#   ubiquitty_add_core_layer_libraries()
#       ubiquitty-core-model, ubiquitty-behaviour, ubiquitty-core (the
#       aggregate a consumer links) and the session facade ubiquitty-kit; sets
#       UBIQUITTY_LAYER_STAGE_DIR, UBIQUITTY_CORE_API_VERSION and
#       UBIQUITTY_CORE_INSTALL_INCLUDE_DIR
#   ubiquitty_add_core_ios_library(<target> EXCLUDE <files...>
#                                  [INCLUDE_DIRECTORIES <directories...>])
#   ubiquitty_add_session_facade(<target> CORE_TARGET <core-target>)
#   ubiquitty_install_core_package()
include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/UbiquittyOptimize.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/UbiquittyCoreSources.cmake")
ubiquitty_require_top_directory(UbiquittyCoreLibrary.cmake)
include("${UBIQUITTY_CORE_SOURCE_DIR}/cmake/LicenseNotices.cmake")

# The session facade, UbiquittyKit::Session, over a Core library target. It
# sits outside the canonical list because it is a layer above it, so a build
# that wants it names it. The caller sets its compile options, which differ
# between the layer chain and the iOS library.
#
# - Parameters:
#   - target: The facade library to create.
#   - CORE_TARGET: The Core library it links publicly.
function(ubiquitty_add_session_facade target)
    cmake_parse_arguments(PARSE_ARGV 1 _ubq_facade "" "CORE_TARGET" "")
    if(NOT _ubq_facade_CORE_TARGET)
        message(FATAL_ERROR "ubiquitty_add_session_facade(${target}) needs CORE_TARGET")
    endif()
    add_library(${target} STATIC "${UBIQUITTY_CORE_SOURCE_DIR}/Source/UbiquittyKit/Session.cpp")
    target_link_libraries(${target} PUBLIC ${_ubq_facade_CORE_TARGET})
endfunction()

# The portable Core as a static library for iOS: the canonical list, which
# swaps the iOS spawn backends in, minus the caller's cut, plus the iOS PAL
# backends, the built-in themes and the vendored sources Core calls.
#
# The cut is the caller's because it is made by policy: every translation unit
# in the canonical list compiles and links for arm64-apple-ios, and what an App
# Store build leaves out is a decision about what the binary may carry.
#
# - Parameters:
#   - target: The library to create.
#   - EXCLUDE: The canonical-list entries the library leaves out, repo-relative.
#   - INCLUDE_DIRECTORIES: The header roots it publishes; Core's alone when
#     omitted.
function(ubiquitty_add_core_ios_library target)
    cmake_parse_arguments(PARSE_ARGV 1 _ubq_ios "" "" "EXCLUDE;INCLUDE_DIRECTORIES")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "ubiquitty_add_core_ios_library(${target}) needs an iOS toolchain")
    endif()
    if(NOT DEFINED _ubq_ios_INCLUDE_DIRECTORIES)
        set(_ubq_ios_INCLUDE_DIRECTORIES "${UBIQUITTY_CORE_SOURCE_DIR}/Source")
    endif()
    ubiquitty_core_subset(_ubq_ios_sources ABSOLUTE EXCLUDE ${_ubq_ios_EXCLUDE})
    ubiquitty_embed_builtin_themes(_ubq_ios_themes
        COMMENT "Embedding built-in themes TOML (${target})" TARGET ${target})
    add_library(${target} STATIC
        ${_ubq_ios_sources}
        ${UBIQUITTY_CORE_IOS_BACKEND_SOURCES}
        ${_ubq_ios_themes}
        # ScrollSearch.cpp is in the canonical list, so the RE2 units ride
        # along; Codec/JSON.cpp wraps yyjson for the same reason.
        ${UBIQUITTY_RE2_SOURCES}
        ${UBIQUITTY_SIMDUTF_SOURCES}
        ${UBIQUITTY_YYJSON_SOURCE})
    target_include_directories(${target} PUBLIC ${_ubq_ios_INCLUDE_DIRECTORIES})
    target_include_directories(${target} SYSTEM PRIVATE
        "${UBIQUITTY_RE2_DIR}" "${UBIQUITTY_SIMDUTF_DIR}")
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_link_libraries(${target} PUBLIC "-framework Foundation")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra
        ${UBIQUITTY_NARROWING_WARNING}
        "$<$<BOOL:${UBIQUITTY_WERROR}>:-Werror>")
endfunction()

# The standalone portable Core: the canonical list packaged as layer libraries,
# the full set rather than a frontend's subset, with no frontend, no platform
# toolkit and no transport stack. It links against nothing but Source/ and a
# caller-supplied PAL. Public umbrella header:
# Source/UbiquittyCore/UbiquittyCore.h (Docs/libubiquittycore.md).
function(ubiquitty_add_core_layer_libraries)
    # Themes.cpp parses the built-in [[themes]] from an embedded TOML symbol
    # (builtinThemesTOML), generated from Source/Core/Themes/builtin-themes.toml.
    # The lib compiles Themes.cpp, so it must also carry the generated TU or any
    # consumer pulling Themes.o gets an unresolved symbol.
    ubiquitty_embed_builtin_themes(_ubq_themes_source
        COMMENT "Embedding built-in themes TOML (ubiquitty-core)" TARGET ubiquitty-behaviour)

    # ---- Per-layer library targets (Docs/package-layers.md phase 5) --------
    # Layer membership has ONE authority: Scripts/check-core-layer-partition.sh.
    # The TU list of each layer is emitted from it at configure time, and the
    # union is asserted equal to the canonical SSOT — a TU in no layer (or a
    # drifted checker) fails configure, never silently mis-packages.
    foreach(_layer L0 L1 L2 L3b)
        execute_process(
            COMMAND bash ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/check-core-layer-partition.sh --emit ${_layer}
            WORKING_DIRECTORY ${UBIQUITTY_CORE_SOURCE_DIR}
            OUTPUT_VARIABLE _layer_out
            RESULT_VARIABLE _layer_rc)
        if(NOT _layer_rc EQUAL 0)
            message(FATAL_ERROR "check-core-layer-partition.sh --emit ${_layer} failed — the SSOT has an unclassified TU")
        endif()
        string(STRIP "${_layer_out}" _layer_out)
        string(REPLACE "\n" ";" UBIQUITTY_${_layer}_SOURCES "${_layer_out}")
    endforeach()
    # The checker classifies the SSOT, which names the POSIX backends. wasm swaps
    # them in UbiquittyCoreSources.cmake, so the layer owning them follows: L3b holds whichever backends
    # this target actually builds, or the layer library compiles a TU the target
    # excluded and the union stops matching the canonical list.
    if(EMSCRIPTEN)
        foreach(_backend IN LISTS UBIQUITTY_BUNDLED_BACKENDS)
            list(REMOVE_ITEM UBIQUITTY_L3b_SOURCES Source/Platform/POSIX/${_backend}.cpp)
            list(APPEND UBIQUITTY_L3b_SOURCES Source/Platform/Wasm/${_backend}.cpp)
        endforeach()
    endif()
    set(_ubq_layer_union ${UBIQUITTY_L0_SOURCES} ${UBIQUITTY_L1_SOURCES}
        ${UBIQUITTY_L2_SOURCES} ${UBIQUITTY_L3b_SOURCES})
    list(SORT _ubq_layer_union)
    set(_ubq_canonical ${UBIQUITTY_CORE_SOURCES})
    list(SORT _ubq_canonical)
    if(NOT _ubq_layer_union STREQUAL _ubq_canonical)
        message(FATAL_ERROR
            "layer partition != the portable-core SSOT — run "
            "Scripts/check-core-layer-partition.sh for the unassigned TU")
    endif()
    foreach(_layer L0 L1 L2 L3b)
        list(TRANSFORM UBIQUITTY_${_layer}_SOURCES PREPEND "${UBIQUITTY_CORE_SOURCE_DIR}/")
    endforeach()

    # ubiquitty-core-model: the L0 foundation/PAL-contract implementation + the
    # L1 terminal model. ScrollSearch.cpp (L1) pulls the RE2 TUs along.
    # Codec/JSON.cpp (in the model) calls the vendored yyjson, so the parser
    # rides the same archive: a consumer that reaches no JSON pulls neither
    # member, and one that does needs no source list of its own to say so.
    add_library(ubiquitty-core-model STATIC
        ${UBIQUITTY_L0_SOURCES}
        ${UBIQUITTY_L1_SOURCES}
        ${UBIQUITTY_RE2_SOURCES}
        ${UBIQUITTY_SIMDUTF_SOURCES}
        ${UBIQUITTY_YYJSON_SOURCE})

    # ubiquitty-behaviour: the L2 behaviour + Transport-SDK layer, over the
    # model. Themes.cpp (L2) needs the generated builtinThemesTOML TU.
    add_library(ubiquitty-behaviour STATIC
        ${UBIQUITTY_L2_SOURCES}
        ${_ubq_themes_source})
    target_link_libraries(ubiquitty-behaviour PUBLIC ubiquitty-core-model)

    # ubiquitty-core: the aggregate every consumer links — surface unchanged by
    # the split (identical symbols + headers). Carries the bundled POSIX PAL
    # backend (L3b), which belongs to neither package.
    add_library(ubiquitty-core STATIC
        ${UBIQUITTY_L3b_SOURCES})
    # The three archives reference each other in both directions by design: L2
    # calls the PAL contract and L3b implements it, so behaviour's undefined
    # symbols resolve in an archive that precedes it on the link line. Apple's
    # linker rescans and does not care; GNU ld makes one pass per archive and
    # leaves them undefined, so every consumer needs the group rather than each
    # consumer discovering this.
    target_link_libraries(ubiquitty-core PUBLIC ubiquitty-behaviour)
    # …and back: L2 calls the PAL contract that this L3b backend defines, so the
    # reference genuinely runs both ways. Declaring the cycle is what makes
    # CMake repeat the archives, which is what a linker that resolves in one
    # pass needs. Apple's linker rescans every archive and never needed the
    # repetition; what it does with it is print `ld: warning: ignoring
    # duplicate libraries` on every link in the tree, tests included — the same
    # words as a link line naming a library twice by accident, which is the
    # only kind worth being told about. So the back-edge is declared only for
    # the linker that needs it. Nothing links behaviour on its own — core is
    # its one consumer — so on Apple the one direction above is the whole
    # graph.
    if(NOT APPLE)
        target_link_libraries(ubiquitty-behaviour PUBLIC ubiquitty-core)
        # The model calls the PAL contract too: OSC 7 asks the platform its
        # host name (PAL::systemHostName, which this backend defines), and the
        # model's archive is the last on a consumer's link line, where one
        # pass leaves that reference undefined.
        target_link_libraries(ubiquitty-core-model PUBLIC ubiquitty-core)
    endif()
    # This target compiles the L3b backend and nothing else, and that backend
    # includes its own Platform/POSIX/*.h. Those headers are in no layer stage,
    # since staging them would put a platform backend inside the model and
    # behaviour include boundary, so the target reads the real source root. The
    # path is PRIVATE so that no consumer of the library inherits it.
    target_include_directories(ubiquitty-core PRIVATE ${UBIQUITTY_CORE_SOURCE_DIR}/Source)

    # ubiquitty-kit: the Tier-3 embedder facade (UbiquittyKit::Session — one
    # object, local or remote origin) over the layer chain. Additive: no
    # in-tree frontend links it; its surface is versioned by kitVersion
    # (Source/UbiquittyKit/Version.h), independent of coreVersion.
    ubiquitty_add_session_facade(ubiquitty-kit CORE_TARGET ubiquitty-core)

    # Installable-package wiring (find_package(UbiquittyCore)). The package
    # version is the SUPPORTED-SURFACE semver from Source/UbiquittyCore/Version.h
    # (single source), independent of the app VERSION.
    include(GNUInstallDirs)
    file(READ ${UBIQUITTY_CORE_SOURCE_DIR}/Source/UbiquittyCore/Version.h _ubq_ver_h)
    string(REGEX MATCH "coreVersionMajor = ([0-9]+)" _ "${_ubq_ver_h}")
    set(_ubq_major ${CMAKE_MATCH_1})
    string(REGEX MATCH "coreVersionMinor = ([0-9]+)" _ "${_ubq_ver_h}")
    set(_ubq_minor ${CMAKE_MATCH_1})
    string(REGEX MATCH "coreVersionPatch = ([0-9]+)" _ "${_ubq_ver_h}")
    set(_ubq_patch ${CMAKE_MATCH_1})
    set(UBIQUITTY_CORE_API_VERSION "${_ubq_major}.${_ubq_minor}.${_ubq_patch}")
    set(_ubq_incdir ${CMAKE_INSTALL_INCLUDEDIR}/UbiquittyCore-${_ubq_major}.${_ubq_minor})

    # Durable published-header topology: each layer's BUILD-interface include
    # dir is a STAGED tree holding only its own modules' headers, so an upward
    # include fails at compile time (no Preferences/ header exists in the model
    # stage) — the compile-time half of the Rule-10 boundary. Lower stages
    # propagate to higher targets through the PUBLIC links; the INSTALL
    # interface stays the versioned install include dir.
    set(_ubq_stage ${CMAKE_CURRENT_BINARY_DIR}/layer-include)
    # The module lists come from check-core-layer-partition.sh, which is also what
    # assigns each TU its layer. They were written here as well until the two
    # copies drifted into opposite match orders: the headers staged correctly
    # while the TUs did not, so ubiquitty-core-model shipped the behaviour layer
    # and nothing reported it. One spelling, read by both.
    #
    # Matched against paths relative to Source/, behaviour before model — a bare
    # "Core" is a model prefix and every behaviour subtree lives under it, so the
    # other order stages behaviour into model and disables the Rule-10
    # compile-time boundary instead of enforcing it.
    foreach(_layer_modules L1 L2)
        execute_process(
            COMMAND ${UBIQUITTY_CORE_SOURCE_DIR}/Scripts/check-core-layer-partition.sh --emit-modules ${_layer_modules}
            WORKING_DIRECTORY ${UBIQUITTY_CORE_SOURCE_DIR}
            OUTPUT_VARIABLE _modules_out RESULT_VARIABLE _modules_rc)
        if(NOT _modules_rc EQUAL 0)
            message(FATAL_ERROR "check-core-layer-partition.sh --emit-modules ${_layer_modules} failed")
        endif()
        string(STRIP "${_modules_out}" _modules_out)
        string(REPLACE "\n" ";" _modules_out "${_modules_out}")
        set(_ubq_${_layer_modules}_modules "${_modules_out}")
    endforeach()
    set(_ubq_behaviour_modules ${_ubq_L2_modules})
    set(_ubq_model_modules ${_ubq_L1_modules})
    # The stage has to be a function of the CURRENT classification, not of every
    # classification this build directory has ever seen: reclassify a module and
    # its old staged copy stays where it was, so the Rule-10 boundary keeps
    # admitting an include it should now reject — silently, because the compile
    # still succeeds.
    #
    # Emptying the stages first achieves that and costs the whole team. Every
    # staged header is then NEW on every configure — fresh mtime, identical
    # contents — so every translation unit including one rebuilds: 159 of them
    # per configure, on every push, for every checkout. `configure_file(COPYONLY)`
    # below is already write-if-different and would have left those mtimes
    # alone; it never got the chance, because the file it would have compared
    # against had just been deleted.
    #
    # So the wanted set is collected as the stage is filled, and the sweep after
    # the loop removes only what is absent from it. Both properties hold: a header
    # that changed layer loses its old copy, and a header that changed nothing
    # keeps its mtime.
    set(_ubq_staged_wanted "")
    file(GLOB_RECURSE _ubq_all_headers RELATIVE ${UBIQUITTY_CORE_SOURCE_DIR}/Source
        CONFIGURE_DEPENDS ${UBIQUITTY_CORE_SOURCE_DIR}/Source/*.h)
    # Top-level directories whose headers go into no stage at all.
    set(_ubq_unstaged_roots ThirdParty/ Platform/ PAL/)
    foreach(_header ${_ubq_all_headers})
        set(_stage_dir "")
        string(REGEX MATCH "^[^/]+/" _header_root "${_header}")
        get_filename_component(_header_dir "${_header}" DIRECTORY)
        if(_header STREQUAL "Forward.h")
            set(_stage_dir model)
        elseif(_header_dir STREQUAL "PAL")
            set(_stage_dir model) # the L0 PAL contract; per-OS backend subdirs stage nowhere
        elseif(_header MATCHES "^UbiquittyKit/")
            set(_stage_dir kit)
        elseif(_header_root IN_LIST _ubq_unstaged_roots)
            set(_stage_dir "") # vendored + concrete backends: no layer stage
        else()
            # A behaviour module's path is longer than the Core/ prefix the model
            # list claims, so the behaviour list gets the first look and the
            # model list only sees what it left unclaimed.
            foreach(_stage_layer behaviour model)
                if(NOT _stage_dir)
                    foreach(_module ${_ubq_${_stage_layer}_modules})
                        if(_header MATCHES "^${_module}/")
                            set(_stage_dir ${_stage_layer})
                        endif()
                    endforeach()
                endif()
            endforeach()
        endif()
        if(_stage_dir)
            # configure_file rather than file(COPY): configure_file registers the input as a
            # configure dependency, so editing a header's CONTENTS re-runs CMake and
            # refreshes the staged copy. file(COPY) runs once and leaves the stage
            # stale until something else forces a reconfigure — and because every
            # target includes the stage ahead of Source/, the build then compiles
            # against the OLD header while reporting success.
            configure_file(${UBIQUITTY_CORE_SOURCE_DIR}/Source/${_header}
                ${_ubq_stage}/${_stage_dir}/${_header} COPYONLY)
            list(APPEND _ubq_staged_wanted ${_ubq_stage}/${_stage_dir}/${_header})
        endif()
    endforeach()
    # Protocol/Tmux stages under model ahead of the generic behaviour match sees
    # Protocol/Sync — the two subtrees land in different stages by the explicit
    # per-subtree patterns above. The generated Terminfo blob header is
    # root-relative in its includer, so it stages at the behaviour root.
    if(EXISTS ${UBIQUITTY_CORE_SOURCE_DIR}/Source/UbiquittyTerminfo.h)
        configure_file(${UBIQUITTY_CORE_SOURCE_DIR}/Source/UbiquittyTerminfo.h
            ${_ubq_stage}/behaviour/UbiquittyTerminfo.h COPYONLY)
        list(APPEND _ubq_staged_wanted ${_ubq_stage}/behaviour/UbiquittyTerminfo.h)
    endif()

    # Remove staged headers the current classification does not want: one whose
    # module moved between layers, and one whose source header was deleted or
    # renamed. Everything else keeps the mtime `configure_file` decided not to
    # touch, which is what keeps the dependent rebuilds off.
    file(GLOB_RECURSE _ubq_staged_present ${_ubq_stage}/model/*.h
        ${_ubq_stage}/behaviour/*.h ${_ubq_stage}/kit/*.h)
    foreach(_staged ${_ubq_staged_present})
        if(NOT _staged IN_LIST _ubq_staged_wanted)
            file(REMOVE ${_staged})
        endif()
    endforeach()

    target_include_directories(ubiquitty-core-model PUBLIC
        $<BUILD_INTERFACE:${_ubq_stage}/model>
        $<INSTALL_INTERFACE:${_ubq_incdir}>)
    target_include_directories(ubiquitty-behaviour PUBLIC
        $<BUILD_INTERFACE:${_ubq_stage}/behaviour>)
    target_include_directories(ubiquitty-kit PUBLIC
        $<BUILD_INTERFACE:${_ubq_stage}/kit>)
    # Each layer's OWN TUs compile against the full tree (vendored ThirdParty
    # reaches through published headers; Rule 10 guards their source-level
    # discipline) — PRIVATE, so CONSUMERS resolve only through the stages.
    foreach(_ubq_layer_target ubiquitty-core-model ubiquitty-behaviour ubiquitty-kit)
        target_include_directories(${_ubq_layer_target} PRIVATE ${UBIQUITTY_CORE_SOURCE_DIR}/Source)
    endforeach()
    # RE2 headers are an implementation detail of ScrollSearch.cpp — PRIVATE,
    # never part of the installed package surface; SYSTEM so re2's own header
    # diagnostics don't surface through ScrollSearch.cpp's TU.
    target_include_directories(ubiquitty-core-model SYSTEM PRIVATE "${UBIQUITTY_RE2_DIR}" "${UBIQUITTY_SIMDUTF_DIR}")
    # WebKit style: no exceptions, no RTTI. The library is OS-clean — every TU
    # is framework- and (above the PAL/Transport seam) POSIX-free, enforced by
    # Scripts/check-boundaries.py.
    foreach(_ubq_layer_target ubiquitty-core-model ubiquitty-behaviour ubiquitty-core ubiquitty-kit)
        target_compile_features(${_ubq_layer_target} PUBLIC cxx_std_23)
        set_target_properties(${_ubq_layer_target} PROPERTIES
            CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_compile_options(${_ubq_layer_target} PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra
        ${UBIQUITTY_NARROWING_WARNING}
            "$<$<BOOL:${UBIQUITTY_WERROR}>:-Werror>"
            -Wno-deprecated-declarations -Wno-multichar -Wno-comment)
    endforeach()

    set(UBIQUITTY_LAYER_STAGE_DIR "${_ubq_stage}" PARENT_SCOPE)
    set(UBIQUITTY_CORE_API_VERSION "${UBIQUITTY_CORE_API_VERSION}" PARENT_SCOPE)
    set(UBIQUITTY_CORE_INSTALL_INCLUDE_DIR "${_ubq_incdir}" PARENT_SCOPE)
endfunction()

# Installs the layer libraries, their staged headers, the licence notices and
# the find_package(UbiquittyCore) files. The package version is the
# supported-surface semver from Source/UbiquittyCore/Version.h, independent of
# the app's VERSION. Called after ubiquitty_add_core_layer_libraries().
function(ubiquitty_install_core_package)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # All four archives ship: the aggregate's PUBLIC links make model +
    # behaviour part of its exported link interface, and Kit links that chain.
    # Every target must therefore be present in the export set.
    install(TARGETS ubiquitty-kit ubiquitty-core ubiquitty-behaviour ubiquitty-core-model
        EXPORT UbiquittyCoreTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    # Install the same classified header trees that build-interface consumers
    # compile against. This includes the Core/Kit umbrellas, Base, PAL and
    # Forward.h, without maintaining a second module list or publishing the
    # concrete platform backends and vendored implementation headers.
    install(DIRECTORY
        "${UBIQUITTY_LAYER_STAGE_DIR}/model/"
        "${UBIQUITTY_LAYER_STAGE_DIR}/behaviour/"
        "${UBIQUITTY_LAYER_STAGE_DIR}/kit/"
        DESTINATION ${UBIQUITTY_CORE_INSTALL_INCLUDE_DIR}
        FILES_MATCHING PATTERN "*.h")

    install(EXPORT UbiquittyCoreTargets
        FILE UbiquittyCoreTargets.cmake
        NAMESPACE Ubiquitty::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UbiquittyCore)

    configure_package_config_file(
        ${UBIQUITTY_CORE_SOURCE_DIR}/cmake/UbiquittyCoreConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/UbiquittyCoreConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UbiquittyCore)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/UbiquittyCoreConfigVersion.cmake
        VERSION ${UBIQUITTY_CORE_API_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/UbiquittyCoreConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/UbiquittyCoreConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UbiquittyCore)

    ubiquitty_install_license_notices("${CMAKE_INSTALL_DOCDIR}")
endfunction()
