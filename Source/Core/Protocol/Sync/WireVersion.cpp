// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/WireVersion.h"

#include "Core/BuildVersion.h"
#include "Core/Protocol/Sync/AppVersion.h"

namespace Ubiquitty::Protocol::Sync {

// The generated header is Core/BuildVersion.cpp's alone; whether its text is a
// canonical version within these bounds is pinned by
// thisBuildStatesAVersionAndAnIdentity in test_sync_wire_version.
std::string_view appVersion() { return Ubiquitty::buildVersion(); }

std::string_view buildIdentity() { return Ubiquitty::buildIdentity(); }

std::string appVersionWithIdentity()
{
    std::string text(appVersion());
    text += " (";
    text += buildIdentity();
    text += ")";
    return text;
}

PeerBuild localPeerBuild()
{
    return { .isKnown = true, .wireVersion = wireVersion, .appVersion = std::string(appVersion()) };
}

std::string appVersionFromPeer(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > maxAppVersionBytes)
        return {};
    for (const std::uint8_t byte : bytes) {
        const bool isVersionByte = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || byte == '.' || byte == '-' || byte == '+';
        if (!isVersionByte)
            return {};
    }
    return std::string(bytes.begin(), bytes.end());
}

} // namespace Ubiquitty::Protocol::Sync
