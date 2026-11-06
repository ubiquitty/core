// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SocketPath.cpp — PAL/Socket.h's filesystem side of local-socket establishment.
//
// Separate from Socket.cpp because preparing a path pulls in nothing a caller
// transferring bytes needs: a consumer that only makes a directory private, or
// clears a stale name, would otherwise link the stream implementation and its
// output queue to reach two functions that touch neither.

#include "PAL/Socket.h"

#include "PAL/UnixFileDescriptor.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace Ubiquitty::PAL {

namespace {

// A live listener answers connect(); a stale name refuses (ECONNREFUSED) or has
// already gone (ENOENT). An unobtainable probe socket reads as live, so an
// inconclusive answer never authorises removing someone else's endpoint.
//
// A path longer than sun_path is such an answer. Copying it in truncates, and
// the probe then describes a DIFFERENT socket from the one the caller named and
// is about to unlink — so the caller would read "no listener" from a path it
// never asked about and remove a live endpoint. The limit is the platform's own
// (Darwin 104, Linux 108); a literal would be wrong on one of them.
bool localSocketHasListener(const std::string& path)
{
    struct sockaddr_un address {};
    if (path.size() >= sizeof(address.sun_path))
        return true;
    UnixFileDescriptor probe(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!probe)
        return true;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return ::connect(probe.get(), reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0;
}

// The reason is the failure value, so a refusal cannot be reported without one.
std::unexpected<std::string> refuse(const char* reason)
{
    return std::unexpected(std::string(reason));
}

} // namespace

bool ensurePrivateSocketDirectory(std::string_view path)
{
    const std::string directory(path);
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        return false;

    // O_NOFOLLOW refuses a symlink planted where the directory belongs, and
    // fstat on the descriptor describes what was opened rather than whatever the
    // name resolves to on a second lookup.
    UnixFileDescriptor descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!descriptor)
        return false;
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0)
        return false;
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid())
        return false;
    // Repaired rather than refused: a directory this user owns with loosened
    // permissions is recoverable, and refusing would strand a caller who cannot
    // fix it without the descriptor already open here.
    return (status.st_mode & 07777) == 0700 || ::fchmod(descriptor.get(), 0700) == 0;
}

std::expected<void, std::string> prepareLocalSocketPathForBind(std::string_view path)
{
    const std::string socketPath(path);
    // Refused by its own name rather than through the probe. The probe reads an
    // over-length path as live, which is the safe answer but not a true one:
    // reported as "already listening" it sends a caller looking for an instance
    // that does not exist, and no bind at this path can succeed regardless.
    struct sockaddr_un address {};
    if (socketPath.size() >= sizeof(address.sun_path))
        return refuse("the socket path is longer than this platform can bind");
    struct stat status {};
    if (::lstat(socketPath.c_str(), &status) != 0) {
        if (errno == ENOENT)
            return {};
        return refuse("could not stat the socket path");
    }
    // lstat never follows a symlink, so S_ISSOCK is false for one and a symlink
    // is refused here rather than followed to wherever it points.
    if (!S_ISSOCK(status.st_mode))
        return refuse("the socket path is occupied by something other than a socket");
    if (status.st_uid != ::geteuid())
        return refuse("the socket path is owned by a different user");
    if (localSocketHasListener(socketPath))
        return refuse("another instance is already listening on this socket path");

    // The checks above describe the object seen at the first lstat. Re-examining
    // immediately before removal narrows the window in which that object is
    // replaced by one the checks were never applied to.
    struct stat recheck {};
    if (::lstat(socketPath.c_str(), &recheck) != 0 || !S_ISSOCK(recheck.st_mode)
        || recheck.st_uid != ::geteuid() || recheck.st_dev != status.st_dev || recheck.st_ino != status.st_ino)
        return refuse("the socket path changed during validation");

    if (::unlink(socketPath.c_str()) != 0)
        return refuse("failed to remove the stale socket");
    return {};
}

std::expected<UnixFileDescriptor, std::string> listenLocal(std::string_view path, int backlog)
{
    const std::string socketPath(path);
    struct sockaddr_un address {};
    if (socketPath.empty() || socketPath.size() >= sizeof(address.sun_path))
        return std::unexpected(std::string("the socket path is empty or longer than this platform can bind"));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.data(), socketPath.size());
    address.sun_path[socketPath.size()] = '\0';

    UnixFileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!listener)
        return std::unexpected(std::string("socket: ") + std::strerror(errno));
    // The umask makes the name owner-only from the moment it exists; the
    // chmod states it whatever the umask was.
    const mode_t previousMask = ::umask(0177);
    const int bindResult = ::bind(listener.get(), reinterpret_cast<const struct sockaddr*>(&address), sizeof(address));
    const int bindError = errno;
    ::umask(previousMask);
    if (bindResult != 0)
        return std::unexpected(std::string("bind: ") + std::strerror(bindError));
    ::chmod(socketPath.c_str(), 0600);
    if (::listen(listener.get(), backlog) != 0) {
        const int listenError = errno;
        ::unlink(socketPath.c_str());
        return std::unexpected(std::string("listen: ") + std::strerror(listenError));
    }
    return listener;
}

void removeLocalSocket(std::string_view path)
{
    const std::string socketPath(path);
    struct stat status {};
    if (::lstat(socketPath.c_str(), &status) == 0 && S_ISSOCK(status.st_mode) && status.st_uid == ::geteuid())
        ::unlink(socketPath.c_str());
}

} // namespace Ubiquitty::PAL
