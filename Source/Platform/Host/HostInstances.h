// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Session/HostInstanceInfo.h"
#include "PAL/Socket.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/HostPaths.h"

#include <array>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <bsm/audit.h>
#include <libproc.h>
#elif defined(__linux__)
#include <asm/socket.h>
#include <poll.h>
#endif

namespace Ubiquitty::Host {

/// Reads only a bounded regular same-user log; discovery never follows a link or waits on a writer.
///
/// The authenticated peer's startup line identifies the running image even after replacement.
/// - Parameters:
///   - socketName: The discovered socket's basename.
///   - process: Its authenticated peer process.
/// - Returns: The reported version, empty when the log cannot establish it.
[[nodiscard]] inline std::string discoveredDaemonVersion(std::string_view socketName, std::uint64_t process)
{
    std::array<char, 1024> path {};
    const std::string name(socketName);
    logfilePathForSocketName(name.c_str(), path);
    PAL::UnixFileDescriptor file(::open(path.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat status {};
    if (!file || ::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077))
        return {};
    std::array<char, 16384> bytes {};
    const auto count = ::pread(file.get(), bytes.data(), bytes.size(), 0);
    return count > 0 ? Session::daemonVersionFromLog({ bytes.data(), static_cast<std::size_t>(count) }, process) : std::string {};
}

/// Probes private same-user sockets without sending a request or trusting PID files.
/// A full listener backlog is skipped; discovery never waits on another reactor.
[[nodiscard]] inline std::vector<Session::HostInstanceInfo> discoverHostInstances()
{
    std::array<char, 1024> directory {};
    struct stat directoryStatus {};
    if (!controlDirectory(directory) || ::lstat(directory.data(), &directoryStatus) != 0
        || !S_ISDIR(directoryStatus.st_mode) || directoryStatus.st_uid != ::geteuid() || (directoryStatus.st_mode & 0077))
        return {};
    std::vector<Session::HostInstanceInfo> instances;
    eachHostSocketNewestFirst([](const char* path, void* context) {
        auto& instances = *static_cast<std::vector<Session::HostInstanceInfo>*>(context);
        struct stat status {};
        sockaddr_un address {};
        const std::string_view socketPath(path);
        if (socketPath.size() >= sizeof(address.sun_path) || ::lstat(path, &status) != 0
            || !S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077))
            return false;
        PAL::UnixFileDescriptor socket(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (!socket || ::fcntl(socket.get(), F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(socket.get(), F_SETFL, O_NONBLOCK) != 0)
            return false;
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path, socketPath.size() + 1);
        if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || !PAL::peerIsSameUser(socket))
            return false;
        std::array<char, 4096> executable {};
        std::uint64_t process = 0;
#if defined(__APPLE__)
        pid_t peerProcess = 0;
        socklen_t processLength = sizeof(peerProcess);
        audit_token_t token {};
        socklen_t length = sizeof(token);
        if (::getsockopt(socket.get(), SOL_LOCAL, LOCAL_PEERPID, &peerProcess, &processLength) != 0 || processLength != sizeof(peerProcess)
            || peerProcess <= 0 || ::getsockopt(socket.get(), SOL_LOCAL, LOCAL_PEERTOKEN, &token, &length) != 0 || length != sizeof(token)
            || ::proc_pidpath_audittoken(&token, executable.data(), executable.size()) <= 0)
            return false;
        process = static_cast<std::uint64_t>(peerProcess);
#elif defined(__linux__)
        struct ucred credentials {};
        socklen_t length = sizeof(credentials);
        int descriptor = -1;
        socklen_t descriptorLength = sizeof(descriptor);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || length != sizeof(credentials)
            || ::getsockopt(socket.get(), SOL_SOCKET, SO_PEERPIDFD, &descriptor, &descriptorLength) != 0)
            return false;
        PAL::UnixFileDescriptor lifetime(descriptor);
        pollfd alive { lifetime.get(), POLLIN, 0 };
        const auto processPath = "/proc/" + std::to_string(credentials.pid) + "/exe";
        const auto count = ::readlink(processPath.c_str(), executable.data(), executable.size() - 1);
        if (!lifetime || descriptorLength != sizeof(descriptor) || count <= 0 || ::poll(&alive, 1, 0) != 0)
            return false;
        executable[static_cast<std::size_t>(count)] = '\0';
        process = static_cast<std::uint64_t>(credentials.pid);
#else
        return false;
#endif
        Session::HostInstanceInfo instance { std::string(socketPath.substr(socketPath.rfind('/') + 1)), executable.data(),
            static_cast<std::uint64_t>(std::max<time_t>(0, status.st_mtime)) };
        instance.appVersion = discoveredDaemonVersion(instance.socketName, process);
        // Endpoint metadata shares one bounded Sync result with the directory.
        std::size_t metadataBytes = 2 * (instance.executable.size() + instance.appVersion.size()) + 192;
        for (const auto& held : instances)
            metadataBytes += 2 * (held.executable.size() + held.appVersion.size()) + 192;
        if (Session::isValidHostInstance(instance) && metadataBytes <= 16384)
            instances.push_back(std::move(instance));
        return instances.size() >= Session::maxDiscoveredHostInstances;
    },
        &instances);
    return instances;
}

} // namespace Ubiquitty::Host
