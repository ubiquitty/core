// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"
#include "PAL/BoundedFile.h"
#include "PAL/DurableFile.h"
#include "PAL/PAL.h"
#include "Platform/POSIX/DurableCommit.h"

#include <algorithm>
#include <array>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace Ubiquitty;

namespace {

constexpr std::array<std::byte, 5> content { std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 4 }, std::byte { 5 } };

struct CommitOperations {
    enum class Failure {
        None,
        Write,
        FileFlush,
        Close,
        Rename,
        DirectoryFlush
    };
    Failure failure { Failure::None };
    std::string events;
    std::size_t written { 0 };
    bool didReplace { false };

    std::size_t write(std::span<const std::byte> bytes)
    {
        events += 'w';
        if (failure == Failure::Write)
            return 0;
        const auto count = std::min<std::size_t>(2, bytes.size());
        CHECK(std::ranges::equal(bytes.first(count), std::span(content).subspan(written, count)));
        written += count;
        return count;
    }

    bool flushFile()
    {
        events += 'f';
        return failure != Failure::FileFlush;
    }
    bool closeFile()
    {
        events += 'c';
        return failure != Failure::Close;
    }
    bool replace()
    {
        events += 'r';
        didReplace = failure != Failure::Rename;
        return didReplace;
    }
    bool flushDirectory()
    {
        events += 'd';
        return failure != Failure::DirectoryFlush;
    }
};

bool hasTemporaryFile(const std::string& path)
{
    DIR* directory = ::opendir(path.c_str());
    if (!directory)
        return true;
    bool found = false;
    while (const auto* entry = ::readdir(directory))
        found |= std::string_view(entry->d_name).starts_with(".ubq-sealed-");
    ::closedir(directory);
    return found;
}

} // namespace

TEST(durableCommitChecksEveryBarrierAndDistinguishesUncertainReplacement)
{
    using Failure = CommitOperations::Failure;
    struct Case {
        Failure failure;
        const char* events;
        PAL::DurableFileResult result;
    };
    constexpr std::array cases {
        Case { Failure::Write, "w", PAL::DurableFileResult::NotCommitted },
        Case { Failure::FileFlush, "wwwf", PAL::DurableFileResult::NotCommitted },
        Case { Failure::Close, "wwwfc", PAL::DurableFileResult::NotCommitted },
        Case { Failure::Rename, "wwwfcr", PAL::DurableFileResult::NotCommitted },
        Case { Failure::DirectoryFlush, "wwwfcrd", PAL::DurableFileResult::DurabilityUnknown },
        Case { Failure::None, "wwwfcrd", PAL::DurableFileResult::Durable },
    };
    for (const auto& item : cases) {
        CommitOperations operations;
        operations.failure = item.failure;
        CHECK(Platform::commitDurableFile(operations, content) == item.result);
        CHECK(operations.events == item.events);
        CHECK(operations.didReplace == (item.result != PAL::DurableFileResult::NotCommitted));
    }
}

TEST(durableEmptyFileStillNeedsBothFlushesAndRename)
{
    CommitOperations operations;
    CHECK(Platform::commitDurableFile(operations, {}) == PAL::DurableFileResult::Durable);
    CHECK(operations.events == "fcrd");
}

TEST(durableReplacementPersistsExactBytesWithPrivatePermissions)
{
    Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    REQUIRE(PAL::storageWriteAtomic(path.c_str(), {}));
    REQUIRE(::chmod(path.c_str(), 0644) == 0);
    CHECK(PAL::storageWriteDurable(path.c_str(), content) == PAL::DurableFileResult::Durable);
    const auto result = PAL::storageReadBounded(path.c_str(), content.size());
    REQUIRE(result);
    CHECK(std::ranges::equal(std::as_bytes(result->view()), content));
    struct stat status {};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    CHECK(!hasTemporaryFile(directory.path()));
    CHECK(PAL::storageWriteDurable(path.c_str(), {}) == PAL::DurableFileResult::Durable);
    const auto empty = PAL::storageReadBounded(path.c_str(), 0);
    REQUIRE(empty);
    CHECK(empty->size == 0);
}

TEST(durableReplacementRefusesUntrustedOrMissingParentsWithoutCreatingThem)
{
    Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    const auto missing = directory.path() + "/missing/record";
    CHECK(PAL::storageWriteDurable(nullptr, content) == PAL::DurableFileResult::NotCommitted);
    CHECK(PAL::storageWriteDurable("relative", content) == PAL::DurableFileResult::NotCommitted);
    CHECK(PAL::storageWriteDurable(missing.c_str(), content) == PAL::DurableFileResult::NotCommitted);
    CHECK(::access((directory.path() + "/missing").c_str(), F_OK) != 0);
    REQUIRE(::chmod(directory.path().c_str(), 0777) == 0);
    CHECK(PAL::storageWriteDurable(path.c_str(), content) == PAL::DurableFileResult::NotCommitted);
    REQUIRE(::chmod(directory.path().c_str(), 0700) == 0);
    CHECK(::access(path.c_str(), F_OK) != 0);
    CHECK(!hasTemporaryFile(directory.path()));
}

TEST(durableReplacementDoesNotFollowFinalSymlinkAndCleansFailedRename)
{
    Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto target = directory.path() + "/target";
    const auto link = directory.path() + "/record";
    REQUIRE(PAL::storageWriteAtomic(target.c_str(), {}));
    REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);
    CHECK(PAL::storageWriteDurable(link.c_str(), content) == PAL::DurableFileResult::Durable);
    const auto unchanged = PAL::storageReadBounded(target.c_str(), 0);
    REQUIRE(unchanged);
    CHECK(unchanged->size == 0);
    const auto replaced = PAL::storageReadBounded(link.c_str(), content.size());
    REQUIRE(replaced);
    CHECK(std::ranges::equal(std::as_bytes(replaced->view()), content));
    const auto subdirectory = directory.path() + "/subdirectory";
    REQUIRE(::mkdir(subdirectory.c_str(), 0700) == 0);
    CHECK(PAL::storageWriteDurable(subdirectory.c_str(), content) == PAL::DurableFileResult::NotCommitted);
    CHECK(!hasTemporaryFile(directory.path()));
    const auto parentLink = directory.path() + "/parent-link";
    REQUIRE(::symlink(subdirectory.c_str(), parentLink.c_str()) == 0);
    CHECK(PAL::storageWriteDurable((parentLink + "/record").c_str(), content) == PAL::DurableFileResult::NotCommitted);
}

UBIQUITTY_TEST_MAIN()
