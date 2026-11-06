# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Samples/find-package-consumer — a STANDALONE downstream project that consumes
# libUbiquittyCore only via find_package against an install prefix. It sits
# outside the main Ubiquitty build and is configured separately (with
# CMAKE_PREFIX_PATH pointing at the install prefix) by
# Scripts/test-libubiquittycore-package.sh. No source-tree include paths.
cmake_minimum_required(VERSION 3.13)
project(ubiquitty-core-package-consumer LANGUAGES CXX)

find_package(UbiquittyCore REQUIRED)

add_executable(package-consumer main.cpp)
target_link_libraries(package-consumer PRIVATE Ubiquitty::ubiquitty-kit)
set_target_properties(package-consumer PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(package-consumer PRIVATE -fno-exceptions -fno-rtti)
