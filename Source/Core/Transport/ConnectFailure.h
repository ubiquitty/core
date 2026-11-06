// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ConnectFailure.h — why a connection to a host never opened, as one sentence.
//
// A frontend's network stack reports a failed connect as a system error code
// wrapped in its own types. The wrapper is the frontend's to unwrap, because
// only it knows its types; what the code means to a person is decided here, so
// every frontend that dials a host says the same thing about the same refusal.
//
// The sentence names the host and the port the person typed and the one fact
// that tells them where to look: nothing listening, nothing answering, no
// route, or no such name. The raw error belongs in a log, never in this text.

#include "Base/Types.h"

#include <cerrno>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Transport {

/// The one fact a failed connect tells a person about where to look.
enum class ConnectFailure : uint8_t {
    /// The host answered and nothing was listening on the port.
    Refused,
    /// Nothing answered before the attempt gave up.
    TimedOut,
    /// The connection opened and the host dropped it before it was used.
    Reset,
    /// The host has no route from this network.
    HostUnreachable,
    /// This device has no network that reaches the host.
    NetworkUnreachable,
    /// No address could be found for the name.
    NameNotResolved,
    /// A failure with no more specific reading.
    Other,
};

/// Reads a connect's system error code as the fact it reports.
///
/// - Parameter errorCode: The `errno` the connect failed with.
/// - Returns: The failure it reports, ``ConnectFailure::Other`` for a code
///   with no more specific reading.
[[nodiscard]] constexpr ConnectFailure classifyConnectError(int errorCode)
{
    switch (errorCode) {
    case ECONNREFUSED:
        return ConnectFailure::Refused;
    case ETIMEDOUT:
        return ConnectFailure::TimedOut;
    case ECONNRESET:
    case ECONNABORTED:
        return ConnectFailure::Reset;
    case EHOSTUNREACH:
#ifdef EHOSTDOWN
    // WASI defines no EHOSTDOWN.
    case EHOSTDOWN:
#endif
        return ConnectFailure::HostUnreachable;
    case ENETUNREACH:
    case ENETDOWN:
        return ConnectFailure::NetworkUnreachable;
    default:
        return ConnectFailure::Other;
    }
}

/// Says whether a failed connect is worth dialling again without asking the
/// person.
///
/// A refusal is the host's own answer: it is up and nothing listens on the
/// port, and the next attempt hears the same thing. Every other failure is
/// also what a path that is still coming up reports — a VPN tunnel finishing
/// its handshake, a radio waking, a resolver not reachable yet — so the next
/// attempt may land where this one did not.
///
/// - Parameter failure: What the connect reported.
/// - Returns: Whether another attempt may succeed where this one failed.
[[nodiscard]] constexpr bool isTransientConnectFailure(ConnectFailure failure)
{
    return failure != ConnectFailure::Refused;
}

/// Returns how long to wait before dialling a host again after a transient
/// failure, or nothing once the attempts are spent.
///
/// Bounded, because the person is watching a spinner: a path that has not come
/// up after three dials is down, and saying so is better than waiting longer.
///
/// - Parameter failedAttempts: How many dials have failed so far, counting the
///   first as 1.
/// - Returns: The seconds to wait, or nothing when the failure is to be shown.
[[nodiscard]] constexpr std::optional<double> connectRetryDelaySeconds(unsigned failedAttempts)
{
    switch (failedAttempts) {
    case 1:
        return 1.0;
    case 2:
        return 2.0;
    default:
        return std::nullopt;
    }
}

/// Says why a connection to a host did not open, in one sentence.
///
/// - Parameters:
///   - failure: What the connect reported.
///   - host: The host as the person typed it.
///   - port: The port dialled.
/// - Returns: A sentence naming the host, and the port where it matters.
[[nodiscard]] inline std::string describeConnectFailure(ConnectFailure failure, std::string_view host, uint16_t port)
{
    const std::string name(host);
    const std::string portText = std::to_string(port);
    switch (failure) {
    case ConnectFailure::Refused:
        return name + " refused the connection on port " + portText + ".";
    case ConnectFailure::TimedOut:
        return name + " did not answer on port " + portText + ".";
    case ConnectFailure::Reset:
        return name + " closed the connection on port " + portText + " before it opened.";
    case ConnectFailure::HostUnreachable:
        return name + " cannot be reached from this network.";
    case ConnectFailure::NetworkUnreachable:
        return "This device has no network that reaches " + name + ".";
    case ConnectFailure::NameNotResolved:
        return "No host named " + name + " could be found.";
    case ConnectFailure::Other:
        break;
    }
    return "The connection to " + name + " on port " + portText + " failed.";
}

} // namespace Ubiquitty::Transport
