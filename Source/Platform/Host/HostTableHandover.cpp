// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HostTableHandover.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Session/WorkspaceWire.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <new>

namespace Ubiquitty::Host {
namespace {
namespace Sync = Protocol::Sync;
namespace Layout = Ubiquitty::Session::Layout;
namespace Identity = Ubiquitty::Session::Identity;
using Error = Sync::HandoverBodyError;
constexpr std::uint16_t version = 1;
constexpr std::uint32_t magic = 0x54544248;
constexpr std::size_t recordOverhead = 2 + 4 + 4 * 4 + 16;

std::expected<Sync::HandoverBytes, Error> join(std::array<Sync::HandoverBytes, 4>&& sections, std::size_t maximum)
{
    if (maximum < recordOverhead)
        return std::unexpected(Error::OverCapacity);
    std::size_t size = recordOverhead;
    for (const auto& section : sections) {
        if (section.size > UINT32_MAX || section.size > maximum - size)
            return std::unexpected(Error::OverCapacity);
        size += section.size;
    }
    Sync::HandoverBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]), size };
    if (!result.data)
        return std::unexpected(Error::AllocationRefused);
    std::size_t offset = 0;
    const auto scalar = [&]<typename T>(T value) {
        for (std::size_t index = 0; index < sizeof(T); ++index)
            result.data[offset++] = static_cast<std::uint8_t>(value >> (index * 8));
    };
    scalar(version);
    scalar(magic);
    for (const auto& section : sections) {
        scalar(static_cast<std::uint32_t>(section.size));
        std::memcpy(result.data.get() + offset, section.data.get(), section.size);
        offset += section.size;
    }
    const auto digest = Sync::digestOf(result.view().first(offset));
    scalar(digest.low);
    scalar(digest.high);
    return result;
}

std::expected<std::array<std::span<const std::uint8_t>, 4>, Error> split(std::span<const std::uint8_t> bytes, const HostTableHandover::Limits& limits)
{
    if (bytes.size() > limits.maxRecordBytes)
        return std::unexpected(Error::OverCapacity);
    Sync::WireReader reader(bytes);
    const auto receivedVersion = reader.u16();
    if (reader.ok() && receivedVersion > version)
        return std::unexpected(Error::NewerVersion);
    if (receivedVersion != version || reader.u32() != magic)
        return std::unexpected(Error::Malformed);
    const std::array maximum { limits.attachments.maxSectionBytes, limits.workspaces.maxSectionBytes,
        limits.authority.maxSectionBytes, limits.globals.maxSectionBytes };
    std::array<std::span<const std::uint8_t>, 4> sections;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto size = reader.u32();
        if (size > maximum[index])
            return std::unexpected(Error::OverCapacity);
        sections[index] = reader.take(size);
    }
    const auto digestOffset = reader.offset();
    const Sync::Digest128 digest { reader.u64(), reader.u64() };
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(Error::Malformed);
    if (Sync::digestOf(bytes.first(digestOffset)) != digest)
        return std::unexpected(Error::DigestMismatch);
    return sections;
}

bool wasIssued(std::uint32_t value, std::uint32_t next)
{
    return value && (!next || value < next);
}
} // namespace

struct HostTableHandover::Candidate::OwnedTables {
    struct AttachmentPane {
        Sync::ID128 attachment;
        Sync::ID128 pane;
        bool isHeld = false;
    };
    HandoverWorkspaceTables workspaceRecords;
    HandoverAuthorityTables authorityRecords;
    HandoverGlobalTables globalRecords;
    PreparedAttachmentAuthority attachmentAuthority;
    std::vector<AttachmentPane> attachmentPanes;
    std::vector<std::uint64_t> paneIssuers;
    std::shared_ptr<Identity::Allocator> identities;
    std::vector<std::unique_ptr<Daemon::HostedWorkspace>> workspaces;
    std::vector<Daemon::RestorableWorkspace> restorable;
    std::map<std::uint32_t, Daemon::StructureRecord> structures;
    Identity::LabelBook labels;
    Identity::TagBook tags;
    CapabilityGrantStore grants;
    CapabilityConsentQueue consent;
    std::vector<Daemon::PendingQUICToken> quicTokens;
    Ubiquitty::Session::EventLog events;
    Ubiquitty::Session::BackgroundEvents backgroundEvents;

    OwnedTables(std::size_t eventCapacity, std::uint64_t epoch)
        : events(eventCapacity)
        , backgroundEvents(epoch)
    {
    }
};

HostTableHandover::Candidate::Candidate(std::unique_ptr<OwnedTables> state)
    : m_state(std::move(state))
{
}
HostTableHandover::Candidate::~Candidate() = default;
HostTableHandover::Candidate::Candidate(Candidate&&) noexcept = default;
HostTableHandover::Candidate& HostTableHandover::Candidate::operator=(Candidate&&) noexcept = default;
const HandoverWorkspaceTables& HostTableHandover::Candidate::workspaces() const { return m_state->workspaceRecords; }
const HandoverAuthorityTables& HostTableHandover::Candidate::authority() const { return m_state->authorityRecords; }
const HandoverGlobalTables& HostTableHandover::Candidate::globals() const { return m_state->globalRecords; }

std::expected<Sync::HandoverBytes, Error> HostTableHandover::capture(const Daemon& daemon, std::uint64_t now, const Limits& limits)
{
    if (!daemon.isQuiesced() || daemon.hasHandoverBlockedOperations())
        return std::unexpected(Error::NotAtGround);
    auto attachmentRecord = encodeHandoverAttachments(daemon.exportAttachmentAuthority(now), limits.attachments);
    if (!attachmentRecord)
        return std::unexpected(attachmentRecord.error());

    HandoverWorkspaceTables workspaces;
    workspaces.identities = daemon.m_layoutIdentities->continuation();
    workspaces.nextSessionID = daemon.m_nextSessionID;
    workspaces.nextClientID = daemon.m_nextClientID;
    workspaces.nextStructureConsentID = daemon.m_nextStructureConsentID;
    for (const auto& source : daemon.m_workspaces) {
        if (!source)
            return std::unexpected(Error::Malformed);
        HandoverWorkspace target;
        target.id = source->workspaceID;
        target.name = source->name;
        target.shouldEndWhenDetached = source->shouldEndWhenDetached;
        target.isClosing = source->isClosing;
        target.wasRestoredAfterHostRestart = source->wasRestoredAfterHostRestart;
        // The receiving instance holds it next; this one joins those before.
        target.formerHolders = Ubiquitty::Session::holdersAfter(source->formerHolders, { .daemonInstanceID = daemon.m_daemonInstanceID, .workspaceID = source->workspaceID });
        target.heldUnixSeconds = source->heldUnixSeconds;
        target.recordedLayout = source->recordedLayout;
        captureHandoverLayout(source->layout, target);
        for (const auto& [pane, session] : source->paneSessions)
            target.paneSessions.emplace_back(std::to_underlying(pane), session);
        workspaces.workspaces.push_back(std::move(target));
    }
    for (const auto& source : daemon.m_restorableWorkspaces)
        workspaces.restorable.push_back({ source.workspaceID, source.record });
    for (const auto& [identity, source] : daemon.m_structureRecords)
        workspaces.structures.push_back({ identity, source.revision, source.unrevised,
            source.record ? std::optional(*source.record) : std::nullopt });
    const auto labels = daemon.m_labels.records();
    workspaces.labels.assign(labels.begin(), labels.end());
    const auto tags = daemon.m_tags.records();
    workspaces.tags.assign(tags.begin(), tags.end());
    workspaces.layoutRecordPath = daemon.m_layoutRecordPath;
    workspaces.layoutRecordDueMilliseconds = daemon.m_layoutRecordWrite.dueMilliseconds;
    workspaces.layoutRecordSignMilliseconds = daemon.m_layoutRecordWrite.latestSignMilliseconds;
    workspaces.writtenLayoutRecords = daemon.m_writtenLayoutRecords;
    workspaces.isLayoutRecordKept = daemon.m_isLayoutRecordKept;
    workspaces.isStructureDirty = daemon.m_isStructureDirty;
    auto workspaceRecord = encodeHandoverWorkspaces(workspaces, limits.workspaces);
    if (!workspaceRecord)
        return std::unexpected(workspaceRecord.error());

    HandoverAuthorityTables authority;
    authority.decisions = daemon.m_capabilityDecisions;
    authority.grants = daemon.m_grantStore.records();
    auto consent = daemon.m_consentQueue.continuation(limits.authority.maxConsentEntries);
    if (!consent)
        return std::unexpected(Error::OverCapacity);
    authority.pendingConsent = std::move(consent->pending);
    authority.newlyPendingConsent = std::move(consent->newlyPending);
    // No question is captured: one belongs to its recipients and this
    // process's clock, and a pending one is a handover blocker above.
    for (const auto& source : daemon.m_pendingQUICTokens)
        authority.quicTokens.push_back({ source.token, source.deadlineMilliseconds, source.mintingClientID, source.scope, source.tokenID, source.isInteractiveUser });
    authority.alwaysAllowedAgents = daemon.m_alwaysAllowedAgents;
    authority.nextConsentRequestID = daemon.m_nextConsentRequestID;
    authority.nextClipboardRequestID = daemon.m_nextClipboardRequestID;
    authority.isTokenRequired = daemon.m_requireToken;
    authority.didFailManagementHistory = daemon.m_didFailManagementHistory;
    auto authorityRecord = encodeHandoverAuthority(authority, limits.authority);
    if (!authorityRecord)
        return std::unexpected(authorityRecord.error());

    HandoverGlobalTables globals;
    globals.configuration = daemon.m_configuration;
    globals.shellPath = daemon.m_shellPath;
    globals.paneHostName = daemon.m_paneHostName;
    globals.paneHomeDirectory = daemon.m_paneHomeDirectory;
    globals.columns = daemon.m_columns;
    globals.rows = daemon.m_rows;
    globals.events = daemon.m_events.continuation();
    globals.backgroundEvents = daemon.m_backgroundEvents.continuation();
    globals.backgroundInventoryRevision = daemon.m_backgroundInventoryRevision;
    globals.backgroundEventPath = daemon.m_backgroundEventPath;
    globals.backgroundEventWriteDue = daemon.m_backgroundEventWriteDue;
    globals.writtenBackgroundEventRevision = daemon.m_writtenBackgroundEventRevision;
    globals.isBackgroundEventStorageAvailable = daemon.m_isBackgroundEventStorageAvailable;
    globals.eventDeliveries = daemon.m_eventDeliveries;
    globals.nextScrollbackFoldMilliseconds = daemon.m_nextScrollbackFoldMilliseconds;
    globals.lastScrollbackFoldSessionID = daemon.m_lastScrollbackFoldSessionID;
    globals.closuresOwed = daemon.m_closuresOwed;
    globals.terminationsOwed = daemon.m_terminationsOwed;
    globals.hasHadSession = daemon.m_everHadSession;
    auto globalRecord = encodeHandoverGlobals(globals, limits.globals);
    if (!globalRecord)
        return std::unexpected(globalRecord.error());
    return join({ std::move(*attachmentRecord), std::move(*workspaceRecord), std::move(*authorityRecord), std::move(*globalRecord) }, limits.maxRecordBytes);
}

bool HostTableHandover::areRecordsKeptBeside(std::string_view layoutRecordPath, std::string_view backgroundEventPath, std::string_view socketPath)
{
    constexpr std::string_view layoutSuffix = ".layouts";
    constexpr std::string_view backgroundSuffix = ".background-events";
    return layoutRecordPath.size() == socketPath.size() + layoutSuffix.size() && layoutRecordPath.starts_with(socketPath)
        && layoutRecordPath.ends_with(layoutSuffix) && backgroundEventPath.size() == socketPath.size() + backgroundSuffix.size()
        && backgroundEventPath.starts_with(socketPath) && backgroundEventPath.ends_with(backgroundSuffix);
}

std::expected<std::unique_ptr<HostTableHandover::Candidate>, HandoverPrepareRefusal> HostTableHandover::prepare(
    Daemon& daemon, std::span<const std::uint8_t> bytes, Sync::ID128 predecessor,
    std::uint64_t now, std::uint64_t nowUnix, std::string_view expectedPredecessorSocketPath, const Limits& limits)
{
    const auto refuse = [](Error error, std::string check) { return std::unexpected(HandoverPrepareRefusal { error, std::move(check) }); };
    if (!daemon.m_sessions.empty() || !daemon.m_clients.empty() || !daemon.m_workspaces.empty())
        return refuse(Error::NotAtGround, "this host already holds sessions, clients or workspaces");
    if (expectedPredecessorSocketPath.empty() || expectedPredecessorSocketPath.front() != '/')
        return refuse(Error::NotAtGround, "the socket it was reached at has no absolute path");
    const auto parts = split(bytes, limits);
    if (!parts)
        return refuse(parts.error(), "the host tables do not divide into their sections");
    auto attachments = decodeHandoverAttachments((*parts)[0], limits.attachments);
    if (!attachments)
        return refuse(attachments.error(), "the attachment table does not decode");
    auto workspaces = decodeHandoverWorkspaces((*parts)[1], limits.workspaces);
    if (!workspaces)
        return refuse(workspaces.error(), "the workspace table does not decode");
    auto authority = decodeHandoverAuthority((*parts)[2], limits.authority);
    if (!authority) {
        // A question belongs to its recipients on the predecessor; one it
        // captured is never given new owners here.
        return refuse(authority.error(), authority.error() == Error::NotAtGround ? "a capability question is still unanswered" : "the authority table does not decode");
    }
    auto globals = decodeHandoverGlobals((*parts)[3], limits.globals);
    if (!globals)
        return refuse(globals.error(), "the global table does not decode");
    const std::string predecessorSocket(expectedPredecessorSocketPath);
    const auto slash = predecessorSocket.find_last_of('/');
    const auto admittedRoot = predecessorSocket.substr(0, slash);
    if (daemon.m_tokenPath != admittedRoot + "/tokens")
        return refuse(Error::Malformed, std::format("its tokens are not kept in {}, the directory of the socket it was reached at", admittedRoot));
    if (!areRecordsKeptBeside(workspaces->layoutRecordPath, globals->backgroundEventPath, predecessorSocket))
        return refuse(Error::Malformed, std::format("its records are kept at {}, not beside {}, the socket it was reached at", workspaces->layoutRecordPath, predecessorSocket));

    auto state = std::unique_ptr<Candidate::OwnedTables>(new (std::nothrow) Candidate::OwnedTables(globals->events.ring.size(), globals->backgroundEvents.epoch));
    if (!state)
        return refuse(Error::AllocationRefused, "the tables could not be allocated");
    for (const auto& record : attachments->attachments.records) {
        for (const auto& pane : record->panes)
            state->attachmentPanes.push_back({ record->attachmentID, pane->paneID, pane->isHeld });
    }
    for (const auto& identity : attachments->hostIdentities)
        state->paneIssuers.push_back(identity.daemonInstanceID);
    auto prepared = daemon.prepareAttachmentAuthority(std::move(*attachments), predecessor, now, nowUnix);
    if (!prepared)
        return refuse(Error::Malformed, "its attachments do not carry over to this host");
    state->attachmentAuthority = std::move(*prepared);
    state->workspaceRecords = std::move(*workspaces);
    state->authorityRecords = std::move(*authority);
    state->globalRecords = std::move(*globals);
    const auto& topology = state->workspaceRecords;
    state->identities = std::shared_ptr<Identity::Allocator>(new (std::nothrow) Identity::Allocator(topology.identities.ceiling));
    if (!state->identities)
        return refuse(Error::AllocationRefused, "the layout identities could not be allocated");
    if (!state->identities->restoreContinuation(topology.identities))
        return refuse(Error::Malformed, "the layout identities do not restore");
    for (const auto& source : topology.workspaces) {
        // Explicit roots avoid minting against the carried high-water marks.
        // restoreHandoverLayout replaces this private seed before publication.
        Layout::RootIdentity root {};
        if (!source.windows.empty() && !source.windows.front().tabs.empty()) {
            const auto& window = source.windows.front();
            const auto& tab = window.tabs.front();
            root = { static_cast<Layout::SurfaceID>(window.id), static_cast<Layout::TabID>(tab.id), static_cast<Layout::PaneID>(source.paneSessions.front().first) };
        }
        auto target = std::unique_ptr<Daemon::HostedWorkspace>(new (std::nothrow) Daemon::HostedWorkspace(state->identities, root));
        if (!target)
            return refuse(Error::AllocationRefused, std::format("workspace {} could not be allocated", source.id));
        if (!restoreHandoverLayout(source, target->layout))
            return refuse(Error::Malformed, std::format("workspace {}'s layout does not restore", source.id));
        target->workspaceID = source.id;
        target->name = source.name;
        target->shouldEndWhenDetached = source.shouldEndWhenDetached;
        target->isClosing = source.isClosing;
        target->wasRestoredAfterHostRestart = source.wasRestoredAfterHostRestart;
        target->formerHolders = source.formerHolders;
        target->heldUnixSeconds = source.heldUnixSeconds;
        target->recordedLayout = source.recordedLayout;
        for (const auto& [pane, session] : source.paneSessions)
            target->paneSessions.emplace_back(static_cast<Layout::PaneID>(pane), session);
        state->workspaces.push_back(std::move(target));
    }
    if (state->identities->continuation() != topology.identities)
        return refuse(Error::Malformed, "the workspaces do not match the layout identities carried with them");
    for (const auto& source : topology.restorable)
        state->restorable.push_back({ source.id, source.record });
    for (const auto& source : topology.structures) {
        std::shared_ptr<const std::vector<std::uint8_t>> record;
        if (source.record)
            record = std::make_shared<const std::vector<std::uint8_t>>(*source.record);
        // The record handed over says the arrangement at its revision, so an
        // operation asked about that revision stays current until the
        // arrangement moves. Only that revision is known to describe it: an
        // older one is refused.
        const auto structure = Ubiquitty::Session::decodeWorkspaceStructure(source.unrevised);
        std::string arrangement = structure ? Layout::arrangementSignature(*structure) : std::string {};
        state->structures.emplace(source.workspaceID, Daemon::StructureRecord { source.revision, source.unrevised, std::move(record), std::move(arrangement), source.revision });
    }
    for (const auto& [identity, label] : topology.labels) {
        if (state->labels.set(identity, label) != Identity::LabelVerdict::Accepted)
            return refuse(Error::Malformed, "a label does not restore");
    }
    for (const auto& [identity, tags] : topology.tags) {
        if (state->tags.set(identity, tags) != Identity::TagVerdict::Accepted)
            return refuse(Error::Malformed, "a tag does not restore");
    }
    const auto& grants = state->authorityRecords;
    for (const auto& [key, grant] : grants.grants)
        state->grants.grant(key.capability, key.origin, key.identity, grant.scope, grant.sessionID);
    CapabilityConsentQueue::Continuation consent { grants.pendingConsent, grants.newlyPendingConsent };
    if (!state->consent.restoreContinuation(std::move(consent), limits.authority.maxConsentEntries))
        return refuse(Error::Malformed, "the consent queue does not restore");
    for (const auto& source : grants.quicTokens) {
        if (!wasIssued(source.mintingClientID, topology.nextClientID))
            return refuse(Error::Malformed, "a QUIC token names a client that was never admitted");
        state->quicTokens.push_back({ source.token, source.deadlineMilliseconds, source.mintingClientID, source.scope, source.tokenID, source.isInteractiveUser });
    }
    auto events = state->globalRecords.events;
    auto background = state->globalRecords.backgroundEvents;
    if (!state->events.restoreContinuation(std::move(events)) || !state->backgroundEvents.restoreContinuation(std::move(background)))
        return refuse(Error::Malformed, "the event journals do not restore");
    auto candidate = std::unique_ptr<Candidate>(new (std::nothrow) Candidate(std::move(state)));
    if (!candidate)
        return refuse(Error::AllocationRefused, "the tables could not be allocated");
    return candidate;
}

bool HostTableHandover::Candidate::isKnownIssuer(std::span<const std::unique_ptr<Daemon::Session>> sessions, std::uint64_t issuer) const
{
    // A pane keeps the identity the host that started it minted for as long as
    // it lives, while the carried host identities are the resume window,
    // which `maxInheritedInstances` bounds. The host of a pane handed over is
    // therefore known after it has left the window.
    return issuer
        && (std::ranges::find(m_state->paneIssuers, issuer) != m_state->paneIssuers.end()
            || std::ranges::any_of(sessions, [&](const auto& session) { return session && session->screenSource && session->screenSource->paneID().high == issuer; }));
}

std::size_t HostTableHandover::Candidate::forgetBaselinesOfDepartedPanes(std::span<const std::unique_ptr<Daemon::Session>> sessions)
{
    if (!m_state)
        return 0;
    auto& table = *m_state->attachmentAuthority.attachments;
    return std::erase_if(m_state->attachmentPanes, [&](const OwnedTables::AttachmentPane& binding) {
        return !binding.isHeld && !isKnownIssuer(sessions, binding.pane.high) && table.forgetUnheldPane(binding.attachment, binding.pane);
    });
}

const Sync::AttachmentTable& HostTableHandover::Candidate::attachments() const { return *m_state->attachmentAuthority.attachments; }

bool HostTableHandover::Candidate::validateSessions(std::span<const std::unique_ptr<Daemon::Session>> sessions) const
{
    if (!m_state)
        return false;
    const auto& topology = m_state->workspaceRecords;
    const auto findSession = [&](std::uint32_t id) -> const Daemon::Session* {
        for (const auto& session : sessions) {
            if (session && session->id == id)
                return session.get();
        }
        return nullptr;
    };
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        const auto& session = sessions[index];
        if (!session || !session->screenSource || !wasIssued(session->id, topology.nextSessionID))
            return false;
        // A pane handed over names the host that minted it, which need not be
        // among the carried identities; it must name a host, and no other
        // pane may hold its identity.
        const auto paneID = session->screenSource->paneID();
        if (paneID.low != session->id || !paneID.high)
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (sessions[previous]->id == session->id || sessions[previous]->screenSource->paneID() == paneID)
                return false;
        }
        std::size_t bindings = 0;
        for (const auto& workspace : m_state->workspaces) {
            for (const auto& [pane, backing] : workspace->paneSessions) {
                if (backing != session->id)
                    continue;
                ++bindings;
                if (pane != session->placement.pane)
                    return false;
                const auto* surface = workspace->layout.surface(session->placement.surface);
                if (!surface)
                    return false;
                bool isPlaced = false;
                for (int tab = 0; tab < surface->tabCount; ++tab) {
                    if (surface->tabs[tab].id == session->placement.tab && surface->tabs[tab].panes.holdsPane(static_cast<int>(std::to_underlying(pane))))
                        isPlaced = true;
                }
                if (!isPlaced)
                    return false;
            }
        }
        if (bindings != 1)
            return false;
        if ((session->lastInputClientID && !wasIssued(session->lastInputClientID, topology.nextClientID))
            || (session->lastHumanInputClientID && !wasIssued(session->lastHumanInputClientID, topology.nextClientID)))
            return false;
    }
    for (const auto& workspace : m_state->workspaces) {
        for (const auto& [pane, session] : workspace->paneSessions) {
            (void)pane;
            if (!findSession(session))
                return false;
        }
    }
    for (const auto& binding : m_state->attachmentPanes) {
        if (!m_state->attachmentAuthority.attachments->find(binding.attachment))
            continue;
        if (binding.isHeld) {
            if (std::ranges::none_of(sessions, [&](const auto& session) { return session->screenSource->paneID() == binding.pane; }))
                return false;
        } else if (binding.pane.low > UINT32_MAX || !wasIssued(static_cast<std::uint32_t>(binding.pane.low), topology.nextSessionID)
            || !isKnownIssuer(sessions, binding.pane.high))
            return false;
    }
    for (const auto& [key, grant] : m_state->authorityRecords.grants) {
        (void)key;
        if (grant.scope == CapabilityGrantScope::Session && !findSession(grant.sessionID))
            return false;
    }
    for (const auto identity : m_state->globalRecords.closuresOwed) {
        if (std::ranges::none_of(m_state->workspaces, [&](const auto& workspace) { return workspace->workspaceID == identity; }))
            return false;
    }
    for (const auto& [identity, pane] : m_state->globalRecords.terminationsOwed) {
        const auto* session = findSession(identity);
        if (!session || session->screenSource->paneID() != pane)
            return false;
    }
    return true;
}

void HostTableHandover::commit(Daemon& daemon, Candidate&& candidate)
{
    auto state = std::move(candidate.m_state);
    auto& topology = state->workspaceRecords;
    auto& authority = state->authorityRecords;
    auto& globals = state->globalRecords;
    daemon.m_attachments = std::move(state->attachmentAuthority.attachments);
    daemon.wireAttachmentRetirement();
    daemon.m_attachmentGrants = std::move(state->attachmentAuthority.grants);
    daemon.m_inheritedHostIdentities = std::move(state->attachmentAuthority.hostIdentities);
    daemon.m_workspaces = std::move(state->workspaces);
    daemon.m_layoutIdentities = std::move(state->identities);
    daemon.m_restorableWorkspaces = std::move(state->restorable);
    daemon.m_structureRecords = std::move(state->structures);
    daemon.m_labels = std::move(state->labels);
    daemon.m_tags = std::move(state->tags);
    daemon.m_nextSessionID = topology.nextSessionID;
    daemon.m_nextClientID = topology.nextClientID;
    daemon.m_nextStructureConsentID = topology.nextStructureConsentID;
    daemon.m_layoutRecordPath = std::move(topology.layoutRecordPath);
    daemon.m_layoutRecordWrite = { .dueMilliseconds = topology.layoutRecordDueMilliseconds, .latestSignMilliseconds = topology.layoutRecordSignMilliseconds };
    daemon.m_writtenLayoutRecords = std::move(topology.writtenLayoutRecords);
    daemon.m_isLayoutRecordKept = topology.isLayoutRecordKept;
    daemon.m_isStructureDirty = topology.isStructureDirty;
    daemon.replaceCapabilityGrants(std::move(state->grants));
    daemon.m_consentQueue = std::move(state->consent);
    daemon.m_pendingQUICTokens = std::move(state->quicTokens);
    daemon.m_alwaysAllowedAgents = std::move(authority.alwaysAllowedAgents);
    daemon.m_capabilityDecisions = authority.decisions;
    daemon.m_nextConsentRequestID = authority.nextConsentRequestID;
    daemon.m_nextClipboardRequestID = authority.nextClipboardRequestID;
    daemon.m_requireToken = authority.isTokenRequired;
    daemon.m_didFailManagementHistory = authority.didFailManagementHistory;
    daemon.m_configuration = std::move(globals.configuration);
    daemon.m_shellPath = std::move(globals.shellPath);
    daemon.m_paneHostName = std::move(globals.paneHostName);
    daemon.m_paneHomeDirectory = std::move(globals.paneHomeDirectory);
    daemon.m_columns = globals.columns;
    daemon.m_rows = globals.rows;
    daemon.m_events = std::move(state->events);
    daemon.m_backgroundEvents = std::move(state->backgroundEvents);
    daemon.m_backgroundInventoryRevision = globals.backgroundInventoryRevision;
    daemon.m_backgroundEventPath = std::move(globals.backgroundEventPath);
    daemon.m_backgroundEventWriteDue = globals.backgroundEventWriteDue;
    daemon.m_writtenBackgroundEventRevision = globals.writtenBackgroundEventRevision;
    daemon.m_isBackgroundEventStorageAvailable = globals.isBackgroundEventStorageAvailable;
    daemon.m_eventDeliveries = globals.eventDeliveries;
    daemon.m_nextScrollbackFoldMilliseconds = globals.nextScrollbackFoldMilliseconds;
    daemon.m_lastScrollbackFoldSessionID = globals.lastScrollbackFoldSessionID;
    daemon.m_closuresOwed = std::move(globals.closuresOwed);
    daemon.m_terminationsOwed = std::move(globals.terminationsOwed);
    daemon.m_everHadSession = globals.hasHadSession;
}

} // namespace Ubiquitty::Host
