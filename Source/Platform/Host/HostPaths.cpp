// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Platform/Host/HostPaths.cpp — see HostPaths.h.
//

#include "Platform/Host/HostPaths.h"

#include "Core/Session/ControlPaths.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace Ubiquitty::Host {

namespace {

bool isLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

std::size_t clearAndReturnZero(std::span<char> output)
{
    char* const outputBuffer = output.data();
    const std::size_t capacity = output.size();
    if (outputBuffer && capacity)
        outputBuffer[0] = '\0';
    return 0;
}

// The ~/.ubiquitty control directory + per-leaf paths are the shared Session/
// ControlPaths primitives; the Host backend names its own files on top.
std::size_t buildDirectory(std::span<char> output)
{
    return Session::controlDirectory(output);
}

std::size_t buildFile(std::span<char> output, const char* leaf) { return Session::controlFilePath(leaf, output); }

/// The most control-directory entries one socket walk reads. A host keeps
/// about ten files beside its socket (its lock, process-ID file, log,
/// workspace and event records, and records set aside), so this holds
/// hundreds of hosts; a directory larger than that is not one hosts filled.
constexpr std::size_t maxSocketWalkEntries = 4096;

} // namespace

bool isHostSocketLeaf(std::string_view name)
{
    // A shared well-known leaf is the cross-binary-bleed surface the
    // per-binary leaf closes (see HostSocketName.h).
    constexpr std::string_view prefix = "host-";
    constexpr std::string_view suffix = ".sock";
    constexpr std::size_t hexLength = 16;
    if (name.size() != prefix.size() + hexLength + suffix.size())
        return false;
    if (!name.starts_with(prefix) || !name.ends_with(suffix))
        return false;
    return std::ranges::all_of(name.substr(prefix.size(), hexLength), isLowerHex);
}

bool isOwnHostSocketPath(const char* path)
{
    if (!path)
        return false;
    const std::string_view full(path);
    const auto separator = full.rfind('/');
    if (separator == std::string_view::npos || !isHostSocketLeaf(full.substr(separator + 1)))
        return false;
    std::array<char, 1024> directory {};
    if (!buildDirectory(directory))
        return false;
    struct stat directoryState {};
    if (::lstat(directory.data(), &directoryState) != 0 || !S_ISDIR(directoryState.st_mode)
        || directoryState.st_uid != ::geteuid() || (directoryState.st_mode & 0077))
        return false;
    const std::string parent(full.substr(0, separator));
    std::array<char, PATH_MAX> resolvedDirectory {};
    std::array<char, PATH_MAX> resolvedParent {};
    if (!::realpath(directory.data(), resolvedDirectory.data()) || !::realpath(parent.c_str(), resolvedParent.data())
        || std::strcmp(resolvedDirectory.data(), resolvedParent.data()))
        return false;
    struct stat socketState {};
    return ::lstat(path, &socketState) == 0 && S_ISSOCK(socketState.st_mode) && socketState.st_uid == ::geteuid();
}

std::size_t controlDirectory(std::span<char> output)
{
    return buildDirectory(output);
}
std::size_t socketPathForName(const char* leaf, std::span<char> output)
{
    // Fail closed on an empty / invalid leaf — never default to a shared socket.
    if (!leaf || !isHostSocketLeaf(leaf))
        return clearAndReturnZero(output);
    return buildFile(output, leaf);
}

namespace {

/// Writes the control file that shares a socket leaf's stem.
///
/// - Parameters:
///   - socketLeaf: A socket leaf; one ``isHostSocketLeaf`` rejects
///     writes nothing.
///   - extension: The replacement for `.sock`, dot included, at most four bytes.
///   - output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 on failure.
std::size_t siblingPathForSocketName(const char* socketLeaf, std::string_view extension, std::span<char> output)
{
    // Fail closed on an empty / invalid leaf — never default to the shared leaf.
    if (!socketLeaf || !isHostSocketLeaf(socketLeaf))
        return clearAndReturnZero(output);
    const std::string_view leaf(socketLeaf);
    const std::string_view stem = leaf.substr(0, leaf.size() - std::string_view(".sock").size());
    char siblingLeaf[32];
    if (stem.size() + extension.size() >= sizeof(siblingLeaf))
        return clearAndReturnZero(output);
    std::memcpy(siblingLeaf, stem.data(), stem.size());
    std::memcpy(siblingLeaf + stem.size(), extension.data(), extension.size());
    siblingLeaf[stem.size() + extension.size()] = '\0';
    return buildFile(output, siblingLeaf);
}

} // namespace

std::size_t processIDFilePathForSocketName(const char* socketLeaf, std::span<char> output)
{
    return siblingPathForSocketName(socketLeaf, ".pid", output);
}

std::size_t logfilePathForSocketName(const char* socketLeaf, std::span<char> output)
{
    return siblingPathForSocketName(socketLeaf, ".log", output);
}

bool eachHostSocketNewestFirst(bool (*visit)(const char* path, void* ctx), void* ctx)
{
    if (!visit)
        return false;
    char directory[1024];
    if (buildDirectory(directory) == 0)
        return false;
    DIR* d = ::opendir(directory);
    if (!d)
        return false;

    struct Candidate {
        std::string leaf;
        struct timespec mtime;
    };
    std::vector<Candidate> candidates;
    std::size_t examined = 0;
    while (const dirent* entry = ::readdir(d)) {
        if (++examined > maxSocketWalkEntries)
            break;
        const std::string_view name(entry->d_name);
        if (!isHostSocketLeaf(name))
            continue;
        char full[1024];
        const int n = std::snprintf(full, sizeof(full), "%s/%s", directory, entry->d_name);
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(full))
            continue;
        struct stat st;
        if (!isOwnHostSocketPath(full) || ::lstat(full, &st) != 0 || !S_ISSOCK(st.st_mode))
            continue;
        // Socket bind-time mtime; st_mtimespec on Apple, st_mtim (POSIX.1-2008)
        // elsewhere (macOS struct stat has no plain st_mtim sans _DARWIN_C_SOURCE).
#if defined(__APPLE__)
        candidates.push_back({ std::string(name), st.st_mtimespec });
#else
        candidates.push_back({ std::string(name), st.st_mtim });
#endif
    }
    ::closedir(d);

    // Newest bind first, so a fresh agent reaches the current binary's daemon
    // first. Every socket read is ordered before any is visited, so a newer
    // socket is never left behind older ones the directory listed first.
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        if (a.mtime.tv_sec != b.mtime.tv_sec)
            return a.mtime.tv_sec > b.mtime.tv_sec;
        if (a.mtime.tv_nsec != b.mtime.tv_nsec)
            return a.mtime.tv_nsec > b.mtime.tv_nsec;
        return a.leaf < b.leaf;
    });
    for (const auto& candidate : candidates) {
        char full[1024];
        if (buildFile(full, candidate.leaf.c_str()) == 0)
            continue;
        if (visit(full, ctx))
            return true;
    }
    return false;
}

namespace {

/// What one nonblocking connect to a socket path says about its listener.
enum class ConnectVerdict : std::uint8_t {
    /// Accepted, or queued behind a backlog that will drain.
    Served,
    /// No socket is bound there.
    Absent,
    /// Refused: nothing listens, or, where the kernel does not tell the two
    /// apart, the listener's backlog is full.
    Refused,
    /// The probe itself could not be made.
    Unknown,
};

ConnectVerdict connectOnce(const sockaddr_un& address)
{
    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0)
        return ConnectVerdict::Unknown;
    ::fcntl(probe, F_SETFD, FD_CLOEXEC);
    ::fcntl(probe, F_SETFL, O_NONBLOCK);
    const int result = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = errno;
    ::close(probe);
    if (result == 0 || error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS)
        return ConnectVerdict::Served;
    if (error == ENOENT || error == ENOTDIR)
        return ConnectVerdict::Absent;
    return error == ECONNREFUSED ? ConnectVerdict::Refused : ConnectVerdict::Unknown;
}

#if defined(__APPLE__)
/// Whether a process of this user holds a listening socket bound at the socket
/// file `path` names, by the kernel's record of every descriptor. Whatever
/// cannot be read counts as holding one, except a process that has exited or
/// whose descriptors are closed to other processes of its user, which no host
/// is.
bool isListenerHeldAt(const char* path)
{
    struct stat target {};
    if (::stat(path, &target) != 0)
        return errno != ENOENT && errno != ENOTDIR;
    const auto isTarget = [&](const char* bound) {
        if (std::strcmp(bound, path) == 0)
            return true;
        struct stat candidate {};
        return bound[0] == '/' && ::stat(bound, &candidate) == 0 && candidate.st_dev == target.st_dev && candidate.st_ino == target.st_ino;
    };
    const int listedBytes = ::proc_listpids(PROC_UID_ONLY, ::geteuid(), nullptr, 0);
    if (listedBytes <= 0)
        return true;
    // Room for processes started between the two calls.
    std::vector<pid_t> processes(static_cast<std::size_t>(listedBytes) / sizeof(pid_t) + 64);
    const int filledBytes = ::proc_listpids(PROC_UID_ONLY, ::geteuid(), processes.data(), static_cast<int>(processes.size() * sizeof(pid_t)));
    if (filledBytes <= 0)
        return true;
    processes.resize(static_cast<std::size_t>(filledBytes) / sizeof(pid_t));
    const auto isUnreadable = [] { return errno != ESRCH && errno != EPERM; };
    std::vector<proc_fdinfo> descriptors;
    for (const pid_t process : processes) {
        if (process <= 0)
            continue;
        const int bytes = ::proc_pidinfo(process, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (bytes <= 0) {
            if (isUnreadable())
                return true;
            continue;
        }
        descriptors.resize(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
        const int read = ::proc_pidinfo(process, PROC_PIDLISTFDS, 0, descriptors.data(), static_cast<int>(descriptors.size() * sizeof(proc_fdinfo)));
        if (read <= 0) {
            if (isUnreadable())
                return true;
            continue;
        }
        for (const auto& descriptor : std::span(descriptors).first(static_cast<std::size_t>(read) / sizeof(proc_fdinfo))) {
            if (descriptor.proc_fdtype != PROX_FDTYPE_SOCKET)
                continue;
            socket_fdinfo socket {};
            if (::proc_pidfdinfo(process, descriptor.proc_fd, PROC_PIDFDSOCKETINFO, &socket, sizeof(socket)) != sizeof(socket))
                continue; // closed since it was listed
            const auto& info = socket.psi;
            if (info.soi_kind != SOCKINFO_UN || !(info.soi_options & SO_ACCEPTCONN))
                continue;
            std::array<char, sizeof(info.soi_proto.pri_un.unsi_addr.ua_sun.sun_path) + 1> bound {};
            std::memcpy(bound.data(), info.soi_proto.pri_un.unsi_addr.ua_sun.sun_path, bound.size() - 1);
            if (isTarget(bound.data()))
                return true;
        }
    }
    return false;
}
#endif

} // namespace

bool isSocketPathServed(const char* path)
{
    sockaddr_un address {};
    if (!path || !*path || std::strlen(path) >= sizeof(address.sun_path))
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, std::strlen(path) + 1);
    switch (connectOnce(address)) {
    case ConnectVerdict::Served:
    case ConnectVerdict::Unknown:
        return true;
    case ConnectVerdict::Absent:
        return false;
    case ConnectVerdict::Refused:
        break;
    }
#if defined(__linux__)
    // A listener with a full backlog answers a nonblocking connect EAGAIN, so
    // a refusal says nothing listens.
    return false;
#elif defined(__APPLE__)
    // A full backlog is refused exactly as a socket nothing listens on, so
    // only the kernel's record of who listens tells the two apart.
    return isListenerHeldAt(path);
#else
    // With no record of listeners to read, a refusal is taken as no listener
    // only once it has lasted while a backlog would have drained.
    for (int attempt = 0; attempt < 5; ++attempt) {
        ::usleep(40'000);
        switch (connectOnce(address)) {
        case ConnectVerdict::Served:
        case ConnectVerdict::Unknown:
            return true;
        case ConnectVerdict::Absent:
            return false;
        case ConnectVerdict::Refused:
            break;
        }
    }
    return false;
#endif
}

namespace {
struct NewestPathContext {
    char* outputBuffer;
    std::size_t capacity;
    bool captured;
};
bool captureNewestPath(const char* path, void* ctx)
{
    NewestPathContext* c = static_cast<NewestPathContext*>(ctx);
    const std::size_t n = std::strlen(path);
    if (n + 1 > c->capacity)
        return true; // won't fit the caller's buffer — stop, leave empty
    std::memcpy(c->outputBuffer, path, n + 1);
    c->captured = true;
    return true; // the first visited is the newest
}
} // namespace

std::size_t newestSocketPath(std::span<char> output)
{
    char* const outputBuffer = output.data();
    const std::size_t capacity = output.size();
    if (!outputBuffer || capacity == 0)
        return 0;
    outputBuffer[0] = '\0';
    NewestPathContext ctx { outputBuffer, capacity, false };
    eachHostSocketNewestFirst(captureNewestPath, &ctx);
    return ctx.captured ? std::strlen(outputBuffer) : 0;
}

bool ensureControlDirectory() { return Session::ensureControlDirectory(); }

} // namespace Ubiquitty::Host
