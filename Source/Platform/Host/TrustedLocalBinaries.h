// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

/// Which file on this machine a path names, by the pair a copy does not share.
///
/// A name is chosen by whoever creates the file; a device and inode are handed
/// out by the filesystem. `cp /bin/sh /tmp/ubiquitty-host` produces a peer whose
/// kernel-verified path ends in the daemon's own name and shares neither.
struct BinaryIdentity {
    /// The filesystem the file is on.
    dev_t device = 0;

    /// The file's number within it.
    ino_t inode = 0;
};

/// The identity of the file at this path, or nothing when there is no file.
///
/// - Parameters:
///   - path: An absolute path to stat.
///   - out: Receives the identity when the file exists.
/// - Returns: True when the path named a file this could identify.
inline bool binaryIdentityOf(const std::string& path, BinaryIdentity& out)
{
    if (path.empty())
        return false;
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0)
        return false;
    out.device = info.st_dev;
    out.inode = info.st_ino;
    return true;
}

/// The path with Linux's deleted-file marker removed.
///
/// `/proc/self/exe` tracks the inode rather than the name, so after an in-place
/// upgrade it reads `<path> (deleted)` — a string that stats to nothing. The name
/// before the marker is where the replacement now sits, and a daemon that keeps
/// running across an upgrade has to trust it: a bridge launched from the new file
/// is otherwise an agent, and on a headless host there is no human to answer the
/// consent prompt that follows. macOS reports a plain path and this returns it
/// unchanged.
///
/// - Parameters:
///   - path: A self-executable path as the platform reported it.
/// - Returns: The path a stat can resolve, or the input when it carries no marker.
inline std::string pathWithoutDeletedMarker(const std::string& path)
{
    static constexpr const char* marker = " (deleted)";
    const std::size_t markerLength = std::string(marker).size();
    if (path.size() <= markerLength)
        return path;
    if (path.compare(path.size() - markerLength, markerLength, marker) != 0)
        return path;
    return path.substr(0, path.size() - markerLength);
}

/// The bundled interface beside a host binary, or empty when it is not in one.
///
/// This half exists because the interface connects directly for ADOPT_SESSION
/// and that gate turns away every agent. The direction of travel is that the
/// interface is a client like any other and reaches the daemon the way every
/// other client does, through the bridge — at which point this returns nothing
/// anyone needs and the host stops knowing what an app bundle is.
///
/// The app bundle puts the daemon under `Contents/Helpers` and the interface
/// under `Contents/MacOS`, so one locates the other without anything being
/// hard-coded about where the bundle itself is. A host that is not in a bundle —
/// a development build, an `~/.ubiquitty` install — has no interface beside it,
/// and gets an empty string rather than a guess.
///
/// - Parameters:
///   - hostPath: The daemon's own resolved executable path.
/// - Returns: The path the bundled interface would have, or an empty string.
inline std::string bundledInterfacePathForHost(const std::string& hostPath)
{
    static constexpr const char* hostTail = "/Contents/Helpers/ubiquitty-host";
    static constexpr const char* interfaceTail = "/Contents/MacOS/Ubiquitty";
    const std::size_t tailLength = std::string(hostTail).size();
    if (hostPath.size() <= tailLength)
        return std::string();
    if (hostPath.compare(hostPath.size() - tailLength, tailLength, hostTail) != 0)
        return std::string();
    return hostPath.substr(0, hostPath.size() - tailLength) + interfaceTail;
}

/// Whether two files hold the same signed code: the same size and the same
/// code directory hash. A file with no signature holds no code to compare,
/// and on a platform that does not sign code nothing matches.
///
/// Read from the files each time it is asked, never remembered: an update
/// that replaces either file changes the answer at the next connection.
///
/// - Parameters:
///   - first: An absolute path.
///   - second: Another.
/// - Returns: Whether both are signed and hold the same code.
[[nodiscard]] bool hasSameCode(const std::string& first, const std::string& second);

/// The helper an app bundle carries beside its interface, or empty when the
/// path is not a bundle's interface.
///
/// - Parameter interfacePath: The app's executable.
/// - Returns: The bundle's `Contents/Helpers/ubiquitty-host`, or an empty string.
inline std::string helperPathForInterface(const std::string& interfacePath)
{
    static constexpr std::string_view interfaceTail = "/Contents/MacOS/Ubiquitty";
    if (interfacePath.empty() || interfacePath.front() != '/' || !std::string_view(interfacePath).ends_with(interfaceTail))
        return std::string();
    return interfacePath.substr(0, interfacePath.size() - interfaceTail.size()) + "/Contents/Helpers/ubiquitty-host";
}

/// The interface a host trusts as its app's: the one bundled beside it, or,
/// for a host installed outside a bundle, the app its install names.
///
/// The macOS app installs its bundled helper as the control directory's
/// `ubiquitty-host`, by a hard link or, where it cannot link, a copy, and
/// leaves `<install>.interface` naming its executable. That name is trusted
/// only while the named app's own helper is the very file at `hostPath`, or
/// holds the same signed code (``hasSameCode``): the host is then the app's
/// own as much as one started from the bundle. A newer app's helper, another
/// build's or a removed app names nothing.
///
/// - Parameter hostPath: The daemon's own resolved executable path.
/// - Returns: The interface's path, or an empty string.
inline std::string interfacePathForHost(const std::string& hostPath)
{
    if (std::string bundled = bundledInterfacePathForHost(hostPath); !bundled.empty())
        return bundled;
    if (hostPath.empty())
        return std::string();
    std::array<char, 4096> target {};
    const ssize_t length = ::readlink((hostPath + ".interface").c_str(), target.data(), target.size() - 1);
    if (length <= 0)
        return std::string();
    const std::string interface(target.data(), static_cast<std::size_t>(length));
    const std::string helper = helperPathForInterface(interface);
    if (helper.empty())
        return std::string();
    struct stat own {}, linked {};
    if (::stat(hostPath.c_str(), &own) != 0 || ::stat(helper.c_str(), &linked) != 0)
        return std::string();
    if ((own.st_dev != linked.st_dev || own.st_ino != linked.st_ino) && !hasSameCode(hostPath, helper))
        return std::string();
    return interface;
}

/// Adds a path's identity to a set, if it names a file and is not already there.
///
/// - Parameters:
///   - path: The path to identify.
///   - trusted: The set to add to.
inline void addBinaryIdentity(const std::string& path, std::vector<BinaryIdentity>& trusted)
{
    BinaryIdentity identity;
    if (!binaryIdentityOf(path, identity))
        return;
    for (const BinaryIdentity& one : trusted) {
        if (one.device == identity.device && one.inode == identity.inode)
            return;
    }
    trusted.push_back(identity);
}

/// The binaries a daemon at this path accepts as its own, by identity.
///
/// The daemon's own executable, which the attach bridge is a second process of,
/// its app's interface (``interfacePathForHost``) and that app's helper, whose
/// bridge is the app's. Each is resolved twice — once as the
/// platform reported the path and once with Linux's deleted-file marker removed —
/// so an upgrade that replaced the file in place is found as well as the file the
/// daemon was executed from.
///
/// A daemon that cannot resolve its own path trusts nothing, which costs a
/// consent prompt and never a wrong grant. On a host outside an app bundle —
/// a Linux install, a headless server, a development build — there is no
/// interface to find, and the set is the daemon's own binary alone. A
/// release's host on macOS consults no file identity: it knows its own
/// binaries by their signature (`isOwnBinary`).
///
/// - Parameters:
///   - selfPath: The daemon's own resolved executable path.
/// - Returns: The identities, without duplicates.
inline std::vector<BinaryIdentity> resolveTrustedLocalBinaries(const std::string& selfPath)
{
    std::vector<BinaryIdentity> trusted;
    const std::string live = pathWithoutDeletedMarker(selfPath);
    addBinaryIdentity(selfPath, trusted);
    addBinaryIdentity(live, trusted);
    const auto addApp = [&trusted](const std::string& hostPath) {
        const std::string interface = interfacePathForHost(hostPath);
        addBinaryIdentity(interface, trusted);
        addBinaryIdentity(helperPathForInterface(interface), trusted);
    };
    addApp(selfPath);
    addApp(live);
    return trusted;
}

/// Merges whatever the daemon's own path names now into a set it already holds.
///
/// A daemon outlives an upgrade: the file it was executed from is replaced, and
/// the next bridge is launched from the replacement. Keeping the old identity is
/// what lets a bridge already running stay trusted; adding the new one is what
/// lets the next one attach without a consent prompt nobody may be there to
/// answer. Nothing is ever removed, and the set grows by at most one per upgrade
/// during a daemon's life.
///
/// - Parameters:
///   - selfPath: The daemon's own resolved executable path, read again.
///   - trusted: The set to merge into.
inline void mergeTrustedLocalBinaries(const std::string& selfPath, std::vector<BinaryIdentity>& trusted)
{
    for (const BinaryIdentity& one : resolveTrustedLocalBinaries(selfPath)) {
        bool isHeld = false;
        for (const BinaryIdentity& held : trusted)
            isHeld = isHeld || (held.device == one.device && held.inode == one.inode);
        if (!isHeld)
            trusted.push_back(one);
    }
}

/// Whether a peer's captured identity is one of the trusted binaries.
///
/// Takes an identity rather than a path on purpose. A path is a name, `::stat`
/// resolves it as it stands at the moment of the call, and a peer chooses what
/// stands at its own: connect from one file, put a link to the daemon's binary
/// in its place, and a check that reads the path reads the wrong file. The
/// identity is taken once, at accept, from what the kernel resolved.
///
/// - Parameters:
///   - peer: The identity captured when the connection was accepted.
///   - trusted: What `resolveTrustedLocalBinaries` found.
/// - Returns: True when the peer is one of those exact files.
inline bool identityIsTrustedLocalBinary(const BinaryIdentity& peer, const std::vector<BinaryIdentity>& trusted)
{
    for (const BinaryIdentity& one : trusted) {
        if (one.device == peer.device && one.inode == peer.inode)
            return true;
    }
    return false;
}

} // namespace Host
} // namespace Ubiquitty
