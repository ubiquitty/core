# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The distribution's licence notices: which files they are, the checkout each
# is read from, and their install. An application stages the same set into its
# bundle with ubiquitty_license_notice_paths and ubiquitty_license_notice_file.

set_property(GLOBAL PROPERTY UBIQUITTY_LICENSE_NOTICE_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/../Scripts/license_notices.py")

# ubiquitty_license_notice_file(<notice> <output>)
#
# A configured copy of the notice, at a unique path for its distribution name.
# One source can appear in several dependency namespaces; Xcode assigns one
# resource destination per physical source file, so each copy needs its own.
# ubiquitty_license_notice_paths has validated and recorded it.
function(ubiquitty_license_notice_file notice output)
    get_property(file GLOBAL PROPERTY "UBIQUITTY_LICENSE_NOTICE_FILE ${notice}")
    if(NOT file)
        message(FATAL_ERROR "No license notice ${notice}: call ubiquitty_license_notice_paths first")
    endif()
    set(${output} "${file}" PARENT_SCOPE)
endfunction()

function(ubiquitty_license_notice_paths output)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_property(script GLOBAL PROPERTY UBIQUITTY_LICENSE_NOTICE_SCRIPT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${script}" "${CMAKE_SOURCE_DIR}/Docs/third-party-inventory.json")
    set(libraryArguments)
    if(DEFINED UBIQUITTY_CORE_SOURCE_DIR)
        set(libraryArguments --core "${UBIQUITTY_CORE_SOURCE_DIR}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${UBIQUITTY_CORE_SOURCE_DIR}/Docs/third-party-inventory.json")
    endif()
    if(DEFINED UBIQUITTY_KIT_SOURCE_DIR)
        list(APPEND libraryArguments --kit "${UBIQUITTY_KIT_SOURCE_DIR}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${UBIQUITTY_KIT_SOURCE_DIR}/Docs/third-party-inventory.json")
    endif()
    foreach(root IN ITEMS "${CMAKE_SOURCE_DIR}" "${UBIQUITTY_CORE_SOURCE_DIR}" "${UBIQUITTY_KIT_SOURCE_DIR}")
        if(NOT root STREQUAL "" AND EXISTS "${root}/Scripts/repository.txt")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/Scripts/repository.txt")
        endif()
    endforeach()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${script}" --source "${CMAKE_SOURCE_DIR}" ${libraryArguments} --files
        RESULT_VARIABLE result OUTPUT_VARIABLE listing ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot collect distribution license notices: ${error}")
    endif()
    string(REPLACE "\n" ";" lines "${listing}")
    set(stagingRoot "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ubiquitty-license-notices")
    set(paths)
    foreach(line IN LISTS lines)
        string(FIND "${line}" "\t" tab)
        string(SUBSTRING "${line}" 0 ${tab} notice)
        math(EXPR fileStart "${tab} + 1")
        string(SUBSTRING "${line}" ${fileStart} -1 file)
        # A changed original refreshes its copy; a deleted copy reruns configure
        # before a bundle resource rule can encounter a missing input.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}" "${stagingRoot}/${notice}")
        set_property(GLOBAL PROPERTY "UBIQUITTY_LICENSE_NOTICE_FILE ${notice}" "${stagingRoot}/${notice}")
        list(APPEND paths "${notice}")
    endforeach()
    # The collector validates the entire source and destination set before
    # writing a copy, including dependency inventories and symlink confinement.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${script}" --source "${CMAKE_SOURCE_DIR}" ${libraryArguments}
            --copy "${stagingRoot}"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot stage distribution license notices: ${error}")
    endif()
    set(${output} "${paths}" PARENT_SCOPE)
endfunction()

function(ubiquitty_install_license_notices destination)
    ubiquitty_license_notice_paths(notices)
    foreach(notice IN LISTS notices)
        get_filename_component(directory "${notice}" DIRECTORY)
        ubiquitty_license_notice_file("${notice}" noticeFile)
        install(FILES "${noticeFile}" DESTINATION "${destination}/${directory}")
    endforeach()
endfunction()
