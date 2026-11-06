// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Platform/test_canonical_path_for_descriptor.cpp
//
// PAL::fileCanonicalPathForDescriptor answers "what file is this descriptor
// actually reading", for a gate that must name the object rather than the name
// it was handed. A path can be made to point elsewhere between resolving it and
// opening it; a descriptor cannot, so re-asking about the descriptor is the
// race-free question.
//
// The case worth the file is the one where the opened name stops naming the
// file, and the two platforms fail it by different amounts. Darwin's F_GETPATH
// reports the last known path with no marker, so it resolves cleanly to whatever
// holds that name now — a different file — and only comparing device and inode
// refuses it. Linux's /proc/self/fd marks the dentry with " (deleted)" as soon
// as the opened name is unlinked, so the stale path stops resolving and the
// refusal lands a step earlier.
//
// That asymmetry was measured, not assumed, and the mutation record below says
// which case each leg is carried by. Dropping the identity check leaves every
// case here green on Linux and fails one on Darwin.

#include "PAL/PAL.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string scratchDirectory()
{
    const char* base = ::getenv("TMPDIR");
    std::string directory = (base && base[0]) ? base : "/tmp";
    if (directory.back() == '/')
        directory.pop_back();
    directory += "/ubq-canonpath-XXXXXX";
    std::string mutableCopy = directory;
    if (!::mkdtemp(mutableCopy.data()))
        return std::string();
    return mutableCopy;
}

int openScratchFile(const std::string& path)
{
    const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor >= 0)
        CHECK_EQ(::write(descriptor, "x", 1), 1);
    return descriptor;
}

}

TEST(anOpenFileResolvesToItsOwnPath)
{
    const std::string directory = scratchDirectory();
    CHECK(!directory.empty());
    if (directory.empty())
        return;
    const std::string path = directory + "/plain";
    const int descriptor = openScratchFile(path);
    CHECK(descriptor >= 0);
    if (descriptor < 0)
        return;

    char resolved[PATH_MAX] = { 0 };
    const int length = Ubiquitty::PAL::fileCanonicalPathForDescriptor(descriptor, resolved, sizeof(resolved));
    CHECK(length > 0);
    CHECK(length == static_cast<int>(std::strlen(resolved)));
    // Compared by device and inode rather than by string: the scratch directory
    // may itself sit behind a symlink (/tmp is one on macOS), so the canonical
    // path is allowed to differ from the path we opened while naming that file.
    struct ::stat resolvedStatus {};
    struct ::stat openedStatus {};
    CHECK(::stat(resolved, &resolvedStatus) == 0);
    CHECK(::fstat(descriptor, &openedStatus) == 0);
    CHECK(resolvedStatus.st_dev == openedStatus.st_dev);
    CHECK(resolvedStatus.st_ino == openedStatus.st_ino);

    ::close(descriptor);
    ::unlink(path.c_str());
    ::rmdir(directory.c_str());
}

TEST(anUnlinkedFileHasNoCanonicalPath)
{
    const std::string directory = scratchDirectory();
    CHECK(!directory.empty());
    if (directory.empty())
        return;
    const std::string path = directory + "/vanishing";
    const int descriptor = openScratchFile(path);
    CHECK(descriptor >= 0);
    if (descriptor < 0)
        return;

    // The descriptor stays open and readable; only the name goes away. This is
    // the case both platforms answer misleadingly on their own.
    CHECK(::unlink(path.c_str()) == 0);

    char resolved[PATH_MAX] = { 0 };
    const int length = Ubiquitty::PAL::fileCanonicalPathForDescriptor(descriptor, resolved, sizeof(resolved));
    CHECK(length < 0);

    ::close(descriptor);
    ::rmdir(directory.c_str());
}

TEST(aPathReusedByAnotherFileIsNotThisDescriptorsPath)
{
    const std::string directory = scratchDirectory();
    CHECK(!directory.empty());
    if (directory.empty())
        return;
    const std::string path = directory + "/reused";
    const int descriptor = openScratchFile(path);
    CHECK(descriptor >= 0);
    if (descriptor < 0)
        return;

    // The name now belongs to a different file. A gate keying on the string a
    // caller supplied would adjudicate this new file and then read the old one;
    // asking the descriptor refuses instead.
    CHECK(::unlink(path.c_str()) == 0);
    const int replacement = openScratchFile(path);
    CHECK(replacement >= 0);

    char resolved[PATH_MAX] = { 0 };
    const int length = Ubiquitty::PAL::fileCanonicalPathForDescriptor(descriptor, resolved, sizeof(resolved));
    CHECK(length < 0);

    // The replacement is a live file at that same name, so the refusal above is
    // about identity and not about the path being unresolvable.
    char replacementPath[PATH_MAX] = { 0 };
    CHECK(Ubiquitty::PAL::fileCanonicalPathForDescriptor(replacement, replacementPath, sizeof(replacementPath)) > 0);

    ::close(descriptor);
    ::close(replacement);
    ::unlink(path.c_str());
    ::rmdir(directory.c_str());
}

TEST(aLiveInodeWhoseNameNowHoldsAnotherFileIsRefused)
{
    const std::string directory = scratchDirectory();
    CHECK(!directory.empty());
    if (directory.empty())
        return;

    // Both platforms refuse here and they refuse for different reasons, which is
    // the point of writing it down: the same green hides two mechanisms.
    //
    // Darwin's F_GETPATH gives the last known path with no marker, so the stale
    // path resolves cleanly to whatever holds that name now — a different file —
    // and the device/inode comparison is what refuses it.
    //
    // Linux does not reach that comparison. A surviving hard link might be
    // expected to keep the inode alive and stop the " (deleted)" marker
    // appearing. Measured, it does not — /proc/self/fd marks
    // the DENTRY, and unlinking the opened name marks it whether or not the
    // inode has other names:
    //
    //     proc reports: "/tmp/dentry-first (deleted)"
    //     stat(resolved) = -1
    //
    // So on Linux the refusal comes from the path no longer resolving, and
    // dropping the identity check leaves this case green. It runs on both legs
    // anyway, because the primitive answers one question rather than a different
    // one per platform.
    const std::string primary = directory + "/first-name";
    const std::string secondary = directory + "/second-name";
    const int descriptor = openScratchFile(primary);
    CHECK(descriptor >= 0);
    if (descriptor < 0)
        return;
    CHECK(::link(primary.c_str(), secondary.c_str()) == 0);
    CHECK(::unlink(primary.c_str()) == 0);
    const int impostor = openScratchFile(primary);
    CHECK(impostor >= 0);

    char resolved[PATH_MAX] = { 0 };
    const int length = Ubiquitty::PAL::fileCanonicalPathForDescriptor(descriptor, resolved, sizeof(resolved));
    CHECK(length < 0);

    ::close(descriptor);
    ::close(impostor);
    ::unlink(primary.c_str());
    ::unlink(secondary.c_str());
    ::rmdir(directory.c_str());
}

TEST(aBufferTooSmallIsRefusedRatherThanTruncated)
{
    const std::string directory = scratchDirectory();
    CHECK(!directory.empty());
    if (directory.empty())
        return;
    const std::string path = directory + "/short-buffer";
    const int descriptor = openScratchFile(path);
    CHECK(descriptor >= 0);
    if (descriptor < 0)
        return;

    // A truncated path names a different file, and usually a directory — which
    // is exactly the object a containment check would then approve.
    char tiny[4] = { 'k', 'e', 'e', 'p' };
    CHECK(Ubiquitty::PAL::fileCanonicalPathForDescriptor(descriptor, tiny, static_cast<int>(sizeof(tiny))) < 0);
    // Refused means untouched: a caller that ignored the return must not find a
    // plausible-looking path sitting in its buffer.
    CHECK(tiny[0] == 'k' && tiny[3] == 'p');

    ::close(descriptor);
    ::unlink(path.c_str());
    ::rmdir(directory.c_str());
}

TEST(aClosedOrNegativeDescriptorIsRefused)
{
    char resolved[PATH_MAX] = { 0 };
    CHECK(Ubiquitty::PAL::fileCanonicalPathForDescriptor(-1, resolved, sizeof(resolved)) < 0);
    // A descriptor number nothing has opened, rather than one that was.
    CHECK(Ubiquitty::PAL::fileCanonicalPathForDescriptor(4096, resolved, sizeof(resolved)) < 0);
}

UBIQUITTY_TEST_MAIN()
