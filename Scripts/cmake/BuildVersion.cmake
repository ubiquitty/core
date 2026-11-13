# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The version of a build, derived from the commit it is built from.
#
# A named version is a tag `vMAJOR.MINOR.PATCH`. A build's version is
# `MAJOR.MINOR.PATCH.N`, where N counts the commits the build has that the
# nearest named version lacks; at the tag itself N is 0 and is not printed. The
# number is a function of the commit alone, so two machines building one commit
# agree, and it never counts builds or reads a clock.
#
# The nearest tag is the one `git describe` picks without `--first-parent`: a
# branch that merged main reaches main's newest tag through the merge's second
# parent, and following first parents only would leave it on the version it
# branched from. N is describe's own count, every commit reachable from the
# build and not from the tag. On main, whose history is linear, that is the
# distance along it; anywhere else it still grows by at least one per commit.
#
# What the tree cannot answer comes from the VERSION file, which holds the last
# named version: a source archive has no repository, and a shallow clone may not
# reach a tag. Such a build reads as the named version and says why in its
# build identity.
#
# The build identity is a separate string for people: the commit's short hash
# and what qualifies it. It is never part of the version and never compared.
#
# The tag, the VERSION file and the environment are data this file does not
# control, and the strings reach a wire field and an Info.plist. Each is matched
# whole against a fixed shape and bound before use; anything else stops the
# configure.
#
# Included by UbiquittyPreProject.cmake for its functions, and run as a script:
#
#   cmake -DUBIQUITTY_VERSION_SOURCE_DIR=<tree> -DUBIQUITTY_VERSION_PRINT=<field> -P BuildVersion.cmake
#       prints one of: version, named, commits, bundle, identity
#   cmake -DUBIQUITTY_VERSION_SOURCE_DIR=<tree> -DUBIQUITTY_VERSION_HEADER=<file> -P BuildVersion.cmake
#       writes the generated header when its text would change
#
# <tree> is the tree whose commit and VERSION file give the version:
# UBIQUITTY_VERSION_SOURCE_DIR as UbiquittyPreProject.cmake settles it, Core's
# checkout unless the including project names its own. Without it the script
# reads the checkout it sits in, which is Core's.
#
# A build whose tree cannot see its repository (a container given the checkout
# without the repository behind a linked worktree) is told its version by the
# machine that can: UBIQUITTY_BUILD_VERSION and UBIQUITTY_BUILD_IDENTITY in the
# environment replace the derivation and pass the same checks.

# The bounds every version field is held to. They size the bundle version's
# decimal fields, so widening one moves the others' positions.
set(UBIQUITTY_VERSION_MAX_MAJOR 999)
set(UBIQUITTY_VERSION_MAX_MINOR 99)
set(UBIQUITTY_VERSION_MAX_PATCH 99)
set(UBIQUITTY_VERSION_MAX_COMMITS 99999)
set(UBIQUITTY_BUILD_IDENTITY_MAX_BYTES 64)

set(_ubq_version_field "(0|[1-9][0-9]*)")

# ubiquitty_parse_version(<text> <what> <prefix>)
#
# Reads `MAJOR.MINOR.PATCH` or `MAJOR.MINOR.PATCH.N` into <prefix>_MAJOR,
# _MINOR, _PATCH and _COMMITS, or stops the configure naming <what>.
function(ubiquitty_parse_version text what prefix)
    set(_named "^${_ubq_version_field}\\.${_ubq_version_field}\\.${_ubq_version_field}$")
    set(_counted "^${_ubq_version_field}\\.${_ubq_version_field}\\.${_ubq_version_field}\\.([1-9][0-9]*)$")
    string(LENGTH "${text}" _length)
    if(_length GREATER 15)
        message(FATAL_ERROR "${what} is not a version: it is ${_length} characters long.")
    endif()
    if(text MATCHES "${_named}")
        set(_commits 0)
    elseif(text MATCHES "${_counted}")
        set(_commits "${CMAKE_MATCH_4}")
    else()
        message(FATAL_ERROR
            "${what} is '${text}', which is not a version.\n"
            "  A version is MAJOR.MINOR.PATCH, or MAJOR.MINOR.PATCH.N with N above zero:\n"
            "  decimal digits only and no leading zeros.")
    endif()
    set(_major "${CMAKE_MATCH_1}")
    set(_minor "${CMAKE_MATCH_2}")
    set(_patch "${CMAKE_MATCH_3}")
    foreach(_part MAJOR MINOR PATCH COMMITS)
        string(TOLOWER "${_part}" _name)
        string(LENGTH "${_${_name}}" _digits)
        if(_digits GREATER 5 OR _${_name} GREATER UBIQUITTY_VERSION_MAX_${_part})
            message(FATAL_ERROR
                "${what} is '${text}': its ${_name} field is above ${UBIQUITTY_VERSION_MAX_${_part}}.")
        endif()
    endforeach()
    set(${prefix}_MAJOR "${_major}" PARENT_SCOPE)
    set(${prefix}_MINOR "${_minor}" PARENT_SCOPE)
    set(${prefix}_PATCH "${_patch}" PARENT_SCOPE)
    set(${prefix}_COMMITS "${_commits}" PARENT_SCOPE)
endfunction()

# _ubq_version_git(<source-dir> <out-result> <out-output> <arguments>...)
#
# git exports the repository it is running in to a hook's environment, and
# those variables override the one `-C` names. A build started from a hook in
# another worktree would then read that worktree's commit, so they are removed
# for the question.
function(_ubq_version_git source_dir out_result out_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE --unset=GIT_COMMON_DIR
            --unset=GIT_NAMESPACE --unset=GIT_OBJECT_DIRECTORY --unset=GIT_ALTERNATE_OBJECT_DIRECTORIES
            --unset=GIT_CEILING_DIRECTORIES --unset=GIT_PREFIX
            git -C "${source_dir}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    # A missing git binary answers with a sentence instead of an exit status.
    if(NOT _result MATCHES "^-?[0-9]+$")
        set(_result 127)
    endif()
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

# ubiquitty_resolve_build_version(<source-dir>)
#
# Sets in the caller's scope:
#   UBIQUITTY_VERSION          MAJOR.MINOR.PATCH.N, or MAJOR.MINOR.PATCH when N is 0
#   UBIQUITTY_VERSION_NAMED    MAJOR.MINOR.PATCH
#   UBIQUITTY_VERSION_COMMITS  N
#   UBIQUITTY_BUNDLE_VERSION   one integer that orders as the version does
#   UBIQUITTY_BUILD_IDENTITY   the short hash and what qualifies it
function(ubiquitty_resolve_build_version source_dir)
    if(NOT EXISTS "${source_dir}/VERSION")
        message(FATAL_ERROR "${source_dir}/VERSION is missing; it holds the last named version.")
    endif()
    file(READ "${source_dir}/VERSION" _file_text)
    string(STRIP "${_file_text}" _file_text)
    ubiquitty_parse_version("${_file_text}" "The VERSION file" _file)
    if(NOT _file_COMMITS EQUAL 0)
        message(FATAL_ERROR
            "The VERSION file holds '${_file_text}'; it holds a named version, MAJOR.MINOR.PATCH.")
    endif()

    set(_major "${_file_MAJOR}")
    set(_minor "${_file_MINOR}")
    set(_patch "${_file_PATCH}")
    set(_commits 0)
    set(_facts "")

    if(NOT "$ENV{UBIQUITTY_BUILD_VERSION}" STREQUAL "")
        ubiquitty_parse_version("$ENV{UBIQUITTY_BUILD_VERSION}" "UBIQUITTY_BUILD_VERSION" _stated)
        set(_major "${_stated_MAJOR}")
        set(_minor "${_stated_MINOR}")
        set(_patch "${_stated_PATCH}")
        set(_commits "${_stated_COMMITS}")
        set(_identity "$ENV{UBIQUITTY_BUILD_IDENTITY}")
        if(_identity STREQUAL "")
            set(_identity "stated by the build environment")
        endif()
        string(LENGTH "${_identity}" _length)
        if(_length GREATER UBIQUITTY_BUILD_IDENTITY_MAX_BYTES OR NOT _identity MATCHES "^[A-Za-z0-9 ,.+-]+$")
            message(FATAL_ERROR
                "UBIQUITTY_BUILD_IDENTITY is not a build identity: at most "
                "${UBIQUITTY_BUILD_IDENTITY_MAX_BYTES} letters, digits, spaces and ',.+-'.")
        endif()
    else()
        # The tree answers only for itself: an archive unpacked inside another
        # repository must not read that repository's tags.
        _ubq_version_git("${source_dir}" _result _toplevel rev-parse --show-toplevel)
        set(_has_repository OFF)
        if(_result EQUAL 0 AND NOT _toplevel STREQUAL "")
            get_filename_component(_toplevel_real "${_toplevel}" REALPATH)
            get_filename_component(_source_real "${source_dir}" REALPATH)
            if(_toplevel_real STREQUAL _source_real)
                set(_has_repository ON)
            endif()
        endif()
        set(_hash "")
        if(_has_repository)
            _ubq_version_git("${source_dir}" _result _hash rev-parse --verify --quiet --short=8 "HEAD^{commit}")
            if(NOT _result EQUAL 0 OR NOT _hash MATCHES "^[0-9a-f]+$")
                set(_hash "")
            endif()
        endif()
        if(_hash STREQUAL "")
            list(APPEND _facts "no git")
        else()
            list(APPEND _facts "${_hash}")
            _ubq_version_git("${source_dir}" _result _status status --porcelain --untracked-files=no)
            if(_result EQUAL 0 AND NOT _status STREQUAL "")
                list(APPEND _facts "modified")
            endif()
            _ubq_version_git("${source_dir}" _result _described
                describe --tags --match "v[0-9]*" --long --abbrev=40 HEAD)
            if(_result EQUAL 0 AND _described MATCHES "^v(.*)-([0-9]+)-g[0-9a-f]+$")
                set(_tag_version "${CMAKE_MATCH_1}")
                set(_distance "${CMAKE_MATCH_2}")
                ubiquitty_parse_version("${_tag_version}" "The tag 'v${_tag_version}'" _tag)
                if(NOT _tag_COMMITS EQUAL 0)
                    message(FATAL_ERROR
                        "The tag 'v${_tag_version}' is not a named version; a named version is vMAJOR.MINOR.PATCH.\n"
                        "  Delete the tag, or name the version: git tag -d v${_tag_version}")
                endif()
                string(LENGTH "${_distance}" _digits)
                if(_digits GREATER 5 OR _distance GREATER UBIQUITTY_VERSION_MAX_COMMITS)
                    message(FATAL_ERROR
                        "This commit is ${_distance} commits past v${_tag_version}; a version counts at most "
                        "${UBIQUITTY_VERSION_MAX_COMMITS}.\n"
                        "  Name a new version (Docs/building-and-releasing.md, Versioning).")
                endif()
                if(_distance EQUAL 0 AND NOT _tag_version STREQUAL _file_text)
                    message(FATAL_ERROR
                        "This commit is tagged v${_tag_version} and its VERSION file holds '${_file_text}'.\n"
                        "  A named version's commit carries its own number in VERSION, so a tree without\n"
                        "  the repository reads the same version. Move the tag to a commit that does.")
                endif()
                set(_major "${_tag_MAJOR}")
                set(_minor "${_tag_MINOR}")
                set(_patch "${_tag_PATCH}")
                set(_commits "${_distance}")
                _ubq_version_git("${source_dir}" _result _shallow rev-parse --is-shallow-repository)
                if(_result EQUAL 0 AND _shallow STREQUAL "true")
                    list(APPEND _facts "shallow clone")
                endif()
            else()
                list(APPEND _facts "no version tag")
            endif()
        endif()
        list(JOIN _facts ", " _identity)
    endif()

    set(_named "${_major}.${_minor}.${_patch}")
    if(_commits EQUAL 0)
        set(_version "${_named}")
    else()
        set(_version "${_named}.${_commits}")
    endif()
    # Decimal fields, widest first: two digits each for minor and patch, five
    # for the count. A later version is a larger integer whichever field moved.
    math(EXPR _bundle "${_major} * 1000000000 + ${_minor} * 10000000 + ${_patch} * 100000 + ${_commits}")

    set(UBIQUITTY_VERSION "${_version}" PARENT_SCOPE)
    set(UBIQUITTY_VERSION_NAMED "${_named}" PARENT_SCOPE)
    set(UBIQUITTY_VERSION_COMMITS "${_commits}" PARENT_SCOPE)
    set(UBIQUITTY_BUNDLE_VERSION "${_bundle}" PARENT_SCOPE)
    set(UBIQUITTY_BUILD_IDENTITY "${_identity}" PARENT_SCOPE)
endfunction()

# ubiquitty_write_build_version_header(<file> <version> <identity>)
#
# Writes the header one translation unit includes. The file is left untouched
# when its text would not change, so a build that moved no commit recompiles
# nothing for it.
function(ubiquitty_write_build_version_header file version identity)
    string(CONCAT _text
        "// Generated by Scripts/cmake/BuildVersion.cmake. Do not edit.\n"
        "#pragma once\n"
        "\n"
        "namespace Ubiquitty::Generated {\n"
        "inline constexpr char buildVersion[] = \"${version}\";\n"
        "inline constexpr char buildIdentity[] = \"${identity}\";\n"
        "} // namespace Ubiquitty::Generated\n")
    set(_existing "")
    if(EXISTS "${file}")
        file(READ "${file}" _existing)
    endif()
    if(NOT _existing STREQUAL _text)
        file(WRITE "${file}" "${_text}")
    endif()
endfunction()

# ubiquitty_build_version_configure_inputs(<source-dir> <out-list>)
#
# The repository files whose change means the commit moved: the worktree's HEAD
# and its log, which every commit, checkout, pull and rebase appends to. A build
# directory that depends on them reconfigures after any of those.
function(ubiquitty_build_version_configure_inputs source_dir out_list)
    set(_inputs "")
    foreach(_name HEAD logs/HEAD)
        _ubq_version_git("${source_dir}" _result _path rev-parse --git-path "${_name}")
        if(_result EQUAL 0 AND NOT _path STREQUAL "")
            get_filename_component(_path "${_path}" ABSOLUTE BASE_DIR "${source_dir}")
            if(EXISTS "${_path}" AND NOT IS_DIRECTORY "${_path}")
                list(APPEND _inputs "${_path}")
            endif()
        endif()
    endforeach()
    set(${out_list} "${_inputs}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    # Standalone queries inherit no project's policy version.
    cmake_policy(SET CMP0057 NEW)
    if(NOT UBIQUITTY_VERSION_SOURCE_DIR)
        get_filename_component(UBIQUITTY_VERSION_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    endif()
    ubiquitty_resolve_build_version("${UBIQUITTY_VERSION_SOURCE_DIR}")
    if(UBIQUITTY_VERSION_HEADER)
        # A build directory configured for another version carries that version
        # in its bundle metadata. The commit moved without the configure seeing
        # it; the stamp makes the next build configure first. The configured
        # version is read from a file the configure wrote, never passed on this
        # command's line: the line is part of the build graph, and a graph that
        # differed per commit would read as a different build to everything
        # that fingerprints one.
        if(UBIQUITTY_VERSION_CONFIGURED_FILE AND EXISTS "${UBIQUITTY_VERSION_CONFIGURED_FILE}")
            file(READ "${UBIQUITTY_VERSION_CONFIGURED_FILE}" _ubq_version_configured)
            string(STRIP "${_ubq_version_configured}" _ubq_version_configured)
            if(NOT _ubq_version_configured STREQUAL UBIQUITTY_VERSION)
                if(UBIQUITTY_VERSION_RECONFIGURE_STAMP)
                    file(WRITE "${UBIQUITTY_VERSION_RECONFIGURE_STAMP}" "${UBIQUITTY_VERSION}\n")
                endif()
                message(FATAL_ERROR
                    "This build directory was configured as ${_ubq_version_configured} and the tree is now "
                    "${UBIQUITTY_VERSION}.\n  Build again: the next build configures first.")
            endif()
        endif()
        ubiquitty_write_build_version_header(
            "${UBIQUITTY_VERSION_HEADER}" "${UBIQUITTY_VERSION}" "${UBIQUITTY_BUILD_IDENTITY}")
    endif()
    if(UBIQUITTY_VERSION_PRINT)
        set(_ubq_version_fields version named commits bundle identity)
        if(NOT UBIQUITTY_VERSION_PRINT IN_LIST _ubq_version_fields)
            message(FATAL_ERROR "UBIQUITTY_VERSION_PRINT is one of: ${_ubq_version_fields}")
        endif()
        set(_ubq_version_value_version "${UBIQUITTY_VERSION}")
        set(_ubq_version_value_named "${UBIQUITTY_VERSION_NAMED}")
        set(_ubq_version_value_commits "${UBIQUITTY_VERSION_COMMITS}")
        set(_ubq_version_value_bundle "${UBIQUITTY_BUNDLE_VERSION}")
        set(_ubq_version_value_identity "${UBIQUITTY_BUILD_IDENTITY}")
        # Standard output, where a caller's command substitution reads it;
        # message() writes to standard error.
        execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_ubq_version_value_${UBIQUITTY_VERSION_PRINT}}")
    endif()
endif()
