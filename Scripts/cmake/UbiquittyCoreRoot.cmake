# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Where ubiquitty-core's sources are, for every module that names a Core file.
#
# A project that compiles Core, its own tree or a consumer of it, includes the
# modules beside this file at its top directory and never through
# add_subdirectory(): what they set is directory-scoped, and a directory below
# the project's top keeps it from the project's own targets. In order:
#
#   include(<core>/Scripts/cmake/UbiquittyCoreRoot.cmake)
#   set(UBIQUITTY_KIT_SOURCE_DIR <kit>)       a project that compiles the Kit
#   set(UBIQUITTY_VERSION_SOURCE_DIR <tree>)  a project with a version of its own
#   include(<core>/Scripts/cmake/UbiquittyPreProject.cmake)
#   project(...)
#   include(<core>/Scripts/cmake/UbiquittyToolchain.cmake)
#   include(<core>/Scripts/cmake/UbiquittyOptimize.cmake)
#   include(<core>/Scripts/cmake/UbiquittyCoreSources.cmake)
#   include(<core>/Scripts/cmake/UbiquittyCoreLibrary.cmake)
#   the project's targets, and the calls that create Core's
#
# A module read after project() first includes the modules it reads, so a
# project may name UbiquittyCoreLibrary.cmake alone; a module already read is
# not read again. Every module refuses a first read below the top source
# directory, so a project that compiles Core is never another project's
# subdirectory either. UbiquittyPreProject.cmake refuses a project whose
# languages are already enabled, and UbiquittyToolchain.cmake refuses one that
# did not read UbiquittyPreProject.cmake.
#
# Each setting is fixed when the module that reads it is read:
#
# - UBIQUITTY_VERSION_SOURCE_DIR, by UbiquittyPreProject.cmake. Unset, the
#   version is Core's: its checkout's commit and VERSION file.
# - UBIQUITTY_KIT_SOURCE_DIR, by UbiquittyOptimize.cmake, whose release path
#   map names every source root. It makes a relative root absolute against the
#   top directory, and a different root named after it is refused when the top
#   directory has been read.
# - The directory-scoped flags, by each target as it is created, so a target
#   created before UbiquittyOptimize.cmake is read has no link-time optimization
#   and no profile.
#
# None of them sets a minimum CMake version or a policy: the including
# project's cmake_minimum_required governs them all.
#
# UBIQUITTY_CORE_SOURCE_DIR is the checkout holding this file unless the project
# named one. It is ABSOLUTE rather than REALPATH, so a compile line spells the
# root the way its caller did, and the path map and the training profile's
# localised keys match it.
include_guard(GLOBAL)

# Refuses a module read below the top source directory: what the module sets is
# scoped to the directory that reads it, and would not reach the targets above.
#
# - Parameters:
#   - module: The module's file name, for the message.
function(ubiquitty_require_top_directory module)
    if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        message(FATAL_ERROR
            "${module} is read in ${CMAKE_CURRENT_SOURCE_DIR}, below the top source directory "
            "${CMAKE_SOURCE_DIR}. Include it there, never through add_subdirectory(), in the order "
            "Scripts/cmake/UbiquittyCoreRoot.cmake lays out.")
    endif()
endfunction()
ubiquitty_require_top_directory(UbiquittyCoreRoot.cmake)

if(NOT DEFINED UBIQUITTY_CORE_SOURCE_DIR)
    set(UBIQUITTY_CORE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
endif()
get_filename_component(UBIQUITTY_CORE_SOURCE_DIR "${UBIQUITTY_CORE_SOURCE_DIR}" ABSOLUTE)
