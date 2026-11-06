// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HostHandover.h"

#include "Core/ClusterPool.h"
#include "Core/Image/TerminalImage.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Core/RunTextStore.h"
#include "Core/Tek/Tek4014.h"
#include "PAL/Log.h"
#include "PAL/PTYFacts.h"
#include "PAL/Stream.h"
#include "Platform/Host/HostPaths.h"
#include "Platform/POSIX/HandoverLease.h"
#include "Platform/POSIX/HandoverTransport.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <limits>
#include <new>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#ifdef UBIQUITTY_QUIC
#include "Platform/Host/HostQUIC.h"
#endif

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
using Clock = std::chrono::steady_clock;
using Refusal = Sync::HandoverRefuseReason;

constexpr std::size_t mebibyte = std::size_t { 1 } << 20;

// Each section admits what a session holds by default at the host's largest
// geometry (the default scrollback at full width, a full image budget). A
// host past them refuses the whole handover and keeps serving beside its
// successor rather than losing part of a pane.
constexpr Sync::HandoverTerminalLimits terminalLimits {
    .maxRecordBytes = 160 * mebibyte,
    .maxCheckpointBytes = 64 * mebibyte,
    .maxScreenCellBytes = 64 * mebibyte,
    .maxHostStateBytes = 8 * mebibyte,
    .maxLinkBytes = mebibyte,
    .maxParserBytes = 4096,
    .images = { .maxSectionBytes = 64 * mebibyte, .maxPixelBytes = Image::terminalImageBytesBudgetDefault },
    .history = { .maxSectionBytes = 64 * mebibyte, .maxCapacity = 1'000'000, .maxRows = 1'000'000, .maxCells = 16 * mebibyte },
    .text = { .maxSectionBytes = 4 * mebibyte, .maxClusterEntries = ClusterPool::maxEntries, .maxRunSlots = RunTextStore::maxEntries, .maxCodePoints = static_cast<std::uint32_t>(mebibyte) },
    .visual = { .maxSectionBytes = 256 * 1024, .maxTekPrimitives = Tek::tekMaxPrimitives },
    .sounds = { .maxSectionBytes = 32 * mebibyte, .maxPCMBytes = 28 * mebibyte },
};

constexpr HostHandoverLimits limits {
    .session = {
        .pane = {
            .terminal = terminalLimits,
            .publication = { .maxSectionBytes = 4 * mebibyte, .maxPens = 65536, .maxLinks = 65536, .maxClusters = 65536, .maxHistoryHashes = 1'048'576 },
            .maxRecordBytes = 192 * mebibyte,
            .maxSourceBytes = 16 * mebibyte,
            .maxSessionBytes = mebibyte,
            .maxEffectRetainedBytes = 4 * mebibyte,
            .maxAcceptedPTYBytes = 32 * mebibyte,
            .maxLandedPastePaths = 64,
        },
        .effects = { .maxSectionBytes = 4 * mebibyte, .maxIdentityBytes = 4096, .maxNotificationBytes = 65536, .maxClipboardBytes = 2 * mebibyte },
        .runtime = { .maxSectionBytes = 4096, .maxPastes = 64 },
        .configuration = { .maxSectionBytes = mebibyte, .maxStringBytes = 65536, .maxCollectionEntries = 4096, .maxDecodedBytes = 4 * mebibyte },
        .maxRecordBytes = 224 * mebibyte,
    },
    .tables = {
        .maxRecordBytes = 16 * mebibyte,
        .attachments = { .maxSectionBytes = 4 * mebibyte, .maxAttachments = 256, .maxPaneRecords = 1024, .maxInFlightGenerations = 1024, .maxRowHashes = 65536, .maxRichTextSerials = 4096, .maxStringBytes = 65536 },
        .workspaces = { .maxSectionBytes = 4 * mebibyte, .maxWorkspaces = 256, .maxWindows = 512, .maxTabs = 1024, .maxPanes = 1024, .maxStructures = 256, .maxStringBytes = 65536, .maxRecordBytes = 65536 },
        .authority = { .maxSectionBytes = mebibyte, .maxGrants = 256, .maxConsentEntries = 256, .maxQUICTokens = 64, .maxStandingAgents = 256, .maxStringBytes = 65536 },
        .globals = { .maxSectionBytes = 2 * mebibyte, .maxEvents = 4096, .maxClosures = 256, .maxTerminations = 256, .maxStringBytes = 65536, .configuration = { .maxSectionBytes = mebibyte, .maxStringBytes = 65536, .maxCollectionEntries = 4096, .maxDecodedBytes = 4 * mebibyte } },
    },
    .maxSessions = 256,
    .maxAggregateBytes = 512 * mebibyte,
};

/// The largest control record either end reads: a `PaneFollows`.
constexpr std::size_t maxControlRecordBytes = 64;

constexpr Sync::HandoverChannelLimits channelLimits {
    .maxBodyVersion = Sync::handoverBodyVersion,
    .maxPanes = static_cast<std::uint16_t>(limits.maxSessions),
    .maxPaneBytes = static_cast<std::uint32_t>(limits.session.maxRecordBytes),
    .maxTableBytes = static_cast<std::uint32_t>(limits.tables.maxRecordBytes),
};
static_assert(limits.maxSessions <= std::numeric_limits<std::uint16_t>::max());
static_assert(limits.session.maxRecordBytes <= std::numeric_limits<std::uint32_t>::max());
static_assert(limits.tables.attachments.maxAttachments == OSC::TextSizing::maxInheritedFaceOffersPerDaemon);

/// The daemon's monotonic clock, in the microseconds its owners count.
std::uint64_t monotonicMicroseconds()
{
    timespec now {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000ull + static_cast<std::uint64_t>(now.tv_nsec) / 1000ull;
}

int remainingMilliseconds(Clock::time_point deadline)
{
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max()));
}

std::int64_t elapsedMilliseconds(Clock::time_point since)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

[[nodiscard]] bool sendRecord(int channel, std::span<const std::uint8_t> bytes, std::span<const int> descriptors, Clock::time_point deadline)
{
    const int remaining = remainingMilliseconds(deadline);
    return remaining && POSIX::sendHandoverRecord(channel, bytes, descriptors, remaining).has_value();
}

[[nodiscard]] bool sendMessage(int channel, const Sync::HandoverMessage& message, Clock::time_point deadline)
{
    return sendRecord(channel, Sync::encodeHandoverMessage(message), {}, deadline);
}

/// Receives one control record of exactly `descriptors` handles and decodes it.
[[nodiscard]] std::optional<Sync::HandoverMessage> receiveMessage(int channel, std::size_t maxBytes, std::size_t descriptors,
    Clock::time_point deadline, POSIX::HandoverRecord* keep = nullptr)
{
    const int remaining = remainingMilliseconds(deadline);
    if (!remaining)
        return std::nullopt;
    auto record = POSIX::receiveHandoverRecord(channel, maxBytes, descriptors, remaining);
    if (!record)
        return std::nullopt;
    auto decoded = Sync::decodeHandoverMessage(record->bytes(), channelLimits);
    if (!decoded)
        return std::nullopt;
    if (keep)
        *keep = std::move(*record);
    return *decoded;
}

/// Receives one state record of exactly the announced length, without handles.
[[nodiscard]] std::optional<Sync::HandoverBytes> receiveBody(int channel, std::size_t bytes, Clock::time_point deadline)
{
    const int remaining = remainingMilliseconds(deadline);
    if (!remaining)
        return std::nullopt;
    auto record = POSIX::receiveHandoverRecord(channel, bytes, 0, remaining);
    if (!record || record->length != bytes)
        return std::nullopt;
    return Sync::HandoverBytes { std::move(record->storage), record->length };
}

/// Reads the length of the next record without consuming it or any handle.
///
/// Only the length prefix is peeked: a stream socket returns no byte of a
/// record past the segment that carries its descriptors, so a peek of the
/// payload can wait on bytes that never join the prefix.
/// - Returns: The record's payload length, or nothing when the channel
///   closed, failed or stayed silent past the deadline.
std::optional<std::uint32_t> peekRecordLength(int channel, Clock::time_point deadline)
{
    std::array<std::uint8_t, sizeof(std::uint32_t)> prefix {};
    for (;;) {
        const int remaining = remainingMilliseconds(deadline);
        if (!remaining)
            return std::nullopt;
        pollfd readable { channel, POLLIN, 0 };
        const int polled = ::poll(&readable, 1, remaining);
        if (polled < 0 && errno != EINTR)
            return std::nullopt;
        if (polled <= 0)
            continue;
        const auto peeked = ::recv(channel, prefix.data(), prefix.size(), MSG_PEEK | MSG_DONTWAIT);
        if (peeked == static_cast<ssize_t>(prefix.size())) {
            std::uint32_t length = 0;
            for (std::size_t index = 0; index < prefix.size(); ++index)
                length |= static_cast<std::uint32_t>(prefix[index]) << (8 * index);
            return length;
        }
        if (peeked == 0 || (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return std::nullopt;
        // A partial prefix keeps the socket readable; wait for the rest.
        (void)::poll(nullptr, 0, 1);
    }
}

Refusal refusalOf(HostHandover::Error error)
{
    return error == HostHandover::Error::NewerVersion ? Refusal::NewerBody : Refusal::Malformed;
}

/// Names a state body's error for the host log.
std::string_view describe(HostHandover::Error error)
{
    switch (error) {
    case HostHandover::Error::NewerVersion:
        return "it is newer than this host reads";
    case HostHandover::Error::Malformed:
        return "it does not decode";
    case HostHandover::Error::OverCapacity:
        return "it is larger than this host takes";
    case HostHandover::Error::DigestMismatch:
        return "its digest does not match";
    case HostHandover::Error::AllocationRefused:
        return "it could not be allocated";
    case HostHandover::Error::NotAtGround:
        return "it is not at ground";
    }
    return "unknown";
}

std::string_view nameOf(Refusal reason)
{
    switch (reason) {
    case Refusal::NotSuccessor:
        return "not the successor";
    case Refusal::NewerBody:
        return "a newer body";
    case Refusal::PaneBusy:
        return "a pane did not reach ground";
    case Refusal::Malformed:
        return "malformed";
    case Refusal::Geometry:
        return "geometry";
    case Refusal::Ancestry:
        return "a pane's process ancestry";
    case Refusal::ApprovalPending:
        return "an agent's change waits for a person's approval";
    case Refusal::GrantPending:
        return "a capability question waits for an answer";
    case Refusal::OutputUnsent:
        return "a client has not yet been sent all it is owed";
    case Refusal::RequestPending:
        return "a client's request is part-way";
    case Refusal::AdoptPending:
        return "a session adoption is under way";
    }
    return "unknown";
}

/// The kernel's session leader and its parent for one held master.
struct LeaderFacts {
    std::uint32_t leader = 0;
    std::uint32_t parent = 0;
};

std::optional<LeaderFacts> leaderOf(int master)
{
    if (master < 0)
        return std::nullopt;
    const pid_t leader = ::tcgetsid(master);
    if (leader <= 1)
        return std::nullopt;
    const auto snapshot = PAL::ptyProcessSnapshot(leader);
    if (!snapshot || snapshot->processGroupID != leader || snapshot->parentProcessID <= 0)
        return std::nullopt;
    return LeaderFacts { static_cast<std::uint32_t>(leader), static_cast<std::uint32_t>(snapshot->parentProcessID) };
}

/// One predecessor listening socket, with the path it is bound at.
struct ReleasedListener {
    PAL::UnixFileDescriptor descriptor;
    std::string path;
};

/// Admits the predecessor's listening sockets this daemon will accept on.
///
/// Each must be a listening Unix stream socket bound at a host socket leaf
/// in this daemon's own control directory, other than this daemon's socket,
/// each path once. Any other socket refuses the whole set.
/// - Parameters:
///   - ownSocketPath: This daemon's bound socket path.
///   - received: The listeners Release carried, consumed.
/// - Returns: Nonblocking owned listeners, or nothing on any mismatch.
std::optional<std::vector<ReleasedListener>> admitListeners(std::string_view ownSocketPath, std::span<PAL::UnixFileDescriptor> received)
{
    const auto slash = ownSocketPath.find_last_of('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const auto directory = ownSocketPath.substr(0, slash);
    std::vector<ReleasedListener> admitted;
    admitted.reserve(received.size());
    for (auto& descriptor : received) {
        const auto bound = descriptor ? listeningSocketPath(descriptor.get()) : std::nullopt;
        if (!bound)
            return std::nullopt;
        const std::string& path = *bound;
        const auto pathSlash = path.find_last_of('/');
        if (pathSlash == std::string::npos || std::string_view(path).substr(0, pathSlash) != directory || path == ownSocketPath
            || !isHostSocketLeaf(std::string_view(path).substr(pathSlash + 1))
            || std::ranges::any_of(admitted, [&](const auto& listener) { return listener.path == path; }))
            return std::nullopt;
        const int flags = ::fcntl(descriptor.get(), F_GETFL);
        if (flags < 0 || ::fcntl(descriptor.get(), F_SETFL, flags | O_NONBLOCK) != 0)
            return std::nullopt;
        admitted.push_back({ std::move(descriptor), path });
    }
    return admitted;
}

/// Removes the path of a socket this host let go, once nothing serves it.
///
/// The path's startup lock is held throughout, so no host is binding a new
/// socket there, and the path is removed only when nothing listens on it.
/// - Parameter path: The let-go socket's path, its listener already closed.
void removeAbandonedSocket(const std::string& path)
{
    PAL::UnixFileDescriptor lock(::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!lock || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0)
        return;
    if (!isSocketPathServed(path.c_str()))
        ::unlink(path.c_str());
}

/// Whether a received socket is a bound datagram socket `startQUIC` can adopt.
bool isBoundDatagramSocket(int descriptor)
{
    int type = 0;
    socklen_t typeLength = sizeof(type);
    return ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &typeLength) == 0 && type == SOCK_DGRAM;
}

} // namespace

const HostHandoverLimits& HostHandover::transactionLimits()
{
    return limits;
}

HostHandover::Candidate::Candidate(Daemon& destination)
    : m_destination(destination)
{
}

HostHandover::Candidate::~Candidate() = default;

std::expected<HostHandoverSnapshot, HostHandover::Error> HostHandover::capture(
    const Daemon& daemon, std::uint64_t now, const HostHandoverLimits& limits)
{
    if (!daemon.isQuiesced())
        return std::unexpected(Error::NotAtGround);
    if (daemon.m_sessions.size() > limits.maxSessions)
        return std::unexpected(Error::OverCapacity);
    HostHandoverSnapshot result { .predecessor = daemon.m_hostInstanceID, .sessions = {}, .tables = {} };
    auto tables = HostTableHandover::capture(daemon, now, limits.tables);
    if (!tables)
        return std::unexpected(tables.error());
    if (tables->size > limits.maxAggregateBytes)
        return std::unexpected(Error::OverCapacity);
    std::size_t used = tables->size;
    result.tables = std::move(*tables);
    for (const auto& session : daemon.m_sessions) {
        auto sessionLimits = limits.session;
        sessionLimits.maxRecordBytes = std::min(sessionLimits.maxRecordBytes, limits.maxAggregateBytes - used);
        auto record = SessionHandover::capture(*session, sessionLimits);
        if (!record)
            return std::unexpected(record.error());
        used += record->size;
        result.sessions.push_back({ session->screenSource->paneID(), std::move(*record) });
    }
    return result;
}

std::expected<std::unique_ptr<HostHandover::Candidate>, HandoverPrepareRefusal> HostHandover::prepare(
    Daemon& daemon, HostHandoverSnapshot&& snapshot, std::vector<std::unique_ptr<PAL::Stream>> streams,
    Protocol::Sync::ID128 predecessor, std::uint64_t now, std::uint64_t nowUnix,
    std::string_view predecessorSocketPath, const HostHandoverLimits& limits)
{
    const auto refuse = [](Error error, std::string check) { return std::unexpected(HandoverPrepareRefusal { error, std::move(check) }); };
    // Every received endpoint is an inherited child, including endpoints in a
    // rejected suffix that never reaches SessionHandover::restore.
    for (const auto& stream : streams) {
        if (stream)
            stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    }
    if (!daemon.m_sessions.empty() || !daemon.m_clients.empty() || daemon.m_quiescingPanes)
        return refuse(Error::Malformed, "this host already holds sessions or clients");
    if (snapshot.predecessor != predecessor || predecessor == Protocol::Sync::ID128 {})
        return refuse(Error::Malformed, "its state names another host than the one admitted");
    if (snapshot.sessions.size() != streams.size())
        return refuse(Error::Malformed, std::format("it sent {} pane records for {} masters", snapshot.sessions.size(), streams.size()));
    if (snapshot.sessions.size() > limits.maxSessions || snapshot.tables.size > limits.maxAggregateBytes)
        return refuse(Error::OverCapacity, "it holds more panes or table bytes than this host takes");
    std::size_t used = snapshot.tables.size;
    for (std::size_t index = 0; index < snapshot.sessions.size(); ++index) {
        const auto& session = snapshot.sessions[index];
        if (!streams[index] || session.paneID == Protocol::Sync::ID128 {}
            || std::ranges::any_of(std::span(snapshot.sessions).first(index), [&](const auto& previous) { return previous.paneID == session.paneID; }))
            return refuse(Error::Malformed, std::format("pane {} has no master, no id or another pane's id", index));
        if (session.record.size > limits.session.maxRecordBytes || session.record.size > limits.maxAggregateBytes - used)
            return refuse(Error::OverCapacity, std::format("pane {}'s record is larger than this host takes", index));
        used += session.record.size;
    }
    std::unique_ptr<Candidate> candidate(new (std::nothrow) Candidate(daemon));
    if (!candidate)
        return refuse(Error::AllocationRefused, "the candidate could not be allocated");
    auto tables = HostTableHandover::prepare(daemon, snapshot.tables.view(), predecessor, now, nowUnix, predecessorSocketPath, limits.tables);
    if (!tables)
        return std::unexpected(std::move(tables.error()));
    candidate->m_tables = std::move(*tables);
    for (std::size_t index = 0; index < snapshot.sessions.size(); ++index) {
        auto session = SessionHandover::restore(daemon, snapshot.sessions[index].record.view(), std::move(streams[index]), limits.session);
        if (!session)
            return refuse(session.error(), std::format("pane {}'s record does not restore: {}", index, describe(session.error())));
        if ((*session)->screenSource->paneID() != snapshot.sessions[index].paneID)
            return refuse(Error::Malformed, std::format("pane {} restores under another id than it was announced with", index));
        candidate->m_sessions.push_back(std::move(*session));
    }
    // A closed pane whose host has left the resume window costs an attachment
    // its baselines for it, not the handover.
    (void)candidate->m_tables->forgetBaselinesOfDepartedPanes(candidate->m_sessions);
    if (!candidate->m_tables->validateSessions(candidate->m_sessions))
        return refuse(Error::Malformed, "the panes and the workspaces they are placed in do not agree");
    return candidate;
}

void HostHandover::commit(Candidate&& candidate)
{
    auto& daemon = candidate.m_destination;
    assert(daemon.m_sessions.empty() && daemon.m_clients.empty() && candidate.m_tables);
    // What this host took in at startup, from hosts no longer running, joins
    // the restorable workspaces it inherits, admitted once the sessions are
    // in place so no id a session holds is given to one.
    auto takenIn = std::move(daemon.m_restorableWorkspaces);
    daemon.m_restorableWorkspaces.clear();
    HostTableHandover::commit(daemon, std::move(*candidate.m_tables));
    for (const auto& session : candidate.m_sessions) {
        session->ownsLandedPastePaths = true;
        session->stream->setProcessClosePolicy(PAL::ProcessClosePolicy::Terminate);
    }
    daemon.m_sessions = std::move(candidate.m_sessions);
    std::vector<Ubiquitty::Session::HostLayoutRecord> records;
    records.reserve(takenIn.size());
    for (auto& restorable : takenIn)
        records.push_back(std::move(restorable.record));
    daemon.admitRestorableRecords(std::move(records));
    candidate.m_tables.reset();
}

bool HostHandover::offer(Daemon& daemon, PAL::UnixFileDescriptor channel, AdmittedHandoverRequest admitted)
{
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::milliseconds(transactionTimeoutMilliseconds);
    const int socket = channel.get();
    const auto refuse = [&](Refusal reason) {
        (void)sendMessage(socket, Sync::HandoverRefuse { reason }, deadline);
        PAL::logLine(stderr, "ubiquitty-host: handover to process {} refused ({}); this host keeps serving", admitted.peerProcessID, nameOf(reason));
        return false;
    };
    PAL::logLine(stderr, "ubiquitty-host: handover requested by process {}", admitted.peerProcessID);

    // Every pane reaches ground with no client read; input waits in the kernel.
    // What keeps it from ground is named when the host knows it, so the
    // person running the install learns what to finish first.
    if (!daemon.beginHandoverQuiesce(monotonicMicroseconds()))
        return refuse(daemon.handoverBlocker().value_or(Refusal::PaneBusy));
    while (daemon.m_quiescingPanes && !daemon.isQuiesced() && remainingMilliseconds(deadline)) {
        daemon.serviceHandoverQuiesce(monotonicMicroseconds());
        if (daemon.m_quiescingPanes && !daemon.isQuiesced())
            (void)::poll(nullptr, 0, 1);
    }
    // A pane short of ground when the time ran out is the reason; an
    // operation that began during the attempt and ended it is named instead.
    if (!daemon.isQuiesced()) {
        daemon.endHandoverQuiesce();
        return refuse(daemon.handoverQuiesceBlocker().value_or(Refusal::PaneBusy));
    }
    const auto frozen = Clock::now();
    auto snapshot = capture(daemon, monotonicMicroseconds(), limits);
    if (!snapshot) {
        daemon.endHandoverQuiesce();
        return refuse(refusalOf(snapshot.error()));
    }
    // Each master and its kernel-reported leader, read before anything moves.
    struct OfferedPane {
        int master = -1;
        LeaderFacts leader;
    };
    std::vector<OfferedPane> panes;
    panes.reserve(daemon.m_sessions.size());
    for (const auto& session : daemon.m_sessions) {
        const int master = session->stream->fileDescriptor();
        const auto leader = leaderOf(master);
        if (!leader) {
            daemon.endHandoverQuiesce();
            return refuse(Refusal::Ancestry);
        }
        panes.push_back({ master, *leader });
    }
    const auto slash = daemon.m_socketPath.find_last_of('/');
    PAL::UnixFileDescriptor directory(slash == std::string::npos ? -1
                                                                 : ::open(daemon.m_socketPath.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    std::expected<POSIX::HandoverLease, POSIX::HandoverLeaseError> lease = std::unexpected(POSIX::HandoverLeaseError::InvalidArgument);
    if (directory)
        lease = POSIX::HandoverLease::create(directory.get());
    if (!lease || !lease->bindSuccessor({ static_cast<pid_t>(admitted.peerProcessID), admitted.request.requesterInstance })) {
        daemon.endHandoverQuiesce();
        return refuse(Refusal::Malformed);
    }
    // Before Release the successor cannot have claimed: cancelling the bound
    // offer while the lock is held fences any late claim, then service resumes.
    const auto resume = [&](std::string_view why) {
        (void)lease->cancelSuccessor();
        daemon.endHandoverQuiesce();
        PAL::logLine(stderr, "ubiquitty-host: handover to process {} abandoned ({}) after {} ms; this host keeps serving",
            admitted.peerProcessID, why, elapsedMilliseconds(started));
        return false;
    };

    // Every listening socket this host serves goes with its panes, up to the
    // bound, so each path a client pinned keeps an owner. Its own goes first.
    // The one its records are kept beside goes next and is never let go: a
    // successor takes the records only through that socket. The rest follow
    // newest first, and past the bound the oldest of them are let go.
    std::vector<int> listeners { daemon.m_listenFileDescriptor.get() };
    const auto keepsRecords = [&](const Daemon::InheritedListener& listener) {
        return HostTableHandover::areRecordsKeptBeside(daemon.m_layoutRecordPath, daemon.m_backgroundEventPath, listener.path);
    };
    if (const auto records = std::ranges::find_if(daemon.m_inheritedListeners, keepsRecords); records != daemon.m_inheritedListeners.end())
        listeners.push_back(records->descriptor.get());
    for (const auto& listener : daemon.m_inheritedListeners) {
        if (listeners.size() < Sync::maxHandoverListeners && !keepsRecords(listener))
            listeners.push_back(listener.descriptor.get());
    }
    bool hasDatagramSocket = false;
#ifdef UBIQUITTY_QUIC
    hasDatagramSocket = daemon.m_quic && daemon.m_quic->fileDescriptor() >= 0;
#endif
    const auto offerBytes = Sync::encodeHandoverMessage(Sync::HandoverOffer { Sync::handoverBodyVersion, snapshot->predecessor,
        static_cast<std::uint16_t>(snapshot->sessions.size()), static_cast<std::uint32_t>(snapshot->tables.size),
        static_cast<std::uint8_t>(listeners.size()), static_cast<std::uint8_t>(hasDatagramSocket ? 1 : 0) });
    const int offerTime = remainingMilliseconds(deadline);
    if (!offerTime || !lease->sendOffer(socket, offerBytes, offerTime))
        return resume("the offer did not reach the successor");
    for (std::size_t index = 0; index < snapshot->sessions.size(); ++index) {
        const auto& session = snapshot->sessions[index];
        const auto follows = Sync::encodeHandoverMessage(Sync::HandoverPaneFollows { static_cast<std::uint16_t>(index), session.paneID,
            static_cast<std::uint32_t>(session.record.size), panes[index].leader.leader, panes[index].leader.parent });
        const std::array master { panes[index].master };
        if (!sendRecord(socket, follows, master, deadline) || !sendRecord(socket, session.record.view(), {}, deadline))
            return resume("a pane did not reach the successor");
    }
    if (!sendRecord(socket, snapshot->tables.view(), {}, deadline))
        return resume("the host tables did not reach the successor");
    const auto verdict = receiveMessage(socket, maxControlRecordBytes, 0, deadline);
    if (!verdict || !std::holds_alternative<Sync::HandoverAccept>(*verdict)) {
        const auto* refusal = verdict ? std::get_if<Sync::HandoverRefuse>(&*verdict) : nullptr;
        return resume(refusal ? nameOf(refusal->reason) : std::string_view("no verdict"));
    }

    // Accepted: every connection is told, every child outlives this process,
    // and the lease the successor must claim is let go before Release.
    for (std::size_t index = 0; index < daemon.m_clients.size(); ++index) {
        if (daemon.m_clients[index].syncConnection && !daemon.m_clients[index].didOverflow)
            daemon.dropWithFarewell(index, Sync::ByeReason::HandingOver, "This host is handing its sessions to a newer host.");
    }
    const auto farewellDeadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(500));
    for (;;) {
        std::vector<pollfd> writable;
        for (std::size_t index = 0; index < daemon.m_clients.size(); ++index) {
            daemon.flushClient(index);
            if (index >= daemon.m_clients.size())
                break;
            const auto& client = daemon.m_clients[index];
            if (client.fileDescriptor && client.hasPendingOutput() && !client.quicConnection)
                writable.push_back({ client.fileDescriptor.get(), POLLOUT, 0 });
        }
        const int remaining = remainingMilliseconds(farewellDeadline);
        if (writable.empty() || !remaining)
            break;
        (void)::poll(writable.data(), static_cast<nfds_t>(writable.size()), remaining);
    }
    // A QUIC connection hears the same reason in its close, sent before the
    // socket moves; the successor serves the same port and certificate.
    PAL::UnixFileDescriptor datagramSocket;
#ifdef UBIQUITTY_QUIC
    if (hasDatagramSocket) {
        for (std::size_t index = daemon.m_clients.size(); index-- > 0;) {
            if (daemon.m_clients[index].quicConnection)
                daemon.reapClient(index);
        }
        daemon.m_quic->flush();
        datagramSocket = daemon.m_quic->releaseBoundSocket();
    }
#endif
    for (const auto& session : daemon.m_sessions)
        session->stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    const auto keepServing = [&](std::string_view why) {
        for (const auto& session : daemon.m_sessions)
            session->stream->setProcessClosePolicy(PAL::ProcessClosePolicy::Terminate);
#ifdef UBIQUITTY_QUIC
        if (datagramSocket && !daemon.m_quic->startOnBoundSocket(std::move(datagramSocket)))
            PAL::logLine(stderr, "ubiquitty-host: QUIC could not resume after the abandoned handover: {}", daemon.m_quic->lastError());
#endif
        return resume(why);
    };
    if (hasDatagramSocket && !datagramSocket)
        return keepServing("the QUIC socket could not be released");
    std::vector<int> released = listeners;
    if (datagramSocket)
        released.push_back(datagramSocket.get());
    if (!lease->release())
        return keepServing("the lease could not be released");
    if (!sendRecord(socket, Sync::encodeHandoverMessage(Sync::HandoverRelease {}), released, deadline)) {
        // An incomplete record is no Release: the successor cannot claim.
        (void)lease->tryAcquire();
        return keepServing("Release did not reach the successor");
    }
    if (const auto serving = receiveMessage(socket, maxControlRecordBytes, 0, deadline);
        !serving || !std::holds_alternative<Sync::HandoverServing>(*serving)) {
        // A successor that never claimed leaves this host the owner; one that
        // claimed owns every master even if it never said so.
        if (lease->tryAcquire() && lease->cancelSuccessor())
            return keepServing("no successor claimed the panes");
    }
    daemon.m_hasRelinquishedService = true;
    // The sockets past the bound went to nobody. Each is closed here, and its
    // path removed unless something else listens on it, so no dead socket is
    // left for a client or a later walk to try.
    std::erase_if(daemon.m_inheritedListeners, [&](Daemon::InheritedListener& listener) {
        if (std::ranges::find(listeners, listener.descriptor.get()) != listeners.end())
            return false;
        listener.descriptor.reset();
        removeAbandonedSocket(listener.path);
        return true;
    });
    PAL::logLine(stderr, "ubiquitty-host: handed {} sessions to process {}; panes were held {} ms (quiesce {} ms); this host exits",
        snapshot->sessions.size(), admitted.peerProcessID, elapsedMilliseconds(started), std::chrono::duration_cast<std::chrono::milliseconds>(frozen - started).count());
    return true;
}

std::expected<std::size_t, Refusal> HostHandover::inherit(Daemon& daemon, PAL::UnixFileDescriptor channel,
    std::string_view predecessorSocketPath, std::uint32_t authenticatedPredecessorPID, Clock::time_point deadline)
{
    const auto started = Clock::now();
    const int socket = channel.get();
    // The holder hears the reason; the log names the check that failed.
    const auto refuse = [&](Refusal reason, std::string_view check) -> std::expected<std::size_t, Refusal> {
        (void)sendMessage(socket, Sync::HandoverRefuse { reason }, deadline);
        PAL::logLine(stderr, "ubiquitty-host: refused the handover from process {} ({}: {}); it keeps serving", authenticatedPredecessorPID, nameOf(reason), check);
        return std::unexpected(reason);
    };
    if (!daemon.m_sessions.empty() || !daemon.m_clients.empty() || authenticatedPredecessorPID <= 1
        || authenticatedPredecessorPID > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max()))
        return std::unexpected(Refusal::Malformed);
    const auto predecessorProcess = static_cast<pid_t>(authenticatedPredecessorPID);

    // An offer carries the lease and a refusal carries nothing. A refusal is
    // the one record of its length, so its reason is read rather than lost.
    const auto length = peekRecordLength(socket, deadline);
    if (length == Sync::encodeHandoverMessage(Sync::HandoverRefuse {}).size()) {
        const auto refused = receiveMessage(socket, maxControlRecordBytes, 0, deadline);
        const auto* refusal = refused ? std::get_if<Sync::HandoverRefuse>(&*refused) : nullptr;
        const auto reason = refusal ? refusal->reason : Refusal::Malformed;
        PAL::logLine(stderr, "ubiquitty-host: process {} kept its sessions ({})", authenticatedPredecessorPID, nameOf(reason));
        return std::unexpected(reason);
    }
    POSIX::HandoverRecord offerRecord;
    const auto offered = length ? receiveMessage(socket, POSIX::maxHandoverLeaseOfferBytes, 1, deadline, &offerRecord) : std::nullopt;
    const auto* offer = offered ? std::get_if<Sync::HandoverOffer>(&*offered) : nullptr;
    if (!offer) {
        PAL::logLine(stderr, "ubiquitty-host: process {} made no handover offer", authenticatedPredecessorPID);
        return std::unexpected(Refusal::NotSuccessor);
    }
    if (offer->bodyVersion > Sync::handoverBodyVersion)
        return refuse(Refusal::NewerBody, std::format("its body version {} is newer than {}, the newest this host reads", offer->bodyVersion, Sync::handoverBodyVersion));
    if (offer->bodyVersion < Sync::oldestHandoverBodyVersion)
        return refuse(Refusal::Malformed, std::format("its body version {} is older than {}, the oldest this host reads", offer->bodyVersion, Sync::oldestHandoverBodyVersion));
    auto lease = POSIX::HandoverLease::accept(std::move(offerRecord.descriptors[0]), predecessorProcess,
        { ::getpid(), daemon.m_hostInstanceID });
    if (!lease)
        return refuse(Refusal::NotSuccessor, "the lease it offered is not bound to this host");

    HostHandoverSnapshot snapshot { .predecessor = offer->predecessorInstance, .sessions = {}, .tables = {} };
    std::vector<std::unique_ptr<PAL::Stream>> streams;
    snapshot.sessions.reserve(offer->paneCount);
    streams.reserve(offer->paneCount);
    std::size_t used = offer->tableBytes;
    for (std::uint16_t index = 0; index < offer->paneCount; ++index) {
        POSIX::HandoverRecord followsRecord;
        const auto message = receiveMessage(socket, maxControlRecordBytes, 1, deadline, &followsRecord);
        const auto* follows = message ? std::get_if<Sync::HandoverPaneFollows>(&*message) : nullptr;
        if (!follows || follows->index != index)
            return refuse(Refusal::Malformed, std::format("pane {} was not announced next, with its master", index));
        if (follows->recordBytes > limits.maxAggregateBytes - used)
            return refuse(Refusal::Malformed, std::format("pane {}'s record would take the handover past the {} bytes this host takes", index, limits.maxAggregateBytes));
        used += follows->recordBytes;
        // The leader is corroborated against the received master: its session,
        // its group, its parent and its birth, read twice by the kernel.
        const std::array parents { predecessorProcess, static_cast<pid_t>(follows->leaderParent) };
        auto stream = PAL::inheritPTY(followsRecord.descriptors[0].release(), static_cast<pid_t>(follows->sessionLeader), parents);
        if (!stream)
            return refuse(Refusal::Ancestry, std::format("pane {}'s master is not the terminal of the session leader it names, process {}", index, follows->sessionLeader));
        stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
        auto record = receiveBody(socket, follows->recordBytes, deadline);
        if (!record)
            return refuse(Refusal::Malformed, std::format("pane {}'s record did not arrive whole", index));
        snapshot.sessions.push_back({ follows->paneID, std::move(*record) });
        streams.push_back(std::move(stream));
    }
    auto tables = receiveBody(socket, offer->tableBytes, deadline);
    if (!tables)
        return refuse(Refusal::Malformed, "the host tables did not arrive whole");
    snapshot.tables = std::move(*tables);
    auto candidate = prepare(daemon, std::move(snapshot), std::move(streams), offer->predecessorInstance, monotonicMicroseconds(),
        static_cast<std::uint64_t>(::time(nullptr)), predecessorSocketPath, limits);
    if (!candidate)
        return refuse(refusalOf(candidate.error().error), candidate.error().check);
    if (!sendMessage(socket, Sync::HandoverAccept {}, deadline))
        return std::unexpected(Refusal::Malformed);
    POSIX::HandoverRecord releaseRecord;
    const auto release = receiveMessage(socket, maxControlRecordBytes, std::size_t { offer->listeners } + offer->datagramSockets, deadline, &releaseRecord);
    if (!release || !std::holds_alternative<Sync::HandoverRelease>(*release)) {
        PAL::logLine(stderr, "ubiquitty-host: process {} kept its sessions after accepting the handover", authenticatedPredecessorPID);
        return std::unexpected(Refusal::Malformed);
    }
    auto listeners = admitListeners(daemon.m_socketPath, std::span(releaseRecord.descriptors).first(offer->listeners));
    PAL::UnixFileDescriptor datagramSocket;
    if (offer->datagramSockets)
        datagramSocket = std::move(releaseRecord.descriptors[offer->listeners]);
    if (!listeners || (datagramSocket && !isBoundDatagramSocket(datagramSocket.get()))) {
        PAL::logLine(stderr, "ubiquitty-host: process {} released {} this host cannot serve; it keeps its sessions", authenticatedPredecessorPID,
            listeners ? "a QUIC socket that is no bound datagram socket" : "a listening socket that is no host socket of this directory, or one twice");
        return std::unexpected(Refusal::Malformed);
    }
    // The holder let the lock go before Release. Claiming it is the commit
    // point: from here the holder can never resume these panes.
    if (!lease->tryAcquire() || !lease->claimSuccessor()) {
        PAL::logLine(stderr, "ubiquitty-host: could not claim the sessions of process {}; it keeps them", authenticatedPredecessorPID);
        return std::unexpected(Refusal::NotSuccessor);
    }
    const std::size_t moved = (*candidate)->sessionCount();
    const std::string startupRecordPath = daemon.m_layoutRecordPath;
    commit(std::move(**candidate));
    // Held while this daemon serves, so the claimed lock outlives the commit.
    daemon.m_claimedHandoverLeases.push_back(std::move(*lease));
    // Only once claimed: a holder that resumes keeps answering as itself.
    for (auto& listener : *listeners) {
        if (!Daemon::listenAsOwner(listener.descriptor.get()))
            PAL::logLine(stderr, "ubiquitty-host: could not listen again on {}; a later successor may not reach this host there", listener.path);
        daemon.m_inheritedListeners.push_back({ std::move(listener.descriptor), std::move(listener.path) });
    }
    daemon.m_inheritedQUICSocket = std::move(datagramSocket);
    (void)sendMessage(socket, Sync::HandoverServing {}, deadline);
    PAL::logLine(stderr, "ubiquitty-host: took {} sessions from process {} in {} ms", moved, authenticatedPredecessorPID, elapsedMilliseconds(started));
    daemon.retireStartupLayoutRecords(startupRecordPath);
    return moved;
}

} // namespace Ubiquitty::Host
