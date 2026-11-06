// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/POSIX/GatedFileRead.h"

#include "Core/Terminal.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty::POSIX {

namespace {

/// The marker a kitty t=t file carries in its path, which the protocol requires
/// before a terminal deletes it.
constexpr std::string_view temporaryTransportMarker = "tty-graphics-protocol";

/// An absolute name with `.` and empty components dropped, or empty when it is
/// relative or holds a `..`.
///
/// Refusing `..` rather than resolving it keeps this lexical answer from
/// disagreeing with the filesystem's: `/tmp/link/..` names wherever the link's
/// parent is, which only a lookup can say.
std::string lexicallyNormalAbsolutePath(std::string_view path)
{
    if (path.empty() || path.front() != '/')
        return {};
    std::string normal;
    for (std::string_view rest = path.substr(1);;) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part == "..")
            return {};
        if (!part.empty() && part != ".") {
            normal += '/';
            normal += part;
        }
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return normal.empty() ? std::string("/") : normal;
}

/// Whether `path` is a proper descendant of `root`.
///
/// The '/' test is what stops "/tmpfoo" passing as a child of "/tmp", and the
/// size test is what stops `root` itself passing as its own child.
bool isProperDescendant(std::string_view path, std::string_view root)
{
    return path.starts_with(root) && path.size() > root.size() && path[root.size()] == '/';
}

/// Both answers about the temporary directories, resolved once.
///
/// They do not change during a session, so each containment test is a prefix
/// comparison rather than a getenv + realpath sweep. Only the derivation is
/// cached: the boundary test, the O_NOFOLLOW open and the identity check stay
/// per operation.
struct TemporaryDirectories {
    /// Each directory's real path, which is what a descriptor's canonical path
    /// is compared against.
    std::vector<std::string> canonical;
    /// Each directory as written and as resolved, which is what a name is
    /// compared against before it is opened. A client writes `$TMPDIR` as the
    /// environment spells it, and on macOS that is under /var, a symlink to
    /// /private/var.
    std::vector<std::string> spellings;
};

const TemporaryDirectories& temporaryDirectories()
{
    static const TemporaryDirectories directories = [] {
#if defined(__APPLE__)
        // The per-user temp directory Darwin gives each session, which is where
        // $TMPDIR normally points and is not /tmp. Declared inside the guard
        // because it backs `platformTemporary`, and outside it there is nothing
        // to hold — the other platforms have no third candidate.
        std::array<char, PATH_MAX> confBuffer {};
        const char* platformTemporary = nullptr;
        if (::confstr(_CS_DARWIN_USER_TEMP_DIR, confBuffer.data(), confBuffer.size()) > 0 && confBuffer[0])
            platformTemporary = confBuffer.data();
#else
        const char* const platformTemporary = nullptr;
#endif
        const std::array<const char*, 3> candidates { ::getenv("TMPDIR"), platformTemporary, "/tmp" };
        TemporaryDirectories found;
        for (const char* directory : candidates) {
            if (!directory || !directory[0])
                continue;
            std::array<char, PATH_MAX> realDirectory {};
            if (!::realpath(directory, realDirectory.data()) || realDirectory[0] == '\0')
                continue;
            std::string real = lexicallyNormalAbsolutePath(realDirectory.data());
            // A temporary directory of "/" would make every path a descendant.
            if (real.empty() || real == "/")
                continue;
            found.canonical.push_back(real);
            found.spellings.push_back(std::move(real));
            std::string written = lexicallyNormalAbsolutePath(directory);
            if (!written.empty() && written != "/")
                found.spellings.push_back(std::move(written));
        }
        return found;
    }();
    return directories;
}

}

namespace {

// The POSIX shape for a shared-memory name: one leading slash, no other, and
// short enough for every implementation that defines a limit. Checked before the
// name reaches the system, so a hostile one is refused here rather than
// interpreted somewhere else.
bool isValidSharedMemoryName(const char* name)
{
    if (!name)
        return false;
    const std::size_t length = std::strlen(name);
    return length >= 2 && length <= 31 && name[0] == '/' && std::strchr(name + 1, '/') == nullptr;
}

}

int readGatedSharedMemory(Ubiquitty::Terminal& terminal, const char* name,
    std::span<unsigned char> outputBuffer, SharedMemoryDisposition disposition)
{
    if (!isValidSharedMemoryName(name) || outputBuffer.empty())
        return -1;
    // The name, because an shm object has no path for the descriptor to be
    // re-gated against. Weaker than the file transport's descriptor gate, and
    // the header says so. effectGranted, so an absent policy refuses.
    if (!Ubiquitty::effectGranted(terminal, Ubiquitty::Capability::FileReadAccess, name))
        return -1;

    const int descriptor = ::shm_open(name, O_RDONLY, 0);
    if (descriptor < 0)
        return -1;
    // shm_open takes no O_CLOEXEC on every platform here, so the flag is set
    // after the fact rather than assumed.
    (void)::fcntl(descriptor, F_SETFD, FD_CLOEXEC);

    int result = -1;
    struct ::stat status {};
    if (::fstat(descriptor, &status) == 0 && status.st_size >= 0) {
        const std::size_t objectSize = static_cast<std::size_t>(status.st_size);
        // The caller's buffer bounds this, never st_size: the writer can grow
        // the object after it is measured.
        const std::size_t copyLength = std::min(objectSize, outputBuffer.size());
        if (copyLength == 0) {
            result = 0; // created and never sized: a valid empty read
        } else {
#if defined(__APPLE__)
            // Mapped because read(2) fails on an shm descriptor here, and safe
            // to map because ftruncate is one-shot: the size observed above
            // cannot be reduced, so every page in range stays backed.
            void* mapping = ::mmap(nullptr, copyLength, PROT_READ, MAP_SHARED, descriptor, 0);
            if (mapping != MAP_FAILED) {
                std::memcpy(outputBuffer.data(), mapping, copyLength);
                ::munmap(mapping, copyLength);
                result = static_cast<int>(copyLength);
            }
#else
            // Read rather than mapped, because here the object can shrink after
            // the size above was read — measured: a second ftruncate succeeds.
            // A mapped copy would then touch a page that is no longer backed and
            // take SIGBUS, which the sender of the escape sequence chooses. A
            // short read is a truncated image instead of a dead terminal.
            std::size_t total = 0;
            bool didSucceed = true;
            while (total < copyLength) {
                const ssize_t amount = ::read(descriptor, outputBuffer.data() + total, copyLength - total);
                if (amount > 0) {
                    total += static_cast<std::size_t>(amount);
                } else if (amount == 0) {
                    break; // shrunk under us, or shorter than it claimed
                } else if (errno == EINTR) {
                    continue;
                } else {
                    didSucceed = false;
                    break;
                }
            }
            if (didSucceed)
                result = static_cast<int>(total);
#endif
        }
    }
    ::close(descriptor);
    if (result >= 0 && disposition == SharedMemoryDisposition::UnlinkAfterReading)
        (void)::shm_unlink(name);
    return result;
}

bool pathInTemporaryDirectory(std::string_view canonicalPath)
{
    for (const std::string& prefix : temporaryDirectories().canonical) {
        if (isProperDescendant(canonicalPath, prefix))
            return true;
    }
    return false;
}

bool pathSpelledInTemporaryDirectory(std::string_view path)
{
    const std::string normal = lexicallyNormalAbsolutePath(path);
    if (normal.empty())
        return false;
    for (const std::string& prefix : temporaryDirectories().spellings) {
        if (isProperDescendant(normal, prefix))
            return true;
    }
    return false;
}

bool pathNamesTemporaryTransportFile(std::string_view canonicalPath)
{
    return pathInTemporaryDirectory(canonicalPath) && canonicalPath.find(temporaryTransportMarker) != std::string_view::npos;
}

bool buildFileIdentity(const char* canonicalPath, dev_t device, ino_t inode, GatedFileIdentity& out)
{
    if (!canonicalPath || canonicalPath[0] == '\0')
        return false;
    const std::size_t length = std::strlen(canonicalPath);
    if (length >= PATH_MAX)
        return false;
    const std::string path(canonicalPath, length);
    const std::size_t slash = path.find_last_of('/');
    // No parent, directly under the root, or a trailing slash. The root case is
    // refused rather than given a parent of "/": nothing this deletes lives
    // there, and admitting it would widen what an unlink can reach for no
    // transport that exists.
    if (slash == std::string::npos || slash == 0 || slash + 1 >= path.size())
        return false;
    out.canonicalPath = path;
    out.parentDirectoryPath = path.substr(0, slash);
    out.baseName = path.substr(slash + 1);
    out.device = device;
    out.inode = inode;
    return true;
}

void verifyAndUnlinkAt(int directoryDescriptor, const char* baseName, dev_t device, ino_t inode)
{
    if (directoryDescriptor < 0 || !baseName || baseName[0] == '\0')
        return;
    struct ::stat verify {};
    // AT_SYMLINK_NOFOLLOW so a symlink planted at the name is inspected rather
    // than followed; it then fails S_ISREG and nothing is deleted.
    if (::fstatat(directoryDescriptor, baseName, &verify, AT_SYMLINK_NOFOLLOW) == 0
        && verify.st_dev == device && verify.st_ino == inode && S_ISREG(verify.st_mode))
        (void)::unlinkat(directoryDescriptor, baseName, 0);
}

void verifiedUnlink(const GatedFileIdentity& identity)
{
    if (!identity.isValid())
        return;
    if (!pathNamesTemporaryTransportFile(identity.canonicalPath))
        return;
    const int directoryDescriptor = ::open(identity.parentDirectoryPath.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryDescriptor < 0)
        return;
    verifyAndUnlinkAt(directoryDescriptor, identity.baseName.c_str(), identity.device, identity.inode);
    ::close(directoryDescriptor);
}

int readGatedFile(Ubiquitty::Terminal& terminal, const char* path, std::span<unsigned char> outputBuffer,
    GatedFileDisposition disposition, GatedFileIdentity* outIdentity, GatedFileReadHook beforeRead, void* hookContext)
{
    if (!path || outputBuffer.empty())
        return -1;
    // Before the open, because the open is an effect: a name under a protected
    // folder raises a privacy prompt naming this application, and one under an
    // automount point mounts it. Refused here, such a name never reaches the
    // filesystem at all.
    if (!pathSpelledInTemporaryDirectory(path))
        return -1;

    // O_NOFOLLOW: the final component cannot be a symlink, so the name cannot be
    // swapped for one between the caller's check and this open. O_NONBLOCK: a
    // FIFO at that name opens and returns rather than hanging the caller.
    const int fileDescriptor = ::open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fileDescriptor < 0)
        return -1;

    int result = -1;
    struct ::stat status {};
    std::array<char, PATH_MAX> canonical {};
    // Every question is asked of the DESCRIPTOR rather than of `path`: S_ISREG
    // here subsumes FIFO, device and symlink-to-special with no second name
    // lookup to race, and the canonical path is the one this descriptor answers
    // to rather than the one the caller was given.
    const bool isRegularFile = ::fstat(fileDescriptor, &status) == 0 && S_ISREG(status.st_mode);
    const bool hasCanonicalPath = isRegularFile
        && Ubiquitty::PAL::fileCanonicalPathForDescriptor(fileDescriptor, canonical.data(), static_cast<int>(canonical.size())) > 0;
    // Inside a temporary directory, asked of where the descriptor really is: a
    // symlinked directory under /tmp passes the check on the name and fails
    // this one. A file the caller will delete must also carry the t=t marker,
    // and is refused unread without it rather than read and then kept, which
    // would make it a t=f under another name.
    const bool isContained = hasCanonicalPath
        && (disposition == GatedFileDisposition::DeleteAfterReading
                ? pathNamesTemporaryTransportFile(canonical.data())
                : pathInTemporaryDirectory(canonical.data()));
    // effectGranted, not effectAllowed: an absent policy refuses here rather than
    // running the effect. This is the one gate on this path, so it cannot afford
    // the permissive default the pre-gate in Core relies on.
    if (isContained && Ubiquitty::effectGranted(terminal, Ubiquitty::Capability::FileReadAccess, canonical.data())) {
        if (beforeRead)
            beforeRead(hookContext, canonical.data());

        std::size_t total = 0;
        bool didSucceed = true;
        while (total < outputBuffer.size()) {
            const ssize_t amount = ::read(fileDescriptor, outputBuffer.data() + total, outputBuffer.size() - total);
            if (amount > 0) {
                total += static_cast<std::size_t>(amount);
            } else if (amount == 0) {
                break; // end of file
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN) {
                break; // O_NONBLOCK: nothing more available
            } else {
                didSucceed = false;
                break;
            }
        }
        if (didSucceed) {
            result = static_cast<int>(total);
            if (outIdentity)
                (void)buildFileIdentity(canonical.data(), status.st_dev, status.st_ino, *outIdentity);
        }
    }
    ::close(fileDescriptor);
    return result;
}

} // namespace Ubiquitty::POSIX
