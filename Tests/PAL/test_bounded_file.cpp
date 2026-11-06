// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"
#include "PAL/BoundedFile.h"
#include "PAL/PAL.h"
#include "PAL/UnixFileDescriptor.h"
#include "ScopedNothrowAllocationRefusal.h"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace Ubiquitty::PAL;

namespace {

// Runs at the allocation boundary, after the production fstat and before its
// read, to make concurrent file mutation deterministic without a product hook.
struct FileMutation {
    const char* path = nullptr;
    const char* replacement = nullptr;
    off_t length = 0;
    bool didMutate = false;

    static FileMutation& current()
    {
        static FileMutation mutation;
        return mutation;
    }

    void run()
    {
        if (!path)
            return;
        const char* target = std::exchange(path, nullptr);
        if (replacement) {
            didMutate = ::rename(replacement, target) == 0;
        } else {
            UnixFileDescriptor descriptor(::open(target, O_WRONLY));
            didMutate = descriptor && ::ftruncate(descriptor.get(), length) == 0;
        }
    }
};

constexpr std::array<std::uint8_t, 4> content { 1, 2, 3, 4 };

} // namespace

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    FileMutation::current().run();
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(boundedFileAdmitsExactSizeAndReturnsWholeContent)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(std::span(content))));
    const auto read = storageReadBounded(path.c_str(), content.size());
    REQUIRE(read);
    CHECK(std::ranges::equal(read->view(), content));
    REQUIRE(storageWriteAtomic(path.c_str(), {}));
    const auto empty = storageReadBounded(path.c_str(), 0);
    REQUIRE(empty);
    CHECK(empty->view().empty());
}

TEST(boundedFileRejectsSizeBeforeAllocationAndHandlesAllocationRefusal)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(std::span(content))));
    UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    const auto oversized = storageReadBounded(path.c_str(), content.size() - 1);
    REQUIRE(!oversized);
    CHECK(oversized.error() == FileReadError::TooLarge);
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    const auto unavailable = storageReadBounded(path.c_str(), content.size());
    REQUIRE(!unavailable);
    CHECK(unavailable.error() == FileReadError::AllocationFailed);
}

TEST(boundedFileRejectsSymlinksDirectoriesAndPipesWithoutWaiting)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    const auto link = directory.path() + "/link";
    const auto pipe = directory.path() + "/pipe";
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(std::span(content))));
    REQUIRE(::symlink(path.c_str(), link.c_str()) == 0);
    REQUIRE(::mkfifo(pipe.c_str(), 0600) == 0);
    CHECK(!storageReadBounded(link.c_str(), 100));
    CHECK(!storageReadBounded(directory.path().c_str(), 100));
    CHECK(!storageReadBounded(pipe.c_str(), 100));
}

TEST(boundedFileRejectsGrowthAndShrinkAfterSizeAdmission)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    for (const off_t length : { 3, 5 }) {
        REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(std::span(content))));
        FileMutation::current() = { .path = path.c_str(), .replacement = nullptr, .length = length, .didMutate = false };
        const auto read = storageReadBounded(path.c_str(), content.size());
        CHECK(FileMutation::current().didMutate);
        REQUIRE(!read);
        CHECK(read.error() == FileReadError::Changed);
    }
    FileMutation::current() = {};
}

TEST(boundedFileReadsThePinnedOwnerAcrossAtomicPathReplacement)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/record";
    const auto replacement = directory.path() + "/replacement";
    REQUIRE(storageWriteAtomic(path.c_str(), std::as_bytes(std::span(content))));
    REQUIRE(storageWriteAtomic(replacement.c_str(), {}));
    FileMutation::current() = { .path = path.c_str(), .replacement = replacement.c_str(), .length = 0, .didMutate = false };
    const auto read = storageReadBounded(path.c_str(), content.size());
    CHECK(FileMutation::current().didMutate);
    REQUIRE(read);
    CHECK(std::ranges::equal(read->view(), content));
    FileMutation::current() = {};
    const auto changed = storageReadBounded(path.c_str(), 0);
    REQUIRE(changed);
    CHECK(changed->view().empty());
}

UBIQUITTY_TEST_MAIN()
