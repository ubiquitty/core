// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Platform/test_atomic_storage_perms.cpp
//
// Security negatives for storageWriteAtomic, storageEnsureDirectory,
// and storageWriteTempFile:
//
//  1. MODE:          after a successful write the target file must be mode 0600
//                    (owner-only), not 0644.
//
//  2. SYMLINK:       a symlink planted at the predictable `<target>.tmp` path
//                    must not be followed; the sentinel file it points to must
//                    remain untouched.
//
//  3. DIR MODE:      storageEnsureDirectory must create dirs at 0700, not 0755.
//
//  4. TEMP SYMLINK:  a symlink planted at the predictable storageWriteTempFile
//                    temp path must not be followed; sentinel unchanged.
//
// Each assertion is accompanied by a verify-by-revert comment that
// describes which source line to restore and what failure the test then
// produces. Both are exercised in the build/run output in the task report.
//
// A fixture that cannot be built fails its case. A /tmp that cannot hold a
// directory, a file or a symlink is a broken environment, and a check that
// declined there would turn the very protection it tests green.

#include "PAL/PAL.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- helpers -------------------------------------------------------

// Write a file with a known mode so we can detect if the sentinel was
// touched (created, truncated, or mode-changed) by the atomic write.
static bool writeSentinel(const char* path, const char* content)
{
    const int fileDescriptor = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fileDescriptor < 0)
        return false;
    if (content && content[0]) {
        const ssize_t n = ::write(fileDescriptor, content, std::strlen(content));
        (void)n;
    }
    ::close(fileDescriptor);
    return true;
}

static bool sentinelUnchanged(const char* path, const char* expectedContent)
{
    char buffer[64] = {};
    const int fileDescriptor = ::open(path, O_RDONLY);
    if (fileDescriptor < 0)
        return false;
    const ssize_t n = ::read(fileDescriptor, buffer, sizeof(buffer) - 1);
    ::close(fileDescriptor);
    if (n < 0)
        return false;
    buffer[n] = '\0';
    return std::strcmp(buffer, expectedContent) == 0;
}

// ---- test 1: target file must land at mode 0600 --------------------
//
// verify-by-revert: change the open mode in SystemPAL.cpp to 0644 and
// remove the fchmod block. This test then fails with:
//
//   FAIL test_atomic_storage_perms.cpp:NN: (mode & 0777) == 0600
//   (mode & 0777) = 0644
//
// The umask(0022) pin is essential: with umask 0022, open(..., 0644) produces
// 0644 (unaffected), so the test catches the bug. Without the pin a permissive
// umask could accidentally mask 0644 → 0600 and hide the defect.
//
TEST(testModeIs0600)
{
    // Pin umask so open(..., 0644) without fchmod yields 0644, not 0600.
    // Restore afterward to avoid perturbing other tests.
    const mode_t oldUmask = ::umask(0022);

    // Use a temp directory so we don't pollute the real config dir.
    char dirTemplate[] = "/tmp/ubq-sec-942-XXXXXX";
    const char* dir = ::mkdtemp(dirTemplate);
    if (!dir) {
        CHECK_MSG(false, (std::string("testModeIs0600 fixture: mkdtemp failed: ") + ::strerror(errno)).c_str());
        ::umask(oldUmask);
        return;
    }

    char target[256];
    std::snprintf(target, sizeof(target), "%s/config.toml", dir);

    const char payload[] = "token = \"secret\"\n";
    const bool ok = Ubiquitty::PAL::storageWriteAtomic(target, std::as_bytes(std::span { payload, std::strlen(payload) }));
    CHECK(ok == true);

    struct stat st;
    CHECK(::stat(target, &st) == 0);
    const mode_t mode = st.st_mode & 0777;
    if (mode != 0600)
        std::printf("  (mode & 0777) = 0%o\n", mode);
    CHECK(mode == 0600);

    // Verify content matches payload.
    {
        char readback[64] = {};
        const int fileDescriptor = ::open(target, O_RDONLY);
        CHECK(fileDescriptor >= 0);
        if (fileDescriptor >= 0) {
            const ssize_t n = ::read(fileDescriptor, readback, sizeof(readback) - 1);
            ::close(fileDescriptor);
            CHECK(n == static_cast<ssize_t>(std::strlen(payload)));
            CHECK(std::strcmp(readback, payload) == 0);
        }
    }

    ::unlink(target);
    ::rmdir(dir);
    ::umask(oldUmask);
}

// ---- test 2: symlink at the known temp path must be refused -----------
//
// The test seam (UBIQUITTY_TEST_ATOMIC_SUFFIX env var, compiled in under
// UBIQUITTY_ENABLE_TEST_HOOKS) forces storageWriteAtomic to use a known
// temp suffix. The test plants a symlink at that exact path pointing to a
// sentinel outside the target dir. The call must return FALSE and the
// sentinel must be byte-for-byte unchanged — O_EXCL|O_NOFOLLOW must refuse
// the pre-placed symlink.
//
// Verify-by-revert: remove O_EXCL|O_NOFOLLOW from the open() call in
// storageWriteAtomic in SystemPAL.cpp. This test then fails with:
//
//   FAIL test_atomic_storage_perms.cpp:NN: !ok
//   FAIL test_atomic_storage_perms.cpp:NN: sentinelUnchanged(sentinel, sentinelContent)
//
// because the unguarded open follows the symlink, overwrites the sentinel,
// and returns true.
//
TEST(testSymlinkResistance)
{
    char dirTemplate[] = "/tmp/ubq-sec-942-sym-XXXXXX";
    const char* dir = ::mkdtemp(dirTemplate);
    if (!dir) {
        CHECK_MSG(false, (std::string("testSymlinkResistance fixture: mkdtemp failed: ") + ::strerror(errno)).c_str());
        return;
    }

    // Sentinel lives outside the target directory.
    char sentinel[256];
    std::snprintf(sentinel, sizeof(sentinel), "%s/sentinel.dat", dir);
    const char sentinelContent[] = "UNTOUCHED\n";
    if (!writeSentinel(sentinel, sentinelContent)) {
        CHECK_MSG(false, "testSymlinkResistance fixture: cannot write sentinel");
        ::rmdir(dir);
        return;
    }

    // target directory — separate from sentinel so a rogue write can't
    // masquerade as a "same-dir" file.
    char targetDir[256];
    std::snprintf(targetDir, sizeof(targetDir), "%s/cfg", dir);
    if (::mkdir(targetDir, 0755) != 0) {
        CHECK_MSG(false, "testSymlinkResistance fixture: cannot mkdir cfg");
        ::unlink(sentinel);
        ::rmdir(dir);
        return;
    }

    // Sized against the longest name written below rather than the path alone,
    // and the result is checked: a silently truncated path would plant the
    // symlink somewhere other than where the seam writes, and the case would
    // pass by testing nothing.
    char target[PATH_MAX];
    if (std::snprintf(target, sizeof(target), "%s/config.toml", targetDir) >= static_cast<int>(sizeof(target))) {
        CHECK_MSG(false, "testSymlinkResistance fixture: target path does not fit");
        ::unlink(sentinel);
        ::rmdir(dir);
        return;
    }

    // Known suffix injected via the test seam. The temp path will be
    // exactly: <target> + knownSuffix.
    static const char knownSuffix[] = ".test_tmp_known";

    // Plant a symlink at the exact temp path the seam will produce.
    char knownTemporary[PATH_MAX + sizeof(knownSuffix)];
    if (std::snprintf(knownTemporary, sizeof(knownTemporary), "%s%s", target, knownSuffix) >= static_cast<int>(sizeof(knownTemporary))) {
        CHECK_MSG(false, "testSymlinkResistance fixture: temporary path does not fit");
        ::unlink(sentinel);
        ::rmdir(dir);
        return;
    }
    if (::symlink(sentinel, knownTemporary) != 0) {
        CHECK_MSG(false, (std::string("testSymlinkResistance fixture: cannot create symlink: ") + ::strerror(errno)).c_str());
        ::unlink(sentinel);
        ::rmdir(targetDir);
        ::rmdir(dir);
        return;
    }

    // Arm the seam so storageWriteAtomic uses knownSuffix as the temp name.
    ::setenv("UBIQUITTY_TEST_ATOMIC_SUFFIX", knownSuffix, 1);

    const char payload[] = "token = \"secret\"\n";
    // O_EXCL|O_NOFOLLOW must refuse the pre-planted symlink and return false.
    const bool ok = Ubiquitty::PAL::storageWriteAtomic(target, std::as_bytes(std::span { payload, std::strlen(payload) }));
    CHECK(!ok);

    // The sentinel must be byte-for-byte unchanged.
    CHECK(sentinelUnchanged(sentinel, sentinelContent));

    // Disarm the seam before cleanup.
    ::unsetenv("UBIQUITTY_TEST_ATOMIC_SUFFIX");

    // Clean up (best-effort).
    ::unlink(target);
    ::unlink(knownTemporary); // the planted symlink
    ::unlink(sentinel);
    ::rmdir(targetDir);
    ::rmdir(dir);
}

// ---- test 3: storageEnsureDirectory must create dirs at mode 0700 ----
//
// verify-by-revert: change 0700 → 0755 in SystemPAL.cpp
// storageEnsureDirectory. This test then fails with:
//
//   FAIL test_atomic_storage_perms.cpp:NN: (dirMode & 0777) == 0700
//   (dirMode & 0777) = 0755
//
// Pin umask=0 so no bits are masked away — a reverted 0755 produces 0755 and
// the test catches it. With umask=022 a 0755 open would still yield 0755, so
// the pin is belt-and-suspenders here, but mirrors the established pattern.
//
TEST(testEnsureDirectoryMode)
{
    const mode_t oldUmask = ::umask(0);

    char dirTemplate[] = "/tmp/ubq-sec-991-XXXXXX";
    const char* base = ::mkdtemp(dirTemplate);
    if (!base) {
        CHECK_MSG(false, (std::string("testEnsureDirectoryMode fixture: mkdtemp failed: ") + ::strerror(errno)).c_str());
        ::umask(oldUmask);
        return;
    }

    char target[256];
    std::snprintf(target, sizeof(target), "%s/cfg/sub", base);

    const bool ok = Ubiquitty::PAL::storageEnsureDirectory(target);
    CHECK(ok == true);

    // Check mode of the leaf directory (the one named "sub").
    struct stat st;
    CHECK(::stat(target, &st) == 0);
    const mode_t dirMode = st.st_mode & 0777;
    if (dirMode != 0700)
        std::printf("  (dirMode & 0777) = 0%o\n", dirMode);
    CHECK(dirMode == 0700);

    // Check the intermediate parent "cfg" too.
    char parent[256];
    std::snprintf(parent, sizeof(parent), "%s/cfg", base);
    CHECK(::stat(parent, &st) == 0);
    const mode_t parentMode = st.st_mode & 0777;
    if (parentMode != 0700)
        std::printf("  (parentMode & 0777) = 0%o\n", parentMode);
    CHECK(parentMode == 0700);

    // Idempotent: calling again on an existing dir must return true.
    CHECK(Ubiquitty::PAL::storageEnsureDirectory(target) == true);

    // Clean up (best-effort).
    ::rmdir(target);
    ::rmdir(parent);
    ::rmdir(base);
    ::umask(oldUmask);
}

TEST(testEnsureDirectoryRejectsExistingFile)
{
    char dirTemplate[] = "/tmp/ubq-directory-kind-XXXXXX";
    const char* base = ::mkdtemp(dirTemplate);
    REQUIRE(base != nullptr);
    char target[256];
    std::snprintf(target, sizeof(target), "%s/config", base);
    REQUIRE(writeSentinel(target, "unchanged"));
    CHECK(!Ubiquitty::PAL::storageEnsureDirectory(target));
    CHECK(sentinelUnchanged(target, "unchanged"));
    ::unlink(target);
    ::rmdir(base);
}

// ---- test 4: storageWriteTempFile must resist planted symlinks -------
//
// The test seam (UBIQUITTY_TEST_TEMP_SUFFIX env var, compiled in under
// UBIQUITTY_ENABLE_TEST_HOOKS) forces storageWriteTempFile to use a known
// temp name. The test plants a symlink at that path pointing to a sentinel.
// The call must return FALSE and the sentinel must be unchanged.
//
// Verify-by-revert: remove O_EXCL|O_NOFOLLOW from the open() call in
// storageWriteTempFile in SystemPAL.cpp. This test then fails with:
//
//   FAIL test_atomic_storage_perms.cpp:NN: !ok
//   FAIL test_atomic_storage_perms.cpp:NN: sentinelUnchanged(sentinel, sentinelContent)
//
// because the unguarded open follows the symlink, overwrites the sentinel,
// and returns true.
//
TEST(testTempFileSymlinkResistance)
{
    char dirTemplate[] = "/tmp/ubq-sec-958-XXXXXX";
    const char* dir = ::mkdtemp(dirTemplate);
    if (!dir) {
        CHECK_MSG(false, (std::string("testTempFileSymlinkResistance fixture: mkdtemp failed: ") + ::strerror(errno)).c_str());
        return;
    }

    // Sentinel lives in the same base dir but outside the write target.
    char sentinel[256];
    std::snprintf(sentinel, sizeof(sentinel), "%s/sentinel.dat", dir);
    const char sentinelContent[] = "UNTOUCHED_TEMP\n";
    if (!writeSentinel(sentinel, sentinelContent)) {
        CHECK_MSG(false, "testTempFileSymlinkResistance fixture: cannot write sentinel");
        ::rmdir(dir);
        return;
    }

    // Known hex suffix injected via the test seam.
    // storageWriteTempFile builds: <dir>/ubiquitty.<testSuffix><wantSuffix>
    // so the full temp path will be: <dir>/ubiquitty.known_hex.terminfo.src
    static const char knownHex[] = "known_hex";
    static const char wantSuffix[] = ".terminfo.src";

    char knownTemporary[512];
    std::snprintf(knownTemporary, sizeof(knownTemporary), "%s/ubiquitty.%s%s", dir, knownHex, wantSuffix);

    if (::symlink(sentinel, knownTemporary) != 0) {
        CHECK_MSG(false, (std::string("testTempFileSymlinkResistance fixture: cannot create symlink: ") + ::strerror(errno)).c_str());
        ::unlink(sentinel);
        ::rmdir(dir);
        return;
    }

    // Arm the seam so storageWriteTempFile uses knownHex as the random part.
    ::setenv("UBIQUITTY_TEST_TEMP_SUFFIX", knownHex, 1);

    char pathOut[512] = {};
    const char payload[] = "ubiquitty-terminfo-src\n";
    const bool ok = Ubiquitty::PAL::storageWriteTempFile(dir, wantSuffix, std::as_bytes(std::span { payload, std::strlen(payload) }), pathOut);
    CHECK(!ok);

    // The sentinel must be byte-for-byte unchanged.
    CHECK(sentinelUnchanged(sentinel, sentinelContent));

    ::unsetenv("UBIQUITTY_TEST_TEMP_SUFFIX");

    // Clean up (best-effort).
    ::unlink(knownTemporary); // the planted symlink
    ::unlink(sentinel);
    ::rmdir(dir);
}

// --------------------------------------------------------------------

UBIQUITTY_TEST_MAIN()
