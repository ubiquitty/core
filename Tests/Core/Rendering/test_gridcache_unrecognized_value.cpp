// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_gridcache_unrecognized_value.cpp — a value outside the switch's four.
//
// Its own binary because the switch is read once per process: the value has
// to be in the environment before the first ask, and no other case may ask.

#include "Harness.h"

#include "Core/Rendering/GridCapturePlan.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using Ubiquitty::Rendering::isRetainedGridCacheEnabled;

// `false` reads as nothing the switch documents. The cache keeps its default,
// and the reader is told once, however often the switch is asked.
TEST(anUnrecognizedValueWarnsOnceAndKeepsTheCacheOn)
{
    REQUIRE(::setenv("UBIQUITTY_GRID_CACHE", "false", 1) == 0);
    std::FILE* capture = std::tmpfile();
    REQUIRE(capture != nullptr);
    std::fflush(stderr);
    const int savedError = ::dup(STDERR_FILENO);
    REQUIRE(savedError >= 0);
    REQUIRE(::dup2(::fileno(capture), STDERR_FILENO) >= 0);
    const bool isFirstAskOn = isRetainedGridCacheEnabled();
    const bool isSecondAskOn = isRetainedGridCacheEnabled();
    std::fflush(stderr);
    ::dup2(savedError, STDERR_FILENO);
    ::close(savedError);

    std::string written;
    std::rewind(capture);
    for (int character = std::fgetc(capture); character != EOF; character = std::fgetc(capture))
        written.push_back(static_cast<char>(character));
    std::fclose(capture);

    CHECK_MSG(isFirstAskOn && isSecondAskOn, "the cache stays on");
    const std::string warning = "UBIQUITTY_GRID_CACHE=false is not one of 0, 1, off or on";
    const std::size_t at = written.find(warning);
    CHECK_MSG(at != std::string::npos, "the reader is told the value was not understood");
    CHECK_MSG(at == std::string::npos || written.find(warning, at + 1) == std::string::npos, "once");
}

UBIQUITTY_TEST_MAIN()
