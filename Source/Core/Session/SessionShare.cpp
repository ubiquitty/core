// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Session/SessionShare.cpp — see SessionShare.h.
//

#include "Core/Session/SessionShare.h"
#include "Core/Session/ShareTerminalLimits.h"

#include "Core/Image/TerminalImage.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/ControlPaths.h"
#include "Core/Session/DaemonSocketLeaf.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/SSHHost.h"
#include "PAL/PAL.h"
#include "PAL/Socket.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty::Session {

std::string localDaemonSocketPath(const std::string& hostBinaryPath)
{
    const std::string leaf = socketLeafForBinary(hostBinaryPath.c_str());
    if (leaf.empty())
        return std::string();
    std::array<char, 1024> path {};
    const std::size_t length = controlFilePath(leaf.c_str(), path);
    return length == 0 ? std::string() : std::string(path.data(), length);
}

std::string localHostInstallPath()
{
    std::array<char, 1024> path {};
    const std::size_t length = controlFilePath("ubiquitty-host", path);
    return length == 0 ? std::string() : std::string(path.data(), length);
}

bool ensureLocalDaemon(const std::string& hostBinaryPath, const std::string& socketPath)
{
    if (hostBinaryPath.empty() || socketPath.empty())
        return false;
    // Asked of the socket's listener rather than of the path: a daemon that
    // died leaves its socket behind, and a share that took the file for a
    // daemon would fail at the handoff instead of starting a new one.
    const auto isAnswering = [&] { return PAL::connectLocal(socketPath) != nullptr; };
    if (isAnswering())
        return true;
    // The grid is the daemon's default for sessions nobody sized; the adopted
    // session arrives with the window's own.
    const char* const arguments[] = { hostBinaryPath.c_str(), "--ensure-daemon", "--columns", "80", "--rows", "24", nullptr };
    // The launcher exits 0 once the daemon's socket listens, whatever that
    // takes on a loaded machine, so its status is the readiness: no poll
    // guesses at how long a start lasts.
    if (PAL::processSpawnAndWait(hostBinaryPath.c_str(), arguments) != 0)
        return false;
    return isAnswering();
}

LocalDaemonTarget readyLocalDaemon(const std::string& hostBinaryPath)
{
    const std::string socketPath = localDaemonSocketPath(hostBinaryPath);
    if (socketPath.empty() || !ensureLocalDaemon(hostBinaryPath, socketPath))
        return {};
    // The leaf is the path's last component: the binary is hashed once.
    return LocalDaemonTarget {
        .bridgeCommand = localBridgeCommand(Transport::shellSingleQuote(hostBinaryPath)),
        .socketName = socketPath.substr(socketPath.find_last_of('/') + 1),
        .socketPath = socketPath,
    };
}

ShareHandoffLimits localShareHandoffLimits()
{
    return {
        .maxRecordBytes = Protocol::Local::maxAdoptBodyBytes + Protocol::Local::adoptWirePrepareHeaderBytes,
        .silenceMicroseconds = 2'000'000,
        .applyMicrosecondsPerMebibyte = 1'000'000,
        .resolutionAttempts = 2,
    };
}

std::expected<Protocol::Sync::HandoverBytes, ShareEncoding> captureShareTerminal(const Ubiquitty::Terminal& term, std::size_t maxBodyBytes)
{
    if (!term.grid || term.columns <= 0 || term.columns > maxColumns || term.rows <= 0 || term.rows > maxRows
        || term.scrollbackLimit < 0 || term.scrollbackCapacity < 0 || term.scrollbackCount < 0)
        return std::unexpected(ShareEncoding::NothingToShare);
    auto terminal = Protocol::Sync::encodeHandoverTerminal(term, 1, localShareTerminalLimits(maxBodyBytes));
    if (!terminal)
        return std::unexpected(terminal.error() == Protocol::Sync::HandoverBodyError::OverCapacity ? ShareEncoding::TooLong : ShareEncoding::NothingToShare);
    return std::move(*terminal);
}

std::string shareFailureTitle(ShareOutcome outcome)
{
    switch (outcome) {
    case ShareOutcome::Shared:
    case ShareOutcome::NotShareable:
        return {};
    case ShareOutcome::NoDaemonSocket:
    case ShareOutcome::DaemonDidNotStart:
        return "Couldn’t Start Sharing";
    case ShareOutcome::HandoffRefused:
        return "Sharing Failed";
    case ShareOutcome::Unresolved:
        return "Sharing Couldn’t Be Confirmed";
    case ShareOutcome::TooLongToShare:
        return "This Window Is Too Large to Share";
    case ShareOutcome::HostFull:
        return "The Local Host Is Full";
    }
    return {};
}

std::string shareFailureDetail(ShareOutcome outcome)
{
    switch (outcome) {
    case ShareOutcome::Shared:
    case ShareOutcome::NotShareable:
        return {};
    case ShareOutcome::NoDaemonSocket:
    case ShareOutcome::DaemonDidNotStart:
        return "The host bundled with Ubiquitty didn’t start, so this window can’t be shared. The window is unchanged.";
    case ShareOutcome::HandoffRefused:
        return "The local Ubiquitty host didn’t take over this session. The window is unchanged.";
    case ShareOutcome::Unresolved:
        return "Ubiquitty couldn’t confirm whether the local host took over this session. This terminal is paused to keep the session safe. Keep Ubiquitty open until this is resolved.";
    case ShareOutcome::TooLongToShare:
        return "This window’s terminal state is more than the local host can take over (up to " + std::to_string(Protocol::Local::maxAdoptBodyBytes / (1024 * 1024))
            + " MiB). The window is unchanged.";
    case ShareOutcome::HostFull:
        return "The local Ubiquitty host is full: it already runs as many sessions as it holds. End one of its sessions, then share this window again. The window is unchanged.";
    }
    return {};
}

} // namespace Ubiquitty::Session
