# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The distribution's licence notices: which files they are, the checkout each
# is read from, and their install. An application stages the same set into its
# bundle with ubiquitty_license_notice_paths and ubiquitty_license_notice_file.

set_property(GLOBAL PROPERTY UBIQUITTY_LICENSE_NOTICE_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/../Scripts/license_notices.py")

# ubiquitty_license_notice_file(<notice> <output>)
#
# The file a repo-relative notice is read from, as Scripts/license_notices.py
# resolves it: Core's checkout carries the notices of what it ships, the
# project's tree the rest. ubiquitty_license_notice_paths has recorded it.
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
    set(coreArguments)
    if(DEFINED UBIQUITTY_CORE_SOURCE_DIR)
        set(coreArguments --core "${UBIQUITTY_CORE_SOURCE_DIR}")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${script}" --source "${CMAKE_SOURCE_DIR}" ${coreArguments} --files
        RESULT_VARIABLE result OUTPUT_VARIABLE listing ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot collect distribution license notices: ${error}")
    endif()
    string(REPLACE "\n" ";" lines "${listing}")
    set(paths)
    foreach(line IN LISTS lines)
        string(FIND "${line}" "\t" tab)
        string(SUBSTRING "${line}" 0 ${tab} notice)
        math(EXPR fileStart "${tab} + 1")
        string(SUBSTRING "${line}" ${fileStart} -1 file)
        set_property(GLOBAL PROPERTY "UBIQUITTY_LICENSE_NOTICE_FILE ${notice}" "${file}")
        list(APPEND paths "${notice}")
    endforeach()
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
