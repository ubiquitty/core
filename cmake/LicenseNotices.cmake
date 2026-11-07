# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The distribution's licence notices: which files they are, the checkout each
# is read from, and their install. An application stages the same set into its
# bundle with ubiquitty_license_notice_paths and ubiquitty_license_notice_file.

set_property(GLOBAL PROPERTY UBIQUITTY_LICENSE_NOTICE_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/../Scripts/license_notices.py")

# ubiquitty_license_notice_file(<notice> <output>)
#
# A generated copy of the notice, at a unique path for its distribution name.
# One source can appear in several dependency namespaces; Xcode assigns one
# resource destination per physical source file, so each copy needs its own.
# ubiquitty_license_notice_paths has validated and recorded it.
# Each file's UBIQUITTY_RESOURCE_PRODUCER source property names one existing
# utility target in this directory. A bundle listing the file in target_sources
# must add a dependency on that producer. Independent always-run resource-copy
# utility targets depend on the producer instead of the generated files, whose
# generation rule belongs to that owner.
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
    set(inputs
        "${script}" "${CMAKE_SOURCE_DIR}/Docs/third-party-inventory.json")
    set(libraryArguments)
    if(DEFINED UBIQUITTY_CORE_SOURCE_DIR)
        set(libraryArguments --core "${UBIQUITTY_CORE_SOURCE_DIR}")
        list(APPEND inputs
            "${UBIQUITTY_CORE_SOURCE_DIR}/Docs/third-party-inventory.json")
    endif()
    if(DEFINED UBIQUITTY_KIT_SOURCE_DIR)
        list(APPEND libraryArguments --kit "${UBIQUITTY_KIT_SOURCE_DIR}")
        list(APPEND inputs
            "${UBIQUITTY_KIT_SOURCE_DIR}/Docs/third-party-inventory.json")
    endif()
    foreach(root IN ITEMS "${CMAKE_SOURCE_DIR}" "${UBIQUITTY_CORE_SOURCE_DIR}" "${UBIQUITTY_KIT_SOURCE_DIR}")
        if(NOT root STREQUAL "" AND EXISTS "${root}/Scripts/repository.txt")
            list(APPEND inputs "${root}/Scripts/repository.txt")
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
    set(outputs)
    foreach(line IN LISTS lines)
        string(FIND "${line}" "\t" tab)
        string(SUBSTRING "${line}" 0 ${tab} notice)
        math(EXPR fileStart "${tab} + 1")
        string(SUBSTRING "${line}" ${fileStart} -1 file)
        list(APPEND inputs "${file}")
        list(APPEND outputs "${stagingRoot}/${notice}")
        set_property(GLOBAL PROPERTY "UBIQUITTY_LICENSE_NOTICE_FILE ${notice}" "${stagingRoot}/${notice}")
        list(APPEND paths "${notice}")
    endforeach()
    list(REMOVE_DUPLICATES inputs)
    # Inventories and source identities can change the resource set. Generated
    # copies are outputs of the build rule below, never configure inputs.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})

    # Bundle and install queries in one directory share a producer. Its identity
    # is the validated path map and command, not notice bytes that may change.
    string(SHA256 identity "${Python3_EXECUTABLE}\n${script}\n${CMAKE_SOURCE_DIR}\n${libraryArguments}\n${listing}")
    string(SHA256 stagingIdentity "${stagingRoot}")
    set(producer "ubiquitty-license-notices-${stagingIdentity}")
    get_property(registered GLOBAL PROPERTY "UBIQUITTY_LICENSE_NOTICE_RULE ${stagingRoot}")
    if(registered AND NOT registered STREQUAL identity)
        message(FATAL_ERROR "Conflicting license notice inputs for ${stagingRoot}")
    endif()
    if(NOT registered AND TARGET "${producer}")
        message(FATAL_ERROR "License notice producer target already exists: ${producer}")
    endif()
    # The collector validates the entire source and destination set before
    # writing a copy, including dependency inventories and symlink confinement.
    # Configure-time copies also support a direct cmake --install invocation.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${script}" --source "${CMAKE_SOURCE_DIR}" ${libraryArguments}
            --copy "${stagingRoot}"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot stage distribution license notices: ${error}")
    endif()
    if(NOT registered)
        # Xcode plans resource copies before ZERO_CHECK refreshes configured
        # files. Explicit original -> generated-file edges let that plan see
        # a changed notice and order its resource copy after this producer.
        add_custom_command(OUTPUT ${outputs}
            COMMAND "${Python3_EXECUTABLE}" "${script}" --source "${CMAKE_SOURCE_DIR}" ${libraryArguments}
                --copy "${stagingRoot}"
            DEPENDS ${inputs}
            COMMENT "Staging distribution license notices"
            VERBATIM)
        # Several resource directories may consume outputs of this whole-set
        # command. Their dependency on one utility owner keeps Make's recursive
        # target builds from independently running the same producer in parallel.
        add_custom_target("${producer}" DEPENDS ${outputs})
        set_property(GLOBAL PROPERTY "UBIQUITTY_LICENSE_NOTICE_RULE ${stagingRoot}" "${identity}")
    endif()
    set_source_files_properties(${outputs} PROPERTIES UBIQUITTY_RESOURCE_PRODUCER "${producer}")
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
