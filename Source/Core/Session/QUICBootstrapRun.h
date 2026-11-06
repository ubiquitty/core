// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Runs a host's QUIC bootstrap command and decides where its carrier dials.
//
// The command prints the daemon's port, one-time token and certificate pin on
// standard output. Over ssh it also says, on standard error, which address
// ssh connected to (`SSHReach.h`), and that is the address the carrier dials:
// the token and the pin belong to the daemon that run reached, and resolving
// the host's name again can name another machine. A run that named no
// address offers no carrier, with the reason, and the session stays on ssh.
//
// What standard error holds is read here and nowhere else: it is never shown
// to the person, and it is kept to a bound.
//
// Blocking: each function waits on its subprocess's pipes, so it runs off the
// event loop, and a waker ends the wait.

#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/SSHReach.h"
#include "PAL/Poll.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// The most of a bootstrap's standard output that is read: a line is under
/// 200 bytes.
inline constexpr std::size_t quicBootstrapOutputLimit = 8192;

/// The most of a run's standard error that is kept. An ssh transcript under
/// `-v` names its connection within its first lines; a configuration with
/// many included files pushes those further down, and the failure a miss is
/// classified by is the transcript's last line.
inline constexpr std::size_t sshTranscriptLimit = std::size_t { 64 } << 10;

/// How long past the host's line a probe of a shared connection is still
/// waited for, in seconds. The probe runs beside the bootstrap and is over in
/// one connect, so it has normally answered by then; one that has not costs
/// the session this much and no more. Its whole life is bounded by ssh's own
/// connect timeout.
inline constexpr int reachProbeGraceSeconds = 2;

/// How a bootstrap run ended.
enum class QUICBootstrapRead : std::uint8_t {
    /// A complete line with a port, a token and a certificate pin, and an
    /// address to dial.
    Answered,
    /// The host was reached and offers no carrier: it printed no usable
    /// line (``QUICBootstrapMiss``), or its line came by a route that names
    /// no address. Asking again gets the same answer.
    Declined,
    /// The host was never reached.
    Unreached,
    /// The bootstrap was pinned to a daemon nothing listens for any longer
    /// (``instanceGoneSentinel``). Asking that daemon again gets the same
    /// answer, and so does its stream.
    InstanceGone,
};

/// Reports whether a bootstrap's output says its pinned daemon is gone: a
/// line that is ``instanceGoneSentinel``, a space and a socket name.
///
/// - Parameter output: What the bootstrap printed on standard output.
/// - Returns: Whether one line says so.
[[nodiscard]] inline bool reportsInstanceGone(std::string_view output)
{
    std::size_t at = 0;
    while (at < output.size()) {
        const auto end = output.find('\n', at);
        const auto line = output.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? output.size() : end + 1;
        if (readInstanceGoneLine(line).kind == InstanceGoneLine::Gone)
            return true;
    }
    return false;
}

/// What a bootstrap run found.
struct QUICBootstrapRun {
    QUICBootstrapRead read = QUICBootstrapRead::Declined;
    /// The dial information, its `reachedAddress` filled in, for `Answered`.
    Protocol::Sync::QUICBootstrapInfo info;
    /// Why no carrier is offered, for the transport's diagnostics; empty for
    /// `Answered`.
    std::string refusal;
};

/// A command being run, and what it has written so far.
struct RunningCommand {
    PAL::PipedCommand command;
    std::string output;
    std::string standardError;
    bool isOutputOpen = false;
    bool isStandardErrorOpen = false;

    /// Starts a command with its standard error captured.
    ///
    /// - Parameter line: The shell command.
    /// - Returns: The command, not running when it could not be started.
    [[nodiscard]] static RunningCommand start(const std::string& line)
    {
        RunningCommand running;
        running.command = PAL::spawnPipedCommand(line, PAL::CommandStandardError::Capture);
        if (!running.command)
            return running;
        running.command.input.reset();
        running.isOutputOpen = true;
        running.isStandardErrorOpen = running.command.standardError != nullptr;
        return running;
    }

    /// Whether the command was started and has not been ended.
    [[nodiscard]] bool isRunning() const { return static_cast<bool>(command.process); }

    /// Reads what the command's pipes hold now, keeping no more than the
    /// bounds. Standard error first: what ssh says of its connection comes
    /// before the line the host prints, and is wanted beside it.
    void drain()
    {
        std::array<char, 512> buffer {};
        const auto read = [&buffer](PAL::PollableByteStream& stream, std::string& into, std::size_t limit) {
            for (;;) {
                const int count = stream.read(std::span<char>(buffer));
                if (count == static_cast<int>(PAL::StreamStatus::Again))
                    return true;
                if (count <= 0)
                    return false;
                const auto kept = std::min(static_cast<std::size_t>(count), limit - std::min(limit, into.size()));
                into.append(buffer.data(), kept);
            }
        };
        if (isStandardErrorOpen)
            isStandardErrorOpen = read(*command.standardError, standardError, sshTranscriptLimit);
        if (isOutputOpen)
            isOutputOpen = read(*command.output, output, quicBootstrapOutputLimit + 1);
    }

    /// Ends the command now and closes its pipes.
    void end()
    {
        command = {};
        isOutputOpen = false;
        isStandardErrorOpen = false;
    }
};

/// Runs a host's QUIC bootstrap and says where its carrier dials.
///
/// The bootstrap decides how long the run takes. A probe of a shared
/// connection runs beside it, from the moment ssh says it is sharing one, and
/// is given `reachProbeGraceSeconds` past the host's line at most: a probe
/// that hangs costs the session that much and no more, and the session then
/// stays on ssh.
///
/// - Parameters:
///   - command: The plan's bootstrap command.
///   - quicHost: The plan's QUIC host: a loopback address, or the host's
///     name.
///   - sharedConnections: Where shared connections are known to reach,
///     which this run reads and adds to; null keeps nothing between runs.
///   - stop: Ends the run when requested.
///   - waker: Wakes the run's waits when the stop is requested.
/// - Returns: What the run found. A run that was stopped is the caller's to
///   discard, whatever it read.
[[nodiscard]] inline QUICBootstrapRun runQUICBootstrap(const std::string& command, std::string_view quicHost, SharedConnectionReaches* sharedConnections, std::stop_token stop, PAL::Waker& waker)
{
    using Clock = std::chrono::steady_clock;
    QUICBootstrapRun run;
    RunningCommand bootstrap = RunningCommand::start(command);
    if (!bootstrap.isRunning()) {
        run.refusal = "the host couldn’t be asked to offer QUIC";
        return run;
    }
    const bool isLoopback = isLoopbackAddress(quicHost);
    RunningCommand probe;
    // Whether the shared connection was looked into: once, when ssh names it.
    bool hasConsideredProbe = isLoopback;
    std::optional<SharedConnectionIdentity> shared;
    std::optional<std::string> probedAddress;
    std::optional<Clock::time_point> lineAt;
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(quicBootstrapDeadlineSeconds);
    Clock::time_point probeDeadline = deadline;
    while (!stop.stop_requested()) {
        bootstrap.drain();
        if (!hasConsideredProbe) {
            if (const SSHReach reach = readSSHReach(bootstrap.standardError); reach.kind == SSHReachKind::Multiplexed) {
                hasConsideredProbe = true;
                shared = identifySharedConnection(reach.sharedConnectionPath);
                if (shared && sharedConnections)
                    probedAddress = sharedConnections->find(*shared);
                if (!probedAddress) {
                    if (const std::string probeCommand = substituteReachProbe(command); probeCommand != command) {
                        probe = RunningCommand::start(probeCommand);
                        probeDeadline = Clock::now() + std::chrono::seconds(sshConnectTimeoutSeconds);
                    }
                }
            }
        }
        if (probe.isRunning()) {
            probe.drain();
            if (const SSHReach reach = readSSHReach(probe.standardError); reach.kind == SSHReachKind::Direct) {
                // The connection is named: the probe has nothing more to do,
                // and is ended before it gets as far as a key exchange.
                probe.end();
                probedAddress = reach.address;
                if (shared && sharedConnections)
                    sharedConnections->note(*shared, reach.address);
            } else if ((!probe.isOutputOpen && !probe.isStandardErrorOpen) || Clock::now() >= probeDeadline) {
                probe.end();
            }
        }
        const bool hasLine = bootstrap.output.find('\n') != std::string::npos;
        if (hasLine && !lineAt)
            lineAt = Clock::now();
        const bool isBootstrapOver = hasLine || bootstrap.output.size() > quicBootstrapOutputLimit || (!bootstrap.isOutputOpen && !bootstrap.isStandardErrorOpen);
        Clock::time_point until = deadline;
        if (isBootstrapOver) {
            // What the host said is in hand; only a probe still running is
            // waited for, and only for its grace.
            if (!probe.isRunning())
                break;
            until = std::min(probeDeadline, lineAt.value_or(Clock::now()) + std::chrono::seconds(reachProbeGraceSeconds));
        } else if (probe.isRunning()) {
            until = std::min(deadline, probeDeadline);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        if (remaining <= 0) {
            if (isBootstrapOver || until == deadline)
                break;
            // The probe's own time is up and the bootstrap's is not.
            probe.end();
            continue;
        }
        std::array<PAL::Watch, 4> watches {};
        std::size_t watchCount = 0;
        watches[watchCount++] = { &waker.handle(), PAL::Ready::Readable };
        if (bootstrap.isOutputOpen && !isBootstrapOver)
            watches[watchCount++] = { &bootstrap.command.output->handle(), PAL::Ready::Readable };
        if (bootstrap.isStandardErrorOpen && !isBootstrapOver)
            watches[watchCount++] = { &bootstrap.command.standardError->handle(), PAL::Ready::Readable };
        if (probe.isStandardErrorOpen)
            watches[watchCount++] = { &probe.command.standardError->handle(), PAL::Ready::Readable };
        if (PAL::waitForReadiness(std::span(watches).first(watchCount), static_cast<int>(remaining)) < 0)
            break;
    }
    probe.end();

    const bool hasLine = bootstrap.output.size() <= quicBootstrapOutputLimit && Protocol::Sync::parseQUICBootstrapLine(bootstrap.output, run.info);
    if (!hasLine) {
        run.info = {};
        if (bootstrap.output.size() <= quicBootstrapOutputLimit && reportsInstanceGone(bootstrap.output)) {
            run.read = QUICBootstrapRead::InstanceGone;
            run.refusal = "the Ubiquitty host this connection used is no longer running";
        } else if (classifyQUICBootstrapMiss(bootstrap.standardError) == QUICBootstrapMiss::Unreached) {
            run.read = QUICBootstrapRead::Unreached;
            run.refusal = "the host wasn’t reached to set up QUIC";
        } else {
            run.refusal = "the host didn’t say how to connect to it over QUIC";
        }
        return run;
    }
    if (isLoopback) {
        run.info.reachedAddress = std::string(quicHost);
        run.read = QUICBootstrapRead::Answered;
        return run;
    }
    const SSHReach reach = readSSHReach(bootstrap.standardError);
    if (reach.kind == SSHReachKind::Direct) {
        run.info.reachedAddress = reach.address;
    } else if (probedAddress) {
        // Only a shared connection is ever probed.
        run.info.reachedAddress = *probedAddress;
    } else {
        run.info = {};
        run.refusal = std::string(sshReachRefusal(reach.kind));
        return run;
    }
    run.read = QUICBootstrapRead::Answered;
    return run;
}

} // namespace Ubiquitty::Session
