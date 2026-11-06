// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/AttachRelay.h"

#include "Core/Protocol/Local/Control.h"
#include "Core/Transport/SSHHost.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <poll.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty::Host {

namespace {

constexpr std::size_t pendingCapacity = 1u << 20;
constexpr int heartbeatMilliseconds = 1000;

void readPending(int fileDescriptor, std::vector<std::uint8_t>& pending, bool& isOpen)
{
    std::array<std::uint8_t, 65536> buffer;
    while (isOpen && pending.size() < pendingCapacity) {
        const std::size_t capacity = std::min(buffer.size(), pendingCapacity - pending.size());
        const ssize_t count = ::read(fileDescriptor, buffer.data(), capacity);
        if (count > 0)
            pending.insert(pending.end(), buffer.begin(), buffer.begin() + count);
        else {
            if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                isOpen = false;
            return;
        }
    }
}

bool writePending(int fileDescriptor, std::vector<std::uint8_t>& pending)
{
    if (pending.empty())
        return true;
    const ssize_t count = ::write(fileDescriptor, pending.data(), pending.size());
    if (count > 0) {
        pending.erase(pending.begin(), pending.begin() + count);
        return true;
    }
    return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
}

} // namespace

RelayMarkOutcome markRelayedFromSSH(int daemonFileDescriptor, std::chrono::milliseconds budget)
{
    constexpr std::uint64_t correlation = 1;
    const auto mark = Protocol::Local::encodeLocalRemoteRelayRequest(correlation);
    if (!mark)
        return RelayMarkOutcome::Unconfirmed;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    // Waited for before every write and every read, so a blocking descriptor
    // cannot outlast the budget in either direction.
    const auto waitFor = [&](short events) {
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return false;
            pollfd watch { .fd = daemonFileDescriptor, .events = events, .revents = 0 };
            const int ready = ::poll(&watch, 1, static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining.count(), 1000)));
            if (ready > 0)
                return (watch.revents & (events | POLLHUP | POLLERR)) != 0;
            if (ready < 0 && errno != EINTR)
                return false;
        }
    };
    std::size_t written = 0;
    while (written < mark->size()) {
        if (!waitFor(POLLOUT))
            return RelayMarkOutcome::Unconfirmed;
        const ssize_t count = ::write(daemonFileDescriptor, mark->data() + written, mark->size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return RelayMarkOutcome::Unconfirmed;
    }
    std::array<std::uint8_t, Protocol::Local::localControlReplyBytes> reply {};
    std::size_t received = 0;
    while (received < reply.size()) {
        if (!waitFor(POLLIN))
            return RelayMarkOutcome::Unconfirmed;
        const ssize_t count = ::read(daemonFileDescriptor, reply.data() + received, reply.size() - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return RelayMarkOutcome::Unconfirmed;
    }
    const auto acknowledged = Protocol::Local::decodeLocalRemoteRelayReply(reply);
    if (!acknowledged || acknowledged->correlation != correlation)
        return RelayMarkOutcome::Unconfirmed;
    return acknowledged->isMarked ? RelayMarkOutcome::Marked : RelayMarkOutcome::Unrecognised;
}

AttachedDaemon attachedDaemonOf(std::string_view pinnedSocket, std::string_view ownLeaf)
{
    return !ownLeaf.empty() && pinnedSocket == ownLeaf ? AttachedDaemon::OwnBinary : AttachedDaemon::Named;
}

std::string relayMarkRefusal(RelayMarkOutcome outcome, std::string_view selfPath, AttachedDaemon daemon)
{
    const std::string_view what = outcome == RelayMarkOutcome::Unrecognised
        ? "The host didn’t recognize this program as its own, so the connection over SSH wasn’t attached."
        : "The host didn’t confirm the connection over SSH, so it wasn’t attached.";
    if (daemon == AttachedDaemon::OwnBinary)
        return std::string(what) + (outcome == RelayMarkOutcome::Unconfirmed ? " Try again." : "");
    return std::string(what) + " If it runs an older version of Ubiquitty, run " + Transport::shellSingleQuote(selfPath)
        + " --ensure-daemon on the host: this version starts and takes over that version’s sessions without ending them. Then connect again.";
}

int relayAttach(int daemonFileDescriptor, int inputFileDescriptor, int outputFileDescriptor,
    std::chrono::milliseconds writeFailureDrainBudget)
{
    assert(writeFailureDrainBudget.count() > 0);
    std::vector<std::uint8_t> toDaemon;
    std::vector<std::uint8_t> toOutput;
    bool isInputOpen = true;
    bool isDaemonOpen = true;
    // Presence is the output-only teardown state. Ordinary forwarding never
    // reads the clock, and no subsequent progress can restart this deadline.
    std::optional<std::chrono::steady_clock::time_point> outputDrainDeadline;

    for (;;) {
        int timeout = heartbeatMilliseconds;
        if (outputDrainDeadline) {
            const auto remaining = *outputDrainDeadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero())
                break;
            timeout = static_cast<int>(std::min<long long>(heartbeatMilliseconds,
                std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
        }

        const bool canReadInput = isInputOpen && !outputDrainDeadline && toDaemon.size() < pendingCapacity;
        const bool canReadDaemon = isDaemonOpen && toOutput.size() < pendingCapacity;
        const bool canWriteDaemon = !outputDrainDeadline && !toDaemon.empty();
        std::array<pollfd, 3> descriptors { {
            { canReadInput ? inputFileDescriptor : -1, POLLIN, 0 },
            { canReadDaemon || canWriteDaemon ? daemonFileDescriptor : -1,
                static_cast<short>((canReadDaemon ? POLLIN : 0) | (canWriteDaemon ? POLLOUT : 0)), 0 },
            { toOutput.empty() ? -1 : outputFileDescriptor, POLLOUT, 0 },
        } };
        // A finite heartbeat also observes named-FIFO EOF on macOS, which
        // does not always wake poll. A full read buffer removes its fd from
        // poll: HUP is returned even when no events were requested.
        const int polled = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (canReadInput)
            readPending(inputFileDescriptor, toDaemon, isInputOpen);
        if (canReadDaemon)
            readPending(daemonFileDescriptor, toOutput, isDaemonOpen);
        // HUP is only a reason to read. Reaching the buffer cap is not EOF;
        // the kernel may still hold the rest of a final reply.
        if (!outputDrainDeadline && !writePending(daemonFileDescriptor, toDaemon)) {
            outputDrainDeadline = std::chrono::steady_clock::now() + writeFailureDrainBudget;
            toDaemon.clear();
        }
        if (!writePending(outputFileDescriptor, toOutput))
            break;

        if (!isDaemonOpen && toOutput.empty())
            break;
        // A normal input EOF detaches without waiting for a surviving
        // session. Failed-write teardown must first drain its reply instead.
        if (!outputDrainDeadline && !isInputOpen && toDaemon.empty())
            break;
    }
    return 0;
}

} // namespace Ubiquitty::Host
