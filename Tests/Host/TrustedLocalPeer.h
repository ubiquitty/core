// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/TrustedLocalBinaries.h"

#include <string>

namespace Ubiquitty {
namespace Host {

/// The peer path a test's Local-origin client presents.
///
/// `CapabilityOrigin::Local` means the peer is the daemon's own executable, by
/// device and inode — the attach bridge is a second process of that file. A test
/// binary is its own daemon, so its Local peer is itself.
///
/// Naming an install path instead classifies as Agent, whether or not the file
/// is there: on a machine with the app installed it is a different file, and on
/// one without it there is no file at all. Both are the right answer and neither
/// is what a test about consent or presence is asking.
///
/// - Returns: The running binary's resolved path.
inline std::string trustedLocalPeerPath()
{
    return resolveSelfExecutablePath();
}

/// Seeds a client's peer identity the way the daemon does when it accepts one.
///
/// `peerProcess` alone is not enough to reach `CapabilityOrigin::Local`: the
/// daemon takes the peer's device and inode at accept and classifies on that,
/// because a path names whatever stands there when it is read. A seam that sets
/// only the path models a client the daemon never builds, and its Local cases
/// silently become Agent ones.
///
/// - Parameters:
///   - client: The seeded client to complete. Its `peerProcess` is set here too.
template<typename ClientType>
inline void seedTrustedLocalPeer(ClientType& client)
{
    client.peerProcess = trustedLocalPeerPath();
    client.hasPeerBinary = binaryIdentityOf(client.peerProcess, client.peerBinary);
}

} // namespace Host
} // namespace Ubiquitty
