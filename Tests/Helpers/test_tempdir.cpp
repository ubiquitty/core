// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Helpers/TempDir.h"

#include <sys/stat.h>
#include <unistd.h>

TEST(testTempDirCreatesValidDirectory)
{
    Ubiquitty::Test::TempDir dir;
    CHECK_MSG(dir.valid(), "TempDir creates a valid directory");
    CHECK_MSG(!dir.path().empty(), "path is non-empty");
    struct stat st {};
    CHECK_MSG(::stat(dir.path().c_str(), &st) == 0, "directory exists on filesystem");
    CHECK_MSG(S_ISDIR(st.st_mode), "path is a directory");
}

TEST(testTempDirCleansUpOnDestruction)
{
    std::string saved;
    {
        Ubiquitty::Test::TempDir dir;
        CHECK_MSG(dir.valid(), "valid in scope");
        saved = dir.path();
        struct stat st {};
        CHECK_MSG(::stat(saved.c_str(), &st) == 0, "exists while alive");
    }
    struct stat st {};
    CHECK_MSG(::stat(saved.c_str(), &st) != 0, "removed after destruction");
}

TEST(testTempDirMoveTransfersOwnership)
{
    std::string first;
    std::string second;
    {
        Ubiquitty::Test::TempDir a;
        first = a.path();
        Ubiquitty::Test::TempDir b = std::move(a);
        CHECK_MSG(!a.valid(), "moved-from is invalid");
        CHECK_MSG(b.valid(), "moved-to is valid");
        CHECK_MSG(b.path() == first, "path transferred");
        second = b.path();
    }
    struct stat st {};
    CHECK_MSG(::stat(second.c_str(), &st) != 0, "moved directory cleaned up once");
    CHECK_MSG(::stat(first.c_str(), &st) != 0, "original path not double-removed");
}

UBIQUITTY_TEST_MAIN()
