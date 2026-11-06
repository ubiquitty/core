// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Connection.h"

#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Notify.h"

#include "Core/Protocol/Sync/Authority.h"
#include "Core/Protocol/Sync/FrameGates.h"

#include "Core/CellGrid.h"
#include "Core/Codec/ZlibDeflate.h"
#include "Core/Crypto/SHA256.h"
#include "Core/LineStore.h"
#include "Core/OSC/Activity.h"

#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

bool isConsentPending(ConnectionState state)
{
    return state == ConnectionState::AwaitingConsent || state == ConnectionState::ApprovedAwaitingDelivery;
}

bool isPendingTransportFrame(FrameKind kind)
{
    return kind == FrameKind::Facts || kind == FrameKind::Ack || kind == FrameKind::Probe || kind == FrameKind::Echo || kind == FrameKind::Bye;
}

} // namespace

bool HostConnection::hasHandoverBlockedOperations() const
{
    return isConsentPending(m_state) || m_isReceiving || m_decoder.pendingBytes() || outstandingCalls()
        || !m_callsOverLimit.empty() || !m_receivedCalls.empty()
        || m_clipboardDeliveryBytes || m_isDrainingClipboard
        || std::ranges::any_of(m_clipboardReads, [](const auto& read) { return read.has_value(); })
        || !m_viewportReports.empty();
}

namespace {

// What a pane says when its checkpoint was refused past the cap, apart for
// each cause; a person reads them where the pane was.
constexpr std::string_view paneContentsMismatchedNotice = "This pane’s contents didn’t match what the host said it sent. Reconnect to load the pane again.";
constexpr std::string_view paneContentsDamagedNotice = "This pane’s contents were damaged on the way from the host. Reconnect to load the pane again.";
constexpr std::string_view paneContentsUnreadableNotice = "This pane’s contents arrived in a form this app couldn’t read. Reconnect to load the pane again.";

/// The digest a checkpoint body ends with.
Digest128 trailingDigest(std::span<const std::uint8_t> body)
{
    if (body.size() < 16)
        return {};
    WireReader reader(body.subspan(body.size() - 16));
    Digest128 digest;
    digest.low = reader.u64();
    digest.high = reader.u64();
    return digest;
}

/// The generation a checkpoint body begins with.
std::optional<std::uint64_t> leadingGeneration(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    const std::uint64_t generation = reader.u64();
    if (!reader.ok())
        return std::nullopt;
    return generation;
}

/// The `ByeReason` an envelope failure closes with.
ByeReason byeReasonOf(EnvelopeError error)
{
    switch (error) {
    case EnvelopeError::Oversize:
        return ByeReason::Oversize;
    case EnvelopeError::UnknownKind:
        return ByeReason::UnknownKind;
    case EnvelopeError::None:
    case EnvelopeError::Malformed:
        return ByeReason::Malformed;
    }
    return ByeReason::Malformed;
}

/// Whether two baselines describe rows of one geometry and epoch, so their
/// row hashes compare.
bool isSameShape(const AttachmentBaseline& a, const AttachmentBaseline& b)
{
    return a.dictionaryEpoch == b.dictionaryEpoch && a.columns == b.columns && a.rowHashes[0].size() == b.rowHashes[0].size()
        && a.rowHashes[1].size() == b.rowHashes[1].size();
}

} // namespace

HostPolicy measuredHostPolicy(std::size_t datagramMaxBytes)
{
    // Default pacing, scheduling and resource bounds for a host connection.
    return {
        .pacing = {
            // The delivery window never falls below this byte bound.
            .windowFloorBytes = 256 * 1024,
            // Probe idle connections and retain round-trip and delivery-rate
            // samples for these configured intervals (Docs/sync.md §6.4).
            .probeIdleMicroseconds = 2'000'000,
            .roundTripStaleMicroseconds = 10'000'000,
            .rateFilterMicroseconds = 10'000'000,
            // A missing datagram's acknowledgement waits at least this long
            // before the datagram is treated as lost (Docs/sync.md §8).
            .datagramLossFloorMicroseconds = 20'000,
            // Probes back off from 2 s to 30 s; outstanding data with no
            // acknowledgement expires at the 30 s silence bound (Docs/sync.md §6.4).
            .maxProbeIntervalMicroseconds = 30'000'000,
            .lostSilenceMicroseconds = 30'000'000,
        },
        // Reserve one quarter of stream bytes for pending bulk transfers
        // (Docs/sync.md §6.5).
        .lanes = { .bulkSharePermille = 250 },
        .datagramMaxBytes = datagramMaxBytes,
        // A pane past this checkpoint-body bound is unbound with
        // CheckpointTooLarge (Docs/sync-measurements.md, "Memory admission").
        .maxCheckpointBytes = defaultMaxCheckpointBytes,
        .limits = provisionalLimits,
        .input = measuredInputPolicy(),
    };
}

Bye spokenBye(std::uint16_t wireVersion, std::string_view version, std::string_view platform, ByeReason reason, std::string_view detail)
{
    return { .reason = reason, .detail = std::string(detail), .peer = PeerMetadata { .build = { .isKnown = true, .wireVersion = wireVersion, .appVersion = std::string(version) }, .platform = std::string(platform) } };
}

// MARK: - PaneSource

std::expected<std::uint64_t, EffectRefusal> PaneSource::appendEffect(const EffectBody& body, ID128 primary)
{
    const EffectKind kind = effectKind(body);
    if (kind == EffectKind::ClipboardWrite)
        return std::unexpected(EffectRefusal::Invalid);
    if (const auto* sound = std::get_if<SoundEffect>(&body); sound && sound->sample.frames)
        return std::unexpected(EffectRefusal::Invalid);
    const auto encoded = encodeEffectBody(body);
    if (!encoded) {
        if (encoded.error() == EffectCodecError::NoMemory)
            return std::unexpected(EffectRefusal::NoMemory);
        return std::unexpected(encoded.error() == EffectCodecError::TooLarge ? EffectRefusal::OverLimit : EffectRefusal::Invalid);
    }
    return m_effects.append({ .generation = m_generation, .primary = primary, .kind = kind, .count = 1, .target = 0, .payload = encoded->view() });
}

std::expected<std::uint64_t, EffectRefusal> PaneSource::appendBells(std::uint32_t count, ID128 primary)
{
    if (!count)
        return std::unexpected(EffectRefusal::Invalid);
    const auto encoded = encodeEffectBody(BellEffect {});
    if (!encoded)
        return std::unexpected(encoded.error() == EffectCodecError::NoMemory ? EffectRefusal::NoMemory : EffectRefusal::Invalid);
    return m_effects.append({ .generation = m_generation, .primary = primary, .kind = EffectKind::Bell, .count = count, .target = 0, .payload = encoded->view() });
}

std::expected<std::uint64_t, EffectRefusal> PaneSource::appendClipboardWrite(ClipboardBuffer buffer,
    const EffectClipboardBundle& bundle, ID128 primary)
{
    if (primary == ID128 {})
        return std::unexpected(EffectRefusal::Invalid);
    const auto measured = effectClipboardEncodedBytes(bundle, maxEffectClipboardWriteBytes);
    if (!measured)
        return std::unexpected(measured.error() == EffectCodecError::TooLarge ? EffectRefusal::OverLimit : EffectRefusal::Invalid);
    // The log's transfer marker is not a carrier ID. Each serving connection
    // replaces it with its own admitted transfer, or an inline descriptor.
    const auto body = encodeEffectBody(ClipboardWriteEffect { buffer,
        { .transfer = 1, .encodedBytes = static_cast<std::uint32_t>(*measured), .inlineBytes = {} } });
    if (!body)
        return std::unexpected(body.error() == EffectCodecError::NoMemory ? EffectRefusal::NoMemory : EffectRefusal::Invalid);
    const auto available = m_effects.availableRetainedBytes();
    if (body->size > available || *measured > available - body->size)
        return std::unexpected(EffectRefusal::OverLimit);
    auto data = encodeEffectClipboard(bundle, maxEffectClipboardWriteBytes);
    if (!data)
        return std::unexpected(data.error() == EffectCodecError::NoMemory ? EffectRefusal::NoMemory : EffectRefusal::Invalid);
    return appendClipboardWrite(buffer, *data, primary);
}

std::expected<std::uint64_t, EffectRefusal> PaneSource::appendClipboardWrite(ClipboardBuffer buffer,
    EffectCodecBytes& encoded, ID128 primary)
{
    if (primary == ID128 {} || !decodeEffectClipboard(encoded.view(), maxEffectClipboardWriteBytes))
        return std::unexpected(EffectRefusal::Invalid);
    const auto body = encodeEffectBody(ClipboardWriteEffect { buffer,
        { .transfer = 1, .encodedBytes = static_cast<std::uint32_t>(encoded.size), .inlineBytes = {} } });
    if (!body)
        return std::unexpected(body.error() == EffectCodecError::NoMemory ? EffectRefusal::NoMemory : EffectRefusal::Invalid);
    const auto retained = m_effects.appendOwnedData({ .generation = m_generation, .primary = primary, .kind = EffectKind::ClipboardWrite, .count = 1, .target = std::to_underlying(buffer), .payload = body->view(), .data = {} },
        encoded.bytes, encoded.size);
    if (retained)
        encoded.size = 0;
    return retained;
}

std::expected<std::uint64_t, EffectRefusal> PaneSource::appendSound(const PAL::PlayTonesRequest& request, std::uint32_t sampleSerial, ID128 primary)
{
    SoundEffect sound { .request = request, .sample = {} };
    sound.request.samplePCM = nullptr;
    sound.request.sampleFrames = 0;
    if (request.wave != 6)
        return appendEffect(sound, primary);
    if (request.sampleFrames > VT::sampleMaxFrames || (request.sampleFrames && !request.samplePCM))
        return std::unexpected(EffectRefusal::Invalid);
    const std::span<const std::int16_t> pcm(request.samplePCM, request.sampleFrames);
    // Canonical producer scratch and the final copied log payload coexist.
    // Reserve both, plus the bounded kind-body copies, before capture.
    constexpr std::size_t bodyReserve = 2 * (sampleIdentityBytes + 29 + PAL::PlayTonesRequest::maxNotes * 7);
    const auto available = m_effects.availableRetainedBytes();
    if (available < bodyReserve || pcm.size_bytes() > (available - bodyReserve) / 2)
        return std::unexpected(EffectRefusal::OverLimit);
    auto captured = SampleBytes::capture(sampleSerial, pcm, (available - bodyReserve) / 2);
    if (!captured)
        return std::unexpected(EffectRefusal::NoMemory);
    sound.sample = captured->sample;
    const auto encoded = encodeEffectBody(sound);
    if (!encoded)
        return std::unexpected(encoded.error() == EffectCodecError::NoMemory ? EffectRefusal::NoMemory : EffectRefusal::Invalid);
    return m_effects.append({ .generation = m_generation, .primary = primary, .kind = EffectKind::Sound, .count = 1, .target = 0, .payload = encoded->view(), .data = captured->view() });
}

EnqueueOutcome PaneSource::queueInput(ID128 clientInstance, std::uint64_t inputSeq, InputIntent intent, std::uint64_t now, ID128 authority, std::size_t room)
{
    m_inputNow = std::max(m_inputNow, now);
    // A key the terminal answers with a signal does not wait its turn: the
    // terminal takes it past the input it holds unread, and so does the pane.
    const SignalEffect effect = m_inputWriter ? m_inputWriter->signalEffect(intent) : SignalEffect::None;
    const EnqueueOutcome outcome = effect == SignalEffect::None
        ? m_inputQueue.enqueue(m_inputLedger, clientInstance, inputSeq, std::move(intent), authority, room)
        : m_inputQueue.enqueueSignal(m_inputLedger, clientInstance, inputSeq, std::move(intent), effect, now, authority);
    if (outcome == EnqueueOutcome::Queued)
        drainInput(now);
    return outcome;
}

void PaneSource::abortPaste(ID128 clientInstance, std::uint64_t inputSeq, std::uint64_t now, ID128 authority)
{
    m_inputNow = std::max(m_inputNow, now);
    if (m_inputQueue.abortPaste(m_inputLedger, clientInstance, inputSeq, authority))
        drainInput(now);
}

void PaneSource::withdrawInputAuthority(ID128 authority, std::uint64_t now)
{
    m_inputNow = std::max(m_inputNow, now);
    m_inputQueue.withdraw(m_inputLedger, authority);
    if (m_inputWriter)
        m_inputWriter->withdraw(authority);
}

void PaneSource::abandonPaste(ID128 clientInstance, std::uint64_t now)
{
    m_inputNow = std::max(m_inputNow, now);
    m_inputQueue.abandonPaste(clientInstance);
    drainInput(now);
}

void PaneSource::noteDriverFocus(ID128 attachmentID, Ubiquitty::Modes::FocusState focus, ClientKind standing)
{
    const bool wasFocused = isAnyDriverFocused();
    const bool isListed = std::ranges::find(m_focusedDrivers, attachmentID) != m_focusedDrivers.end();
    if (focus == Ubiquitty::Modes::FocusState::Focused && !isListed) {
        m_focusedDrivers.push_back(attachmentID);
        if (standing == ClientKind::Human) {
            m_focusedPersons.push_back(attachmentID);
            ++m_personFocusGains;
        }
    } else if (focus == Ubiquitty::Modes::FocusState::Unfocused && isListed) {
        std::erase(m_focusedDrivers, attachmentID);
        std::erase(m_focusedPersons, attachmentID);
    }
    if (isAnyDriverFocused() == wasFocused)
        return;
    const Ubiquitty::Modes::FocusState pane = isAnyDriverFocused() ? Ubiquitty::Modes::FocusState::Focused : Ubiquitty::Modes::FocusState::Unfocused;
    if (m_inputQueue.enqueueUnnumbered(FocusInput { .focus = pane }) == EnqueueOutcome::Queued)
        drainInput(m_inputNow);
}

void PaneSource::drainInput(std::uint64_t now)
{
    m_inputNow = std::max(m_inputNow, now);
    if (m_inputWriter && m_inputQueue.drain(*m_inputWriter, m_inputLedger, now))
        noteInputWritten();
}

void PaneSource::publish(FrameClass frameClass, std::uint64_t dirtySinceMicroseconds)
{
    ++m_generation;
    m_dirtySince[m_generation % paneDirtyRingCapacity] = dirtySinceMicroseconds;
    ++m_changes;
    m_frameClass = frameClass;
    if (frameClass == FrameClass::Structural)
        m_structuralGeneration = m_generation;
    m_isEncodable = m_publisher.capture(m_terminal);
    // A line that cannot be said is left out of the published hashes; the
    // attachments' bodies skip it the same way.
    (void)m_history.capture(m_terminal);
    (void)m_images.capture();
    m_publisher.noteLinks(m_terminal, m_history.capturedLinkIDs());
    m_isFrameHeld = true;
    // The frame holds every echo settled by now: a settled write was read
    // whole before this capture.
    m_inputLedger.copySettled(m_settledAtPublish);
    // The echo context names the frame's modes and who typed into it.
    const std::uint64_t modesHash = m_publisher.blockHashes()[std::to_underlying(StateBlock::Modes)];
    if (modesHash != m_publishedModesHash) {
        m_publishedModesHash = modesHash;
        ++m_modesRevision;
    }
    m_frameWriters = m_inputLedger.takeRecentWriters();
}

std::optional<PaneSource::PublicationContinuation> PaneSource::publicationContinuation() const
{
    if (!m_isFrameHeld || !m_isEncodable || !m_generation)
        return std::nullopt;
    auto screen = m_publisher.continuation();
    auto history = m_history.continuation();
    if (!screen || !history)
        return std::nullopt;
    return PublicationContinuation { .paneID = m_paneID, .generation = m_generation, .structuralGeneration = m_structuralGeneration, .changes = m_changes, .modesRevision = m_modesRevision, .publishedModesHash = m_publishedModesHash, .dirtySince = m_dirtySince, .frameClass = m_frameClass, .screen = std::move(*screen), .history = std::move(*history), .images = m_images.state() };
}

bool PaneSource::restorePublication(PublicationContinuation continuation)
{
    if (m_generation || m_changes || !m_inputQueue.isEmpty() || continuation.paneID != m_paneID
        || !continuation.generation || continuation.generation == std::numeric_limits<std::uint64_t>::max()
        || continuation.structuralGeneration > continuation.generation || continuation.changes < continuation.generation
        || continuation.changes == std::numeric_limits<std::uint64_t>::max()
        || continuation.modesRevision == std::numeric_limits<std::uint32_t>::max()
        || (!continuation.modesRevision && continuation.publishedModesHash)
        || std::to_underlying(continuation.frameClass) > std::to_underlying(FrameClass::Structural))
        return false;
    ScreenPublisher screen;
    HistoryPublisher history;
    if (!screen.restore(m_terminal, std::move(continuation.screen)) || !history.restore(m_terminal, std::move(continuation.history)))
        return false;
    if (continuation.publishedModesHash != screen.blockHashes()[std::to_underlying(StateBlock::Modes)])
        return false;
    if (!m_images.restore(continuation.images))
        return false;
    m_publisher = std::move(screen);
    m_history = std::move(history);
    m_generation = continuation.generation;
    m_structuralGeneration = continuation.structuralGeneration;
    m_changes = continuation.changes;
    m_modesRevision = continuation.modesRevision;
    m_publishedModesHash = continuation.publishedModesHash;
    m_dirtySince = continuation.dirtySince;
    m_frameClass = continuation.frameClass;
    m_isEncodable = true;
    m_isFrameHeld = true;
    return true;
}

std::optional<PaneSource::Continuation> PaneSource::continuation() const
{
    auto publication = publicationContinuation();
    auto input = m_inputLedger.continuation();
    auto geometry = m_geometry.continuation();
    auto queue = m_inputQueue.continuation(m_inputLedger);
    if (!publication || !input || !geometry || !queue
        || m_frameWriters.size() > maxLastWrittenEntries || m_settledAtPublish.size() > maxLastWrittenEntries
        || m_focusedDrivers.size() > maxEffectReaders || m_focusedPersons.size() > maxEffectReaders)
        return std::nullopt;
    return Continuation { .publication = std::move(*publication), .effects = m_effects.continuation(), .input = *input, .geometry = *geometry, .queue = std::move(*queue), .inputPolicy = m_inputPolicy, .inputAwaitedGeneration = m_inputAwaitedGeneration, .echoFacts = m_echoFacts, .facts = m_facts, .revisedActivity = m_revisedActivity, .status = m_status, .frameWriters = m_frameWriters, .settledAtPublish = m_settledAtPublish, .focusedDrivers = m_focusedDrivers, .focusedPersons = m_focusedPersons, .personFocusGains = m_personFocusGains, .inputNow = m_inputNow };
}

std::unique_ptr<PaneSource> PaneSource::restored(const Ubiquitty::Terminal& terminal, Continuation state)
{
    const auto uniqueIdentities = [](const auto& values, std::size_t maximum) {
        if (values.size() > maximum)
            return false;
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (values[index] == ID128 {} || std::find(values.begin(), values.begin() + index, values[index]) != values.begin() + index)
                return false;
        }
        return true;
    };
    if (!uniqueIdentities(state.frameWriters, maxLastWrittenEntries)
        || !uniqueIdentities(state.focusedDrivers, maxEffectReaders) || !uniqueIdentities(state.focusedPersons, maxEffectReaders)
        || state.settledAtPublish.size() > maxLastWrittenEntries
        || state.publication.generation == std::numeric_limits<std::uint64_t>::max()
        || state.inputAwaitedGeneration > state.publication.generation + 1
        || state.effects.paneID != state.publication.paneID || state.effects.lastGeneration > state.publication.generation
        || state.inputPolicy.pasteWindowBytes > maxPendingInputBytes)
        return nullptr;
    for (const auto identity : state.focusedPersons) {
        if (std::ranges::find(state.focusedDrivers, identity) == state.focusedDrivers.end())
            return nullptr;
    }
    auto candidate = std::unique_ptr<PaneSource>(new (std::nothrow) PaneSource(terminal, state.publication.paneID,
        state.inputPolicy, state.effects.policy));
    if (!candidate)
        return nullptr;
    candidate->m_inputLedger = InputLedger(state.input.capacity);
    candidate->m_geometry = GeometryArbiter(state.geometry.policy);
    if (!candidate->m_inputLedger.restoreContinuation(state.input) || !candidate->m_geometry.restoreContinuation(state.geometry)
        || !candidate->m_effects.restoreContinuation(state.effects)
        || !candidate->restorePublication(std::move(state.publication))
        || !candidate->m_inputQueue.restoreContinuation(std::move(state.queue), candidate->m_inputLedger))
        return nullptr;
    for (std::size_t index = 0; index < state.settledAtPublish.size(); ++index) {
        const auto& mark = state.settledAtPublish[index];
        if (mark.clientInstance == ID128 {} || mark.settledSeq > candidate->m_inputLedger.settledThrough(mark.clientInstance))
            return nullptr;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (state.settledAtPublish[previous].clientInstance == mark.clientInstance)
                return nullptr;
        }
    }
    candidate->m_inputAwaitedGeneration = state.inputAwaitedGeneration;
    candidate->m_echoFacts = state.echoFacts;
    candidate->m_facts = std::move(state.facts);
    candidate->m_revisedActivity = state.revisedActivity;
    candidate->m_status = std::move(state.status);
    candidate->m_frameWriters = std::move(state.frameWriters);
    candidate->m_settledAtPublish = std::move(state.settledAtPublish);
    candidate->m_focusedDrivers = std::move(state.focusedDrivers);
    candidate->m_focusedPersons = std::move(state.focusedPersons);
    candidate->m_personFocusGains = state.personFocusGains;
    candidate->m_inputNow = state.inputNow;
    return candidate;
}

void PaneSource::republish(std::uint64_t now)
{
    ++m_generation;
    m_dirtySince[m_generation % paneDirtyRingCapacity] = now;
}

void PaneSource::setFacts(PaneFacts facts, ActivityStamps stamps)
{
    facts.geometry = m_geometry.status();
    facts = boundedForWire(std::move(facts));
    // Stamps are compared with the ones a client was last sent, not with the
    // ones that rode along since: otherwise a stamp that waited would never
    // be the one a publish finds new.
    const bool isMoved = m_facts.revision == 0 || differsBeyondActivity(m_facts, facts) || (stamps == ActivityStamps::Publish && !(facts.activity == m_revisedActivity));
    facts.revision = m_facts.revision + (isMoved ? 1 : 0);
    if (isMoved)
        m_revisedActivity = facts.activity;
    m_facts = std::move(facts);
}

void PaneSource::setStatus(StatusRecords records)
{
    records = boundedForWire(std::move(records));
    const bool isMoved = m_status.revision == 0 || !(records.store == m_status.store);
    if (!isMoved)
        return;
    records.revision = m_status.revision + 1;
    m_status = std::move(records);
}

EchoContext PaneSource::echoContextFor(ID128 clientInstance) const
{
    EchoContext context { .modesRevision = m_modesRevision, .foregroundGeneration = m_echoFacts.foregroundGeneration, .flags = {}, .interruptCharacters = m_echoFacts.interruptCharacters, .eraseCharacter = m_echoFacts.eraseCharacter };
    context.flags.set(EchoFlag::SecureEntry, m_echoFacts.isSecureEntry);
    context.flags.set(EchoFlag::AtPrompt, m_echoFacts.isAtPrompt);
    context.flags.set(EchoFlag::ForeignInput, std::ranges::any_of(m_frameWriters, [&](ID128 writer) { return writer != clientInstance; }));
    return context;
}

std::uint64_t PaneSource::settledAtPublish(ID128 clientInstance) const
{
    const auto found = std::ranges::find(m_settledAtPublish, clientInstance, &SettledMark::clientInstance);
    return found == m_settledAtPublish.end() ? 0 : found->settledSeq;
}

std::uint64_t PaneSource::dirtySince(std::uint64_t generation) const
{
    if (m_generation == 0)
        return 0;
    const std::uint64_t oldest = m_generation >= paneDirtyRingCapacity ? m_generation - paneDirtyRingCapacity + 1 : 1;
    const std::uint64_t held = std::clamp(generation, oldest, m_generation);
    return m_dirtySince[held % paneDirtyRingCapacity];
}

// MARK: - HostConnection

HostConnection::HostConnection(const HostPolicy& policy, HostIdentity identity)
    : m_policy(policy)
    , m_identity(std::move(identity))
    , m_scheduler(policy.lanes)
    , m_pacer(policy.pacing)
{
}

void HostConnection::receiveStream(std::span<const std::uint8_t> bytes, std::uint64_t now)
{
    if (m_state == ConnectionState::Closed)
        return;
    if (m_isReceiving) {
        close(ByeReason::Malformed, "a reentrant receive on the same connection");
        return;
    }
    m_lastReceiveNow = now;
    releaseInputBehindCalls();
    if (m_state == ConnectionState::Closed)
        return;
    m_isReceiving = true;
    FrameHeader header;
    EnvelopeError error = EnvelopeError::None;
    bool wasCapacityRefused = false;
    bool wasConsentRefused = false;
    const auto result = m_decoder.consume(bytes, [&](std::span<const std::uint8_t> prefix) {
        switch (readFrameHeader(prefix, header, error)) {
        case FrameHeaderState::NeedsMore:
            return FrameHeaderDecision {};
        case FrameHeaderState::Failed:
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        case FrameHeaderState::Complete:
            break;
        }
        if (directionOf(header.kind) == Direction::HostToClient
            || (m_state == ConnectionState::Greeting && header.kind != FrameKind::Greet && header.kind != FrameKind::Bye)
            || (m_state != ConnectionState::Greeting && header.kind == FrameKind::Greet)) {
            error = EnvelopeError::Malformed;
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        }
        if (isConsentPending(m_state) && !isPendingTransportFrame(header.kind)) {
            wasConsentRefused = true;
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        }
        if (m_inboundFrameAdmission && !m_inboundFrameAdmission(std::max(header.bodyBytes, m_decoder.retainedBytes()))) {
            wasCapacityRefused = true;
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        }
        return FrameHeaderDecision { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
        // A body may finish on a later call, whose local header starts empty.
        (void)readFrameHeader(prefix, header, error);
        handleFrame({ .kind = header.kind, .channel = header.channel, .body = body }, Carrier::Stream, now);
        return m_state == ConnectionState::Closed ? FrameDelivery::Stop : FrameDelivery::Continue; });
    m_isReceiving = false;
    if (result.state == FrameIngress::State::Refused && m_state != ConnectionState::Closed)
        close(wasConsentRefused ? ByeReason::OutsideScope : wasCapacityRefused || m_decoder.error() == FrameIngressError::AllocationFailed ? ByeReason::TransferRefused
                                                                                                                                           : byeReasonOf(error),
            {});
}

bool HostConnection::isHeldBehindCalls(FrameKind kind)
{
    switch (kind) {
    case FrameKind::Raw:
    case FrameKind::Keys:
    case FrameKind::Pointer:
    case FrameKind::Paste:
    case FrameKind::PasteAbort:
    case FrameKind::Focus:
    case FrameKind::Viewport:
        return true;
    default:
        return false;
    }
}

void HostConnection::holdBehindCalls(const Frame& frame)
{
    if (m_inputBehindCalls.size() >= maxFramesHeldBehindCalls || frame.body.size() > maxBytesHeldBehindCalls - m_bytesHeldBehindCalls) {
        close(ByeReason::Oversize, "input held behind unserved calls past its bound");
        return;
    }
    m_inputBehindCalls.push_back({ .kind = frame.kind, .channel = frame.channel, .body = { frame.body.begin(), frame.body.end() } });
    m_bytesHeldBehindCalls += frame.body.size();
}

void HostConnection::releaseInputBehindCalls()
{
    if (!m_receivedCalls.empty() || m_inputBehindCalls.empty() || m_state == ConnectionState::Closed)
        return;
    const std::vector<HeldFrame> held = std::exchange(m_inputBehindCalls, {});
    m_bytesHeldBehindCalls = 0;
    for (const HeldFrame& frame : held) {
        handleFrame({ .kind = frame.kind, .channel = frame.channel, .body = frame.body }, Carrier::Stream, m_lastReceiveNow);
        if (m_state == ConnectionState::Closed)
            return;
    }
}

void HostConnection::receiveDatagram(std::span<const std::uint8_t> datagram, std::uint64_t now)
{
    if (m_state == ConnectionState::Closed)
        return;
    if (m_isReceiving) {
        close(ByeReason::Malformed, "a reentrant receive on the same connection");
        return;
    }
    EnvelopeError error = EnvelopeError::None;
    const std::optional<Frame> frame = decodeDatagram(datagram, error);
    if (!frame) {
        close(byeReasonOf(error), {});
        return;
    }
    m_isReceiving = true;
    handleFrame(*frame, Carrier::Datagram, now);
    m_isReceiving = false;
}

void HostConnection::handleFrame(const Frame& frame, Carrier carrier, std::uint64_t now)
{
    if (isConsentPending(m_state) && !isPendingTransportFrame(frame.kind)) {
        close(ByeReason::OutsideScope, "consent has not admitted this attachment");
        return;
    }
    if (m_state == ConnectionState::Attached && m_table && m_record
        && (m_record->state != AttachmentState::Live || m_record->servingConnection != m_connection)) {
        supersede();
        return;
    }
    if (directionOf(frame.kind) == Direction::HostToClient) {
        close(ByeReason::Malformed, "a host-to-client frame from the client");
        return;
    }
    if (m_state == ConnectionState::Greeting && frame.kind != FrameKind::Greet && frame.kind != FrameKind::Bye) {
        close(ByeReason::Malformed, "a frame before Greet");
        return;
    }
    if (!admitsSender(frame.kind, m_grantedRole)) {
        const std::string_view detail = frame.kind == FrameKind::Viewport ? viewportAuthorityRefusal() : std::string_view {};
        close(ByeReason::Malformed, detail.empty() ? "a frame the granted role may not send" : detail);
        return;
    }
    // The client's first frame after its Welcome ends a resume's handshake.
    if (m_state == ConnectionState::Attached && !m_hasHeardSinceWelcome) {
        m_hasHeardSinceWelcome = true;
        if (m_table && m_record)
            m_table->noteFirstFrame(m_record->attachmentID, m_connection);
    }
    // Only a Probe or an Echo may ride a datagram: a pane's frames and the
    // client's Acks, Facts and Resyncs are ordered by the stream.
    if (carrier == Carrier::Datagram && frame.kind != FrameKind::Echo && frame.kind != FrameKind::Probe) {
        close(ByeReason::Malformed, "a stream-only frame in a datagram");
        return;
    }
    // What a pane's driver sends waits behind calls the client sent before
    // it, until the owner has served them: a call can retire the pane the
    // input names, and the stream's order is the client's.
    if (isHeldBehindCalls(frame.kind) && (!m_receivedCalls.empty() || !m_inputBehindCalls.empty())) {
        holdBehindCalls(frame);
        return;
    }
    switch (frame.kind) {
    case FrameKind::Greet: {
        if (m_state != ConnectionState::Greeting || carrier == Carrier::Datagram) {
            close(ByeReason::Malformed, "an unreadable or repeated Greet");
            return;
        }
        const auto version = decodeHandshakeVersion(frame.body);
        if (!version) {
            close(ByeReason::Malformed, "an incomplete Greet version");
            return;
        }
        // A build whose layouts differ is refused from the prefix every
        // build reads alike, before anything past it is taken as this
        // build's: a standing version admits a peer whose bodies moved.
        const std::optional<EngineFingerprint> fingerprint = decodeHandshakeFingerprint(frame.body);
        if (*version == m_identity.wireVersion && !fingerprint) {
            close(ByeReason::Malformed, "an incomplete Greet build");
            return;
        }
        if (*version != m_identity.wireVersion || fingerprint != m_identity.engineFingerprint) {
            close(ByeReason::VersionMismatch, versionMismatchNotice());
            return;
        }
        const std::optional<Greet> greet = decodeGreet(frame.body);
        if (!greet) {
            close(ByeReason::Malformed, "an unreadable Greet");
            return;
        }
        if (m_greetingAdmission) {
            auto admitted = m_greetingAdmission(*greet);
            if (!admitted) {
                auto refusal = std::move(admitted.error());
                if (!refusal.peer)
                    refusal.peer = spokenBye(m_identity.wireVersion, m_identity.appVersion, m_identity.selection.hostPlatform, refusal.reason, {}).peer;
                close(std::move(refusal));
                return;
            }
            m_identity.verifiedKind = admitted->verifiedKind;
            m_identity.mayDrive = admitted->mayDrive;
            m_identity.maxRole = admitted->maxRole;
            m_identity.principal = std::move(admitted->principal);
            m_identity.selection = std::move(admitted->selection);
            m_identity.pendingConsent = admitted->pendingConsent;
            m_authority = { .principal = m_identity.principal, .mayDrive = m_identity.mayDrive, .verifiedKind = m_identity.verifiedKind };
        }
        // A workspace recreated from its record answers the saved reference
        // with fresh sessions, under the id this host gave it: its identity
        // is the record's of that reference, and the instance the client
        // named.
        const bool isRestoredReference = isRestoredReferenceAnswer(greet->request, m_identity.selection);
        if (!isValidSelection(m_identity.selection) || m_identity.selection.target != greet->request.target
            || (m_identity.selection.restoredFromDaemonInstanceID && !isRestoredReference)
            || (greet->request.sessionID && !isRestoredReference && m_identity.selection.sessionID != greet->request.sessionID)
            || (greet->request.workspaceID && !isRestoredReference && m_identity.selection.workspaceID != greet->request.workspaceID)) {
            close(ByeReason::SessionGone, "the authenticated selection does not name the requested target");
            return;
        }
        m_facts = greet->facts;
        m_factsAt = now;
        m_clientKind = m_identity.verifiedKind == ClientKind::Agent ? ClientKind::Agent : greet->clientKind;
        m_clientInstance = greet->clientInstance;
        m_historyCacheLines = greet->historyCacheLines;
        m_capabilities = greet->capabilities;
        m_backfillDepthLines = greet->backfillDepthLines;
        Welcome welcome;
        welcome.wireVersion = m_identity.wireVersion;
        welcome.appVersion = m_identity.appVersion;
        welcome.engineFingerprint = m_identity.engineFingerprint;
        welcome.hostInstanceID = m_identity.hostInstanceID;
        welcome.predecessorHostInstanceID = m_identity.predecessorHostInstanceID;
        welcome.attachmentID = m_identity.attachmentID;
        welcome.resumeSecret = m_identity.resumeSecret;
        // What it declares only ever lowers what it is granted: a client the
        // host verified as an agent is one whatever its Greet says.
        m_grantedRole = grantRole(greet->requestedRole, m_identity.verifiedKind == ClientKind::Agent ? ClientKind::Agent : greet->clientKind, m_identity.maxRole);
        welcome.grantedRole = m_grantedRole;
        welcome.selection = m_identity.selection;
        welcome.principal = m_identity.principal;
        welcome.limits = m_policy.limits;
        welcome.limits.probeSteps = 0;
        for (std::size_t step = 0; step < maxProbeSteps; ++step) {
            const std::uint64_t interval = m_pacer.probeIntervalMicroseconds(step);
            welcome.limits.probeScheduleMicroseconds[step] = static_cast<std::uint32_t>(std::min<std::uint64_t>(interval, std::numeric_limits<std::uint32_t>::max()));
            welcome.limits.probeSteps = static_cast<std::uint8_t>(step + 1);
            if (interval >= m_policy.pacing.maxProbeIntervalMicroseconds)
                break;
        }
        welcome.hostTime = now;
        welcome.pasteLimits = PasteLimits { .chunkBytes = static_cast<std::uint32_t>(maxPasteChunkBytes), .pasteBytes = static_cast<std::uint32_t>(maxPasteBytes), .windowBytes = m_policy.input.pasteWindowBytes };
        std::vector<PaneResumeBinding> bindings;
        if (m_identity.pendingConsent && !m_table) {
            close(ByeReason::NoConsent, "a pending attachment requires its authority table");
            return;
        }
        if (const std::optional<ByeReason> refusal = m_table ? admitAttachment(*greet, welcome, bindings, now) : std::nullopt) {
            close(*refusal, {});
            return;
        }
        std::vector<std::uint8_t> body;
        encode(body, welcome);
        if (body.empty()) {
            close(ByeReason::Oversize, "the complete Welcome cannot be encoded");
            return;
        }
        enqueue(FrameKind::Welcome, 0, body);
        m_state = welcome.admission == AttachmentAdmission::Granted ? ConnectionState::Attached : ConnectionState::AwaitingConsent;
        for (const PaneResumeBinding& binding : bindings)
            continueResumedPane(binding);
        return;
    }
    case FrameKind::Facts: {
        const std::optional<Facts> facts = decodeFacts(frame.body);
        if (!facts) {
            close(ByeReason::Malformed, "an unreadable Facts");
            return;
        }
        const bool wasMetered = isMetered();
        m_facts = *facts;
        m_factsAt = now;
        // A window losing the focus takes its driver's focus off every pane,
        // and its size reaches every pane's geometry.
        for (PaneAttachment& attachment : m_panes) {
            if (!attachment.source)
                continue;
            noteDriverFocus(attachment);
            noteViewport(attachment);
        }
        // A link that turns metered stops the push; one that turns unmetered
        // resumes it from where the history stands.
        if (wasMetered != isMetered()) {
            for (PaneAttachment& attachment : m_panes) {
                if (attachment.source && attachment.history.epoch == attachment.source->historyAddress().epoch)
                    setImplicitBackfill(attachment, BackfillCause::LinkChanged);
            }
        }
        return;
    }
    case FrameKind::TypographyFacts: {
        const std::optional<RichText::TypographyFacts> facts = decodeTypographyFacts(frame.body);
        if (!facts) {
            close(ByeReason::Malformed, "an unreadable TypographyFacts");
            return;
        }
        // A client publishes typography whenever its renderer changes, so one
        // can cross a PaneUnbind the host has already sent.
        if (isUnbound(frame.channel))
            return;
        PaneAttachment* attachment = pane(frame.channel);
        if (!attachment) {
            close(ByeReason::UnboundHandle, "a TypographyFacts for an unbound pane");
            return;
        }
        if (!attachment->typography || *attachment->typography != *facts)
            attachment->typography = std::make_shared<const RichText::TypographyFacts>(*facts);
        return;
    }
    case FrameKind::ProportionalFaces: {
        if (!m_capabilities.contains(ClientCapability::PublishesFaces)) {
            close(ByeReason::Malformed, "ProportionalFaces without the Greet capability");
            return;
        }
        std::optional<OSC::TextSizing::ProportionalFaceFacts> faces = decodeProportionalFaces(frame.body);
        if (!faces) {
            close(ByeReason::Malformed, "an unreadable ProportionalFaces");
            return;
        }
        if (!m_proportionalFaces || *m_proportionalFaces != *faces)
            m_proportionalFaces = std::make_shared<const OSC::TextSizing::ProportionalFaceFacts>(std::move(*faces));
        if (m_record)
            m_record->inheritedProportionalFaces.reset();
        return;
    }
    case FrameKind::Ack: {
        const std::optional<Ack> ack = decodeAck(frame.body);
        if (!ack) {
            close(ByeReason::Malformed, "an unreadable Ack");
            return;
        }
        handleAck(*ack, now);
        return;
    }
    case FrameKind::Echo: {
        const std::optional<Echo> echo = decodeEcho(frame.body);
        if (!echo) {
            close(ByeReason::Malformed, "an unreadable Echo");
            return;
        }
        (void)m_pacer.noteEcho(now, echo->timeEcho, echo->holdMicroseconds);
        m_unheardSince.reset();
        return;
    }
    case FrameKind::Probe: {
        const std::optional<Probe> probe = decodeProbe(frame.body);
        if (!probe) {
            close(ByeReason::Malformed, "an unreadable Probe");
            return;
        }
        // The newest probe is answered; one owed already is replaced, so a
        // peer that floods probes and reads nothing is owed one Echo.
        m_owedEcho = Echo { .timeEcho = probe->time, .holdMicroseconds = 0 };
        m_owedEchoReceivedAt = now;
        m_owedEchoCarrier = carrier;
        return;
    }
    case FrameKind::Resync: {
        const std::optional<Resync> resync = decodeResync(frame.body);
        PaneAttachment* attachment = pane(frame.channel);
        if (resync && isUnbound(frame.channel))
            return;
        if (!resync || !attachment) {
            close(resync ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable Resync or one for an unbound pane");
            return;
        }
        handleResync(*attachment, *resync);
        return;
    }
    case FrameKind::View: {
        const std::optional<View> view = decodeView(frame.body);
        if (view && isUnbound(frame.channel))
            return;
        PaneAttachment* attachment = pane(frame.channel);
        if (!view || !attachment) {
            close(view ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable View or one for an unbound pane");
            return;
        }
        attachment->isViewStated = true;
        setPaneView(*attachment, view->state);
        return;
    }
    case FrameKind::Raw:
    case FrameKind::Keys:
    case FrameKind::Pointer:
    case FrameKind::Paste:
    case FrameKind::PasteAbort:
        handleInput(frame, carrier, now);
        return;
    case FrameKind::NativeDragEvent: {
        auto input = decodeNativeDragEvent(frame.body);
        if (!input || carrier == Carrier::Datagram) {
            close(ByeReason::Malformed, "invalid native drag input");
            return;
        }
        if (m_clientKind != ClientKind::Human || !mayDrive() || !supportsNativeDrag() || !pane(frame.channel) || !m_nativeDragAdmission
            || !m_nativeDragAdmission(frame.channel, input->epoch))
            return;
        if (m_nativeDragEvents.size() == 256) {
            close(ByeReason::TransferRefused, "native drag input queue is full");
            return;
        }
        input->handle = frame.channel;
        m_nativeDragEvents.push_back(std::move(*input));
        return;
    }
    case FrameKind::Focus: {
        const std::optional<Focus> focus = decodeFocus(frame.body);
        if (!focus || carrier == Carrier::Datagram) {
            close(ByeReason::Malformed, "an unreadable Focus, or one in a datagram");
            return;
        }
        if (isUnbound(frame.channel))
            return;
        PaneAttachment* attachment = pane(frame.channel);
        if (!attachment) {
            close(ByeReason::UnboundHandle, "a Focus for an unbound pane");
            return;
        }
        attachment->clientFocus = focus->isFocused ? Ubiquitty::Modes::FocusState::Focused : Ubiquitty::Modes::FocusState::Unfocused;
        noteDriverFocus(*attachment);
        return;
    }
    case FrameKind::Viewport:
        handleViewport(frame, carrier, now);
        return;
    case FrameKind::RichTextMeasured: {
        WireReader reader(frame.body);
        const auto token = reader.u32();
        const auto found = std::ranges::find_if(m_pendingRichText, [&](const auto& held) { return held.handle == frame.channel && held.token == token; });
        // An answer may arrive after its timeout or screen teardown. It has
        // no effect without the exact outstanding pane/request binding.
        if (found == m_pendingRichText.end())
            return;
        if (found->expiresAt <= now) {
            m_pendingRichText.erase(found);
            return;
        }
        const auto total = reader.u32();
        const auto offset = reader.u32();
        const auto chunk = reader.take(reader.remaining());
        if (!reader.ok() || !total || total > (found->fontCatalog ? maxRichTextFontCatalogBytes : maxRichTextMeasurementBytes) || chunk.empty()
            || offset != found->bytes.size() || offset > total || chunk.size() > total - offset
            || (found->totalBytes && found->totalBytes != total)) {
            close(ByeReason::Malformed, "invalid rich-text measurement chunk");
            return;
        }
        found->totalBytes = total;
        found->bytes.insert(found->bytes.end(), chunk.begin(), chunk.end());
        if (found->bytes.size() != total)
            return;
        WireReader body(found->bytes);
        RichText::Measurement measured;
        if (!readMeasurement(body, measured) || !body.isAtEnd()
            || measured.rendererRevision != found->rendererRevision
            || measured.fontCatalog.has_value() != found->fontCatalog.has_value()
            || (found->fontCatalog ? !RichText::isFontCatalogPage(*found->fontCatalog, *measured.fontCatalog) || measured.fontCatalog->scope != found->fontCatalogScope
                                   : measured.shown > found->characters || measured.lineEnds.empty() || measured.lineEnds.back() != found->characters)) {
            close(ByeReason::Malformed, "invalid rich-text measurement geometry");
            return;
        }
        m_pendingRichText.erase(found);
        m_richTextReplies.push_back({ frame.channel, token, std::move(measured) });
        return;
    }
    case FrameKind::ClipboardData:
        receiveClipboardData(frame.channel, frame.body);
        return;
    case FrameKind::BulkChunk: {
        const auto chunk = decodeBulkChunk(frame.body);
        if (!chunk) {
            close(ByeReason::Malformed, "an unreadable client bulk chunk");
            return;
        }
        if (m_controlUploadReceiver) {
            switch (m_controlUploadReceiver(*chunk)) {
            case ClientBulkDisposition::Accepted:
                return;
            case ClientBulkDisposition::Refused:
                close(ByeReason::Malformed, "a client chunk refused by its credited upload");
                return;
            case ClientBulkDisposition::Unhandled:
                break;
            }
        }
        receiveClipboardReplyChunk(*chunk);
        return;
    }
    case FrameKind::Bye: {
        const std::optional<Bye> bye = decodeBye(frame.body);
        m_byeReason = bye ? bye->reason : ByeReason::Malformed;
        m_state = ConnectionState::Closed;
        discardInboundClipboard();
        // No Ack follows a Bye, so a chunk the window holds would never leave.
        m_scheduler.abandonTransfers();
        releaseDriverFocus();
        // A client that says goodbye sets no pane's size again.
        if (m_byeReason == ByeReason::Normal) {
            for (PaneAttachment& attachment : m_panes) {
                if (attachment.source)
                    attachment.source->geometry().forget(attachmentIdentity());
            }
        }
        // A client that says goodbye leaves its attachment; one that closes
        // for any other reason may come back for it.
        if (m_table && m_record) {
            const ID128 attachmentID = m_record->attachmentID;
            releaseRecord();
            if (m_byeReason == ByeReason::Normal)
                m_table->noteDetached(attachmentID);
            else
                m_table->noteTransportLost(attachmentID, m_connection, now);
        }
        return;
    }
    case FrameKind::HistoryWant: {
        const std::optional<HistoryWant> want = decodeHistoryWant(frame.body);
        if (want && isUnbound(frame.channel))
            return;
        if (!want || !pane(frame.channel)) {
            close(want ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable HistoryWant or one for an unbound pane");
            return;
        }
        // A client that keeps no history is sent none, wanted or not.
        if (m_historyCacheLines > 0)
            takeWant(*pane(frame.channel), *want);
        return;
    }
    case FrameKind::ImageWant: {
        const auto want = decodeImageWant(frame.body);
        if (want && isUnbound(frame.channel))
            return;
        PaneAttachment* attachment = pane(frame.channel);
        if (!want || !attachment) {
            close(want ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable ImageWant or one for an unbound pane");
            return;
        }
        // A hash addresses only the assets this pane has already offered on
        // this admitted connection, never another pane or a global cache.
        if (attachment->view != ViewState::Live)
            return;
        for (auto& image : attachment->images) {
            if (image && image->asset.hash == want->hash && image->isOffered) {
                if (!image->transfer || !m_scheduler.isTransferQueued(image->transfer))
                    image->isWanted = true;
                break;
            }
        }
        return;
    }
    case FrameKind::Call: {
        std::optional<Call> call = decodeCall(frame.body);
        if (!call) {
            close(ByeReason::Malformed, "a Call that is not UTF-8");
            return;
        }
        handleCall(std::move(*call));
        return;
    }
    // The host's own kinds, refused by their direction before this switch.
    case FrameKind::Result:
    case FrameKind::Notify:
    case FrameKind::Welcome:
    case FrameKind::PaneBind:
    case FrameKind::PaneUnbind:
    case FrameKind::SampleHead:
    case FrameKind::RichTextMeasure:
    case FrameKind::ClipboardRequest:
    case FrameKind::NativeDragCommand:
    case FrameKind::WorkspaceStructure:
    case FrameKind::PaneListed:
    case FrameKind::BulkAbort:
    case FrameKind::CheckpointHead:
    case FrameKind::Update:
    case FrameKind::PaneDirty:
    case FrameKind::PanePalette:
    case FrameKind::HistoryInline:
    case FrameKind::HistoryHead:
    case FrameKind::HistoryReset:
    case FrameKind::PaneFacts:
    case FrameKind::StatusRecords:
    case FrameKind::Effect:
    case FrameKind::ImageHead:
    case FrameKind::ImagePlacements:
    case FrameKind::Receipt:
    case FrameKind::PasteReceipt:
    case FrameKind::InputDiscarded:
        break;
    }
    close(ByeReason::Malformed, "a frame the host does not accept");
}

void HostConnection::handleAck(const Ack& ack, std::uint64_t now)
{
    for (const PaneAck& paneAck : ack.panes) {
        if (isUnbound(paneAck.handle))
            continue;
        PaneAttachment* attachment = pane(paneAck.handle);
        if (!attachment) {
            close(ByeReason::UnboundHandle, "an Ack for an unbound pane");
            return;
        }
        if (paneAck.effectsAcknowledged > attachment->base->effectsSent) {
            close(ByeReason::Malformed, "an Ack for effects never emitted");
            return;
        }
        attachment->sampleEffectsAcknowledged = std::max(attachment->sampleEffectsAcknowledged, paneAck.effectsAcknowledged);
        if (paneAck.effectsAcknowledged > attachment->base->effectsAcknowledged) {
            if (!attachment->source->effects().acknowledge(attachmentIdentity(), paneAck.effectsAcknowledged)) {
                close(ByeReason::Malformed, "an invalid effect Ack");
                return;
            }
            attachment->base->effectsAcknowledged = paneAck.effectsAcknowledged;
            attachment->source->effects().trim(now);
        }
        // A generation the host never sent, or one an earlier Ack already
        // passed, acknowledges nothing: a forged one must not retire the
        // datagrams of generations still in flight.
        attachment->history.ackedHead = std::max(attachment->history.ackedHead, paneAck.historyAcked);
        const auto sent = std::ranges::find_if(attachment->base->inFlight, [&](const SentGeneration& entry) { return entry.generation == paneAck.generation; });
        if (sent == attachment->base->inFlight.end())
            continue;
        // Retired before the connection fields, so the delivery-rate sample
        // this Ack closes counts the datagrams it acknowledges.
        m_pacer.retireDatagrams(attachment->handle, paneAck.generation);
        attachment->base->acknowledgedGeneration = paneAck.generation;
        attachment->base->acknowledgedBaseline = sent->baseline;
        attachment->base->acknowledgedHistory = sent->history;
        std::erase_if(attachment->base->inFlight, [&](const SentGeneration& entry) { return entry.generation <= paneAck.generation; });
        if (attachment->unacknowledgedCheckpoint && paneAck.generation >= *attachment->unacknowledgedCheckpoint)
            attachment->unacknowledgedCheckpoint.reset();
    }
    m_pacer.noteAck(now, ack.hostTimeEcho, ack.holdMicroseconds, ack.bytesReceived);
    m_unheardSince.reset();
    m_isWindowRefused = false;
}

void HostConnection::handleResync(PaneAttachment& attachment, const Resync& resync)
{
    ++m_counters.resyncsReceived;
    m_counters.lastResyncGeneration = resync.generation;
    m_counters.lastResyncFirstRow = -1;
    const AttachmentBaseline* held = nullptr;
    if (attachment.base->acknowledgedGeneration == resync.generation)
        held = &attachment.base->acknowledgedBaseline;
    for (const SentGeneration& entry : attachment.base->inFlight) {
        if (entry.generation == resync.generation)
            held = &entry.baseline;
    }
    if (held) {
        const std::vector<std::uint64_t>& heldRows = held->activeRowHashes();
        const std::size_t rows = std::min(heldRows.size(), resync.sectionHashes.size());
        for (std::size_t row = 0; row < rows; ++row) {
            if (heldRows[row] != resync.sectionHashes[row]) {
                m_counters.lastResyncFirstRow = static_cast<int>(row);
                break;
            }
        }
    }
    // A refused body is refused for its state, so the state is remembered
    // and not sent again.
    if (resync.reason == ResyncReason::CheckpointRefused && attachment.sentCheckpointDigest) {
        attachment.refusalsOfDigest = attachment.refusedCheckpointDigest == attachment.sentCheckpointDigest ? attachment.refusalsOfDigest + 1 : 1;
        attachment.refusedCheckpointDigest = attachment.sentCheckpointDigest;
        ++m_counters.checkpointsRefused;
    }
    // The latest want wins: a body still crossing is of the state the client
    // has just said it cannot build on.
    abortCheckpoint(attachment);
    attachment.needsCheckpoint = true;
}

std::uint32_t HostConnection::bindPane(PaneSource& source)
{
    if (m_state != ConnectionState::Attached || isUnboundTarget(m_identity.selection.target))
        return 0;
    if (std::ranges::count_if(m_panes, [](const PaneAttachment& bound) { return bound.source != nullptr; }) >= static_cast<std::ptrdiff_t>(maxPanesPerConnection))
        return 0;
    PaneAttachment attachment;
    attachment.source = &source;
    PaneBaselines* const previous = m_record ? m_record->findPane(source.paneID()) : nullptr;
    if (m_record) {
        attachment.base = &m_record->paneFor(source.paneID());
    } else {
        attachment.ownedBase = std::make_unique<PaneBaselines>();
        attachment.base = attachment.ownedBase.get();
    }
    attachment.base->paneID = source.paneID();
    const auto refuseBinding = [&] {
        // A refused resource admission must not create a resumable holding.
        if (m_record && !previous)
            std::erase_if(m_record->panes, [&](const auto& held) { return held.get() == attachment.base; });
        return std::uint32_t { 0 };
    };
    EffectLog& effects = source.effects();
    if (!attachment.base->hasEffectReader) {
        const std::uint64_t through = effects.newestSequence();
        if (!m_table && !m_effectReaderLifetime)
            m_effectReaderLifetime = std::make_shared<EffectReaderLifetime>();
        const auto registered = m_table ? effects.addReader(attachmentIdentity(), m_clientInstance, through)
                                        : effects.addTransientReader(attachmentIdentity(), m_clientInstance, through, m_effectReaderLifetime);
        if (!registered) {
            closeDiscardingQueue(ByeReason::HostFull, "the pane has no room for another effect reader");
            return refuseBinding();
        }
        attachment.base->hasEffectReader = true;
        attachment.base->effectsAcknowledged = through;
        attachment.base->effectsSent = through;
        attachment.isEffectBaselineOwed = through != 0;
    } else if (!effects.resumeReader(attachmentIdentity(), m_clientInstance, attachment.base->effectsAcknowledged))
        return refuseBinding();
    attachment.base->isHeld = true;
    if (m_record)
        m_table->noteHoldingChanged();
    attachment.effectsQueued = attachment.base->effectsAcknowledged;
    attachment.history.wants = HistoryWants(m_policy.limits.maxPendingRanges);
    // A pane starts Dormant until the client says it shows it; an agent's
    // starts RecordsOnly (Docs/sync.md §6.8).
    attachment.view = m_clientKind == ClientKind::Agent ? ViewState::RecordsOnly : ViewState::Dormant;
    attachment.dormantChanges = source.changes();
    attachment.reportedDiscardSerial = source.inputLedger().discardSerial();
    // The lowest handle the client has acknowledged the unbinding of, so the
    // handles a connection uses stay as few as the panes it has bound at once.
    const std::uint64_t acknowledged = m_pacer.acknowledgedStreamBytes();
    const auto freed = std::ranges::find_if(m_panes, [&](const PaneAttachment& slot) {
        return !slot.source && slot.unbindOffset && *slot.unbindOffset <= acknowledged;
    });
    attachment.handle = freed != m_panes.end() ? freed->handle : static_cast<std::uint32_t>(m_panes.size() + 1);
    std::vector<std::uint8_t> body;
    encode(body, PaneBind { .handle = attachment.handle, .paneID = source.paneID() });
    enqueue(FrameKind::PaneBind, 0, body);
    noteViewport(attachment);
    if (freed != m_panes.end()) {
        *freed = std::move(attachment);
        return freed->handle;
    }
    m_panes.push_back(std::move(attachment));
    return m_panes.back().handle;
}

GeometryStanding HostConnection::geometryStandingOfClient() const
{
    return geometryStanding(standingKind(m_clientKind, m_identity.verifiedKind), mayDrive());
}

void HostConnection::noteViewport(PaneAttachment& attachment)
{
    attachment.source->geometry().noteWindowViewport(attachmentIdentity(), geometryStandingOfClient(), { .rows = m_facts.viewportRows, .columns = m_facts.viewportColumns }, m_factsAt);
}

void HostConnection::unbindPane(std::uint32_t handle, std::uint8_t reason)
{
    // The owner letting a pane go ends any wait to bind it again, whether it
    // is still unbound for its size or was bound again under this handle.
    if (reason != std::to_underlying(PaneUnbindReason::CheckpointTooLarge))
        std::erase_if(m_oversizedPanes, [&](const OversizedPane& oversized) { return oversized.handle == handle; });
    PaneAttachment* attachment = pane(handle);
    if (!attachment)
        return;
    for (auto& request : m_clipboardReads) {
        if (request && request->handle == handle)
            request.reset();
    }
    (void)m_scheduler.withdraw(FrameKind::NativeDragCommand, handle);
    std::erase_if(m_nativeDragEvents, [&](const auto& event) { return event.handle == handle; });
    std::erase_if(m_pendingRichText, [&](const auto& request) { return request.handle == handle; });
    std::erase_if(m_richTextReplies, [&](const auto& reply) { return reply.handle == handle; });
    // The attachment gives the pane up: the clients that show it are told.
    if (m_record && m_table && attachment->base) {
        attachment->base->isHeld = false;
        m_table->noteHoldingChanged();
    }
    // The client forgets the pane's transfer when it reads the PaneUnbind,
    // which the interactive lane sends ahead of any chunk still queued; a
    // chunk after it would name a transfer the client no longer knows, and
    // the client ends the connection over it. A run's frames are dropped for
    // the same reason, and the client drops what of it it holds.
    abortCheckpoint(*attachment);
    abortImages(*attachment);
    // The client fences the pane's history transfers when it reads the
    // PaneUnbind; what of them is still queued would only be discarded.
    for (const std::uint32_t transfer : historyTransferIDs(attachment->history)) {
        if (abortTransfer(transfer) == TransferAbort::Aborted && m_state == ConnectionState::Attached) {
            std::vector<std::uint8_t> abortBody;
            encode(abortBody, BulkAbort { .transfer = transfer });
            enqueue(FrameKind::BulkAbort, 0, abortBody);
            ++m_counters.bulkAborts;
        }
    }
    attachment->source->noteDriverFocus(attachmentIdentity(), Ubiquitty::Modes::FocusState::Unfocused, standingKind(m_clientKind, m_identity.verifiedKind));
    (void)attachment->source->effects().noteState(attachmentIdentity(), AttachmentState::Detached, 0);
    attachment->base->hasEffectReader = false;
    (void)m_scheduler.withdraw(FrameKind::Effect, handle);
    std::array<std::uint32_t, VT::sampleSlotCount> sampleTransfers {};
    for (std::size_t index = 0; index < attachment->samples.size(); ++index)
        sampleTransfers[index] = attachment->samples[index].transfer;
    const auto clipboardTransfer = attachment->clipboard ? attachment->clipboard->transfer : 0;
    PaneAttachment unbound;
    unbound.handle = handle;
    unbound.ownedBase = std::make_unique<PaneBaselines>();
    unbound.base = unbound.ownedBase.get();
    *attachment = std::move(unbound);
    if (m_state != ConnectionState::Attached)
        return;
    std::vector<std::uint8_t> body;
    encode(body, PaneUnbind { .handle = handle, .reason = reason });
    enqueue(FrameKind::PaneUnbind, 0, body);
    // Definitive unbind retires unconsumed sounds before an announced sample
    // is aborted. An abort on a still-bound sample is a delivery failure.
    for (const auto transfer : sampleTransfers) {
        if (transfer)
            abortHistoryTransfer(transfer);
    }
    if (clipboardTransfer)
        abortHistoryTransfer(clipboardTransfer);
    m_queuedUnbinds.push_back(handle);
}

void HostConnection::noteStreamFramePopped(const std::vector<std::uint8_t>& output, std::size_t before)
{
    if (standingKind(m_clientKind, m_identity.verifiedKind) == ClientKind::Agent
        && output.size() > before && (output[before] == std::to_underlying(FrameKind::Update) || output[before] == std::to_underlying(FrameKind::CheckpointHead))) {
        EnvelopeError error = EnvelopeError::None;
        const auto frame = decodeDatagram(std::span(output).subspan(before), error);
        if (PaneAttachment* attachment = frame ? pane(frame->channel) : nullptr)
            attachment->hasDisclosedScreen = true;
    }
    m_streamBytesPopped += output.size() - before;
    if (output.size() > before && output[before] != std::to_underlying(FrameKind::Probe))
        m_pacer.noteAckOwedThroughSent();
    if (output.size() > before && output[before] == std::to_underlying(FrameKind::Effect)) {
        EnvelopeError error = EnvelopeError::None;
        const auto frame = decodeDatagram(std::span(output).subspan(before), error);
        PaneAttachment* attachment = frame ? pane(frame->channel) : nullptr;
        const auto packet = attachment ? decodeEffectPacket(attachment->source->paneID(), frame->body) : std::expected<EffectPacket, EffectCodecError>(std::unexpected(EffectCodecError::Malformed));
        if (attachment && packet && !packet->remainingRecords && !attachment->clipboard) {
            if (attachment->source->effects().noteSent(attachmentIdentity(), packet->throughSequence))
                attachment->base->effectsSent = packet->throughSequence;
        }
    }
    if (output.size() > before && output[before] == std::to_underlying(FrameKind::BulkChunk)) {
        EnvelopeError error = EnvelopeError::None;
        const auto frame = decodeDatagram(std::span(output).subspan(before), error);
        const auto chunk = frame ? decodeBulkChunk(frame->body) : std::nullopt;
        if (chunk && chunk->isLast) {
            for (auto& attachment : m_panes) {
                if (!attachment.clipboard || attachment.clipboard->transfer != chunk->transfer)
                    continue;
                const auto through = attachment.clipboard->throughSequence;
                if (attachment.source->effects().noteSent(attachmentIdentity(), through))
                    attachment.base->effectsSent = through;
                attachment.clipboard.reset();
                break;
            }
        }
    }
    // Interactive frames leave in the order they were queued, so the oldest
    // PaneUnbind queued is the one that left.
    if (output.size() > before && output[before] == std::to_underlying(FrameKind::PaneUnbind) && !m_queuedUnbinds.empty()) {
        const std::uint32_t handle = m_queuedUnbinds.front();
        m_queuedUnbinds.pop_front();
        if (isUnbound(handle))
            m_panes[handle - 1].unbindOffset = m_streamBytesPopped;
    }
}

bool HostConnection::isCheckpointWithheld(const PaneAttachment& attachment)
{
    return attachment.needsCheckpoint && attachment.source && attachment.withheldGeneration && *attachment.withheldGeneration == attachment.source->generation();
}

bool HostConnection::isOwed(const PaneAttachment& attachment) const
{
    // Only a Live pane is sent its screen; a Dormant one owes at most a
    // PaneDirty, which isDirtyNoticeOwed says.
    if (attachment.view != ViewState::Live || isCheckpointWithheld(attachment))
        return false;
    return attachment.needsCheckpoint || attachment.isLossPending || attachment.source->generation() > attachment.base->sentGeneration || isEchoContextOwed(attachment);
}

bool HostConnection::isDirtyNoticeOwed(const PaneAttachment& attachment)
{
    return attachment.source && attachment.view == ViewState::Dormant && !attachment.hasSentDirty
        && attachment.source->changes() > attachment.dormantChanges;
}

bool HostConnection::isPaletteLookOwed(const PaneAttachment& attachment)
{
    // Until the client has said how it holds the pane, a `View{Live}` may be
    // on its way, and the block would be bytes for nothing.
    return attachment.source && attachment.view == ViewState::Dormant && attachment.isViewStated
        && attachment.palette.examinedChanges != attachment.source->changes();
}

HostConnection::PaneService HostConnection::servicePalette(PaneAttachment& attachment, std::uint64_t now)
{
    if (!isPaletteLookOwed(attachment))
        return {};
    PaletteProgress& palette = attachment.palette;
    const Ubiquitty::Terminal& terminal = attachment.source->terminal();
    if (palette.examinedChanges && palette.examinedEpoch == terminal.paletteEpoch) {
        palette.examinedChanges = attachment.source->changes();
        return {};
    }
    std::vector<std::uint8_t> block;
    encodePaletteState(terminal, block);
    const std::uint64_t hash = blockHash(block);
    constexpr std::size_t paletteIndex = std::to_underlying(StateBlock::Palette);
    PaneBaselines& base = *attachment.base;
    if (!palette.examinedChanges) {
        // The span's first look. A pane never sent a frame shows the client's
        // own colours until it is shown or its palette moves. Otherwise the
        // client holds the block its updates carried, unless what it was sent
        // and what it acknowledged disagree on it: a block in flight, or one
        // in an update withdrawn as the pane was hidden, is sent again.
        const std::uint64_t sent = base.sentBaseline.blockHashes[paletteIndex];
        if (base.sentGeneration == 0 && !base.acknowledgedGeneration)
            palette.heldHash = hash;
        else if (base.acknowledgedGeneration && base.acknowledgedBaseline.blockHashes[paletteIndex] == sent)
            palette.heldHash = sent;
    }
    PaneService served;
    // A block is a live palette, its baseline and a bounded stack of saved
    // ones, which a frame always holds; the check keeps a grammar that grew
    // past one from being cut, and the reveal carries what it could not.
    if (palette.heldHash != hash && block.size() <= maxFrameBodyBytes) {
        const std::size_t bytes = frameBytes(attachment.handle, block.size());
        if (!m_pacer.admits(bytes, now, queuedInteractiveBytes())) {
            ++m_counters.windowRefusals;
            m_isWindowRefused = true;
            return { .bytes = 0, .isRefused = true };
        }
        enqueue(FrameKind::PanePalette, attachment.handle, block);
        // The client's replica takes the block's hash as it applies it; every
        // baseline a resume or a reset update reads moves with it.
        base.sentBaseline.blockHashes[paletteIndex] = hash;
        base.acknowledgedBaseline.blockHashes[paletteIndex] = hash;
        for (SentGeneration& sent : base.inFlight)
            sent.baseline.blockHashes[paletteIndex] = hash;
        ++m_counters.palettesSent;
        served.bytes = bytes;
    }
    palette.heldHash = hash;
    palette.examinedChanges = attachment.source->changes();
    palette.examinedEpoch = terminal.paletteEpoch;
    return served;
}

void HostConnection::setPaneView(PaneAttachment& attachment, ViewState state)
{
    if (attachment.view == state)
        return;
    const std::size_t queuedBefore = queuedInteractiveBytes();
    const bool wasLive = attachment.view == ViewState::Live;
    attachment.view = state;
    // Each Dormant span looks at the palette afresh against what the client
    // was sent by then.
    attachment.palette = {};
    if (state == ViewState::Live) {
        // The screen carries the pane's status from here on: a set still
        // leaving is abandoned, so it holds no screen behind it.
        attachment.records.statusRevisionInTransit = 0;
        attachment.records.statusCursor = 0;
        return;
    }
    // The screen stops carrying the pane's status, so the set is owed whole
    // whatever revision the client was last sent.
    if (wasLive)
        attachment.records.sentStatusRevision = 0;
    // A pane the client stops showing is sent no screen, so the rest of a
    // body in transfer is bytes for nothing.
    abortCheckpoint(attachment);
    abortImages(attachment);
    bool didWithdrawScreen = m_scheduler.withdraw(FrameKind::Update, attachment.handle) != 0;
    std::erase_if(m_datagrams, [&](const std::vector<std::uint8_t>& datagram) {
        EnvelopeError error = EnvelopeError::None;
        const std::optional<Frame> frame = decodeDatagram(datagram, error);
        if (!frame || frame->kind != FrameKind::Update || frame->channel != attachment.handle)
            return false;
        const std::optional<DecodedUpdateHeader> update = decodeUpdateHeader(frame->body);
        if (update)
            m_pacer.cancelDatagram(attachment.handle, update->header.generation);
        didWithdrawScreen = true;
        return true;
    });
    if (didWithdrawScreen) {
        // Sent baselines include queued updates. A later Live view cannot
        // build on one the transport never took.
        attachment.needsCheckpoint = true;
        attachment.sentEchoContext.reset();
        attachment.base->inFlight.clear();
    }
    if (state == ViewState::RecordsOnly) {
        HistoryState& history = attachment.history;
        bool didWithdrawHistory = m_scheduler.withdraw(FrameKind::HistoryInline, attachment.handle) != 0;
        for (const std::uint32_t transfer : historyTransferIDs(history)) {
            if (m_scheduler.isTransferQueued(transfer)) {
                abortHistoryTransfer(transfer);
                didWithdrawHistory = true;
            }
        }
        if (didWithdrawHistory) {
            // Appends advanced their sent head when queued. Resume from
            // confirmed history, so withdrawn inline lines go again too.
            history.sentHead = history.ackedHead;
            history.sentOpenLine.reset();
            attachment.needsCheckpoint = true;
        }
        history.appendTransfers.clear();
        history.openLineTransfer.reset();
        history.wantTransfers = {};
        history.wants.clear();
    }
    if (didWithdrawScreen || queuedInteractiveBytes() < queuedBefore)
        m_isWindowRefused = false;
    attachment.dormantChanges = attachment.source->changes();
    attachment.hasSentDirty = false;
}

std::optional<ViewState> HostConnection::paneView(std::uint32_t handle) const
{
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return std::nullopt;
    return m_panes[handle - 1].view;
}

bool HostConnection::isProbing() const
{
    return m_facts.isFocused && hasLivePane();
}

std::uint64_t HostConnection::rateIntervalMicroseconds() const
{
    const std::uint64_t interval = std::uint64_t { m_facts.refreshIntervalMicroseconds } * (m_facts.power == PowerState::Saver ? 2 : 1);
    return m_facts.link == LinkCost::Constrained ? std::max(interval, m_policy.constrainedIntervalMicroseconds) : interval;
}

bool HostConnection::isPasteReceiptOwed(const PaneAttachment& attachment) const
{
    if (!attachment.source)
        return false;
    const std::optional<PaneInputQueue::PasteWritten> written = attachment.source->pasteWritten(m_clientInstance);
    return written && written->bytesWritten != 0 && attachment.sentPasteWritten != written;
}

bool HostConnection::isInputDiscardOwed(const PaneAttachment& attachment) const
{
    if (!attachment.source)
        return false;
    const std::span<const InputDiscardRecord> runs = attachment.source->inputLedger().discards(m_clientInstance);
    return !runs.empty() && runs.back().serial > attachment.reportedDiscardSerial;
}

bool HostConnection::isEchoContextOwed(const PaneAttachment& attachment) const
{
    const PaneSource& source = *attachment.source;
    return m_capabilities.contains(ClientCapability::EchoContext) && source.isFrameHeld() && source.isEncodable() && attachment.sentEchoContext != source.echoContextFor(m_clientInstance);
}

ChainOps HostConnection::chainOps() const
{
    ChainOps ops;
    ops.set(ChainOp::Spans, m_capabilities.contains(ClientCapability::SpanOps));
    ops.set(ChainOp::ScrollRect, m_capabilities.contains(ClientCapability::ScrollRectOps));
    return ops;
}

bool HostConnection::isAwaitingClient() const
{
    // Bytes that carried only probes are answered by the Echo, not an Ack;
    // counting them would hold an idle client to an Ack it never owes.
    return m_pacer.isAckOwed() || m_pacer.isProbeOutstanding();
}

std::optional<std::uint64_t> HostConnection::silenceDeadlineMicroseconds() const
{
    if (!m_unheardSince || !isAwaitingClient())
        return std::nullopt;
    return *m_unheardSince + m_policy.pacing.lostSilenceMicroseconds;
}

bool HostConnection::hasLivePane() const
{
    return std::ranges::any_of(m_panes, [](const PaneAttachment& attachment) { return attachment.source && attachment.view == ViewState::Live; });
}

std::uint64_t HostConnection::receiptSettled(const PaneAttachment& attachment) const
{
    const PaneSource& source = *attachment.source;
    if (source.isFrameHeld() && attachment.base->sentGeneration == source.generation() && !attachment.needsCheckpoint)
        return std::max(attachment.reportedInputSettled, source.inputLedger().settledThrough(m_clientInstance));
    return attachment.reportedInputSettled;
}

std::optional<std::uint64_t> HostConnection::receiptDeadlineMicroseconds(const PaneAttachment& attachment, std::uint64_t now) const
{
    if (!attachment.source)
        return std::nullopt;
    const InputLedger& ledger = attachment.source->inputLedger();
    std::optional<std::uint64_t> deadline;
    const std::optional<std::uint64_t> written = ledger.lastWritten(m_clientInstance);
    if (written && *written > attachment.reportedInputWritten)
        deadline = ledger.lastWrittenAt(m_clientInstance).value_or(0) + m_policy.input.receiptDelayMicroseconds;
    if (receiptSettled(attachment) > attachment.reportedInputSettled)
        deadline = std::min(deadline.value_or(now), now);
    return deadline;
}

namespace {

/// A numbered input frame's number and event.
struct DecodedInput {
    std::uint64_t inputSeq = 0;
    InputIntent intent;
};

/// Whether bytes are whole UTF-8 scalars: no piece of a paste starts or ends
/// inside one, so the host never writes half a character.
bool isWholeScalars(std::span<const std::uint8_t> bytes)
{
    for (std::size_t index = 0; index < bytes.size();) {
        const std::uint8_t lead = bytes[index];
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
            : (lead & 0xF0) == 0xE0                                        ? 3
            : (lead & 0xF8) == 0xF0                                        ? 4
                                                                           : 0;
        if (length == 0 || index + length > bytes.size())
            return false;
        for (std::size_t next = index + 1; next < index + length; ++next) {
            if ((bytes[next] & 0xC0) != 0x80)
                return false;
        }
        index += length;
    }
    return true;
}

/// Whether a key's legacy byte is one of the characters.
bool isInterruptKey(const KeyEvent& event, const InterruptCharacters& characters)
{
    const std::optional<std::uint8_t> byte = legacyControlCharacterOf(event);
    return byte && characters.contains(*byte);
}

std::optional<DecodedInput> decodeInput(FrameKind kind, std::span<const std::uint8_t> body)
{
    switch (kind) {
    case FrameKind::Raw:
        if (const std::optional<Raw> raw = decodeRaw(body))
            return DecodedInput { .inputSeq = raw->inputSeq, .intent = RawInput { .bytes = { raw->bytes.begin(), raw->bytes.end() } } };
        return std::nullopt;
    case FrameKind::Keys:
        if (std::optional<Keys> keys = decodeKeys(body))
            return DecodedInput { .inputSeq = keys->inputSeq, .intent = KeyInput { .events = std::move(keys->events) } };
        return std::nullopt;
    case FrameKind::Pointer:
        if (std::optional<Pointer> pointer = decodePointer(body))
            return DecodedInput { .inputSeq = pointer->inputSeq, .intent = PointerInput { .events = std::move(pointer->events) } };
        return std::nullopt;
    case FrameKind::Paste:
        if (const std::optional<Paste> paste = decodePaste(body); paste && isWholeScalars(paste->text))
            return DecodedInput { .inputSeq = paste->inputSeq, .intent = PasteInput { .text = { paste->text.begin(), paste->text.end() }, .offset = paste->offset, .end = paste->state == PasteState::Final ? PasteEnd::Final : PasteEnd::More } };
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace

bool isInterrupt(const InputIntent& intent, const InterruptCharacters& characters)
{
    if (const auto* raw = std::get_if<RawInput>(&intent))
        return std::ranges::any_of(raw->bytes, [&](std::uint8_t byte) { return characters.contains(byte); });
    if (const auto* keys = std::get_if<KeyInput>(&intent))
        return std::ranges::any_of(keys->events, [&](const KeyEvent& event) { return isInterruptKey(event, characters); });
    return false;
}

ID128 HostConnection::attachmentIdentity() const
{
    return m_record ? m_record->attachmentID : m_identity.attachmentID;
}

bool HostConnection::mayDrive() const
{
    return m_grantedRole != Role::Viewer && m_clientKind != ClientKind::Viewer && m_identity.mayDrive;
}

void HostConnection::handleInput(const Frame& frame, Carrier carrier, std::uint64_t now)
{
    if (frame.kind == FrameKind::PasteAbort) {
        handlePasteAbort(frame, carrier, now);
        return;
    }
    std::optional<DecodedInput> input = decodeInput(frame.kind, frame.body);
    if (!input || carrier == Carrier::Datagram) {
        close(ByeReason::Malformed, "an unreadable input frame, or one in a datagram");
        return;
    }
    if (!mayDrive()) {
        close(ByeReason::Malformed, "input from a client that may not drive");
        return;
    }
    if (isUnbound(frame.channel))
        return;
    PaneAttachment* attachment = pane(frame.channel);
    if (!attachment) {
        close(ByeReason::UnboundHandle, "input for an unbound pane");
        return;
    }
    // A connection holds no more than its bound across all of its panes, so
    // the host's input is bounded by its connections, not by its connections
    // times its panes.
    std::size_t held = 0;
    for (const PaneAttachment& bound : m_panes) {
        if (bound.source)
            held += bound.source->heldInputBytes(m_clientInstance);
    }
    const std::size_t room = std::min(held < maxConnectionHeldInputBytes ? maxConnectionHeldInputBytes - held : 0, m_heldInputRoom ? m_heldInputRoom() : SIZE_MAX);
    const EnqueueOutcome outcome = attachment->source->queueInput(m_clientInstance, input->inputSeq, std::move(input->intent), now, attachmentIdentity(), room);
    if (outcome == EnqueueOutcome::Refused) {
        close(ByeReason::Oversize, "input past its sender's share of the pane or of its connection, or for a program that is gone");
        return;
    }
    if (outcome == EnqueueOutcome::Malformed) {
        close(ByeReason::Malformed, "a paste piece past what was taken of the paste");
        return;
    }
    if (outcome == EnqueueOutcome::Duplicate)
        ++m_counters.inputDuplicates;
    // Typing takes the size; pointer motion never does (Docs/sync.md §6.10).
    if (frame.kind == FrameKind::Keys || frame.kind == FrameKind::Raw || frame.kind == FrameKind::Paste)
        attachment->source->geometry().noteTyping(attachmentIdentity(), geometryStandingOfClient(), now);
    noteClientInput(now);
}

std::string_view HostConnection::viewportAuthorityRefusal() const
{
    if (standingKind(m_clientKind, m_identity.verifiedKind) == ClientKind::Agent)
        return "a Viewport from an agent, which has no viewport of a pane; it sets a size by the call pane.overrideGeometry";
    if (!admitsSender(FrameKind::Viewport, m_grantedRole) || geometryStandingOfClient() != GeometryStanding::Driver)
        return "a Viewport from a client that may not drive";
    return {};
}

void HostConnection::handleViewport(const Frame& frame, Carrier carrier, std::uint64_t now)
{
    const std::optional<Viewport> viewport = decodeViewport(frame.body);
    if (!viewport || carrier == Carrier::Datagram) {
        close(ByeReason::Malformed, "an unreadable Viewport, or one in a datagram");
        return;
    }
    // One meaning per frame: a viewport is a person's window on a pane.
    if (const std::string_view refusal = viewportAuthorityRefusal(); !refusal.empty()) {
        close(ByeReason::Malformed, refusal);
        return;
    }
    if (isUnbound(frame.channel))
        return;
    PaneAttachment* attachment = pane(frame.channel);
    if (!attachment) {
        close(ByeReason::UnboundHandle, "a Viewport for an unbound pane");
        return;
    }
    if (viewport->rows == 0 || viewport->columns == 0)
        return;
    // Ceilinged once, here, so the arbiter, the program and Core are given
    // the same size. No floor: a program on a one-row window is told one row.
    Viewport clamped = *viewport;
    clamped.rows = static_cast<std::uint16_t>(std::min<int>(viewport->rows, Ubiquitty::maxRows));
    clamped.columns = static_cast<std::uint16_t>(std::min<int>(viewport->columns, Ubiquitty::maxColumns));
    attachment->source->geometry().noteViewport(attachmentIdentity(), GeometryStanding::Driver, { .rows = clamped.rows, .columns = clamped.columns }, now);
    const auto pending = std::ranges::find(m_viewportReports, frame.channel, &ViewportReport::handle);
    if (pending != m_viewportReports.end())
        pending->viewport = clamped;
    else
        m_viewportReports.push_back({ .handle = frame.channel, .viewport = clamped });
}

void HostConnection::handlePasteAbort(const Frame& frame, Carrier carrier, std::uint64_t now)
{
    const std::optional<PasteAbort> abort = decodePasteAbort(frame.body);
    if (!abort || carrier == Carrier::Datagram) {
        close(ByeReason::Malformed, "an unreadable PasteAbort, or one in a datagram");
        return;
    }
    if (!mayDrive()) {
        close(ByeReason::Malformed, "input from a client that may not drive");
        return;
    }
    if (isUnbound(frame.channel))
        return;
    PaneAttachment* attachment = pane(frame.channel);
    if (!attachment) {
        close(ByeReason::UnboundHandle, "a PasteAbort for an unbound pane");
        return;
    }
    attachment->source->abortPaste(m_clientInstance, abort->inputSeq, now, attachmentIdentity());
    noteClientInput(now);
}

void HostConnection::noteDriverFocus(PaneAttachment& attachment)
{
    const bool isFocused = mayDrive() && m_facts.isFocused && attachment.clientFocus == Ubiquitty::Modes::FocusState::Focused;
    attachment.source->noteDriverFocus(attachmentIdentity(), isFocused ? Ubiquitty::Modes::FocusState::Focused : Ubiquitty::Modes::FocusState::Unfocused, standingKind(m_clientKind, m_identity.verifiedKind));
}

void HostConnection::releaseDriverFocus()
{
    for (PaneAttachment& attachment : m_panes) {
        if (attachment.source)
            attachment.source->noteDriverFocus(attachmentIdentity(), Ubiquitty::Modes::FocusState::Unfocused, standingKind(m_clientKind, m_identity.verifiedKind));
    }
}

std::optional<std::pair<std::uint64_t, Digest128>> HostConnection::sentDigest(std::uint32_t handle) const
{
    const auto found = std::ranges::find(m_panes, handle, &PaneAttachment::handle);
    if (found == m_panes.end() || !found->source || found->base->sentGeneration == 0)
        return std::nullopt;
    return std::pair { found->base->sentGeneration, baselineDigest(found->base->sentBaseline) };
}

bool HostConnection::hasDisclosedScreen(std::uint32_t handle) const
{
    const auto found = std::ranges::find(m_panes, handle, &PaneAttachment::handle);
    return found != m_panes.end() && found->source && found->hasDisclosedScreen;
}

void HostConnection::serveFromTable(AttachmentTable& table, ConnectionAuthority authority, std::uint64_t connection, std::function<PaneSource*(ID128)> resolvePane)
{
    m_table = &table;
    m_authority = std::move(authority);
    m_connection = connection;
    m_resolvePane = std::move(resolvePane);
    m_identity.mayDrive = m_authority.mayDrive;
    m_identity.verifiedKind = m_authority.verifiedKind;
    m_identity.principal = m_authority.principal;
    m_identity.hostInstanceID = table.hostInstanceID();
}

void HostConnection::releaseRecord()
{
    if (!m_record)
        return;
    // The panes keep copies of what they borrowed, so nothing here reads a
    // record another connection now writes or the table frees.
    for (PaneAttachment& attachment : m_panes) {
        if (attachment.base && !attachment.ownedBase) {
            attachment.ownedBase = std::make_unique<PaneBaselines>(*attachment.base);
            attachment.base = attachment.ownedBase.get();
        }
    }
    m_record = nullptr;
}

std::vector<std::pair<std::uint32_t, ID128>> HostConnection::boundPanes() const
{
    std::vector<std::pair<std::uint32_t, ID128>> bound;
    for (const PaneAttachment& attachment : m_panes) {
        if (attachment.source)
            bound.emplace_back(attachment.handle, attachment.source->paneID());
    }
    return bound;
}

void HostConnection::supersede()
{
    if (m_state == ConnectionState::Closed)
        return;
    // The attachment's baselines are the new connection's now; this one
    // touches them no more.
    m_wasSuperseded = true;
    releaseRecord();
    close(ByeReason::Superseded, {});
}

void HostConnection::withdrawAuthority()
{
    // The carrier owns the refusal. Stop decoding before releasing the record;
    // no later frame may borrow a baseline from a retired attachment.
    m_state = ConnectionState::Closed;
    releaseDriverFocus();
    m_scheduler.abandonTransfers();
    discardInboundClipboard();
    m_panes.clear();
    m_record = nullptr;
}

void HostConnection::refuse(ByeReason reason)
{
    if (m_state != ConnectionState::Closed)
        close(isRefusal(reason) ? reason : ByeReason::Malformed, {});
}

void HostConnection::finish()
{
    if (m_state != ConnectionState::Closed)
        close(ByeReason::Normal, {});
}

std::optional<ByeReason> HostConnection::admitAttachment(const Greet& greet, Welcome& welcome, std::vector<PaneResumeBinding>& bindings, std::uint64_t now)
{
    if (isUnboundTarget(greet.request.target)) {
        if (m_table->provenAttachment(m_connection)) {
            m_table->abandonProof(m_connection);
            return ByeReason::Malformed;
        }
        if (greet.resume || m_identity.pendingConsent)
            return ByeReason::Malformed;
        m_record = &m_table->createUnbound(greet.clientInstance, m_clientKind, m_authority, m_connection, now);
        welcome.attachmentID = m_record->attachmentID;
        welcome.resumeSecret = m_record->resumeSecret;
        return std::nullopt;
    }
    if (m_identity.pendingConsent) {
        const auto proven = m_table->provenAttachment(m_connection);
        if (proven)
            m_table->abandonProof(m_connection);
        if (greet.resume || proven || !m_identity.pendingConsent->requestID || !m_identity.pendingConsent->sessionID)
            return ByeReason::NoConsent;
        m_record = &m_table->createPending(greet.clientInstance,
            m_identity.verifiedKind == ClientKind::Agent ? ClientKind::Agent : greet.clientKind, m_authority, m_connection, now);
        welcome.attachmentID = m_record->attachmentID;
        welcome.resumeSecret = m_record->resumeSecret;
        welcome.admission = AttachmentAdmission::AwaitingConsent;
        welcome.consent = *m_identity.pendingConsent;
        return std::nullopt;
    }
    // A connection a resume proof admitted continues that attachment and
    // does nothing else: it attaches nothing fresh and resumes no other
    // (Docs/sync.md §6.9). A Greet that carries the secret a
    // proof exists not to send is refused by the table, below.
    const std::optional<ID128> proven = m_table->provenAttachment(m_connection);
    if (proven && (!greet.resume || greet.resume->attachmentID != *proven)) {
        m_table->abandonProof(m_connection);
        return ByeReason::Malformed;
    }
    if (greet.resume) {
        const auto* previous = m_table->find(greet.resume->attachmentID);
        const std::optional<ID128> issuer = previous ? std::optional { previous->issuer } : std::nullopt;
        const std::expected<AttachmentRecord*, ResumeRefusal> resumed = m_table->resume(*greet.resume, m_authority, m_connection, now);
        if (resumed) {
            m_record = *resumed;
            m_resumeRefusal = std::nullopt;
            welcome.predecessorHostInstanceID = issuer && *issuer != m_table->hostInstanceID() ? issuer : std::nullopt;
        } else {
            m_resumeRefusal = resumed.error();
            // A resume racing one still in its handshake is not attached
            // fresh: the client that holds the attachment is on the other
            // connection.
            if (*m_resumeRefusal == ResumeRefusal::InProgress)
                return ByeReason::ResumeInProgress;
            // A refused resume attaches fresh, which a proof's connection
            // may not: the proof authorised one attachment, not a new one.
            if (proven) {
                m_table->abandonProof(m_connection);
                return ByeReason::Malformed;
            }
        }
    }
    // A refused resume attaches fresh (Docs/sync.md §6.9).
    if (!m_record)
        m_record = &m_table->create(greet.clientInstance,
            m_identity.verifiedKind == ClientKind::Agent ? ClientKind::Agent : greet.clientKind, m_authority, m_connection, now);
    if (m_proportionalFaces || !m_capabilities.contains(ClientCapability::PublishesFaces))
        m_record->inheritedProportionalFaces.reset();
    welcome.attachmentID = m_record->attachmentID;
    welcome.resumeSecret = m_record->resumeSecret;
    if (!greet.resume || m_resumeRefusal)
        return std::nullopt;
    for (const PaneResume& pane : greet.resume->panes) {
        // Only a pane the attachment held is continued: a resume names no
        // pane its attachment was never bound to.
        PaneBaselines* held = m_record->findPane(pane.paneID);
        PaneSource* source = held && m_resolvePane ? m_resolvePane(pane.paneID) : nullptr;
        PaneResumed outcome { .paneID = pane.paneID, .outcome = ResumeOutcome::Gone, .inputWritten = std::nullopt };
        if (source) {
            outcome.inputWritten = source->inputLedger().lastWritten(m_clientInstance);
            // A cursor behind the effects this attachment acknowledged comes
            // from a record written before them, as a client restarted from
            // it after a crash keeps: its picture is as old, so it is drawn
            // again rather than continued.
            const bool hasLostEffectReceipts = held->hasEffectReader && pane.effectsAcknowledged < held->effectsAcknowledged;
            const std::optional<SentGeneration> matched = hasLostEffectReceipts ? std::nullopt : matchingBaseline(*held, pane.ackedRowHashesDigest, pane.ackedGeneration);
            outcome.outcome = matched ? ResumeOutcome::Diffed : ResumeOutcome::Checkpointed;
            bindings.push_back({ .source = source, .resume = pane, .baseline = matched });
        }
        welcome.resumed.push_back(outcome);
    }
    return std::nullopt;
}

std::optional<SentGeneration> HostConnection::matchingBaseline(const PaneBaselines& baselines, const Digest128& digest, std::uint64_t generation)
{
    // The client holds what it acknowledged, or a generation sent after it
    // whose acknowledgement the drop lost. The newest generation sent is one
    // of the two, so it needs no match of its own.
    // Prefer the exact publication if the same picture appeared more than
    // once: its history revision can differ even when its screen does not.
    for (const SentGeneration& sent : baselines.inFlight) {
        if (sent.generation == generation && baselineDigest(sent.baseline) == digest)
            return sent;
    }
    if (baselines.acknowledgedGeneration && baselineDigest(baselines.acknowledgedBaseline) == digest)
        return SentGeneration { .generation = *baselines.acknowledgedGeneration, .baseline = baselines.acknowledgedBaseline, .history = baselines.acknowledgedHistory };
    for (const SentGeneration& sent : baselines.inFlight) {
        if (baselineDigest(sent.baseline) == digest)
            return sent;
    }
    return std::nullopt;
}

void HostConnection::continueResumedPane(const PaneResumeBinding& binding)
{
    const std::uint32_t handle = bindPane(*binding.source);
    PaneAttachment* attachment = pane(handle);
    if (!attachment)
        return;
    // Binding resumed the reader at what the attachment acknowledged. A
    // cursor behind it lost its receipts: the effects it acknowledged were
    // presented and are not sent again, and a completion through them tells
    // the client which it will not receive. A cursor beyond what the
    // attachment was sent acknowledges effects no client was given.
    const std::uint64_t named = binding.resume.effectsAcknowledged;
    if (named < attachment->base->effectsAcknowledged) {
        attachment->effectsQueued = attachment->base->effectsAcknowledged;
        attachment->isEffectBaselineOwed = true;
    } else {
        if (!attachment->source->effects().resumeReader(attachmentIdentity(), m_clientInstance, named)) {
            close(ByeReason::Malformed, "a resume effect cursor outside emitted batches");
            return;
        }
        attachment->base->effectsAcknowledged = named;
        attachment->effectsQueued = named;
    }
    attachment->view = binding.resume.view;
    // A diff can cross a history clear without needing a screen checkpoint.
    // Remember the client's committed epoch so service sends its reset before
    // any diff in the new epoch, even after the previous reset was refused.
    attachment->history.epoch = binding.resume.historyEpoch;
    // The resume said what the client shows; it states no view again.
    attachment->isViewStated = true;
    attachment->shouldOfferRetainedImages = true;
    if (!binding.baseline)
        return;
    // The client holds this picture at the generation it acknowledged: the
    // next update is a diff from it, and no checkpoint is owed.
    PaneBaselines& base = *attachment->base;
    base.sentGeneration = binding.resume.ackedGeneration;
    base.sentBaseline = binding.baseline->baseline;
    base.acknowledgedGeneration = binding.resume.ackedGeneration;
    base.acknowledgedBaseline = binding.baseline->baseline;
    // Equal screen digests can occur at different generations while an
    // offscreen history line changes. Only the exact named generation may
    // vouch for the client's history; a picture-only match remains useful
    // for screen diffs, but carries no history suppression across resumes.
    base.acknowledgedHistory = binding.baseline->generation == binding.resume.ackedGeneration ? binding.baseline->history : HistoryBaseline {};
    base.inFlight.clear();
    attachment->needsCheckpoint = false;
    attachment->hasSentUpdate = true;
    const HistoryBaseline& held = base.acknowledgedHistory;
    const Ubiquitty::Screen::HistoryAddress address = binding.source->historyAddress();
    // A client range is only a cache hint. The matched publication proves
    // its lines have not been rewritten; a changed or unknown revision
    // keeps the conservative resend path. Each exclusion can add one gap,
    // so admit the resulting backfill before splitting its range set.
    if (held.epoch != 0 && held.epoch == binding.resume.historyEpoch && held.epoch == address.epoch
        && held.revision == binding.source->history().revision() && !binding.resume.cachedRanges.empty()
        && binding.resume.cachedRanges.size() < m_policy.limits.maxPendingRanges) {
        HistoryState& history = attachment->history;
        history.sentHead = held.head;
        history.revision = held.revision;
        setImplicitBackfill(*attachment, BackfillCause::EpochServed);
        const std::uint64_t cachedThrough = std::min(held.head, address.head);
        for (const LineRange& range : binding.resume.cachedRanges)
            history.wants.exclude({ std::min(range.from, cachedThrough), std::min(range.to, cachedThrough) });
        // New appends precede the next screen update, independently of the
        // backfill gaps. Never let an unproven hint suppress those appends.
        history.wants.exclude({ cachedThrough, address.head });
    }
}

std::optional<std::uint64_t> HostConnection::nextServiceMicroseconds(std::uint64_t now) const
{
    if (m_state != ConnectionState::Attached)
        return std::nullopt;
    std::uint64_t deadline = isProbing() ? m_pacer.nextProbeMicroseconds() : std::numeric_limits<std::uint64_t>::max();
    for (const auto& request : m_pendingRichText)
        deadline = std::min(deadline, request.expiresAt);
    if (const std::optional<std::uint64_t> silence = silenceDeadlineMicroseconds())
        deadline = std::min(deadline, *silence);
    // Bytes that left between services start the silence clock at the next.
    if (m_owedEcho || (!m_unheardSince && isAwaitingClient()))
        deadline = std::min(deadline, now);
    // A queued topology leaves on the next pass that can take it. While the
    // interactive lane is past its bound nothing is due for it: the pass the
    // transport's draining asks for sends it.
    const bool isStructureOwed = std::ranges::any_of(m_structures, [&](const auto& entry) { return isStructureDue(entry.second); });
    if (isStructureOwed && wantsInput() && !m_isWindowRefused && m_pacer.isOpen(now, queuedInteractiveBytes()))
        deadline = std::min(deadline, now);
    // A listed pane's records are due at the listing's cadence, and the rest
    // of a status set begun as soon as the window has room.
    for (const ListedPane& listed : m_listed) {
        if (!isListedOwed(listed))
            continue;
        const std::uint64_t due = listed.progress.statusRevisionInTransit != 0 || !listed.servedAt ? now : std::max(now, *listed.servedAt + listedRecordsIntervalMicroseconds);
        if (due > now)
            deadline = std::min(deadline, due);
        else if (!m_isWindowRefused && m_pacer.isOpen(now, queuedInteractiveBytes()))
            deadline = std::min(deadline, now);
    }
    if (const std::optional<std::uint64_t> expiry = m_pacer.nextDatagramExpiryMicroseconds())
        deadline = std::min(deadline, *expiry);
    const std::uint64_t interval = rateIntervalMicroseconds();
    for (const PaneAttachment& attachment : m_panes) {
        if (!attachment.source)
            continue;
        if (const std::optional<std::uint64_t> receipt = receiptDeadlineMicroseconds(attachment, now))
            deadline = std::min(deadline, *receipt);
        if (isPasteReceiptOwed(attachment) || isInputDiscardOwed(attachment))
            deadline = std::min(deadline, now);
        const std::uint64_t effectGeneration = attachment.effectBatch ? attachment.effectGeneration : attachment.source->generation();
        const bool canSendEffects = isEffectOwed(attachment) && !m_scheduler.isQueued(FrameKind::Effect, attachment.handle)
            && attachment.sampleEffectsAcknowledged >= attachment.sampleAckAwaited
            && (attachment.view != ViewState::Live || attachment.base->acknowledgedGeneration.value_or(0) >= effectGeneration);
        if (isDirtyNoticeOwed(attachment) || isPaletteLookOwed(attachment) || isRecordOwed(attachment) || canSendEffects || isImageOwed(attachment)) {
            if (!m_isWindowRefused && m_pacer.isOpen(now, queuedInteractiveBytes()))
                deadline = std::min(deadline, now);
        }
        const bool isWantOwed = std::ranges::any_of(std::array { HistoryPriority::Viewport, HistoryPriority::Search, HistoryPriority::Backfill }, [&](HistoryPriority priority) { return this->isWantOwed(attachment, priority); });
        if (isWantOwed || isResetOwed(attachment) || isOpenLineOwed(attachment))
            deadline = std::min(deadline, now);
        if (!attachment.source->isFrameHeld() || !isOwed(attachment))
            continue;
        // An update of the pane waiting for the transport holds its next one
        // back; the transport taking it is what makes one due, not the clock.
        if (!attachment.needsCheckpoint && m_policy.datagramMaxBytes == 0 && m_scheduler.isQueued(FrameKind::Update, attachment.handle))
            continue;
        if (m_isWindowRefused && !attachment.needsCheckpoint)
            continue;
        // A checkpoint is due once the one in the lanes has left, which the
        // transport taking it makes so, not the clock.
        if (isCheckpointInLanes() && (attachment.needsCheckpoint || (attachment.checkpointTransfer && m_scheduler.isTransferQueued(*attachment.checkpointTransfer))))
            continue;
        if (!attachment.needsCheckpoint && attachment.hasSentUpdate && now - attachment.lastUpdateMicroseconds < interval)
            deadline = std::min(deadline, attachment.lastUpdateMicroseconds + interval);
        else if (m_pacer.isOpen(now, queuedInteractiveBytes()))
            deadline = std::min(deadline, now);
    }
    return deadline;
}

void HostConnection::service(std::uint64_t now)
{
    std::erase_if(m_pendingRichText, [now](const auto& request) { return request.expiresAt <= now; });
    if (isConsentPending(m_state)) {
        // Pending connections owe transport echoes, never pane work. One
        // newest echo is held, bounded even if the peer floods probes.
        if (m_owedEcho) {
            std::vector<std::uint8_t> body;
            encode(body, *m_owedEcho);
            const std::size_t queued = queuedInteractiveBytes();
            if (queued <= m_policy.maxQueuedInteractiveBytes && frameBytes(0, body.size()) <= m_policy.maxQueuedInteractiveBytes - queued) {
                enqueue(FrameKind::Echo, 0, body);
                m_owedEcho.reset();
            }
        }
        return;
    }
    if (m_state != ConnectionState::Attached)
        return;
    if (m_table && m_record) {
        if (m_record->state != AttachmentState::Live || m_record->servingConnection != m_connection) {
            supersede();
            return;
        }
        for (PaneAttachment& attachment : m_panes) {
            if (attachment.source)
                attachment.source->effects().reconcileReaders(*m_table, now);
        }
    }
    if (!m_oversizedPanes.empty())
        serviceOversizedPanes(now);
    // Bytes outstanding with nothing heard past the bound: the path is gone.
    // A connection with nothing outstanding is never lost this way, however
    // long it idles.
    if (const std::optional<std::uint64_t> silence = silenceDeadlineMicroseconds(); silence && now >= *silence) {
        m_state = ConnectionState::Lost;
        ++m_counters.lostDeclared;
        discardInboundClipboard();
        releaseDriverFocus();
        // The table may free the record: nothing here holds it past the note.
        if (m_table && m_record) {
            const ID128 attachmentID = m_record->attachmentID;
            releaseRecord();
            m_table->noteTransportLost(attachmentID, m_connection, now);
        }
        m_scheduler.abandonTransfers();
        return;
    }
    if (m_owedEcho) {
        Echo echo = *std::exchange(m_owedEcho, std::nullopt);
        echo.holdMicroseconds = static_cast<std::uint32_t>(std::min<std::uint64_t>(now - m_owedEchoReceivedAt, std::numeric_limits<std::uint32_t>::max()));
        std::vector<std::uint8_t> body;
        encode(body, echo);
        if (m_owedEchoCarrier == Carrier::Datagram && m_policy.datagramMaxBytes > 0) {
            std::vector<std::uint8_t> datagram;
            (void)appendFrame(datagram, FrameKind::Echo, 0, body);
            m_datagrams.push_back(std::move(datagram));
        } else {
            enqueue(FrameKind::Echo, 0, body);
        }
        ++m_counters.echoesSent;
    }
    for (const DatagramInFlight& lost : m_pacer.expireDatagrams(now)) {
        ++m_counters.datagramsLost;
        PaneAttachment* attachment = pane(lost.handle);
        if (attachment && (!attachment->base->acknowledgedGeneration || lost.generation > *attachment->base->acknowledgedGeneration))
            attachment->isLossPending = true;
        // The bytes counted lost no longer hold the window.
        m_isWindowRefused = false;
    }
    // A client is told of its events a signal discarded before any update
    // or receipt whose count passes them, so no count says the program was
    // written them.
    for (PaneAttachment& attachment : m_panes) {
        if (!attachment.source)
            continue;
        for (const InputDiscardRecord& run : attachment.source->inputLedger().discards(m_clientInstance)) {
            if (run.serial <= attachment.reportedDiscardSerial)
                continue;
            attachment.reportedDiscardSerial = run.serial;
            std::vector<std::uint8_t> body;
            encode(body, InputDiscarded { .inputWritten = run.inputWritten, .throughSeq = run.throughSeq });
            enqueue(FrameKind::InputDiscarded, attachment.handle, body);
            ++m_counters.inputDiscardsSent;
        }
    }
    // The panes holding input go first, in bind order.
    bool isRefused = false;
    for (PaneAttachment& attachment : m_panes) {
        if (!attachment.source || !holdsInput(attachment))
            continue;
        const PaneService served = servicePane(attachment, now);
        attachment.servedBytes += served.bytes;
        if (served.isRefused) {
            isRefused = true;
            break;
        }
    }
    // The rest by deficit round-robin over the bytes each is sent: the owed
    // pane furthest behind the most-served one goes next, ties in turn from
    // the cursor. A pane's deficit is capped at one quantum, so one that was
    // quiet cannot take the window for more than that once it has something
    // to send. Each pane is served at most once a service and is ahead by at
    // most the one frame it was last sent, so two panes that stay owed are
    // sent within two quanta of each other however long they flood.
    constexpr std::uint64_t quantum = maxEnvelopeHeaderBytes + maxFrameBodyBytes;
    std::uint64_t mostServed = 0;
    for (const PaneAttachment& attachment : m_panes) {
        if (attachment.source)
            mostServed = std::max(mostServed, attachment.servedBytes);
    }
    const std::size_t count = m_panes.size();
    std::vector<std::size_t> order;
    for (std::size_t visit = 0; visit < count && !isRefused; ++visit) {
        const std::size_t index = (m_roundRobinCursor + visit) % count;
        PaneAttachment& attachment = m_panes[index];
        if (!attachment.source || holdsInput(attachment) || (!isOwed(attachment) && !isDirtyNoticeOwed(attachment) && !isPaletteLookOwed(attachment) && !isResetOwed(attachment) && !isRecordOwed(attachment) && !isEffectOwed(attachment) && !isImageOwed(attachment)))
            continue;
        attachment.servedBytes = std::max(attachment.servedBytes, mostServed > quantum ? mostServed - quantum : 0);
        order.push_back(index);
    }
    std::ranges::stable_sort(order, {}, [&](std::size_t index) { return m_panes[index].servedBytes; });
    for (const std::size_t index : order) {
        if (isRefused)
            break;
        PaneAttachment& attachment = m_panes[index];
        const PaneService served = servicePane(attachment, now);
        attachment.servedBytes += served.bytes;
        if (served.isRefused) {
            // The refused pane is first when the window next opens.
            m_roundRobinCursor = index;
            isRefused = true;
        } else if (served.bytes > 0) {
            m_roundRobinCursor = (index + 1) % count;
        }
    }
    // A structure the owner offered leaves behind the pane frames this pass
    // took, so a frame taken before the structure changed goes ahead of the
    // record that names the change (Docs/sync.md §7.8). A frame the window
    // held back may follow it: the leaf's generation is what the client goes
    // by. Listed panes' records go last: nobody is waiting on a keystroke
    // for them.
    if (!isRefused && wantsInput())
        isRefused = !serviceStructures(now);
    if (!isRefused)
        (void)serviceListed(now);
    // Input written with no update on the stream to report it within the
    // receipt delay is reported on its own.
    for (PaneAttachment& attachment : m_panes) {
        const std::optional<std::uint64_t> due = receiptDeadlineMicroseconds(attachment, now);
        if (!due || now < *due)
            continue;
        const std::uint64_t written = attachment.source->inputLedger().lastWritten(m_clientInstance).value_or(0);
        const std::uint64_t settled = receiptSettled(attachment);
        std::vector<std::uint8_t> body;
        encode(body, Receipt { .inputWritten = written, .inputSettled = settled });
        enqueue(FrameKind::Receipt, attachment.handle, body);
        attachment.reportedInputWritten = written;
        attachment.reportedInputSettled = settled;
        ++m_counters.receiptsSent;
    }
    // A paste's client is told how far the program has taken it, so it sends
    // no more than the window past that (Docs/sync.md §6.7).
    for (PaneAttachment& attachment : m_panes) {
        if (isPasteReceiptOwed(attachment)) {
            const std::optional<PaneInputQueue::PasteWritten> written = attachment.source->pasteWritten(m_clientInstance);
            std::vector<std::uint8_t> body;
            encode(body, PasteReceipt { .inputSeq = written->inputSeq, .bytesWritten = written->bytesWritten });
            enqueue(FrameKind::PasteReceipt, attachment.handle, body);
            attachment.sentPasteWritten = written;
            ++m_counters.pasteReceiptsSent;
        }
    }
    // History a client wants takes the bulk lane, one transfer per priority
    // in the lanes at a time, ranked so what the user looks at leaves first;
    // backfill takes what is left.
    for (PaneAttachment& attachment : m_panes) {
        for (const HistoryPriority priority : { HistoryPriority::Viewport, HistoryPriority::Search, HistoryPriority::Backfill }) {
            if (isWantOwed(attachment, priority))
                serviceWant(attachment, priority);
        }
        if (isOpenLineOwed(attachment)) {
            HistoryAppends appends = historyAppends(attachment);
            if (m_pacer.admits(appends.interactiveBytes, now, queuedInteractiveBytes()))
                sendHistoryAppends(attachment, std::move(appends));
        }
    }
    // A probe goes on the backing-off schedule while some pane is Live and the
    // client focused, and only when the window has room for it.
    std::vector<std::uint8_t> probeBody;
    encode(probeBody, Probe { .time = now });
    if (isProbing() && m_pacer.isProbeDue(now) && m_pacer.admits(frameBytes(0, probeBody.size()), now, queuedInteractiveBytes())) {
        const std::vector<std::uint8_t>& body = probeBody;
        if (m_policy.datagramMaxBytes > 0) {
            std::vector<std::uint8_t> datagram;
            (void)appendFrame(datagram, FrameKind::Probe, 0, body);
            m_datagrams.push_back(std::move(datagram));
        } else {
            enqueue(FrameKind::Probe, 0, body);
        }
        m_pacer.noteProbe(now, now);
        ++m_counters.probesSent;
    }
    if (!m_unheardSince && isAwaitingClient())
        m_unheardSince = now;
}

bool HostConnection::holdsInput(const PaneAttachment& attachment)
{
    return attachment.source && attachment.base->sentGeneration < attachment.source->inputAwaitedGeneration();
}

bool HostConnection::isResetOwed(const PaneAttachment& attachment)
{
    return attachment.source && attachment.history.epoch != 0 && attachment.source->historyAddress().epoch != attachment.history.epoch;
}

HostConnection::PaneService HostConnection::servicePane(PaneAttachment& attachment, std::uint64_t now)
{
    // A clear goes ahead of everything else the pane sends, in every view: a
    // clear is a purge the client makes before it applies anything after
    // (Docs/sync.md §6.6, §7.5).
    if (isResetOwed(attachment))
        resetHistory(attachment);
    // A pane's records go ahead of its screen (Docs/sync.md §7.8).
    // A status set still leaving holds the screen behind it: the rest of its
    // frames go on the passes after this one.
    const PaneService records = serviceRecords(attachment, now);
    if (records.isRefused || attachment.records.statusRevisionInTransit != 0)
        return { .bytes = records.bytes, .isRefused = records.isRefused };
    const PaneService screen = serviceScreen(attachment, now);
    if (screen.isRefused)
        return { .bytes = records.bytes + screen.bytes, .isRefused = true };
    const PaneService effects = serviceEffects(attachment, now);
    if (effects.isRefused || m_state != ConnectionState::Attached)
        return { .bytes = records.bytes + screen.bytes + effects.bytes, .isRefused = true };
    const PaneService images = serviceImages(attachment, now);
    return { .bytes = records.bytes + screen.bytes + effects.bytes + images.bytes, .isRefused = images.isRefused };
}

bool HostConnection::isEffectOwed(const PaneAttachment& attachment) const
{
    return attachment.source && attachment.isViewStated && !attachment.clipboard
        && (attachment.isEffectBaselineOwed || attachment.effectBatch || attachment.source->effects().newestSequence() > attachment.effectsQueued);
}

HostConnection::PaneService HostConnection::serviceEffects(PaneAttachment& attachment, std::uint64_t now)
{
    if (!isEffectOwed(attachment) || attachment.clipboard || m_scheduler.isQueued(FrameKind::Effect, attachment.handle))
        return {};
    PaneSource& source = *attachment.source;
    if (!attachment.effectBatch) {
        if (attachment.isEffectBaselineOwed) {
            attachment.effectBatch = EffectBatch {};
            attachment.effectBatch->throughSequence = attachment.effectsQueued;
        } else {
            const EffectReadMode mode = m_clientKind == ClientKind::Agent                ? EffectReadMode::Consecutive
                : attachment.view != ViewState::Live || attachment.didSkipEffectPictures ? EffectReadMode::Skipped
                                                                                         : EffectReadMode::Consecutive;
            const auto batch = source.effects().readAfter(attachment.effectsQueued, mode);
            if (!batch) {
                close(ByeReason::TransferRefused, "an effect batch is no longer retained");
                return {};
            }
            attachment.effectBatch = *batch;
            // Clipboard data is private to its immutable human recipient.
            // Other readers receive only completion, never a hidden payload.
            std::size_t retained = 0;
            for (std::size_t index = 0; index < attachment.effectBatch->size; ++index) {
                const auto& record = attachment.effectBatch->records[index];
                if (record.kind != EffectKind::ClipboardWrite
                    || effectDisposition(record, m_clientInstance, m_clientKind, m_grantedRole) == EffectDisposition::Post)
                    attachment.effectBatch->records[retained++] = record;
            }
            attachment.effectBatch->size = retained;
            // An identity change ends a selected batch. If this batch already
            // uses the old identity, end before the replacement instead: only
            // a complete-batch ACK permits that slot to be overwritten.
            std::array<bool, VT::sampleSlotCount> usedSlots {};
            for (std::size_t index = 0; index < attachment.effectBatch->size; ++index) {
                const auto& record = attachment.effectBatch->records[index];
                // This final packet may name bulk clipboard bytes. No later
                // batch can become sent before that transfer finishes.
                if (record.kind == EffectKind::ClipboardWrite) {
                    attachment.effectBatch->size = index + 1;
                    attachment.effectBatch->throughSequence = record.sequence;
                    break;
                }
                if (record.kind != EffectKind::Sound)
                    continue;
                const auto decoded = decodeEffectBody(record.kind, record.payload);
                const auto* sound = decoded ? std::get_if<SoundEffect>(&*decoded) : nullptr;
                if (!sound || sound->request.wave != 6 || !sound->request.sampleSlot)
                    continue;
                const auto slot = sound->request.sampleSlot - 1;
                if (attachment.samples[slot].identity != sound->sample) {
                    const std::size_t count = usedSlots[slot] ? index : index + 1;
                    attachment.effectBatch->size = count;
                    attachment.effectBatch->throughSequence = attachment.effectBatch->records[count - 1].sequence;
                    break;
                }
                usedSlots[slot] = true;
            }
        }
        attachment.effectGeneration = source.generation();
        attachment.effectCursor = 0;
        attachment.didSkipEffectPictures = false;
    }
    // A lost datagram cannot establish the effect's picture dependency. A
    // visible pane waits for acknowledgement of the selected batch's picture.
    if (attachment.view == ViewState::Live && (attachment.needsCheckpoint || (attachment.checkpointTransfer && m_scheduler.isTransferQueued(*attachment.checkpointTransfer)) || attachment.base->acknowledgedGeneration.value_or(0) < attachment.effectGeneration))
        return {};
    const EffectBatch& batch = *attachment.effectBatch;
    std::size_t sampleHeadWireBytes = 0;
    if (batch.size) {
        const auto& record = batch.records[attachment.effectCursor];
        const auto decoded = record.kind == EffectKind::Sound ? decodeEffectBody(record.kind, record.payload) : std::expected<EffectBody, EffectCodecError> { std::unexpected(EffectCodecError::Malformed) };
        const auto* sound = decoded ? std::get_if<SoundEffect>(&*decoded) : nullptr;
        if (sound && sound->request.wave == 6 && sound->request.sampleSlot
            && effectDisposition(record, m_clientInstance, m_clientKind, m_grantedRole) == EffectDisposition::Post) {
            auto& delivery = attachment.samples[sound->request.sampleSlot - 1];
            if (delivery.identity != sound->sample) {
                if (attachment.sampleEffectsAcknowledged < delivery.lastUsingSequence) {
                    attachment.sampleAckAwaited = delivery.lastUsingSequence;
                    return {};
                }
                attachment.sampleAckAwaited = 0;
                if (record.data.size() != static_cast<std::size_t>(sound->sample.frames) * 2) {
                    close(ByeReason::Malformed, "a sampled effect lost its immutable PCM owner");
                    return {};
                }
                const auto headBytes = frameBytes(attachment.handle, sampleHeadBytes);
                const auto growth = record.data.size() + headBytes;
                prepareOutboundGrowth(growth);
                if (growth > m_policy.limits.maxQueuedBulkBytes || retainedOutboundPayloadBytes() > m_policy.limits.maxQueuedBulkBytes - growth)
                    return { .bytes = 0, .isRefused = true };
                if (m_imageSourceAdmission) {
                    const auto admission = m_imageSourceAdmission();
                    if (admission.connection && admission.aggregate
                        && (growth > admission.connection->availableBytes(admission.otherConnectionBytes)
                            || growth > admission.aggregate->availableBytes(admission.otherAggregateBytes)))
                        return { .bytes = 0, .isRefused = true };
                }
                if (!m_pacer.admits(headBytes, now, queuedInteractiveBytes())) {
                    ++m_counters.windowRefusals;
                    m_isWindowRefused = true;
                    return { .bytes = 0, .isRefused = true };
                }
                const auto transfer = sound->sample.frames ? m_scheduler.nextTransferID() : 0;
                std::vector<std::uint8_t> head;
                if (!encodeSampleHead(head, { .slot = sound->request.sampleSlot, .sample = sound->sample, .transfer = transfer })) {
                    close(ByeReason::Malformed, "a sampled effect has an invalid SampleHead");
                    return {};
                }
                sampleHeadWireBytes = transfer
                    ? m_scheduler.enqueueCopiedTransfer(FrameKind::SampleHead, attachment.handle, head, record.data, TransferRank::Answer)
                    : m_scheduler.enqueue(FrameKind::SampleHead, attachment.handle, head);
                if (!sampleHeadWireBytes) {
                    close(ByeReason::TransferRefused, "a SampleHead could not be queued");
                    return {};
                }
                delivery.identity = sound->sample;
                delivery.transfer = transfer;
            }
            delivery.lastUsingSequence = record.sequence;
        }
    }
    // The record, its batch wrapper and the queued envelope can coexist.
    // Retire image ownership before any of those bounded copies allocate.
    prepareOutboundGrowth(3 * (maxEnvelopeHeaderBytes + maxFrameBodyBytes));
    EffectCodecBytes encoded;
    EffectCodecBytes clipboardBody;
    std::span<const std::uint8_t> clipboardData;
    std::uint32_t clipboardTransfer = 0;
    if (batch.size) {
        EffectView view = batch.records[attachment.effectCursor];
        if (view.kind == EffectKind::ClipboardWrite) {
            const auto decoded = decodeEffectBody(view.kind, view.payload);
            const auto* write = decoded ? std::get_if<ClipboardWriteEffect>(&*decoded) : nullptr;
            if (!write || !decodeEffectClipboard(view.data, maxEffectClipboardWriteBytes)) {
                close(ByeReason::Malformed, "a clipboard effect lost its complete owner");
                return {};
            }
            // Envelope record metadata, batch header and clipboard descriptor.
            constexpr std::size_t inlineLimit = maxFrameBodyBytes - effectRecordHeaderBytes - effectPacketHeaderBytes - 9;
            if (view.data.size() > inlineLimit) {
                const auto growth = view.data.size() + 3 * (maxEnvelopeHeaderBytes + maxFrameBodyBytes);
                prepareOutboundGrowth(growth);
                if (growth > m_policy.limits.maxQueuedBulkBytes || retainedOutboundPayloadBytes() > m_policy.limits.maxQueuedBulkBytes - growth)
                    return { .bytes = 0, .isRefused = true };
                if (m_imageSourceAdmission) {
                    const auto admission = m_imageSourceAdmission();
                    if (admission.connection && admission.aggregate
                        && (growth > admission.connection->availableBytes(admission.otherConnectionBytes)
                            || growth > admission.aggregate->availableBytes(admission.otherAggregateBytes)))
                        return { .bytes = 0, .isRefused = true };
                }
                clipboardTransfer = m_scheduler.nextTransferID();
                clipboardData = view.data;
            }
            auto prepared = encodeEffectBody(ClipboardWriteEffect { write->buffer,
                { .transfer = clipboardTransfer, .encodedBytes = static_cast<std::uint32_t>(view.data.size()), .inlineBytes = clipboardTransfer ? std::span<const std::uint8_t> {} : view.data } });
            if (!prepared) {
                close(ByeReason::TransferRefused, "a clipboard descriptor could not be encoded");
                return {};
            }
            clipboardBody = std::move(*prepared);
            view.payload = clipboardBody.view();
        }
        auto record = encodeEffect(view);
        if (!record) {
            close(ByeReason::TransferRefused, "an effect cannot be encoded");
            return {};
        }
        encoded = std::move(*record);
    }
    const std::uint16_t remaining = batch.size ? static_cast<std::uint16_t>(batch.size - attachment.effectCursor - 1) : 0;
    auto packet = encodeEffectPacket(source.paneID(), attachment.effectGeneration, batch.throughSequence, remaining, encoded.view());
    if (!packet) {
        close(ByeReason::TransferRefused, "an effect packet cannot be encoded");
        return {};
    }
    const std::size_t bytes = frameBytes(attachment.handle, packet->size);
    if (!m_pacer.admits(bytes, now, queuedInteractiveBytes())) {
        ++m_counters.windowRefusals;
        m_isWindowRefused = true;
        return { .bytes = sampleHeadWireBytes, .isRefused = true };
    }
    const auto queuedBytes = clipboardTransfer
        ? m_scheduler.enqueueCopiedTransfer(FrameKind::Effect, attachment.handle, packet->view(), clipboardData, TransferRank::Answer)
        : m_scheduler.enqueue(FrameKind::Effect, attachment.handle, packet->view());
    if (!queuedBytes) {
        close(ByeReason::TransferRefused, "an effect packet was refused");
        return {};
    }
    if (clipboardTransfer)
        attachment.clipboard = PaneAttachment::ClipboardDelivery { clipboardTransfer, batch.throughSequence };
    ++attachment.effectCursor;
    if (!remaining) {
        attachment.effectsQueued = batch.throughSequence;
        attachment.effectBatch.reset();
        attachment.isEffectBaselineOwed = false;
    }
    return { .bytes = sampleHeadWireBytes + bytes, .isRefused = false };
}

bool HostConnection::admitsImageSource(const PaneAttachment& attachment, Digest128 hash) const
{
    if (!m_imageSourceAdmission || outstandingCalls() != 0 || !m_receivedCalls.empty())
        return false;
    std::size_t active = 0;
    for (const auto& bound : m_panes) {
        for (const auto& image : bound.images)
            active += image && (image->source || (image->transfer && m_scheduler.isTransferQueued(image->transfer))) ? 1 : 0;
    }
    if (active >= Image::maxTerminalImageCount)
        return false;
    const auto charge = attachment.source->images().transferReservationBytes(hash, imageCoding());
    if (!charge)
        return false;
    const BulkSourceAdmission admission = m_imageSourceAdmission();
    // The queued head and its release ID remain ordinary scheduler owners,
    // separate from the source's chunk scratch reservation.
    const auto headBytes = frameBytes(attachment.handle, 16 + 1 + 4 + 4 + 4) + sizeof(std::uint32_t);
    if (!admission.connection || !admission.aggregate || admission.connection == admission.aggregate)
        return false;
    const auto connectionRoom = admission.connection->availableBytes(admission.otherConnectionBytes);
    const auto aggregateRoom = admission.aggregate->availableBytes(admission.otherAggregateBytes);
    return headBytes <= connectionRoom && *charge <= connectionRoom - headBytes
        && headBytes <= aggregateRoom && *charge <= aggregateRoom - headBytes;
}

std::size_t HostConnection::retainedOutboundPayloadBytes() const
{
    std::size_t bytes = m_scheduler.retainedPayloadBytes();
    for (const auto& [id, structure] : m_structures) {
        (void)id;
        if (structure.record)
            bytes += structure.record->capacity();
    }
    for (const auto& datagram : m_datagrams)
        bytes += datagram.capacity();
    return bytes;
}

void HostConnection::preemptImageTransfers()
{
    if (m_isPreemptingImages)
        return;
    m_isPreemptingImages = true;
    for (auto& attachment : m_panes) {
        if (std::ranges::any_of(attachment.images, [&](const auto& image) {
                return image && (image->source || (image->transfer && m_scheduler.isTransferQueued(image->transfer)));
            })) {
            ++m_imagePreemptionGeneration;
            abortImages(attachment);
        }
    }
    m_isPreemptingImages = false;
}

void HostConnection::prepareOutboundGrowth(std::size_t bytes)
{
    if (!bytes || !m_imageSourceAdmission || m_isPreemptingImages)
        return;
    const BulkSourceAdmission admission = m_imageSourceAdmission();
    if (!admission.connection || !admission.aggregate)
        return;
    if (bytes <= admission.connection->availableBytes(admission.otherConnectionBytes)
        && bytes <= admission.aggregate->availableBytes(admission.otherAggregateBytes))
        return;
    // The callback may retire only image owners, across the aggregate. Abort
    // frames are queued under each connection's guard, after source release,
    // so they cannot recursively preempt the image slot being retired.
    if (m_preemptAggregateImages)
        m_preemptAggregateImages();
    else
        preemptImageTransfers();
}

bool HostConnection::isImageRevisionHeld(const PaneAttachment& attachment) const
{
    const auto& state = attachment.source->images().state();
    if (!state.revision || state.revision == attachment.sentImageRevision)
        return false;
    const auto bindings = std::span(state.images).first(state.imageCount);
    const auto placements = std::span(state.placements).first(state.placementCount);
    // A program can replace pictures faster than the link carries them. Each
    // picture of the record sent finishes before the newest record is
    // sampled: one whose transfer has started, and one waiting to start
    // behind it, so two pictures replaced together both arrive rather than
    // the second being dropped whenever the first completes. A removal is not
    // coalesced, and the view, unbind and pressure paths still abort owners.
    bool isHeld = false;
    for (const auto& image : attachment.images) {
        if (!image || std::ranges::any_of(bindings, [&](const auto& binding) { return binding.asset == image->asset; }))
            continue;
        const bool isSending = image->transfer && m_scheduler.isTransferQueued(image->transfer);
        const bool isWaiting = image->source || (!image->transfer && image->isWanted && admitsImageSource(attachment, image->asset.hash));
        if (!isSending && !isWaiting)
            continue;
        const bool isReplaced = image->placement && std::ranges::any_of(placements, [&](const auto& placement) {
            if (!hasSamePlacementGeometry(*image->placement, placement.geometry))
                return false;
            const auto binding = std::ranges::find(bindings, placement.geometry.imageID, &ImageBinding::imageID);
            return binding != bindings.end() && binding->asset.width == image->asset.width && binding->asset.height == image->asset.height
                && binding->asset.format == image->asset.format
                && std::ranges::none_of(attachment.images, [&](const auto& held) { return held && held->asset == binding->asset; });
        });
        if (!isReplaced)
            return false;
        isHeld = true;
    }
    return isHeld;
}

bool HostConnection::isImageOwed(const PaneAttachment& attachment) const
{
    if (!m_imageSourceAdmission || !attachment.source || attachment.view != ViewState::Live)
        return false;
    // Captured pixels no longer depend on the terminal, so their heads leave
    // while it moves on; everything else reads the frame it published.
    if (std::ranges::any_of(attachment.images, [](const auto& image) { return image && image->source; }))
        return true;
    if (!attachment.source->isFrameHeld())
        return false;
    const auto& state = attachment.source->images().state();
    if (state.revision && state.revision != attachment.sentImageRevision && !isImageRevisionHeld(attachment))
        return true;
    return std::ranges::any_of(attachment.images, [&](const auto& image) {
        return image && (!image->isOffered || (image->isWanted && admitsImageSource(attachment, image->asset.hash)));
    });
}

std::unique_ptr<BulkSource> HostConnection::captureImageSource(const PaneAttachment& attachment, Digest128 hash) const
{
    // The head this source waits for, and its release ID, are ordinary
    // scheduler owners counted beside it.
    auto admission = m_imageSourceAdmission();
    const auto headBytes = frameBytes(attachment.handle, 16 + 1 + 4 + 4 + 4) + sizeof(std::uint32_t);
    admission.otherConnectionBytes += headBytes;
    admission.otherAggregateBytes += headBytes;
    return attachment.source->images().copyForTransfer(hash, admission, imageCoding());
}

void HostConnection::abortImages(PaneAttachment& attachment)
{
    const bool wasPreempting = std::exchange(m_isPreemptingImages, true);
    for (auto& image : attachment.images) {
        if (!image)
            continue;
        if (image->transfer)
            abortHistoryTransfer(image->transfer);
        image.reset();
    }
    // Returning to Live offers again, because an aborted transfer did not
    // establish a client cache. Verified pixels it retained need not cross.
    attachment.sentImageRevision = 0;
    attachment.shouldOfferRetainedImages = true;
    m_isPreemptingImages = wasPreempting;
}

HostConnection::PaneService HostConnection::serviceImages(PaneAttachment& attachment, std::uint64_t now)
{
    if (!isImageOwed(attachment))
        return {};
    PaneService served;
    const auto admitHead = [&](std::size_t bodyBytes) {
        const auto bytes = frameBytes(attachment.handle, bodyBytes);
        if (!m_pacer.admits(bytes, now, queuedInteractiveBytes())) {
            ++m_counters.windowRefusals;
            m_isWindowRefused = true;
            served.isRefused = true;
            return false;
        }
        served.bytes += bytes;
        return true;
    };
    const auto& state = attachment.source->images().state();
    const bool isFrameHeld = attachment.source->isFrameHeld();
    if (isFrameHeld && state.revision != attachment.sentImageRevision && !isImageRevisionHeld(attachment)) {
        std::vector<std::uint8_t> body;
        encode(body, state);
        if (!admitHead(body.size()))
            return served;
        // Make room before borrowing any mutable delivery slot: preemption
        // can reset the entire image table while this immutable state stays.
        prepareOutboundGrowth(frameBytes(attachment.handle, body.size()));
        const auto bindings = std::span(state.images).first(state.imageCount);
        // Retain transfers for unchanged content, even if image IDs, crops or
        // positions changed. Removing content fences its partial transfer.
        for (auto& image : attachment.images) {
            if (image && std::ranges::none_of(bindings, [&](const auto& binding) { return binding.asset == image->asset; })) {
                if (image->transfer)
                    abortHistoryTransfer(image->transfer);
                image.reset();
            }
        }
        for (const auto& binding : bindings) {
            if (std::ranges::any_of(attachment.images, [&](const auto& image) { return image && image->asset == binding.asset; }))
                continue;
            const auto empty = std::ranges::find_if(attachment.images, [](const auto& image) { return !image; });
            if (empty != attachment.images.end())
                *empty = PaneAttachment::ImageDelivery { .asset = binding.asset, .placement = std::nullopt, .source = nullptr, .isWanted = !attachment.shouldOfferRetainedImages };
        }
        for (auto& image : attachment.images) {
            if (!image)
                continue;
            image->placement.reset();
            for (const auto& placement : std::span(state.placements).first(state.placementCount)) {
                if (placement.hash != image->asset.hash)
                    continue;
                if (image->placement) {
                    image->placement.reset();
                    break;
                }
                image->placement = placement.geometry;
            }
        }
        enqueue(FrameKind::ImagePlacements, attachment.handle, body);
        attachment.sentImageRevision = state.revision;
        attachment.shouldOfferRetainedImages = false;
        ++m_counters.imagePlacementsSent;
        // Every picture the record names is captured with it. The program
        // can replace a picture in the host's model before its head leaves,
        // and a picture the model no longer holds cannot be sent.
        for (auto& image : attachment.images) {
            if (!image || !image->isWanted || image->transfer || image->source || !admitsImageSource(attachment, image->asset.hash))
                continue;
            image->source = captureImageSource(attachment, image->asset.hash);
            if (!image->source) {
                close(ByeReason::TransferRefused, "an image source could not be retained");
                return { .bytes = served.bytes, .isRefused = true };
            }
        }
    }
    // One asset head a pass; the next pass observes the preceding head in the
    // paced interactive queue. No complete encoded pixel payload is built.
    for (auto& image : attachment.images) {
        if (!image || (image->isOffered && !image->isWanted) || (!image->source && !isFrameHeld))
            continue;
        const bool canTransfer = image->source || (image->isWanted && admitsImageSource(attachment, image->asset.hash));
        if (image->isOffered && !canTransfer)
            continue;
        std::vector<std::uint8_t> body;
        encode(body, ImageHead { .asset = image->asset, .transfer = canTransfer ? m_scheduler.nextTransferID() : 0 });
        if (!admitHead(body.size()))
            return served;
        const auto preemption = m_imagePreemptionGeneration;
        prepareOutboundGrowth(frameBytes(attachment.handle, body.size()) + (canTransfer ? sizeof(std::uint32_t) : 0));
        if (preemption != m_imagePreemptionGeneration)
            return served; // Rebuild the reset delivery table on the next pass.
        if (canTransfer) {
            auto source = std::move(image->source);
            if (!source)
                source = captureImageSource(attachment, image->asset.hash);
            if (!source) {
                close(ByeReason::TransferRefused, "an image source could not be retained");
                return { .bytes = served.bytes, .isRefused = true };
            }
            image->transfer = m_scheduler.nextTransferID();
            const auto pixelBytes = *imagePixelBytes(image->asset);
            m_counters.imagePixelBytesQueued += pixelBytes;
            m_counters.imageWireBytesQueued += source->bytes().size();
            const PayloadCoding coding = imageCoding() == Compression::Deflate
                ? PayloadCoding { .compression = Compression::Deflate, .inflatedBytes = static_cast<std::uint32_t>(pixelBytes) }
                : PayloadCoding {};
            (void)m_scheduler.enqueueSourceTransfer(FrameKind::ImageHead, attachment.handle, body, std::move(source), TransferRank::Image, FrameRole::Message, coding);
            image->isWanted = false;
            ++m_counters.imageTransfersSent;
        } else {
            // Growth was admitted before the slot was used. A second generic
            // preemption here could invalidate it before isOffered is stored.
            (void)m_scheduler.enqueue(FrameKind::ImageHead, attachment.handle, body);
        }
        image->isOffered = true;
        break;
    }
    return served;
}

bool HostConnection::isRecordOwed(const PaneAttachment& attachment) const
{
    if (!attachment.source)
        return false;
    const PaneSource& source = *attachment.source;
    return (source.facts().revision != 0 && source.facts().revision != attachment.records.sentFactsRevision) || isStatusOwed(attachment);
}

bool HostConnection::isStatusOwed(const PaneAttachment& attachment) const
{
    // One carrier per reader: a pane the client holds Live has its status in
    // its screen (Docs/sync.md §7.7). Until the client has said how it holds
    // a pane, the host does not know which carrier that is.
    const StatusRecords& status = attachment.source->status();
    return attachment.isViewStated && attachment.view != ViewState::Live && status.revision != 0 && status.revision != attachment.records.sentStatusRevision;
}

HostConnection::PaneService HostConnection::serviceRecords(PaneAttachment& attachment, std::uint64_t now)
{
    if (!isRecordOwed(attachment))
        return {};
    // A pane bound to the connection is one whose screen it reads.
    return sendRecords(attachment.handle, *attachment.source, attachment.records, isStatusOwed(attachment) ? StatusCarriage::Sent : StatusCarriage::Withheld, ListedDetail::Whole, now);
}

HostConnection::PaneService HostConnection::sendRecords(std::uint32_t handle, const PaneSource& source, RecordsProgress& progress, StatusCarriage carriage, ListedDetail detail, std::uint64_t now)
{
    PaneService served;
    const auto send = [&](FrameKind kind, const std::vector<std::uint8_t>& body) {
        const std::size_t bytes = frameBytes(handle, body.size());
        if (!m_pacer.admits(bytes, now, queuedInteractiveBytes())) {
            ++m_counters.windowRefusals;
            m_isWindowRefused = true;
            served.isRefused = true;
            return false;
        }
        enqueue(kind, handle, body);
        served.bytes += bytes;
        return true;
    };
    if (const PaneFacts& facts = source.facts(); facts.revision != 0 && facts.revision != progress.sentFactsRevision) {
        std::vector<std::uint8_t> body;
        if (detail == ListedDetail::WithoutArguments) {
            // The names of what runs, in place of its command line.
            PaneFacts withheld = facts;
            withheld.processChainWithArguments = withheld.processChain;
            encodePaneFacts(body, withheld);
        } else {
            encodePaneFacts(body, facts);
        }
        if (!send(FrameKind::PaneFacts, body))
            return served;
        progress.sentFactsRevision = facts.revision;
        ++m_counters.paneFactsSent;
    }
    const StatusRecords& status = source.status();
    if (carriage == StatusCarriage::Withheld || status.revision == 0 || status.revision == progress.sentStatusRevision)
        return served;
    // A set that moved while its frames were leaving starts again: the
    // client drops what it assembled when a frame begins a set.
    if (progress.statusRevisionInTransit != status.revision) {
        progress.statusRevisionInTransit = status.revision;
        progress.statusCursor = 0;
    }
    // One frame of a set a pass: the window counts a frame once the
    // transport has taken it, so a second in the same pass would be admitted
    // against a window the first had not yet filled.
    std::vector<std::uint8_t> body;
    const std::size_t next = encodeStatusFragment(body, status, progress.statusCursor);
    if (!send(FrameKind::StatusRecords, body))
        return served;
    progress.statusCursor = next;
    if (next < status.store.records.size())
        return served;
    progress.sentStatusRevision = status.revision;
    progress.statusRevisionInTransit = 0;
    progress.statusCursor = 0;
    ++m_counters.statusSetsSent;
    return served;
}

std::uint32_t HostConnection::listPane(PaneSource& source, ListedDetail detail)
{
    if (m_state != ConnectionState::Attached)
        return 0;
    if (const auto held = std::ranges::find(m_listed, &source, &ListedPane::source); held != m_listed.end())
        return held->handle;
    if (m_listed.size() >= maxPanesPerConnection || m_nextListedHandle == std::numeric_limits<std::uint32_t>::max())
        return 0;
    ListedPane listed;
    listed.handle = m_nextListedHandle++;
    listed.source = &source;
    listed.detail = detail;
    std::vector<std::uint8_t> body;
    encode(body, PaneBind { .handle = listed.handle, .paneID = source.paneID() });
    enqueue(FrameKind::PaneListed, 0, body);
    m_listed.push_back(listed);
    return listed.handle;
}

void HostConnection::unlistPane(std::uint32_t handle)
{
    const auto listed = std::ranges::find(m_listed, handle, &ListedPane::handle);
    if (listed == m_listed.end())
        return;
    m_listed.erase(listed);
    if (m_state != ConnectionState::Attached)
        return;
    std::vector<std::uint8_t> body;
    encode(body, PaneUnbind { .handle = handle, .reason = 0 });
    enqueue(FrameKind::PaneUnbind, 0, body);
}

std::vector<std::pair<std::uint32_t, ID128>> HostConnection::listedPanes() const
{
    std::vector<std::pair<std::uint32_t, ID128>> panes;
    panes.reserve(m_listed.size());
    for (const ListedPane& listed : m_listed)
        panes.emplace_back(listed.handle, listed.source->paneID());
    return panes;
}

bool HostConnection::isListedOwed(const ListedPane& listed)
{
    const PaneSource& source = *listed.source;
    return (source.facts().revision != 0 && source.facts().revision != listed.progress.sentFactsRevision)
        || (source.status().revision != 0 && source.status().revision != listed.progress.sentStatusRevision);
}

bool HostConnection::serviceListed(std::uint64_t now)
{
    for (ListedPane& listed : m_listed) {
        if (!isListedOwed(listed))
            continue;
        // A set begun is finished a frame a pass; anything new waits for
        // the listing's cadence.
        const bool isSetLeaving = listed.progress.statusRevisionInTransit != 0;
        if (!isSetLeaving && listed.servedAt && now - *listed.servedAt < listedRecordsIntervalMicroseconds)
            continue;
        const PaneService served = sendRecords(listed.handle, *listed.source, listed.progress, StatusCarriage::Sent, listed.detail, now);
        if (served.bytes > 0 && !isSetLeaving)
            listed.servedAt = now;
        if (served.isRefused)
            return false;
    }
    return true;
}

void HostConnection::offerWorkspaceStructure(std::uint32_t workspaceID, std::uint64_t revision, std::shared_ptr<const std::vector<std::uint8_t>> record)
{
    if (m_state != ConnectionState::Attached || !record || revision == 0 || record->empty() || record->size() > maxWorkspaceStructureRecordBytes)
        return;
    OwedStructure& owed = m_structures[workspaceID];
    // A workspace told gone and made again is a structure once more.
    if (owed.record && revision <= owed.revision)
        return;
    prepareOutboundGrowth(record->capacity());
    owed.revision = std::max(revision, owed.revision + 1);
    owed.record = std::move(record);
}

void HostConnection::withdrawWorkspaceStructure(std::uint32_t workspaceID)
{
    const auto owed = m_structures.find(workspaceID);
    if (owed == m_structures.end())
        return;
    // A client never told of the workspace has nothing to forget.
    if (owed->second.sentRevision == 0 || m_state != ConnectionState::Attached) {
        m_structures.erase(owed);
        return;
    }
    if (!owed->second.record)
        return;
    owed->second.record.reset();
    ++owed->second.revision;
}

bool HostConnection::isWorkspaceStructureSent(std::uint32_t workspaceID) const
{
    const auto owed = m_structures.find(workspaceID);
    return owed != m_structures.end() && owed->second.record && owed->second.revision == owed->second.sentRevision
        && (owed->second.transfer == 0 || !m_scheduler.isTransferQueued(owed->second.transfer));
}

bool HostConnection::isStructureDue(const OwedStructure& owed) const
{
    return owed.revision != owed.sentRevision && (owed.transfer == 0 || !m_scheduler.isTransferQueued(owed.transfer));
}

bool HostConnection::serviceStructures(std::uint64_t now)
{
    // The head beside an inline record: the id, the revision, the flags and
    // a transfer of zero.
    constexpr std::size_t inlineHeadBytes = 4 + 8 + 1 + 1;
    for (auto entry = m_structures.begin(); entry != m_structures.end();) {
        OwedStructure& owed = entry->second;
        if (!isStructureDue(owed)) {
            ++entry;
            continue;
        }
        WorkspaceStructureHead head { .workspaceID = entry->first, .revision = owed.revision, .transfer = 0, .isGone = !owed.record, .record = {} };
        std::vector<std::uint8_t> body;
        if (!owed.record || inlineHeadBytes + owed.record->size() <= maxFrameBodyBytes) {
            if (owed.record)
                head.record = *owed.record;
            encode(body, head);
            if (!m_pacer.admits(frameBytes(0, body.size()), now, queuedInteractiveBytes())) {
                ++m_counters.windowRefusals;
                m_isWindowRefused = true;
                return false;
            }
            enqueue(FrameKind::WorkspaceStructure, 0, body);
            owed.transfer = 0;
        } else {
            // Past the queued-bulk cap the record waits, owed, for the lanes
            // to drain: it is not queued behind what is already held.
            if (owed.record->size() > m_policy.limits.maxQueuedBulkBytes || queuedBytes() > m_policy.limits.maxQueuedBulkBytes - owed.record->size()) {
                ++entry;
                continue;
            }
            head.transfer = m_scheduler.nextTransferID();
            encode(body, head);
            preemptImageTransfers();
            (void)m_scheduler.enqueueTransfer(FrameKind::WorkspaceStructure, 0, body, std::vector<std::uint8_t>(*owed.record), TransferRank::Answer);
            owed.transfer = head.transfer;
            ++m_counters.structureTransfersSent;
        }
        owed.sentRevision = owed.revision;
        ++m_counters.structuresSent;
        entry = owed.record ? std::next(entry) : m_structures.erase(entry);
    }
    return true;
}

bool HostConnection::admitsCall(std::size_t answerBytes) const
{
    return outstandingCalls() < m_policy.limits.maxOutstandingCalls
        && answerBytes <= m_policy.limits.maxQueuedBulkBytes && queuedBytes() <= m_policy.limits.maxQueuedBulkBytes - answerBytes;
}

void HostConnection::handleCall(Call call)
{
    // Retire lower-priority pixel snapshots before the owner computes a reply,
    // not after a potentially large response has already been materialized.
    preemptImageTransfers();
    const std::uint32_t callIndex = m_nextCallIndex++;
    // Over the cap the call is answered busy and the connection stays: the
    // cap bounds what one attachment holds, and a caller that reached it may
    // call again once an answer has left. The answer is written here, where
    // the call is read, and nothing of the call is kept.
    if (outstandingCalls() >= m_policy.limits.maxOutstandingCalls) {
        if (m_busyAnswerer) {
            ++m_counters.callsAnsweredBusy;
            std::vector<std::uint8_t> body;
            encode(body, Result { .transfer = 0, .text = m_busyAnswerer(call.text) });
            if (body.size() <= maxFrameBodyBytes)
                enqueue(FrameKind::Result, 0, body, FrameRole::Reply);
            return;
        }
        m_callsOverLimit.insert(callIndex);
        m_receivedCalls.push_back({ .callIndex = callIndex, .text = std::move(call.text), .admission = CallAdmission::OverLimit });
        return;
    }
    m_pendingCalls.insert(callIndex);
    m_receivedCalls.push_back({ .callIndex = callIndex, .text = std::move(call.text), .admission = CallAdmission::Admitted });
    ++m_counters.callsAdmitted;
}

CallAnswer HostConnection::answerCall(std::uint32_t callIndex, std::string response)
{
    if (!m_pendingCalls.contains(callIndex) || m_state != ConnectionState::Attached)
        return CallAnswer::NotOutstanding;
    // The queued-bulk cap bounds the answers held for the attachment, whatever
    // they say.
    if (response.size() > m_policy.limits.maxQueuedBulkBytes || queuedBytes() > m_policy.limits.maxQueuedBulkBytes - response.size())
        return CallAnswer::OverBulkLimit;
    m_pendingCalls.erase(callIndex);
    sendResult(std::move(response), FrameRole::Reply);
    return CallAnswer::Sent;
}

void HostConnection::refuseCall(std::uint32_t callIndex, std::string_view response, CallRefusal refusal)
{
    if ((m_pendingCalls.erase(callIndex) == 0 && m_callsOverLimit.erase(callIndex) == 0) || m_state != ConnectionState::Attached)
        return;
    m_counters.callsAnsweredBusy += refusal == CallRefusal::Busy ? 1 : 0;
    std::vector<std::uint8_t> body;
    encode(body, Result { .transfer = 0, .text = std::string(response) });
    if (body.size() <= maxFrameBodyBytes)
        enqueue(FrameKind::Result, 0, body, FrameRole::Reply);
}

void HostConnection::sendResult(std::string response, FrameRole role)
{
    preemptImageTransfers();
    std::vector<std::uint8_t> body;
    encode(body, Result { .transfer = 0, .text = response });
    if (body.size() <= maxFrameBodyBytes) {
        enqueue(FrameKind::Result, 0, body, role);
        return;
    }
    body.clear();
    encode(body, Result { .transfer = m_scheduler.nextTransferID(), .text = {} });
    (void)m_scheduler.enqueueTransfer(FrameKind::Result, 0, body, std::vector<std::uint8_t>(response.begin(), response.end()), TransferRank::Answer, role);
}

bool HostConnection::sendNotify(std::span<const std::uint8_t> body)
{
    if (m_state != ConnectionState::Attached || body.empty() || body.size() > maxFrameBodyBytes || !wantsInput())
        return false;
    // Borrowed until the scheduler's admitted copy; no unbounded temporary
    // body precedes admission.
    enqueue(FrameKind::Notify, 0, body);
    return true;
}

bool HostConnection::sendConsentStatus(const ConsentStatus& status)
{
    if (!isConsentPending(m_state) || !m_identity.pendingConsent || status.question != *m_identity.pendingConsent
        || status.phase == ConsentStatusPhase::Approved || m_isReceiving
        || (status.phase == ConsentStatusPhase::Pending ? m_hasSentPendingConsent : !m_hasSentPendingConsent))
        return false;
    const auto text = consentStatusNotify(status);
    if (!text)
        return false;
    const std::size_t statusBytes = frameBytes(0, text->size());
    std::vector<std::uint8_t> farewell;
    if (status.phase != ConsentStatusPhase::Pending)
        encode(farewell, Bye { .reason = ByeReason::NoConsent, .detail = {}, .peer = {} });
    const std::size_t bytes = statusBytes + (farewell.empty() ? 0 : frameBytes(0, farewell.size()));
    const std::size_t queued = queuedInteractiveBytes();
    if (queued > m_policy.maxQueuedInteractiveBytes || bytes > m_policy.maxQueuedInteractiveBytes - queued)
        return false;
    if (!m_scheduler.enqueue(FrameKind::Notify, 0, *text))
        return false;
    if (status.phase == ConsentStatusPhase::Pending) {
        m_hasSentPendingConsent = true;
        return true;
    }
    if (m_table && m_record) {
        const ID128 attachmentID = m_record->attachmentID;
        releaseRecord();
        m_table->noteDetached(attachmentID);
    }
    close(ByeReason::NoConsent, {});
    return true;
}

bool HostConnection::deliverConsentApproval(ConsentBinding question, const ConnectionAuthority& authority, std::uint64_t now, std::uint64_t deadline)
{
    if (!isConsentPending(m_state) || !m_identity.pendingConsent || question != *m_identity.pendingConsent
        || now >= deadline || !m_table || !m_record || m_isReceiving
        || !m_table->canPromotePending(m_record->attachmentID, authority, m_connection))
        return false;
    m_state = ConnectionState::ApprovedAwaitingDelivery;
    if (!m_hasSentPendingConsent) {
        (void)sendConsentStatus({ .question = question, .phase = ConsentStatusPhase::Pending });
        return false;
    }
    const auto text = consentStatusNotify({ .question = question, .phase = ConsentStatusPhase::Approved });
    const std::size_t queued = queuedInteractiveBytes();
    if (!text || queued > m_policy.maxQueuedInteractiveBytes
        || frameBytes(0, text->size()) > m_policy.maxQueuedInteractiveBytes - queued)
        return false;
    // No callbacks, admission queries or owner mutations occur after this copy
    // and before promotion. Its prechecked exact table record cannot change.
    if (!m_scheduler.enqueue(FrameKind::Notify, 0, *text))
        return false;
    const bool promoted = m_table->promotePending(m_record->attachmentID, authority, m_connection);
    assert(promoted);
    (void)promoted;
    m_state = ConnectionState::Attached;
    return true;
}

std::optional<AttachmentsList> HostConnection::owedAttachments()
{
    if (m_state != ConnectionState::Attached || !m_table || !m_record)
        return std::nullopt;
    // What the list is made of is compared before the list is built: on a
    // pass where nothing moved this allocates nothing.
    bool isStale = m_areAttachmentsStale || m_table->revision() != m_listedTableRevision;
    for (PaneAttachment& attachment : m_panes) {
        if (!attachment.source)
            continue;
        const GeometryArbiter& geometry = attachment.source->geometry();
        const std::optional<ID128> owner = geometry.status().owner;
        const std::optional<ID128> primary = m_table->primary(geometry);
        if (owner == attachment.listedOwner && primary == attachment.listedPrimary)
            continue;
        attachment.listedOwner = owner;
        attachment.listedPrimary = primary;
        isStale = true;
    }
    if (!isStale)
        return std::nullopt;
    std::vector<ID128> panes;
    for (const PaneAttachment& attachment : m_panes) {
        if (attachment.source)
            panes.push_back(attachment.source->paneID());
    }
    std::vector<const AttachmentRecord*> listed = m_table->holding(panes);
    // A client is told of itself though it shows no pane yet.
    if (std::ranges::find(listed, m_record) == listed.end())
        listed.push_back(m_record);
    AttachmentsList list;
    list.attachments.reserve(listed.size());
    for (const AttachmentRecord* record : listed) {
        const auto isOf = [&](std::optional<ID128> PaneAttachment::* member) {
            return std::ranges::any_of(m_panes, [&](const PaneAttachment& attachment) { return attachment.source && attachment.*member == std::optional { record->attachmentID }; });
        };
        list.attachments.push_back({
            .attachmentID = record->attachmentID,
            .role = record->authority.mayDrive ? AttachmentRole::Driver : AttachmentRole::Observer,
            .clientKind = record->kind,
            .state = record->state,
            .isGeometryOwner = isOf(&PaneAttachment::listedOwner),
            .isPrimary = isOf(&PaneAttachment::listedPrimary),
            .panes = {},
            .isSelf = record == m_record,
            .principal = m_grantedRole == Role::Owner ? std::optional { record->authority.principal } : std::nullopt,
        });
        AttachmentEntry& entry = list.attachments.back();
        for (const ID128 pane : panes) {
            if (std::ranges::any_of(record->panes, [&](const auto& held) { return held->isHeld && held->paneID == pane; }))
                entry.panes.push_back(pane);
        }
        if (record->state == AttachmentState::Live && m_attachmentPresenceProvider)
            entry.presence = m_attachmentPresenceProvider(entry.attachmentID, entry.panes, entry.isSelf);
    }
    // Admit one complete roster before caching it. Optional display enrichment
    // is all-or-none; it can never withhold or selectively hide a base row.
    list.revision = m_table->nextNotifyRevision();
    if (!attachmentsNotifySize(list)) {
        for (auto& entry : list.attachments)
            entry.presence.reset();
        if (!attachmentsNotifySize(list)) {
            close(ByeReason::Oversize, {});
            return std::nullopt;
        }
    }
    m_listedTableRevision = m_table->revision();
    if (m_sentAttachments && *m_sentAttachments == list.attachments) {
        m_areAttachmentsStale = false;
        return std::nullopt;
    }
    // Owed until it is noted sent: a notify the queue had no room for is
    // built again on the next pass.
    m_areAttachmentsStale = true;
    return list;
}

void HostConnection::noteAttachmentsSent(const AttachmentsList& list)
{
    m_sentAttachments = list.attachments;
    m_areAttachmentsStale = false;
    ++m_counters.attachmentsNotifiesSent;
}

bool HostConnection::isCheckpointInLanes() const
{
    return std::ranges::any_of(m_panes, [&](const PaneAttachment& attachment) {
        return attachment.checkpointTransfer && m_scheduler.isTransferQueued(*attachment.checkpointTransfer);
    });
}

HostConnection::PaneService HostConnection::serviceScreen(PaneAttachment& attachment, std::uint64_t now)
{
    // A pane the client does not show is sent no screen: a Dormant one is
    // sent its palette when that moves and told once that it changed, and a
    // RecordsOnly one nothing.
    if (attachment.view != ViewState::Live) {
        const PaneService palette = servicePalette(attachment, now);
        if (palette.isRefused || !isDirtyNoticeOwed(attachment))
            return palette;
        const std::size_t bytes = frameBytes(attachment.handle, 0);
        if (!m_pacer.admits(bytes, now, queuedInteractiveBytes())) {
            ++m_counters.windowRefusals;
            m_isWindowRefused = true;
            return { .bytes = palette.bytes, .isRefused = true };
        }
        enqueue(FrameKind::PaneDirty, attachment.handle, {});
        attachment.hasSentDirty = true;
        ++m_counters.paneDirtiesSent;
        return { .bytes = palette.bytes + bytes, .isRefused = false };
    }
    PaneSource& source = *attachment.source;
    if (!source.isFrameHeld())
        return {};
    // A pane's updates chain from its checkpoint, so they wait for its body
    // to leave the lanes.
    if (attachment.checkpointTransfer && m_scheduler.isTransferQueued(*attachment.checkpointTransfer))
        return {};
    // A checkpoint the client asked for goes as soon as no other body is in
    // the lanes: until it arrives the client applies nothing. Every other
    // frame waits for the rate and the window, a checkpoint standing in for
    // an update included.
    if (attachment.needsCheckpoint) {
        if (!isCheckpointWithheld(attachment))
            sendCheckpoint(attachment, now);
        return {};
    }
    if (source.generation() <= attachment.base->sentGeneration && !attachment.isLossPending) {
        // A client told no echo context since its checkpoint, or one that no
        // longer holds, is owed it though the screen stands still. The frame
        // is taken again under the next generation, and the update to it,
        // with no row to say, carries the context (Docs/sync.md §6.7).
        if (!isEchoContextOwed(attachment))
            return {};
        source.republish(now);
    }
    const std::uint64_t newest = source.generation();
    if (newest > attachment.base->sentGeneration && newest - attachment.base->sentGeneration > 1)
        attachment.didSkipEffectPictures = true;
    // Appends too many for one frame travel as transfers, and the update that
    // scrolls them waits for them to leave (Docs/sync.md §7.8).
    std::erase_if(attachment.history.appendTransfers, [&](std::uint32_t transfer) { return !m_scheduler.isTransferQueued(transfer); });
    if (!attachment.history.appendTransfers.empty())
        return {};
    const std::uint64_t interval = rateIntervalMicroseconds();
    if (attachment.hasSentUpdate && now - attachment.lastUpdateMicroseconds < interval)
        return {};
    if (!m_pacer.isOpen(now, queuedInteractiveBytes())) {
        ++m_counters.windowRefusals;
        m_isWindowRefused = true;
        return { .bytes = 0, .isRefused = true };
    }
    if (!source.isEncodable()) {
        if (newest > attachment.base->sentGeneration)
            sendCheckpoint(attachment, now);
        return {};
    }

    const bool isSuperset = m_policy.datagramMaxBytes > 0 && !attachment.unacknowledgedCheckpoint && attachment.base->acknowledgedGeneration;
    // An update still waiting for the transport is the newest state that will
    // leave; one encoded behind it would only send a staler state first. The
    // pane stays owed, and the service after the transport takes the queue
    // encodes one update to whatever is newest then. Another pane's waiting
    // frames hold nothing of this one's back.
    if (!isSuperset && m_scheduler.isQueued(FrameKind::Update, attachment.handle))
        return {};
    const std::uint64_t base = isSuperset ? *attachment.base->acknowledgedGeneration : attachment.base->sentGeneration;
    if (base >= newest) {
        // Everything lost was already acknowledged by a later generation.
        attachment.isLossPending = false;
        return {};
    }
    AttachmentBaseline baseline = isSuperset ? supersetBaseline(attachment) : attachment.base->sentBaseline;
    std::vector<std::uint8_t> body;
    // The oldest change this update delivers is the first of the generation
    // after its base, so its age is how far behind the picture is.
    const std::uint64_t dirtySince = source.dirtySince(base + 1);
    encode(body, UpdateHeader {
                     .mode = isSuperset ? UpdateMode::Superset : UpdateMode::Chain,
                     .baseGeneration = base,
                     .generation = newest,
                     .frameClass = source.frameClass(),
                     .hostTime = now,
                     .lagMicroseconds = now > dirtySince ? now - dirtySince : 0,
                     .inputWritten = source.inputLedger().lastWritten(m_clientInstance).value_or(0),
                     .inputSettled = source.settledAtPublish(m_clientInstance),
                     .historyHead = source.historyAddress().head,
                     .effectsThrough = 0,
                 });
    // The echo context rides the update when it changed, and that update
    // goes on the stream: a datagram may be lost.
    std::optional<EchoContext> echo;
    if (m_capabilities.contains(ClientCapability::EchoContext)) {
        const EchoContext current = source.echoContextFor(m_clientInstance);
        if (attachment.sentEchoContext != current)
            echo = current;
    }
    // A superset update must apply onto any generation the client may hold,
    // so it says rows only whole.
    if (source.publisher().rowUpdate(source.terminal(), baseline, body, isSuperset ? ChainOps {} : chainOps(), echo ? &*echo : nullptr) == UpdateOutcome::NeedsCheckpoint || body.size() > maxFrameBodyBytes) {
        sendCheckpoint(attachment, now);
        return {};
    }
    const std::size_t bytes = frameBytes(attachment.handle, body.size());
    // The appends and the update that scrolls them are one admission: both
    // go, or neither does.
    HistoryAppends appends = historyAppends(attachment);
    if (!m_pacer.admits(bytes + appends.interactiveBytes, now, queuedInteractiveBytes())) {
        ++m_counters.windowRefusals;
        m_isWindowRefused = true;
        return { .bytes = 0, .isRefused = true };
    }
    const std::size_t historyBytes = appends.interactiveBytes;
    sendHistoryAppends(attachment, std::move(appends));
    if (isSuperset && !echo && bytes <= m_policy.datagramMaxBytes) {
        prepareOutboundGrowth(bytes);
        std::vector<std::uint8_t> datagram;
        datagram.reserve(bytes);
        (void)appendFrame(datagram, FrameKind::Update, attachment.handle, body);
        m_datagrams.push_back(std::move(datagram));
        m_pacer.noteDatagram(attachment.handle, newest, bytes, now);
        ++m_counters.datagramsSent;
    } else {
        enqueue(FrameKind::Update, attachment.handle, body);
        if (echo) {
            attachment.sentEchoContext = echo;
            ++m_counters.echoContextsSent;
        }
        // A datagram may be lost, so only an update on the stream reports
        // the client's input written.
        attachment.reportedInputWritten = std::max(attachment.reportedInputWritten, source.inputLedger().lastWritten(m_clientInstance).value_or(0));
        attachment.reportedInputSettled = std::max(attachment.reportedInputSettled, source.settledAtPublish(m_clientInstance));
    }
    ++(isSuperset ? m_counters.supersetUpdatesSent : m_counters.chainUpdatesSent);
    m_counters.rowsSent += source.publisher().lastUpdateRows();
    m_counters.spansOps += source.publisher().lastUpdateSpans();
    m_counters.scrollRectOps += source.publisher().lastUpdateScrollRects();
    m_pacer.noteScreenData(now);
    const Ubiquitty::Screen::HistoryAddress address = source.historyAddress();
    attachment.base->inFlight.push_back({ .generation = newest, .baseline = baseline, .history = { .epoch = address.epoch, .head = address.head, .revision = source.history().revision() } });
    attachment.base->sentGeneration = newest;
    attachment.base->sentBaseline = std::move(baseline);
    attachment.isLossPending = false;
    attachment.hasSentUpdate = true;
    attachment.lastUpdateMicroseconds = now;
    return { .bytes = bytes + historyBytes, .isRefused = false };
}

HostConnection::HistoryAppends HostConnection::historyAppends(const PaneAttachment& attachment)
{
    HistoryAppends appends;
    PaneSource& source = *attachment.source;
    HistoryPublisher& publisher = source.history();
    const Ubiquitty::Screen::HistoryAddress address = source.historyAddress();
    const HistoryState& history = attachment.history;
    appends.sentHead = std::max(history.sentHead, address.head);
    appends.sentOpenLine = history.sentOpenLine;
    // A client that keeps no history is sent none; the head it is told of
    // moves on without it.
    if (m_historyCacheLines == 0)
        return appends;

    // On a metered link only the newest viewport's worth is pushed: older
    // lines are a gap the client pulls when it scrolls to them (Docs/sync.md
    // §6.6).
    const std::uint64_t pushFloor = isMetered() && address.head > m_facts.viewportRows ? address.head - m_facts.viewportRows : 0;

    // The runs to send, ascending: lines rewritten below what was sent, then
    // the appends past it.
    std::vector<LineRange> ranges;
    for (const std::uint64_t number : publisher.rewrittenSince(history.revision, std::min(history.sentHead, address.head))) {
        if (number < pushFloor)
            continue;
        if (!ranges.empty() && ranges.back().to == number)
            ++ranges.back().to;
        else
            ranges.push_back({ number, number + 1 });
    }
    if (address.head > history.sentHead)
        ranges.push_back({ std::max({ history.sentHead, address.oldest, pushFloor }), address.head });

    constexpr std::size_t inlineHeaderBytes = 4 + 8;
    for (const LineRange& range : ranges) {
        if (range.from >= range.to)
            continue;
        std::vector<std::uint8_t> inlineBody;
        encode(inlineBody, HistoryInlineHeader { .historyEpoch = address.epoch, .firstLine = range.from });
        const std::size_t written = publisher.writeBody(source.terminal(), range, HistoryOrder::Ascending, maxFrameBodyBytes - inlineHeaderBytes, inlineBody);
        if (written == range.to - range.from) {
            appends.interactiveBytes += frameBytes(attachment.handle, inlineBody.size());
            appends.lines += written;
            appends.inlineBodies.push_back(std::move(inlineBody));
            continue;
        }
        preemptImageTransfers();
        for (std::uint64_t from = range.from; from < range.to;) {
            const LineRange part { from, std::min<std::uint64_t>(range.to, from + m_policy.history.backfillChunkLines) };
            HistoryTransferPlan plan;
            const std::size_t lines = publisher.writeBody(source.terminal(), part, HistoryOrder::Ascending, m_policy.history.maxHistoryTransferBytes, plan.body);
            if (lines == 0)
                break;
            plan.firstLine = from;
            plan.count = static_cast<std::uint16_t>(lines);
            plan.order = HistoryOrder::Ascending;
            appends.interactiveBytes += frameBytes(attachment.handle, 4 + 4 + 8 + 2 + 1);
            appends.lines += lines;
            appends.transfers.push_back(std::move(plan));
            from += lines;
        }
    }

    // The open line, when the client holds another. One it holds that
    // completed, or that a head past it retired, the client dropped itself.
    const std::optional<OpenLine> open = publisher.openLine();
    const bool isRetired = !open && history.sentOpenLine && history.sentOpenLine->number < address.head;
    if (open != history.sentOpenLine && !isRetired) {
        const std::uint64_t number = open ? open->number : address.head;
        std::vector<std::uint8_t> inlineBody;
        encode(inlineBody, HistoryInlineHeader { .historyEpoch = address.epoch, .firstLine = number });
        if (publisher.writeOpenLine(source.terminal(), maxFrameBodyBytes - inlineHeaderBytes, inlineBody)) {
            appends.interactiveBytes += frameBytes(attachment.handle, inlineBody.size());
            appends.inlineBodies.push_back(std::move(inlineBody));
            appends.sentOpenLine = open;
        } else {
            preemptImageTransfers();
            HistoryTransferPlan plan { .firstLine = number, .count = 1, .order = HistoryOrder::Ascending, .body = {}, .isOpenLine = true };
            if (publisher.writeOpenLine(source.terminal(), m_policy.history.maxHistoryTransferBytes, plan.body)) {
                appends.interactiveBytes += frameBytes(attachment.handle, 4 + 4 + 8 + 2 + 1);
                appends.transfers.push_back(std::move(plan));
                appends.sentOpenLine = open;
            }
        }
    } else if (isRetired) {
        appends.sentOpenLine.reset();
    }
    return appends;
}

void HostConnection::sendHistoryAppends(PaneAttachment& attachment, HistoryAppends appends)
{
    HistoryState& history = attachment.history;
    // An open line replaces the one in flight: the transfer still carrying
    // that one must never deliver it after this.
    if (appends.sentOpenLine != history.sentOpenLine || std::ranges::any_of(appends.transfers, &HistoryTransferPlan::isOpenLine)) {
        if (history.openLineTransfer)
            abortHistoryTransfer(*std::exchange(history.openLineTransfer, std::nullopt));
    }
    if (appends.sentOpenLine != history.sentOpenLine)
        ++m_counters.historyOpenLinesSent;
    for (const std::vector<std::uint8_t>& body : appends.inlineBodies) {
        enqueue(FrameKind::HistoryInline, attachment.handle, body);
        m_counters.historyBytesSent += frameBytes(attachment.handle, body.size());
    }
    for (HistoryTransferPlan& plan : appends.transfers) {
        const HistoryHead head { .transfer = m_scheduler.nextTransferID(), .historyEpoch = history.epoch, .firstLine = plan.firstLine, .count = plan.count, .order = plan.order };
        std::vector<std::uint8_t> headBody;
        encode(headBody, head);
        m_counters.historyBytesSent += frameBytes(attachment.handle, headBody.size()) + plan.body.size();
        (void)m_scheduler.enqueueTransfer(FrameKind::HistoryHead, attachment.handle, headBody, std::move(plan.body), TransferRank::Viewport);
        if (plan.isOpenLine)
            history.openLineTransfer = head.transfer;
        else
            history.appendTransfers.push_back(head.transfer);
    }
    m_counters.historyLinesSent += appends.lines;
    history.sentHead = appends.sentHead;
    history.sentOpenLine = appends.sentOpenLine;
    history.revision = attachment.source->history().revision();
}

bool HostConnection::isWantOwed(const PaneAttachment& attachment, HistoryPriority priority) const
{
    if (!attachment.source || attachment.needsCheckpoint || attachment.view == ViewState::RecordsOnly || (priority == HistoryPriority::Backfill && attachment.view != ViewState::Live))
        return false;
    const HistoryState& history = attachment.history;
    if (history.wants.ranges(priority).empty() || history.epoch != attachment.source->historyAddress().epoch)
        return false;
    // The client takes history of the epoch its checkpoint named.
    if (attachment.checkpointTransfer && m_scheduler.isTransferQueued(*attachment.checkpointTransfer))
        return false;
    // One transfer per priority, cut once the one before has left.
    const std::optional<WantTransfer>& inFlight = history.wantTransfers[static_cast<std::size_t>(priority)];
    if (inFlight && m_scheduler.isTransferQueued(inFlight->transfer))
        return false;
    return m_scheduler.queuedBulkBytes() < m_policy.history.maxQueuedBulkBytes;
}

bool HostConnection::isOpenLineOwed(const PaneAttachment& attachment) const
{
    if (!attachment.source || m_historyCacheLines == 0 || attachment.view != ViewState::Live || attachment.needsCheckpoint || isOwed(attachment))
        return false;
    const Ubiquitty::Screen::HistoryAddress address = attachment.source->historyAddress();
    const HistoryState& history = attachment.history;
    if (history.epoch != address.epoch || (attachment.checkpointTransfer && m_scheduler.isTransferQueued(*attachment.checkpointTransfer)))
        return false;
    const std::optional<OpenLine> open = attachment.source->history().openLine();
    // One the client holds that a head past it retired, it dropped itself.
    return open != history.sentOpenLine && (open || history.sentOpenLine->number >= address.head);
}

void HostConnection::serviceWant(PaneAttachment& attachment, HistoryPriority priority)
{
    HistoryState& history = attachment.history;
    PaneSource& source = *attachment.source;
    const std::optional<LineRange> slice = history.wants.nextSlice(priority, m_policy.history.backfillChunkLines);
    if (!slice)
        return;
    // Lines the ring evicted or a taller screen pulled back since the want
    // arrived are not history; the client asks again for what it misses.
    const Ubiquitty::Screen::HistoryAddress address = source.historyAddress();
    const LineRange lines { std::max(slice->from, address.oldest), std::min(slice->to, address.head) };
    if (lines.from >= lines.to) {
        history.wants.exclude(*slice);
        return;
    }
    // Backfill goes newest first: the lines nearest the screen are the ones
    // the user scrolls to first.
    const HistoryOrder order = priority == HistoryPriority::Backfill ? HistoryOrder::Descending : HistoryOrder::Ascending;
    preemptImageTransfers();
    std::vector<std::uint8_t> body;
    const std::size_t room = m_policy.history.maxQueuedBulkBytes - m_scheduler.queuedBulkBytes();
    const std::size_t written = source.history().writeBody(source.terminal(), lines, order, std::min(room, m_policy.history.maxHistoryTransferBytes), body);
    if (written == 0) {
        // A line too long for the room left waits for the room; one too long
        // for any transfer is never sent.
        if (room >= m_policy.history.maxHistoryTransferBytes)
            history.wants.exclude(*slice);
        return;
    }
    const LineRange sent = order == HistoryOrder::Ascending ? LineRange { lines.from, lines.from + written } : LineRange { lines.to - written, lines.to };
    const HistoryHead head { .transfer = m_scheduler.nextTransferID(), .historyEpoch = history.epoch, .firstLine = order == HistoryOrder::Ascending ? sent.from : sent.to - 1, .count = static_cast<std::uint16_t>(written), .order = order };
    std::vector<std::uint8_t> headBody;
    encode(headBody, head);
    m_counters.historyBytesSent += frameBytes(attachment.handle, headBody.size()) + body.size();
    m_counters.historyLinesSent += written;
    const TransferRank rank = priority == HistoryPriority::Viewport ? TransferRank::Viewport : priority == HistoryPriority::Search ? TransferRank::Search
                                                                                                                                   : TransferRank::Backfill;
    (void)m_scheduler.enqueueTransfer(FrameKind::HistoryHead, attachment.handle, headBody, std::move(body), rank);
    history.wantTransfers[static_cast<std::size_t>(priority)] = WantTransfer { .transfer = head.transfer, .lines = sent };
    history.wants.exclude(sent);
    history.wants.exclude({ slice->from, lines.from });
    history.wants.exclude({ lines.to, slice->to });
}

void HostConnection::setImplicitBackfill(PaneAttachment& attachment, BackfillCause cause)
{
    HistoryState& history = attachment.history;
    const Ubiquitty::Screen::HistoryAddress address = attachment.source->historyAddress();
    // Unmetered, the backfill goes as deep as the client chose; metered, it
    // is the one viewport above the screen when the epoch is first served,
    // and nothing after. Never deeper than the client keeps.
    const std::uint64_t wanted = !isMetered() ? m_backfillDepthLines : cause == BackfillCause::EpochServed ? m_facts.viewportRows
                                                                                                           : 0;
    const std::uint64_t depth = std::min<std::uint64_t>(wanted, m_historyCacheLines);
    if (isMetered()) {
        // A backfill a link that turned metered had in the lanes is dropped.
        if (std::optional<WantTransfer>& inFlight = history.wantTransfers[static_cast<std::size_t>(HistoryPriority::Backfill)]) {
            abortHistoryTransfer(inFlight->transfer);
            inFlight.reset();
        }
    }
    std::vector<LineRange> ranges;
    if (depth > 0)
        ranges.push_back({ address.head > depth ? address.head - depth : 0, address.head });
    (void)history.wants.replace(HistoryWant { .historyEpoch = address.epoch, .ranges = std::move(ranges), .priority = HistoryPriority::Backfill }, address);
}

void HostConnection::takeWant(PaneAttachment& attachment, const HistoryWant& want)
{
    // A metered client pulls what it scrolls to; backfill is the host's to
    // push, and only on an unmetered link.
    if (want.priority == HistoryPriority::Backfill && isMetered())
        return;
    HistoryState& history = attachment.history;
    const WantOutcome outcome = history.wants.replace(want, attachment.source->historyAddress());
    if (outcome == WantOutcome::RefusedOverLimit)
        ++m_counters.historyWantsRefused;
    if (outcome != WantOutcome::Accepted)
        return;
    // Latest want wins: a transfer no set wants a line of any more is
    // dropped, and one still wanted is not owed again.
    for (std::optional<WantTransfer>& inFlight : history.wantTransfers) {
        if (!inFlight || !m_scheduler.isTransferQueued(inFlight->transfer))
            continue;
        if (history.wants.intersectsAny(inFlight->lines)) {
            history.wants.exclude(inFlight->lines);
            continue;
        }
        abortHistoryTransfer(inFlight->transfer);
        inFlight.reset();
    }
}

std::vector<std::uint32_t> HostConnection::historyTransferIDs(const HistoryState& history)
{
    std::vector<std::uint32_t> ids = history.appendTransfers;
    if (history.openLineTransfer)
        ids.push_back(*history.openLineTransfer);
    for (const std::optional<WantTransfer>& inFlight : history.wantTransfers) {
        if (inFlight)
            ids.push_back(inFlight->transfer);
    }
    return ids;
}

TransferAbort HostConnection::abortTransfer(std::uint32_t transfer)
{
    const std::size_t queuedBefore = queuedInteractiveBytes();
    const TransferAbort result = m_scheduler.abortTransfer(transfer);
    if (queuedInteractiveBytes() < queuedBefore)
        m_isWindowRefused = false;
    return result;
}

void HostConnection::abortHistoryTransfer(std::uint32_t transfer)
{
    // A head that never left announced nothing, so no abort follows it.
    if (abortTransfer(transfer) != TransferAbort::Aborted)
        return;
    std::vector<std::uint8_t> body;
    encode(body, BulkAbort { .transfer = transfer });
    enqueue(FrameKind::BulkAbort, 0, body);
    ++m_counters.bulkAborts;
}

void HostConnection::resetHistory(PaneAttachment& attachment)
{
    HistoryState& history = attachment.history;
    for (const std::uint32_t transfer : historyTransferIDs(history))
        abortHistoryTransfer(transfer);
    history.appendTransfers.clear();
    history.openLineTransfer.reset();
    history.wantTransfers = {};
    PaneSource& source = *attachment.source;
    const Ubiquitty::Screen::HistoryAddress address = source.historyAddress();
    std::vector<std::uint8_t> body;
    encode(body, HistoryReset { .newEpoch = address.epoch, .clearedThrough = address.clearedThrough, .historyHead = address.head });
    enqueue(FrameKind::HistoryReset, attachment.handle, body);
    ++m_counters.historyResetsSent;
    // The lines that survived a partial clear are numbered from
    // clearedThrough in the new epoch: they go as appends.
    history.epoch = address.epoch;
    history.sentHead = address.clearedThrough;
    history.ackedHead = address.clearedThrough;
    history.revision = source.history().revision();
    // Wants name lines of the old epoch.
    history.wants.clear();
    history.sentOpenLine.reset();
}

std::vector<HostConnection::QueuedHistoryTransfer> HostConnection::queuedHistoryTransfers(std::uint32_t handle) const
{
    std::vector<QueuedHistoryTransfer> transfers;
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return transfers;
    for (const std::uint32_t id : historyTransferIDs(m_panes[handle - 1].history)) {
        if (m_scheduler.isTransferQueued(id))
            transfers.push_back({ .transfer = id, .isAnnounced = m_scheduler.isTransferAnnounced(id) });
    }
    return transfers;
}

std::size_t HostConnection::historyTransfers(std::uint32_t handle, HistoryPriority priority) const
{
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return 0;
    const std::optional<WantTransfer>& inFlight = m_panes[handle - 1].history.wantTransfers[static_cast<std::size_t>(priority)];
    return inFlight && m_scheduler.isTransferQueued(inFlight->transfer) ? 1 : 0;
}

std::vector<LineRange> HostConnection::wantedHistory(std::uint32_t handle, HistoryPriority priority) const
{
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return {};
    const std::span<const LineRange> ranges = m_panes[handle - 1].history.wants.ranges(priority);
    return { ranges.begin(), ranges.end() };
}

std::optional<std::uint64_t> HostConnection::historyAcked(std::uint32_t handle) const
{
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return std::nullopt;
    return m_panes[handle - 1].history.ackedHead;
}

AttachmentBaseline HostConnection::supersetBaseline(const PaneAttachment& attachment) const
{
    AttachmentBaseline baseline = attachment.base->acknowledgedBaseline;
    const ScreenPublisher& publisher = attachment.source->publisher();
    // Any value other than the current hash makes the encoder carry what it
    // names.
    const auto settle = [&](std::uint64_t& held, std::uint64_t current, auto&& inFlightHolds) {
        bool isSettled = held == current;
        for (const SentGeneration& entry : attachment.base->inFlight)
            isSettled = isSettled && inFlightHolds(entry.baseline);
        held = isSettled ? current : ~current;
    };
    for (std::size_t screen = 0; screen < 2; ++screen) {
        const RowEncodingCache& rows = publisher.rows(static_cast<int>(screen));
        if (static_cast<int>(baseline.rowHashes[screen].size()) != rows.rowCount())
            return baseline;
        for (std::size_t row = 0; row < baseline.rowHashes[screen].size(); ++row) {
            const std::uint64_t current = rows.hash(static_cast<int>(row));
            settle(baseline.rowHashes[screen][row], current, [&](const AttachmentBaseline& sent) {
                return isSameShape(sent, attachment.base->acknowledgedBaseline) && sent.rowHashes[screen][row] == current;
            });
        }
    }
    for (std::size_t block = 0; block < stateBlockCount; ++block) {
        const std::uint64_t current = publisher.blockHashes()[block];
        settle(baseline.blockHashes[block], current, [&](const AttachmentBaseline& sent) {
            return sent.isAlternate == attachment.base->acknowledgedBaseline.isAlternate && sent.blockHashes[block] == current;
        });
    }
    return baseline;
}

void HostConnection::abortCheckpoint(PaneAttachment& attachment)
{
    if (!attachment.checkpointTransfer || !m_scheduler.isTransferQueued(*attachment.checkpointTransfer))
        return;
    const std::uint32_t transfer = *std::exchange(attachment.checkpointTransfer, std::nullopt);
    // A head that never left announced nothing, so it leaves with its body
    // and no abort follows.
    if (abortTransfer(transfer) == TransferAbort::Aborted) {
        std::vector<std::uint8_t> body;
        encode(body, BulkAbort { .transfer = transfer });
        enqueue(FrameKind::BulkAbort, 0, body);
        ++m_counters.bulkAborts;
    }
    attachment.needsCheckpoint = true;
    attachment.unacknowledgedCheckpoint.reset();
    attachment.base->inFlight.clear();
}

void HostConnection::serviceOversizedPanes(std::uint64_t now)
{
    for (OversizedPane& pane : m_oversizedPanes) {
        if (pane.isRebound || now < pane.retryAt || pane.source->generation() <= pane.refusedGeneration || !pane.source->isFrameHeld())
            continue;
        std::vector<std::uint8_t> body;
        AttachmentBaseline measured;
        if (!pane.source->publisher().checkpoint(pane.source->terminal(), pane.source->generation(), body, measured))
            continue;
        ++m_counters.oversizedPanesMeasured;
        if (body.size() > m_policy.maxCheckpointBytes) {
            pane.refusedGeneration = pane.source->generation();
            ++pane.refusals;
            const std::uint32_t doublings = std::min<std::uint32_t>(pane.refusals - 1, 16);
            pane.retryAt = now + std::min(oversizedRetryMicroseconds << doublings, maxOversizedRetryMicroseconds);
            continue;
        }
        const std::uint32_t handle = bindPane(*pane.source);
        if (!handle)
            continue;
        m_paneRebinds.push_back({ .previousHandle = pane.handle, .handle = handle });
        pane.handle = handle;
        pane.isRebound = true;
        ++m_counters.oversizedPanesRebound;
    }
}

void HostConnection::sendCheckpoint(PaneAttachment& attachment, std::uint64_t now)
{
    // A checkpoint resets the client's picture; the echo context goes again
    // with the next update.
    attachment.sentEchoContext.reset();
    // Bulk is encoded only as it is about to leave: one checkpoint body is in
    // the lanes at a time, whichever pane owes it, so however many panes owe
    // one the connection holds one. A pane passed over stays owed.
    if (isCheckpointInLanes())
        return;
    preemptImageTransfers();
    PaneSource& source = *attachment.source;
    std::vector<std::uint8_t> body;
    AttachmentBaseline baseline;
    if (!source.publisher().checkpoint(source.terminal(), source.generation(), body, baseline))
        return;
    // A body past the cap could never be sent, and re-encoding it each time
    // the pane is owed one would be a silent loop: the binding ends, typed,
    // and the client tells the person why. The caller touches the attachment
    // no further.
    if (body.size() > m_policy.maxCheckpointBytes) {
        ++m_counters.checkpointsTooLarge;
        // The pane comes back once a later frame fits; a pane refused again
        // straight after a rebind waits longer each time.
        const auto previous = std::ranges::find(m_oversizedPanes, &source, &OversizedPane::source);
        const std::uint32_t refusals = previous == m_oversizedPanes.end() ? 1 : previous->refusals + 1;
        if (previous != m_oversizedPanes.end())
            m_oversizedPanes.erase(previous);
        const std::uint32_t doublings = std::min<std::uint32_t>(refusals - 1, 16);
        m_oversizedPanes.push_back({ .source = &source,
            .handle = attachment.handle,
            .refusedGeneration = source.generation(),
            .retryAt = now + std::min(oversizedRetryMicroseconds << doublings, maxOversizedRetryMicroseconds),
            .refusals = refusals,
            .isRebound = false });
        unbindPane(attachment.handle, std::to_underlying(PaneUnbindReason::CheckpointTooLarge));
        return;
    }
    const Digest128 stateDigest = trailingDigest(body);
    // The client refused this state twice and would again; the pane waits for
    // its next generation, so a body the client cannot take is not a loop.
    if (attachment.refusalsOfDigest >= 2 && attachment.refusedCheckpointDigest && *attachment.refusedCheckpointDigest == stateDigest) {
        attachment.withheldGeneration = source.generation();
        attachment.needsCheckpoint = true;
        ++m_counters.refusedCheckpointsWithheld;
        return;
    }
    attachment.sentCheckpointDigest = stateDigest;
    attachment.withheldGeneration.reset();
    m_counters.lastCheckpointBodyBytes = body.size();
    // The checkpoint names the history's epoch and head; the lines below the
    // head follow as backfill, newest first (Docs/sync.md §6.6).
    // A checkpoint in an epoch the attachment was already served leaves its
    // history as it was: the client keeps its cache across a resize, and the
    // backfill carries on where it stood.
    if (isResetOwed(attachment))
        resetHistory(attachment);
    const Ubiquitty::Screen::HistoryAddress address = source.historyAddress();
    HistoryState& history = attachment.history;
    if (history.epoch != address.epoch) {
        history.epoch = address.epoch;
        history.sentHead = address.head;
        history.ackedHead = 0;
        history.revision = source.history().revision();
        // A client sent its first checkpoint of an epoch holds no open line;
        // the one there is follows the checkpoint.
        history.sentOpenLine.reset();
        history.wants.clear();
        setImplicitBackfill(attachment, BackfillCause::EpochServed);
    } else {
        // Lines that left the screen since the attachment was last sent its
        // appends precede the checkpoint that names the head past them.
        sendHistoryAppends(attachment, historyAppends(attachment));
    }
    CheckpointHead head { .generation = source.generation(), .transfer = m_scheduler.nextTransferID(), .frameClass = source.frameClass(), .stateDigest = stateDigest };
    std::vector<std::uint8_t> headBody;
    encode(headBody, head);
    attachment.checkpointTransfer = head.transfer;
    // A body above 4 KiB crosses deflated when the client takes deflate and
    // the stream is the smaller (Docs/sync.md §7.4); an editor client that
    // takes none is sent it as it is.
    PayloadCoding coding;
    if (body.size() > minDeflatedCheckpointBytes && m_capabilities.contains(ClientCapability::DeflateCheckpoints) && body.size() <= std::numeric_limits<std::uint32_t>::max()) {
        std::vector<std::uint8_t> deflated = Codec::Zlib::compress(body);
        if (deflated.size() < body.size()) {
            coding = { .compression = Compression::Deflate, .inflatedBytes = static_cast<std::uint32_t>(body.size()) };
            body = std::move(deflated);
            ++m_counters.checkpointsDeflated;
        }
    }
    m_counters.lastCheckpointWireBytes = body.size();
    (void)m_scheduler.enqueueTransfer(FrameKind::CheckpointHead, attachment.handle, headBody, std::move(body), TransferRank::Checkpoint, FrameRole::Message, coding);
    ++m_counters.checkpointsSent;
    attachment.needsCheckpoint = false;
    attachment.unacknowledgedCheckpoint = source.generation();
    attachment.base->sentGeneration = source.generation();
    attachment.base->inFlight.clear();
    attachment.base->inFlight.push_back({ .generation = source.generation(), .baseline = baseline, .history = { .epoch = address.epoch, .head = address.head, .revision = source.history().revision() } });
    attachment.base->sentBaseline = std::move(baseline);
    attachment.isLossPending = false;
    attachment.hasSentUpdate = true;
    attachment.lastUpdateMicroseconds = now;
}

std::optional<Lane> HostConnection::popStreamFrame(std::vector<std::uint8_t>& output, std::uint64_t now)
{
    if (m_state == ConnectionState::Attached && m_table && m_record
        && (m_record->state != AttachmentState::Live || m_record->servingConnection != m_connection))
        supersede();
    // Bulk is encoded as the lanes take it: once one checkpoint body has
    // left, the next a pane asked for is cut, so a transport that takes
    // several frames at once takes them back to back.
    if (m_state == ConnectionState::Attached && !isCheckpointInLanes()) {
        for (PaneAttachment& attachment : m_panes) {
            if (attachment.source && attachment.view == ViewState::Live && attachment.needsCheckpoint && attachment.source->isFrameHeld() && !isCheckpointWithheld(attachment)) {
                sendCheckpoint(attachment, now);
                break;
            }
        }
    }
    const std::size_t before = output.size();
    const auto queued = queuedInteractiveBytes();
    const auto outstanding = m_pacer.outstandingBytes();
    std::size_t payloadLimit = maxFrameBodyBytes;
    if (outstanding || queued) {
        // Small interactive updates must not starve a chunk larger than the
        // initial window. Cut to its remaining credit instead of enlarging
        // that window or requiring a completely idle screen.
        const auto window = m_pacer.windowBytes(now);
        const auto room = outstanding < window && queued < window - outstanding ? window - outstanding - queued : 0;
        constexpr auto overhead = maxEnvelopeHeaderBytes + bulkChunkOverheadBytes;
        payloadLimit = room > overhead ? static_cast<std::size_t>(std::min<std::uint64_t>(maxFrameBodyBytes, room - overhead)) : 0;
    }
    const std::optional<Lane> lane = m_scheduler.popFrame(output, m_pacer.admitsBulk(m_scheduler.nextBulkFrameBytes(payloadLimit), now, queued) ? BulkPacing::Admitted : BulkPacing::Held, payloadLimit);
    if (lane == Lane::Bulk) {
        m_pacer.noteBulkStreamBytes(output.size() - before);
        m_pacer.noteScreenData(now);
    } else if (lane == Lane::Interactive)
        m_pacer.noteStreamBytes(output.size() - before);
    if (lane)
        noteStreamFramePopped(output, before);
    if (!m_unheardSince && isAwaitingClient())
        m_unheardSince = now;
    return lane;
}

std::vector<std::vector<std::uint8_t>> HostConnection::takeDatagrams()
{
    if (standingKind(m_clientKind, m_identity.verifiedKind) == ClientKind::Agent) {
        for (const auto& datagram : m_datagrams) {
            if (datagram.empty() || datagram.front() != std::to_underlying(FrameKind::Update))
                continue;
            EnvelopeError error = EnvelopeError::None;
            const auto frame = decodeDatagram(datagram, error);
            if (PaneAttachment* attachment = frame ? pane(frame->channel) : nullptr)
                attachment->hasDisclosedScreen = true;
        }
    }
    return std::exchange(m_datagrams, {});
}

void HostConnection::enqueue(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body, FrameRole role)
{
    prepareOutboundGrowth(frameBytes(channel, body.size()));
    (void)m_scheduler.enqueue(kind, channel, body, role);
}

void HostConnection::close(ByeReason reason, std::string_view detail)
{
    close(spokenBye(m_identity.wireVersion, m_identity.appVersion, m_identity.selection.hostPlatform, reason, detail));
}

void HostConnection::closeDiscardingQueue(ByeReason reason, std::string_view detail)
{
    m_scheduler.abandonAll();
    m_datagrams.clear();
    close(reason, detail);
}

void HostConnection::close(Bye refusal)
{
    // A closed connection reads no Ack, so a chunk the window holds would
    // never leave, and the owner reaps a closed connection only once its
    // queue is empty. The Bye is all that is left to send.
    m_scheduler.abandonTransfers();
    discardInboundClipboard();
    releaseDriverFocus();
    // A sender that broke the protocol has what it queued unwritten dropped;
    // one that left or lost its link keeps what was accepted from it.
    if (isViolation(refusal.reason)) {
        for (PaneAttachment& attachment : m_panes) {
            if (attachment.source)
                attachment.source->withdrawInputAuthority(attachmentIdentity(), m_lastReceiveNow);
        }
    }
    if (!m_table) {
        for (PaneAttachment& attachment : m_panes) {
            if (attachment.source)
                (void)attachment.source->effects().noteState(attachmentIdentity(), AttachmentState::Detached, 0);
        }
    }
    std::vector<std::uint8_t> body;
    encode(body, refusal);
    enqueue(FrameKind::Bye, 0, body);
    m_byeReason = refusal.reason;
    m_state = ConnectionState::Closed;
}

void HostConnection::discardInboundClipboard()
{
    for (auto& request : m_clipboardReads)
        request.reset();
}

bool HostConnection::admitsCatalogCapacity(std::size_t additional) const
{
    // Bound typed clipboard owners even on a carrier without a daemon ledger;
    // the daemon may impose a tighter aggregate bound across its clients.
    const std::size_t held = inboundClipboardBytes();
    return held <= maxCatalogMessageBytes && additional <= maxCatalogMessageBytes - held
        && (!m_inboundAssemblyAdmission || m_inboundAssemblyAdmission(additional));
}

namespace {

/// A MIME inventory carries no contents; an item answer names only its request.
bool matchesClipboardRead(const ClipboardRead& request, const EffectClipboardBundle& bundle)
{
    if (request.kind == EffectClipboardRequestKind::Item)
        return bundle.count == 1 && mimeNamesSameType(bundle.items[0].mime, request.mime);
    return std::ranges::all_of(std::span(bundle.items).first(bundle.count), [](const auto& item) { return item.data.empty(); });
}

std::size_t clipboardReadBytes(const std::array<std::optional<ClipboardReadState>, maxPendingClipboardReads>& requests)
{
    std::size_t result = 0;
    for (const auto& request : requests) {
        if (request)
            result += request->request.size + request->data.size;
    }
    return result;
}

} // namespace

std::size_t HostConnection::inboundClipboardBytes() const
{
    return clipboardReadBytes(m_clipboardReads) + m_clipboardDeliveryBytes;
}

bool HostConnection::requestRichText(std::uint32_t handle, const RichText::MeasureRequest& request, std::uint64_t now, std::uint64_t waitMicroseconds)
{
    const auto facts = typography(handle);
    if (!pane(handle) || !facts || (request.fontCatalog ? facts->fontCatalogScope == RichText::FontCatalogScope::Unavailable : !facts->shaping) || !request.token || !waitMicroseconds
        || (request.fontCatalog && (!request.fontCatalog->count || request.fontCatalog->count > RichText::maxFontCatalogPageFamilies))
        || waitMicroseconds > RichText::maxMeasurementWaitMilliseconds * 1000ULL
        || now > std::numeric_limits<std::uint64_t>::max() - waitMicroseconds
        || std::ranges::any_of(m_pendingRichText, [&](const auto& held) { return held.handle == handle && held.token == request.token; })
        || std::ranges::any_of(m_richTextReplies, [&](const auto& held) { return held.handle == handle && held.token == request.token; })
        || m_pendingRichText.size() + m_richTextReplies.size() >= RichText::maxPendingMeasurements)
        return false;
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writeMeasureRequest(writer, request, facts->rendererRevision);
    if (body.size() > maxFrameBodyBytes || !admitsCatalogCapacity(body.size()))
        return false;
    auto catalog = request.fontCatalog;
    if (catalog)
        catalog->rendererRevision = facts->rendererRevision;
    m_pendingRichText.push_back({ handle, request.token, request.document.text.size(), now + waitMicroseconds, facts->rendererRevision, catalog, facts->fontCatalogScope });
    enqueue(FrameKind::RichTextMeasure, handle, body);
    return true;
}

void HostConnection::cancelRichText(std::uint32_t handle, std::uint32_t token)
{
    std::erase_if(m_pendingRichText, [&](const auto& held) { return held.handle == handle && held.token == token; });
    std::erase_if(m_richTextReplies, [&](const auto& held) { return held.handle == handle && held.token == token; });
}

void HostConnection::retainRichTextRequests(std::uint32_t handle, std::span<const std::uint32_t> tokens)
{
    const auto isRetired = [&](const auto& held) {
        return held.handle == handle && std::ranges::find(tokens, held.token) == tokens.end();
    };
    std::erase_if(m_pendingRichText, isRetired);
    std::erase_if(m_richTextReplies, isRetired);
}

bool HostConnection::requestClipboard(std::uint32_t handle, const ClipboardRead& request)
{
    if (m_state != ConnectionState::Attached || !pane(handle)
        || (m_grantedRole != Role::Owner && m_grantedRole != Role::Driver)
        || std::ranges::any_of(m_clipboardReads, [&](const auto& held) { return held && held->requestID == request.requestID; }))
        return false;
    const auto slot = std::ranges::find_if(m_clipboardReads, [](const auto& held) { return !held; });
    if (slot == m_clipboardReads.end() || !admitsCatalogCapacity(maxFrameBodyBytes))
        return false;
    auto encoded = encodeClipboardRead(request);
    if (!encoded)
        return false;
    slot->emplace();
    (*slot)->handle = handle;
    (*slot)->requestID = request.requestID;
    (*slot)->request = std::move(*encoded);
    enqueue(FrameKind::ClipboardRequest, handle, (*slot)->request.view());
    return true;
}

bool HostConnection::sendNativeDragCommand(std::uint32_t handle, std::uint64_t epoch, const DragDrop::Command& command)
{
    if (m_state != ConnectionState::Attached || !pane(handle) || m_clientKind != ClientKind::Human || !mayDrive() || !supportsNativeDrag()
        || (m_grantedRole != Role::Owner && m_grantedRole != Role::Driver)
        || queuedBulkBytes() + command.payload.size() + 128 > m_policy.limits.maxQueuedBulkBytes)
        return false;
    std::vector<std::uint8_t> bytes;
    if (!encodeNativeDragCommand(bytes, epoch, command))
        return false;
    prepareOutboundGrowth(frameBytes(handle, bytes.size()));
    return m_scheduler.enqueue(FrameKind::NativeDragCommand, handle, bytes) != 0;
}

void HostConnection::cancelClipboardRequest(std::uint32_t requestID)
{
    for (auto& request : m_clipboardReads) {
        if (request && request->requestID == requestID)
            request.reset();
    }
}

void HostConnection::receiveClipboardData(std::uint32_t handle, std::span<const std::uint8_t> body)
{
    const auto answer = decodeClipboardData(body);
    const auto slot = std::ranges::find_if(m_clipboardReads, [&](const auto& held) {
        return answer && held && held->handle == handle && held->requestID == answer->requestID && !held->hasAnswer;
    });
    if (!answer || slot == m_clipboardReads.end() || !pane(handle)
        || (m_clipboardReplyAdmission && !m_clipboardReplyAdmission(handle, answer->requestID))) {
        close(ByeReason::Malformed, "a clipboard answer without its authorized request");
        return;
    }
    ClipboardReadState& held = **slot;
    if (answer->status == EffectClipboardStatus::OK) {
        if (answer->payload.transfer && (isClipboardTransferInUse(answer->payload.transfer) || (m_isControlUploadReserved && m_isControlUploadReserved(answer->payload.transfer)))) {
            close(ByeReason::Malformed, "a clipboard transfer ID reused before completion");
            return;
        }
        const auto request = decodeClipboardRead(held.request.view());
        if (!answer->payload.transfer) {
            const auto bundle = decodeEffectClipboard(answer->payload.inlineBytes, maxEffectClipboardReadBytes);
            if (!bundle || !request || !matchesClipboardRead(*request, *bundle)) {
                close(ByeReason::Malformed, "a clipboard answer for a different operation");
                return;
            }
        }
        if (!admitsCatalogCapacity(answer->payload.encodedBytes)) {
            close(ByeReason::TransferRefused, "no room for a clipboard answer");
            return;
        }
        std::unique_ptr<std::uint8_t[]> bytes(new (std::nothrow) std::uint8_t[answer->payload.encodedBytes]);
        if (!bytes) {
            close(ByeReason::TransferRefused, "clipboard answer allocation refused");
            return;
        }
        held.data = { .bytes = std::move(bytes), .size = answer->payload.encodedBytes };
        held.transfer = answer->payload.transfer;
        if (!held.transfer)
            std::copy(answer->payload.inlineBytes.begin(), answer->payload.inlineBytes.end(), held.data.bytes.get());
    }
    held.status = answer->status;
    held.hasAnswer = true;
    held.isComplete = held.transfer == 0;
}

bool HostConnection::isClipboardTransferInUse(std::uint32_t transfer) const
{
    return transfer && std::ranges::any_of(m_clipboardReads, [transfer](const auto& held) {
        return held && held->hasAnswer && held->transfer == transfer;
    });
}

void HostConnection::receiveClipboardReplyChunk(const BulkChunk& chunk)
{
    const auto slot = std::ranges::find_if(m_clipboardReads, [&](const auto& held) {
        return held && held->hasAnswer && !held->isComplete && held->transfer == chunk.transfer;
    });
    if (slot == m_clipboardReads.end()) {
        close(ByeReason::Malformed, "a client bulk chunk without its clipboard request");
        return;
    }
    ClipboardReadState& held = **slot;
    if (!pane(held.handle) || (m_clipboardReplyAdmission && !m_clipboardReplyAdmission(held.handle, held.requestID))) {
        close(ByeReason::Malformed, "clipboard request authority was withdrawn");
        return;
    }
    if (chunk.index != held.nextIndex || chunk.bytes.empty() || chunk.bytes.size() > bulkChunkPayloadBytes
        || chunk.compression != Compression::None || chunk.inflatedBytes
        || chunk.bytes.size() > held.data.size - held.offset) {
        close(ByeReason::Malformed, "an invalid clipboard answer chunk");
        return;
    }
    std::copy(chunk.bytes.begin(), chunk.bytes.end(), held.data.bytes.get() + held.offset);
    held.offset += chunk.bytes.size();
    ++held.nextIndex;
    if (!chunk.isLast) {
        if (held.offset == held.data.size)
            close(ByeReason::Malformed, "clipboard bytes ended before their final marker");
        return;
    }
    if (held.offset != held.data.size) {
        close(ByeReason::Malformed, "an incomplete final clipboard answer");
        return;
    }
    std::array<std::uint8_t, 32> digest;
    Crypto::sha256(held.data.view(), digest);
    if (digest != chunk.sha256) {
        close(ByeReason::Malformed, "an invalid complete clipboard answer");
        return;
    }
    const auto request = decodeClipboardRead(held.request.view());
    const auto bundle = decodeEffectClipboard(held.data.view(), maxEffectClipboardReadBytes);
    if (!request || !bundle || !matchesClipboardRead(*request, *bundle)) {
        close(ByeReason::Malformed, "an invalid complete clipboard answer");
        return;
    }
    held.isComplete = true;
}

bool HostConnection::drainClipboardReplies(const std::function<bool(std::uint32_t, const ClipboardRead&, EffectClipboardStatus, const EffectClipboardBundle&)>& deliver)
{
    if (m_isDrainingClipboard)
        return false;
    m_isDrainingClipboard = true;
    for (auto& slot : m_clipboardReads) {
        if (!slot || !slot->isComplete)
            continue;
        ClipboardReadState held = std::move(*slot);
        slot.reset();
        const std::size_t capacity = held.request.size + held.data.size;
        m_clipboardDeliveryBytes += capacity;
        const auto request = decodeClipboardRead(held.request.view());
        const auto bundle = held.status == EffectClipboardStatus::OK
            ? decodeEffectClipboard(held.data.view(), maxEffectClipboardReadBytes)
            : std::expected<EffectClipboardBundle, EffectCodecError>(EffectClipboardBundle {});
        const bool isAdmitted = pane(held.handle) && (!m_clipboardReplyAdmission || m_clipboardReplyAdmission(held.handle, held.requestID));
        const bool shouldContinue = isAdmitted && request && bundle && deliver(held.handle, *request, held.status, *bundle);
        held = {};
        m_clipboardDeliveryBytes -= capacity;
        if (!shouldContinue || m_state != ConnectionState::Attached) {
            m_isDrainingClipboard = false;
            return false;
        }
    }
    m_isDrainingClipboard = false;
    return true;
}

bool HostConnection::popInteractiveFrame(std::vector<std::uint8_t>& output)
{
    if (m_scheduler.interactiveCount() == 0)
        return false;
    const std::size_t before = output.size();
    if (m_scheduler.popFrame(output, BulkPacing::Held) != Lane::Interactive)
        return false;
    m_pacer.noteStreamBytes(output.size() - before);
    noteStreamFramePopped(output, before);
    return true;
}

HostConnection::PaneAttachment* HostConnection::pane(std::uint32_t handle)
{
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return nullptr;
    return &m_panes[handle - 1];
}

bool HostConnection::isUnbound(std::uint32_t handle) const
{
    return handle != 0 && handle <= m_panes.size() && !m_panes[handle - 1].source;
}

// MARK: - ClientConnection

namespace {

/// The scrollback of a replica the client owns: as deep as its cache.
int replicaScrollbackLines(const ClientPolicy& policy)
{
    return static_cast<int>(std::min<std::uint32_t>(policy.historyCacheLines, std::numeric_limits<int>::max()));
}

} // namespace

ClientPolicy measuredClientPolicy(std::uint32_t historyCacheLines)
{
    return {
        // The host's cap (Docs/sync-measurements.md, "Memory admission"): the
        // client takes every body the host may send and nothing larger.
        .maxCheckpointBytes = defaultMaxCheckpointBytes,
        // Overflow costs one Resync, never the connection (Docs/sync.md §6.5).
        .maxBufferedUpdateBytes = std::size_t { 1 } << 20,
        // The hold the shaped-link measurement (test_sync_shaped_link) ran
        // with; the host's constants were chosen against it.
        .minAckIntervalMicroseconds = 4'000,
        .historyCacheLines = historyCacheLines,
    };
}

ClientConnection::ClientConnection(const ClientPolicy& policy, ClientIdentity identity, ClientPaneDelegate* delegate, HistoryPurgeSink* purgeSink, ReplicaSet* replicas, EffectReceiptSink* effectSink, ClientWelcomeSink* welcomeSink)
    : m_policy(policy)
    , m_identity(std::move(identity))
    , m_delegate(delegate)
    , m_purgeSink(purgeSink)
    , m_effectSink(effectSink)
    , m_welcomeSink(welcomeSink)
    , m_replicas(replicas)
    , m_receiveMemory(std::make_shared<MemoryBudget>(policy.maxReceiveBytes))
    , m_reassembler(m_receiveMemory)
    , m_installedImageCharge(m_receiveMemory)
    , m_catalogCharge(m_receiveMemory)
{
    m_distinctTerminals.reserve(maxPanesPerConnection);
}

ClientConnection::~ClientConnection()
{
    storeReplicas();
}

void ClientConnection::storeReplicas()
{
    if (!m_replicas)
        return;
    for (const PaneReplica& pane : m_panes) {
        ReplicaSet::Held* held = m_replicas->find(pane.paneID);
        if (!held || held->picture() != pane.terminal)
            continue;
        held->replica = pane.replica;
        held->hasCheckpoint = pane.hasCheckpoint;
        held->screenStatus = pane.screenStatus;
    }
}

void ClientConnection::abandon()
{
    for (auto& request : m_clipboardReads)
        request.reset();
    storeReplicas();
    m_replicas = nullptr;
    for (const auto& replica : m_panes) {
        abandonImages(replica.handle);
        abandonEffects(replica.handle);
    }
    m_state = ConnectionState::Closed;
}

std::size_t retainedPictureBytes(const Ubiquitty::Terminal& terminal)
{
    std::size_t bytes = 0;
    if (terminal.grid) {
        if (const auto backing = terminal.grid->backingStoragePlan(terminal.rows, terminal.columns))
            bytes += backing->retainedBytes;
    }
    if (terminal.lineStore) {
        if (const auto scrollback = terminal.lineStore->ownedBackingBytes())
            bytes += *scrollback;
    }
    return bytes;
}

std::size_t ReplicaSet::retainedBytes() const
{
    std::vector<const Ubiquitty::Terminal*> pictures;
    pictures.reserve(m_held.size() + m_ended.size());
    for (const auto& [paneID, held] : m_held) {
        if (held && held->picture())
            pictures.push_back(held->picture());
    }
    for (const Ended& ended : m_ended) {
        if (ended.held && ended.held->picture())
            pictures.push_back(ended.held->picture());
    }
    std::ranges::sort(pictures);
    const auto [first, last] = std::ranges::unique(pictures);
    pictures.erase(first, last);
    std::size_t bytes = 0;
    for (const Ubiquitty::Terminal* picture : pictures)
        bytes += retainedPictureBytes(*picture);
    for (const auto& history : m_history) {
        if (history)
            bytes += history->retainedStorageBytes();
    }
    return bytes;
}

ReplicaSet::Held* ReplicaSet::find(ID128 paneID)
{
    const auto found = std::ranges::find(m_held, paneID, &std::pair<ID128, std::unique_ptr<Held>>::first);
    return found == m_held.end() ? nullptr : found->second.get();
}

const ReplicaSet::Held* ReplicaSet::find(ID128 paneID) const
{
    const auto found = std::ranges::find(m_held, paneID, &std::pair<ID128, std::unique_ptr<Held>>::first);
    return found == m_held.end() ? nullptr : found->second.get();
}

EffectReceiptState ReplicaSet::effectReceipt(ID128 paneID) const
{
    const auto found = std::ranges::find(m_effectReceipts, paneID, &std::pair<ID128, EffectReceiptState>::first);
    return found == m_effectReceipts.end() ? EffectReceiptState {} : found->second;
}

bool ReplicaSet::keepEffectReceipt(ID128 paneID, EffectReceiptState state)
{
    if (paneID == ID128 {} || state.consumed < state.acknowledged)
        return false;
    const auto found = std::ranges::find(m_effectReceipts, paneID, &std::pair<ID128, EffectReceiptState>::first);
    if (found != m_effectReceipts.end()) {
        if (state.acknowledged < found->second.acknowledged || state.consumed < found->second.consumed)
            return false;
        found->second = state;
        return true;
    }
    if (m_effectReceipts.size() == maxPanesPerConnection)
        return false;
    m_effectReceipts.emplace_back(paneID, state);
    return true;
}

void ReplicaSet::forgetEffectReceipt(ID128 paneID)
{
    std::erase_if(m_effectReceipts, [&](const auto& entry) { return entry.first == paneID; });
}

std::shared_ptr<HistoryCache> ReplicaSet::history(ID128 paneID) const
{
    const auto found = std::ranges::find_if(m_history, [&](const auto& cache) { return cache->key().paneID == paneID; });
    return found == m_history.end() ? nullptr : *found;
}

bool ReplicaSet::keepHistory(std::shared_ptr<HistoryCache> cache)
{
    if (!cache || !m_hostInstanceID || cache->key().hostInstanceID != *m_hostInstanceID || cache->key().paneID == ID128 {})
        return false;
    const auto found = std::ranges::find_if(m_history, [&](const auto& held) { return held->key().paneID == cache->key().paneID; });
    if (found != m_history.end()) {
        *found = std::move(cache);
        return true;
    }
    if (m_history.size() == maxPanesPerConnection)
        return false;
    m_history.push_back(std::move(cache));
    return true;
}

void ReplicaSet::forgetHistory(ID128 paneID)
{
    std::erase_if(m_history, [&](const auto& cache) { return cache->key().paneID == paneID; });
}

ReplicaSet::Held& ReplicaSet::hold(ID128 paneID, std::unique_ptr<Ubiquitty::Terminal> terminal)
{
    forget(paneID);
    m_held.emplace_back(paneID, std::make_unique<Held>(Held { .terminal = std::move(terminal), .replica = {}, .hasCheckpoint = false, .lentTerminal = nullptr, .screenStatus = std::nullopt }));
    return *m_held.back().second;
}

ReplicaSet::Held& ReplicaSet::holdLent(ID128 paneID, Ubiquitty::Terminal& terminal)
{
    forget(paneID);
    m_held.emplace_back(paneID, std::make_unique<Held>(Held { .terminal = nullptr, .replica = {}, .hasCheckpoint = false, .lentTerminal = &terminal, .screenStatus = std::nullopt }));
    return *m_held.back().second;
}

const Ubiquitty::Terminal* ReplicaSet::terminal(ID128 paneID) const
{
    const auto found = std::ranges::find(m_held, paneID, &std::pair<ID128, std::unique_ptr<Held>>::first);
    return found == m_held.end() ? nullptr : found->second->picture();
}

void ReplicaSet::forget(ID128 paneID)
{
    std::erase_if(m_held, [&](const auto& entry) { return entry.first == paneID; });
}

void ReplicaSet::forgetLent(const Ubiquitty::Terminal& terminal)
{
    std::erase_if(m_held, [&](const auto& entry) { return entry.second->lentTerminal == &terminal; });
}

bool ReplicaSet::noteHostInstance(ID128 instance, std::optional<ID128> predecessor)
{
    const auto continuity = m_hostInstanceID ? continuityOf(*m_hostInstanceID, instance, predecessor) : InstanceContinuity::Same;
    if (continuity == InstanceContinuity::HandedOver)
        std::erase_if(m_history, [&](const auto& cache) { return !cache->rekeyForHandover(*m_hostInstanceID, instance); });
    else if (continuity == InstanceContinuity::Restarted)
        m_history.clear();
    // The replicas end under the instance they came from, before it changes.
    const bool hasRestarted = continuity == InstanceContinuity::Restarted;
    while (hasRestarted && !m_held.empty())
        end(m_held.front().first, EndReason::HostRestarted);
    if (hasRestarted)
        m_effectReceipts.clear();
    m_hostInstanceID = instance;
    return hasRestarted;
}

void ReplicaSet::end(ID128 paneID, EndReason reason)
{
    forgetEffectReceipt(paneID);
    forgetHistory(paneID);
    const auto found = std::ranges::find(m_held, paneID, &std::pair<ID128, std::unique_ptr<Held>>::first);
    if (found == m_held.end())
        return;
    std::unique_ptr<Held> held = std::move(found->second);
    m_held.erase(found);
    // An ended pane is kept to be read, perhaps long after: a terminal that
    // was only lent is the client's to keep or free, and is not kept here.
    held->lentTerminal = nullptr;
    if (m_ended.size() >= maxEnded)
        m_ended.erase(m_ended.begin());
    m_ended.push_back({ .hostInstanceID = m_hostInstanceID.value_or(ID128 {}), .paneID = paneID, .held = std::move(held), .reason = reason, .discardedInputEvents = 0 });
}

ReplicaSet::Ended* ReplicaSet::findEnded(ID128 hostInstanceID, ID128 paneID)
{
    const auto found = std::ranges::find_if(m_ended.rbegin(), m_ended.rend(), [&](const Ended& ended) { return ended.hostInstanceID == hostInstanceID && ended.paneID == paneID; });
    return found == m_ended.rend() ? nullptr : &*found;
}

std::shared_ptr<const RichText::TypographyFacts> HostConnection::typography(std::uint32_t handle) const
{
    if (handle == 0 || handle > m_panes.size() || !m_panes[handle - 1].source)
        return nullptr;
    return m_panes[handle - 1].typography;
}

void ClientConnection::publishTypography(std::uint32_t handle, const RichText::TypographyFacts& facts)
{
    auto* replica = pane(handle);
    if (!replica || m_state != ConnectionState::Attached || !facts.rendererRevision || !RichText::isAdoptable(facts)
        || (replica->publishedTypography && *replica->publishedTypography == facts))
        return;
    replica->publishedTypography = facts;
    std::vector<std::uint8_t> body;
    encode(body, facts);
    send(FrameKind::TypographyFacts, handle, body);
}

void ClientConnection::publishProportionalFaces(const OSC::TextSizing::ProportionalFaceFacts& faces)
{
    if (!OSC::TextSizing::isAdoptable(faces) || (m_proportionalFaces && *m_proportionalFaces == faces))
        return;
    auto held = std::unique_ptr<OSC::TextSizing::ProportionalFaceFacts>(new (std::nothrow) OSC::TextSizing::ProportionalFaceFacts(faces));
    if (!held)
        return;
    m_proportionalFaces = std::move(held);
    m_areProportionalFacesSent = false;
    sendProportionalFaces();
}

void ClientConnection::sendProportionalFaces()
{
    if (!m_policy.capabilities.contains(ClientCapability::PublishesFaces) || !m_proportionalFaces
        || m_areProportionalFacesSent || m_state != ConnectionState::Attached)
        return;
    std::vector<std::uint8_t> body;
    encode(body, *m_proportionalFaces);
    send(FrameKind::ProportionalFaces, 0, body);
    m_areProportionalFacesSent = true;
}

void ClientConnection::start()
{
    Greet greet;
    greet.wireVersion = m_identity.wireVersion;
    greet.appVersion = m_identity.appVersion;
    greet.engineFingerprint = m_identity.engineFingerprint;
    greet.clientInstance = m_identity.clientInstance;
    greet.resume = m_identity.resume;
    greet.clientKind = m_identity.kind;
    greet.requestedRole = m_identity.requestedRole;
    greet.request = m_identity.request;
    greet.credential = m_identity.credential;
    greet.facts = m_identity.facts;
    greet.historyCacheLines = m_policy.historyCacheLines;
    greet.backfillDepthLines = m_policy.backfillDepthLines;
    greet.capabilities = m_policy.capabilities;
    std::vector<std::uint8_t> body;
    encode(body, greet);
    if (body.empty()) {
        close(ByeReason::Malformed, "the complete Greet cannot be encoded");
        return;
    }
    send(FrameKind::Greet, 0, body);
    m_isGreetSent = true;
}

void ClientConnection::receiveStream(std::span<const std::uint8_t> bytes, std::uint64_t now)
{
    if (m_state == ConnectionState::Closed)
        return;
    if (m_isReceiving) {
        close(ByeReason::Malformed, "a reentrant receive on the same connection");
        return;
    }
    m_isReceiving = true;
    m_streamBytesReceived += bytes.size();
    // Bytes that complete no frame — part of a chunk larger than the window —
    // owe an Ack like any others.
    bool isOnlyProbes = true;
    bool hasFrame = false;
    FrameHeader header;
    EnvelopeError error = EnvelopeError::None;
    std::string_view refusalDetail = "an unreadable Sync frame header";
    const auto result = m_decoder.consume(bytes, [&](std::span<const std::uint8_t> prefix) {
        switch (readFrameHeader(prefix, header, error)) {
        case FrameHeaderState::NeedsMore:
            return FrameHeaderDecision {};
        case FrameHeaderState::Failed:
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        case FrameHeaderState::Complete:
            break;
        }
        if (directionOf(header.kind) == Direction::ClientToHost
            || (m_state == ConnectionState::Greeting && header.kind != FrameKind::Welcome && header.kind != FrameKind::Bye)
            || (m_state != ConnectionState::Greeting && header.kind == FrameKind::Welcome)) {
            refusalDetail = directionOf(header.kind) == Direction::ClientToHost ? "a client-only Sync frame from the host"
                : m_state == ConnectionState::Greeting ? "a Sync frame before Welcome" : "a repeated Welcome";
            error = EnvelopeError::Malformed;
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        }
        if (m_state == ConnectionState::AwaitingConsent
            && ((header.kind != FrameKind::Notify && header.kind != FrameKind::Probe && header.kind != FrameKind::Echo && header.kind != FrameKind::Bye)
                || (header.kind == FrameKind::Notify && (header.bodyBytes > maxConsentStatusBytes
                    || (m_ownConsentPhase && *m_ownConsentPhase != ConsentStatusPhase::Pending))))) {
            refusalDetail = "a Sync frame before attachment consent";
            error = EnvelopeError::Malformed;
            return FrameHeaderDecision { .state = FrameHeaderDecision::State::Refused };
        }
        return FrameHeaderDecision { .state = FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
        (void)readFrameHeader(prefix, header, error);
        hasFrame = true;
        isOnlyProbes = isOnlyProbes && header.kind == FrameKind::Probe;
        if (m_delegate)
            m_delegate->connectionFrameDecoded(header.kind, prefix.size() + body.size());
        if (m_state == ConnectionState::Closed)
            return FrameDelivery::Stop;
        handleFrame({ .kind = header.kind, .channel = header.channel, .body = body }, Carrier::Stream, now);
        if (m_state != ConnectionState::Closed && m_delegate)
            m_delegate->connectionFrameApplied(*this);
        return m_state == ConnectionState::Closed ? FrameDelivery::Stop : FrameDelivery::Continue; });
    m_isReceiving = false;
    if (m_state == ConnectionState::Closed)
        return;
    settleInputWait();
    if (result.state == FrameIngress::State::Refused) {
        if (m_decoder.error() == FrameIngressError::AllocationFailed)
            close(ByeReason::TransferRefused, "the Sync frame could not be allocated");
        else
            close(byeReasonOf(error), error == EnvelopeError::Oversize ? "a Sync frame exceeds the receive limit" : error == EnvelopeError::UnknownKind ? "an unknown Sync frame kind"
                                                                                                                                                        : refusalDetail);
        return;
    }
    // The host's window retires stream bytes only on an Ack, so bytes that
    // complete no generation — the chunks of a checkpoint larger than the
    // window — are acknowledged too, or the transfer waits on an Ack that
    // waits on the transfer. Bytes that carried only the host's probes owe
    // none: the Echo answers them, so an idle client's radio wakes once per
    // probe and not twice. The next Ack counts them all the same.
    if (m_streamBytesReceived > m_streamBytesAcknowledged && !m_ackOwedSince && !(hasFrame && isOnlyProbes && m_decoder.pendingBytes() == 0))
        m_ackOwedSince = now;
}

void ClientConnection::receiveDatagram(std::span<const std::uint8_t> datagram, std::uint64_t now)
{
    if (m_state == ConnectionState::Closed)
        return;
    if (m_isReceiving) {
        close(ByeReason::Malformed, "a reentrant receive on the same connection");
        return;
    }
    EnvelopeError error = EnvelopeError::None;
    const std::optional<Frame> frame = decodeDatagram(datagram, error);
    if (!frame) {
        close(byeReasonOf(error), {});
        return;
    }
    m_isReceiving = true;
    if (m_delegate)
        m_delegate->connectionFrameDecoded(frame->kind, datagram.size());
    if (m_state == ConnectionState::Closed) {
        m_isReceiving = false;
        return;
    }
    handleFrame(*frame, Carrier::Datagram, now);
    if (m_state != ConnectionState::Closed && m_delegate)
        m_delegate->connectionFrameApplied(*this);
    m_isReceiving = false;
    if (m_state != ConnectionState::Closed)
        settleInputWait();
}

void ClientConnection::handleFrame(const Frame& frame, Carrier carrier, std::uint64_t now)
{
    // Any frame is contact: the probe outstanding is answered, and input
    // still waiting for its receipt starts its wait again. Only a receipt
    // ends the wait (settleInputWait).
    m_lastHeardAt = now;
    // The host's idle schedule restarts with screen data and steps with each
    // probe; the client follows it to know how long a quiet host is silent.
    if (frame.kind == FrameKind::Probe)
        ++m_hostProbesSinceData;
    else if (frame.kind == FrameKind::Update || frame.kind == FrameKind::BulkChunk)
        m_hostProbesSinceData = 0;
    if (m_stallProbeSentAt) {
        m_stallProbeSentAt.reset();
        if (m_inputWaitingSince)
            m_inputWaitingSince = now;
    }
    // What the host sent each pane: its own frames, and its checkpoints'
    // chunks, which ride channel 0 under a transfer the pane's head named.
    if (PaneReplica* addressed = frame.channel != 0 ? pane(frame.channel) : nullptr)
        addressed->bytesReceived += frameBytes(frame.channel, frame.body.size());
    else if (frame.kind == FrameKind::BulkChunk && frame.body.size() >= 4) {
        const std::uint32_t transfer = static_cast<std::uint32_t>(frame.body[0]) | (static_cast<std::uint32_t>(frame.body[1]) << 8)
            | (static_cast<std::uint32_t>(frame.body[2]) << 16) | (static_cast<std::uint32_t>(frame.body[3]) << 24);
        if (const auto owner = m_transferPanes.find(transfer); owner != m_transferPanes.end()) {
            if (PaneReplica* replica = pane(owner->second))
                replica->bytesReceived += frameBytes(0, frame.body.size());
        } else {
            const auto image = std::ranges::find_if(m_incomingImages, [&](const auto& entry) { return entry && entry->transfer == transfer; });
            if (image != m_incomingImages.end()) {
                if (PaneReplica* replica = pane((*image)->handle))
                    replica->bytesReceived += frameBytes(0, frame.body.size());
            }
        }
    }
    if (directionOf(frame.kind) == Direction::ClientToHost) {
        close(ByeReason::Malformed, "a client-to-host frame from the host");
        return;
    }
    // A datagram may overtake the stream frames sent before it: a probe can
    // land ahead of the Welcome, and an update ahead of its PaneBind or behind
    // its PaneUnbind. Such a datagram is dropped as a lost one would be, and
    // the host's next superset update or probe covers it.
    if (m_state == ConnectionState::Greeting && frame.kind != FrameKind::Welcome && frame.kind != FrameKind::Bye) {
        if (carrier == Carrier::Datagram)
            return;
        close(ByeReason::Malformed, "a frame before Welcome");
        return;
    }
    if (m_state == ConnectionState::AwaitingConsent
        && frame.kind != FrameKind::Notify && frame.kind != FrameKind::Probe && frame.kind != FrameKind::Echo && frame.kind != FrameKind::Bye) {
        close(ByeReason::Malformed, "pane or operation data before own consent approval");
        return;
    }
    // Only an update, a probe or an echo may ride a datagram.
    if (carrier == Carrier::Datagram && frame.kind != FrameKind::Update && frame.kind != FrameKind::Probe && frame.kind != FrameKind::Echo) {
        close(ByeReason::Malformed, "a stream-only frame in a datagram");
        return;
    }
    switch (frame.kind) {
    case FrameKind::Welcome: {
        if (m_state != ConnectionState::Greeting) {
            close(ByeReason::Malformed, "an unreadable or repeated Welcome");
            return;
        }
        const auto version = decodeHandshakeVersion(frame.body);
        if (!version) {
            close(ByeReason::Malformed, "an incomplete Welcome version");
            return;
        }
        // A host of another build is refused from the prefix every build
        // reads alike, keeping what it said of its build for the sentence
        // that names the older end. A host that states no fingerprint was
        // built before fingerprints were compared, so it is the older end
        // whatever version it states, and reads as one that says nothing.
        const std::optional<EngineFingerprint> fingerprint = decodeHandshakeFingerprint(frame.body);
        if (*version == m_identity.wireVersion && !fingerprint) {
            close(ByeReason::Malformed, "an incomplete Welcome build");
            return;
        }
        if (*version != m_identity.wireVersion || fingerprint != m_identity.engineFingerprint) {
            m_refusedHostBuild = fingerprint == EngineFingerprint {} ? std::optional { PeerBuild {} } : decodeHandshakeBuild(frame.body);
            close(ByeReason::VersionMismatch, versionMismatchNotice());
            return;
        }
        std::optional<Welcome> welcome = decodeWelcome(frame.body);
        if (!welcome) {
            close(ByeReason::Malformed, "an unreadable Welcome");
            return;
        }
        // A host that recreated the referenced workspace from its record
        // answers with that workspace's fresh sessions, under the id it gave
        // the workspace; the instance it names must still be the one this
        // client pinned.
        const bool isRestoredReference = isRestoredReferenceAnswer(m_identity.request, welcome->selection);
        if (welcome->selection.target != m_identity.request.target
            || (welcome->selection.restoredFromDaemonInstanceID && !isRestoredReference)
            || (m_identity.request.sessionID && !isRestoredReference && welcome->selection.sessionID != m_identity.request.sessionID)
            || (m_identity.request.workspaceID && !isRestoredReference && welcome->selection.workspaceID != m_identity.request.workspaceID)) {
            close(ByeReason::SessionGone, "Welcome changed the requested selection");
            return;
        }
        if (m_welcomeSink && !m_welcomeSink->acceptWelcome(*welcome)) {
            close(ByeReason::TransferRefused, "protected resume identity was refused");
            return;
        }
        // A pane bound after this is the named instance's: an unplanned
        // restart ends what the replicas held before any of it is bound.
        if (m_replicas && welcome->admission == AttachmentAdmission::Granted && !isUnboundTarget(welcome->selection.target))
            (void)m_replicas->noteHostInstance(welcome->hostInstanceID, welcome->predecessorHostInstanceID);
        m_welcome = std::move(welcome);
        m_state = m_welcome->admission == AttachmentAdmission::Granted ? ConnectionState::Attached : ConnectionState::AwaitingConsent;
        m_ownConsentPhase.reset();
        // The theme goes first, so no colour query is answered from a palette
        // the client no longer draws in; the Greet could not carry it.
        if (m_state == ConnectionState::Attached && m_identity.facts.themePalette)
            sendFacts();
        sendProportionalFaces();
        // The client's own round trip, from the start: what its stall bound
        // is measured in.
        sendProbe(now);
        if (m_delegate)
            m_delegate->hostWelcomed(*m_welcome);
        return;
    }
    case FrameKind::Probe: {
        const std::optional<Probe> probe = decodeProbe(frame.body);
        if (!probe) {
            close(ByeReason::Malformed, "an unreadable Probe");
            return;
        }
        std::vector<std::uint8_t> body;
        encode(body, Echo { .timeEcho = probe->time, .holdMicroseconds = 0 });
        if (carrier == Carrier::Datagram) {
            std::vector<std::uint8_t> datagram;
            (void)appendFrame(datagram, FrameKind::Echo, 0, body);
            m_datagrams.push_back(std::move(datagram));
        } else {
            send(FrameKind::Echo, 0, body);
        }
        return;
    }
    case FrameKind::PaneBind: {
        if (isUnboundTarget(m_identity.request.target)) {
            close(ByeReason::Malformed, "a pane bound to an unbound control target");
            return;
        }
        const std::optional<PaneBind> bind = decodePaneBind(frame.body);
        if (!bind || pane(bind->handle)) {
            close(ByeReason::Malformed, "an unreadable PaneBind or one for a bound handle");
            return;
        }
        if (m_panes.size() >= maxPanesPerConnection || bind->handle >= firstListedHandle) {
            close(ByeReason::Malformed, "a PaneBind past the connection's pane cap, or for a handle a listing takes");
            return;
        }
        PaneReplica replica;
        replica.handle = bind->handle;
        replica.paneID = bind->paneID;
        replica.effects.reset(new (std::nothrow) EffectReceiveCursor(bind->paneID));
        if (!replica.effects) {
            close(ByeReason::TransferRefused, "effect receipt allocation refused");
            return;
        }
        replica.effectReceipt = effectReceiptForPane(bind->paneID);
        if (!replica.effects->restore(replica.effectReceipt)) {
            close(ByeReason::Malformed, "invalid retained effect receipt state");
            return;
        }
        replica.terminal = m_delegate ? m_delegate->terminalForPane(bind->paneID) : nullptr;
        if (m_state == ConnectionState::Closed)
            return;
        // Changing the retained picture must retire every existing borrow
        // before holdLent can release its owner, including other handles.
        if (m_replicas && replica.terminal) {
            const auto previous = std::ranges::find(m_panes, bind->paneID, &PaneReplica::paneID);
            if (previous != m_panes.end() && previous->terminal != replica.terminal)
                setPaneTerminal(previous->handle, replica.terminal);
        }
        // A pane this client held through an earlier connection is bound to
        // the same picture and what is known of it. A pane the client draws
        // into a terminal of its own is the same picture only while it is
        // the same terminal.
        ReplicaSet::Held* held = m_replicas ? m_replicas->find(bind->paneID) : nullptr;
        if (held && held->picture() && (!replica.terminal || held->picture() == replica.terminal)) {
            replica.terminal = held->picture();
            replica.replica = held->replica;
            replica.hasCheckpoint = held->hasCheckpoint;
            replica.screenStatus = held->screenStatus;
        } else if (held && replica.terminal && held->terminal && held->hasCheckpoint
            && copyPicture(*held->terminal, *replica.terminal, { .maxDecodedScreenCellBytes = m_policy.maxDecodedScreenCellBytes, .maxStorageCost = m_policy.maxCheckpointStorageCost })) {
            // A pane restored into a terminal the set owned, before the
            // frontend named one of its own: the picture moves into the
            // frontend's terminal, and what is known of it stays known, so a
            // resume the host continued with a diff goes on from it.
            held->lentTerminal = replica.terminal;
            held->terminal.reset();
            replica.replica = held->replica;
            replica.hasCheckpoint = true;
            replica.screenStatus = held->screenStatus;
        } else if (replica.terminal && m_replicas) {
            (void)m_replicas->holdLent(bind->paneID, *replica.terminal);
        }
        if (!replica.terminal && m_replicas) {
            auto made = std::make_unique<Ubiquitty::Terminal>();
            if (m_policy.historyCacheLines > 0)
                Ubiquitty::setScrollbackLimit(made.get(), replicaScrollbackLines(m_policy));
            Ubiquitty::resize(*made, std::max<int>(1, m_identity.facts.viewportRows), std::max<int>(1, m_identity.facts.viewportColumns));
            replica.terminal = m_replicas->hold(bind->paneID, std::move(made)).terminal.get();
        }
        if (!replica.terminal) {
            replica.ownedTerminal = std::make_unique<Ubiquitty::Terminal>();
            replica.terminal = replica.ownedTerminal.get();
            if (m_policy.historyCacheLines > 0)
                Ubiquitty::setScrollbackLimit(replica.terminal, replicaScrollbackLines(m_policy));
            Ubiquitty::resize(*replica.terminal, std::max<int>(1, m_identity.facts.viewportRows), std::max<int>(1, m_identity.facts.viewportColumns));
        }
        // The client's cap is its own (Docs/sync.md §6.6).
        replica.images = std::make_unique<ImageReplica>(*replica.terminal);
        replica.history = m_replicas ? m_replicas->history(bind->paneID) : nullptr;
        const bool isRetainedHistory = replica.history != nullptr;
        if (!replica.history)
            replica.history = std::make_shared<HistoryCache>(m_welcome ? m_welcome->hostInstanceID : ID128 {}, bind->paneID, m_policy.historyCacheLines, m_policy.historyCacheBytes);
        if (m_replicas && !m_replicas->keepHistory(replica.history)) {
            close(ByeReason::TransferRefused, "retained history owner refused");
            return;
        }
        replica.history->invalidateProjection();
        // The cache is new, but a continued picture already has a committed
        // history epoch. There is no external old epoch to retire here.
        if (replica.hasCheckpoint && !isRetainedHistory)
            (void)replica.history->reset(HistoryReset { .newEpoch = replica.replica.historyEpoch, .clearedThrough = 0, .historyHead = replica.replica.historyHead });
        // The host starts a pane Dormant, or RecordsOnly for an agent; the
        // client shows a pane it is bound to unless its owner says otherwise.
        // A pane the host continued starts in the view the resume gave it, so
        // a resumed pane the client keeps hidden costs nothing. A refused
        // resume binds fresh, and the pane starts as any other.
        const PaneResume* resumed = nullptr;
        const bool wasContinued = m_welcome && std::ranges::any_of(m_welcome->resumed, [&](const PaneResumed& pane) { return pane.paneID == bind->paneID && pane.outcome != ResumeOutcome::Gone; });
        if (m_identity.resume && wasContinued) {
            const auto found = std::ranges::find(m_identity.resume->panes, bind->paneID, &PaneResume::paneID);
            resumed = found == m_identity.resume->panes.end() ? nullptr : &*found;
        }
        const ViewState startView = m_identity.kind == ClientKind::Agent ? ViewState::RecordsOnly : ViewState::Dormant;
        const ViewState hostView = resumed ? resumed->view : startView;
        const ViewState shownView = resumed ? resumed->view : (m_identity.kind == ClientKind::Agent ? ViewState::RecordsOnly : ViewState::Live);
        replica.view = hostView;
        // A pane bound live has its status in its screen, so a status set
        // that stood in its picture while it was hidden, on this connection
        // or the one before it, gives way to the screen's.
        const bool didRestoreScreenStatus = replica.view == ViewState::Live && restoreScreenStatus(replica);
        m_panes.push_back(std::move(replica));
        ++m_counters.paneBindings;
        const std::optional<ViewState> chosen = m_delegate ? m_delegate->viewOnBind(bind->handle, bind->paneID) : std::nullopt;
        if (m_state == ConnectionState::Closed)
            return;
        const ViewState view = chosen.value_or(shownView);
        // A pane bound fresh is told its view even when that is the one the
        // host starts it in: the host sends no status set for a pane until
        // it knows how the client holds it. A resume already said.
        if (!resumed && view == hostView)
            sendView(bind->handle, view);
        else
            setView(bind->handle, view);
        if (m_delegate)
            m_delegate->paneBound(bind->handle, bind->paneID);
        if (didRestoreScreenStatus && m_delegate)
            m_delegate->paneChanged(bind->handle, bind->paneID);
        return;
    }
    case FrameKind::PaneListed: {
        const std::optional<PaneBind> listed = decodePaneBind(frame.body);
        // A listed pane's handle is from the range no bound pane takes, and
        // one the host has not used: handles are not reused.
        if (!listed || listed->handle < firstListedHandle || listedPane(listed->handle) || m_listed.size() >= maxPanesPerConnection) {
            close(ByeReason::Malformed, "an unreadable PaneListed, one for a handle in use, or one past the connection's pane cap");
            return;
        }
        m_listed.push_back({ .handle = listed->handle, .paneID = listed->paneID, .facts = std::nullopt, .status = std::nullopt, .statusAssembly = {} });
        if (m_delegate)
            m_delegate->paneListed(listed->paneID);
        return;
    }
    case FrameKind::WorkspaceStructure: {
        std::optional<WorkspaceStructureHead> head = decodeWorkspaceStructureHead(frame.body);
        if (!head) {
            close(ByeReason::Malformed, "an unreadable WorkspaceStructure");
            return;
        }
        if (head->transfer == 0) {
            takeStructure(*head, head->record);
            return;
        }
        if (!m_reassembler.expect(head->transfer, maxWorkspaceStructureRecordBytes)) {
            close(ByeReason::Malformed, "a WorkspaceStructure naming a transfer already announced");
            return;
        }
        m_structureTransfers[head->transfer] = { .workspaceID = head->workspaceID, .revision = head->revision };
        return;
    }
    case FrameKind::PaneUnbind: {
        const std::optional<PaneUnbind> unbind = decodePaneUnbind(frame.body);
        if (const ListedPane* listed = unbind ? listedPane(unbind->handle) : nullptr) {
            const ID128 paneID = listed->paneID;
            std::erase_if(m_listed, [&](const ListedPane& candidate) { return candidate.handle == unbind->handle; });
            if (m_delegate)
                m_delegate->paneUnlisted(paneID);
            return;
        }
        const PaneReplica* replica = unbind ? pane(unbind->handle) : nullptr;
        if (!replica) {
            close(ByeReason::UnboundHandle, "an unreadable PaneUnbind or one for an unbound handle");
            return;
        }
        const ID128 paneID = replica->paneID;
        // A pane the host could not send is a failure the person is told of,
        // not an ordinary end.
        if (unbind->reason == std::to_underlying(PaneUnbindReason::CheckpointTooLarge)) {
            ++m_counters.panesFailed;
            if (m_delegate)
                m_delegate->paneFailed(unbind->handle, paneID, "The host could not send this pane: its screen is larger than the host's checkpoint limit.");
            if (m_state == ConnectionState::Closed)
                return;
            replica = pane(unbind->handle);
            if (!replica)
                return;
        }
        for (auto& request : m_clipboardReads) {
            if (request && request->handle == unbind->handle)
                request.reset();
        }
        m_nativeDragEpochs.erase(unbind->handle);
        abandonImages(unbind->handle);
        abandonEffects(unbind->handle);
        // Chunks of the pane's transfers may still be on their way; the fence
        // discards them rather than taking them for a host's mistake.
        if (replica->pending) {
            m_reassembler.abandon(replica->pending->transfer);
            m_transferPanes.erase(replica->pending->transfer);
        }
        std::erase_if(m_historyTransfers, [&](const auto& entry) {
            if (entry.second.handle != unbind->handle)
                return false;
            m_reassembler.abandon(entry.first);
            return true;
        });
        // A run the host dropped when it unbound the pane ends here.
        std::erase_if(m_panes, [&](const PaneReplica& candidate) { return candidate.handle == unbind->handle; });
        // The last binding ended, unlike a transport interruption. Drop its
        // receipt before a callback can reconnect or reuse the slot; another
        // handle for the same stable pane still needs that receipt.
        if (m_replicas && !std::ranges::any_of(m_panes, [&](const PaneReplica& candidate) { return candidate.paneID == paneID; })) {
            m_replicas->forgetEffectReceipt(paneID);
            m_replicas->forgetHistory(paneID);
        }
        ++m_counters.paneBindings;
        // The pane's pixels left the connection-wide image allowance; an
        // image another pane was refused room for may fit now.
        requestMissingImages();
        if (m_delegate)
            m_delegate->paneUnbound(unbind->handle, paneID);
        return;
    }
    case FrameKind::CheckpointHead: {
        const std::optional<CheckpointHead> head = decodeCheckpointHead(frame.body);
        PaneReplica* replica = pane(frame.channel);
        if (!head || !replica || !m_reassembler.expect(head->transfer, m_policy.maxCheckpointBytes)) {
            close(head && !replica ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable or misplaced CheckpointHead");
            return;
        }
        if (replica->pending) {
            m_reassembler.abandon(replica->pending->transfer);
            m_transferPanes.erase(replica->pending->transfer);
        }
        replica->pending = PendingCheckpoint { .generation = head->generation, .transfer = head->transfer, .stateDigest = head->stateDigest };
        m_transferPanes[head->transfer] = replica->handle;
        // Updates chained before this checkpoint can never apply after it.
        std::erase_if(replica->buffered, [&](const std::vector<std::uint8_t>& body) {
            const std::optional<DecodedUpdateHeader> header = decodeUpdateHeader(body);
            if (header && header->header.generation > head->generation)
                return false;
            replica->bufferedBytes -= body.size();
            return true;
        });
        return;
    }
    case FrameKind::BulkChunk: {
        const std::optional<BulkChunk> chunk = decodeBulkChunk(frame.body);
        if (!chunk) {
            close(ByeReason::Malformed, "an unreadable BulkChunk");
            return;
        }
        if (receiveImageChunk(*chunk))
            return;
        if (receiveSampleChunk(*chunk, now))
            return;
        if (receiveClipboardChunk(*chunk, now))
            return;
        refreshReceiveCharges();
        const ChunkOutcome outcome = m_reassembler.accept(*chunk);
        if (outcome == ChunkOutcome::OverBudget) {
            refuseOverBudget(chunk->transfer, now);
            return;
        }
        if (outcome == ChunkOutcome::Corrupt) {
            close(ByeReason::Malformed, "a deflated transfer that does not inflate to the length it declares");
            return;
        }
        if (outcome == ChunkOutcome::Refused) {
            close(ByeReason::TransferRefused, "a bulk transfer failed its check");
            return;
        }
        if (outcome == ChunkOutcome::Discarded) {
            // A head refused for transient image capacity is fenced until its
            // final chunk. Recheck the missing set then; no unrelated future
            // screen change is needed to make the image eligible again.
            if (chunk->isLast)
                requestMissingImages();
            return;
        }
        if (outcome == ChunkOutcome::Pending)
            return;
        // The payload stays charged to the receive owner while it is applied.
        MemoryCharge payloadCharge;
        std::vector<std::uint8_t> payload = m_reassembler.take(chunk->transfer, &payloadCharge);
        if (const auto structure = m_structureTransfers.find(chunk->transfer); structure != m_structureTransfers.end()) {
            const WorkspaceStructureHead head { .workspaceID = structure->second.workspaceID, .revision = structure->second.revision, .transfer = 0, .isGone = false, .record = {} };
            m_structureTransfers.erase(structure);
            takeStructure(head, payload);
            return;
        }
        if (m_resultTransfers.erase(chunk->transfer) > 0) {
            if (!isValidUTF8(payload)) {
                close(ByeReason::Malformed, "a Result that is not UTF-8");
                return;
            }
            const std::string text(payload.begin(), payload.end());
            if (!m_delegate || !m_delegate->consumeControlResult(text))
                m_results.push_back(text);
            return;
        }
        if (const auto history = m_historyTransfers.find(chunk->transfer); history != m_historyTransfers.end()) {
            const HistoryTransfer transfer = history->second;
            m_historyTransfers.erase(history);
            PaneReplica* replica = pane(transfer.handle);
            if (replica && !storeHistory(*replica, transfer.head.historyEpoch, payload, transfer.head.firstLine, transfer.head.order))
                close(ByeReason::Malformed, "a history transfer that does not decode");
            return;
        }
        const auto owner = m_transferPanes.find(chunk->transfer);
        if (owner == m_transferPanes.end())
            return;
        PaneReplica* replica = pane(owner->second);
        m_transferPanes.erase(owner);
        if (replica && replica->pending && replica->pending->transfer == chunk->transfer)
            applyCheckpointBody(*replica, payload, now);
        return;
    }
    case FrameKind::Update: {
        PaneReplica* replica = pane(frame.channel);
        if (!replica && carrier == Carrier::Datagram)
            return;
        if (!replica) {
            close(ByeReason::UnboundHandle, "an Update for an unbound pane");
            return;
        }
        handleUpdate(*replica, frame.body, now);
        return;
    }
    case FrameKind::Receipt: {
        const std::optional<Receipt> receipt = decodeReceipt(frame.body);
        PaneReplica* replica = pane(frame.channel);
        if (!receipt || !replica) {
            close(ByeReason::Malformed, "an unreadable Receipt or one for an unbound pane");
            return;
        }
        noteReceipt(*replica, *receipt);
        return;
    }
    case FrameKind::InputDiscarded: {
        const std::optional<InputDiscarded> run = decodeInputDiscarded(frame.body);
        PaneReplica* replica = run ? pane(frame.channel) : nullptr;
        if (!replica) {
            close(ByeReason::Malformed, "an unreadable InputDiscarded or one for an unbound pane");
            return;
        }
        // Past the bound the oldest two are told as one.
        if (replica->discards.size() == maxInputDiscardRecords) {
            replica->discards[1].inputWritten = replica->discards[0].inputWritten;
            replica->discards.erase(replica->discards.begin());
        }
        replica->discards.push_back(*run);
        // A paste among them takes no more pieces.
        std::erase_if(m_pastes, [&](const OutgoingPaste& paste) { return paste.handle == frame.channel && paste.inputSeq > run->inputWritten && paste.inputSeq <= run->throughSeq; });
        return;
    }
    case FrameKind::PasteReceipt: {
        const std::optional<PasteReceipt> receipt = decodePasteReceipt(frame.body);
        if (!receipt || !pane(frame.channel)) {
            close(ByeReason::Malformed, "an unreadable PasteReceipt or one for an unbound pane");
            return;
        }
        // A receipt for a paste that left whole, or was given up, says
        // nothing the client still acts on.
        for (OutgoingPaste& paste : m_pastes) {
            if (paste.handle == frame.channel && paste.inputSeq == receipt->inputSeq)
                paste.written = std::max(paste.written, std::min<std::uint64_t>(receipt->bytesWritten, paste.sent));
        }
        return;
    }
    case FrameKind::Echo: {
        const std::optional<Echo> echo = decodeEcho(frame.body);
        if (!echo) {
            close(ByeReason::Malformed, "an unreadable Echo");
            return;
        }
        if (m_livenessProbe && echo->timeEcho == *m_livenessProbe)
            m_livenessProbe.reset();
        // Only the probe outstanding is a sample, as on the host.
        if (m_outstandingProbe && echo->timeEcho == *m_outstandingProbe && now >= echo->timeEcho && now - echo->timeEcho >= echo->holdMicroseconds) {
            m_outstandingProbe.reset();
            const std::uint64_t sample = now - echo->timeEcho - echo->holdMicroseconds;
            // RFC 6298 §2, as the host's pacer smooths.
            m_smoothedRoundTrip = m_smoothedRoundTrip ? (*m_smoothedRoundTrip * 7 + sample) / 8 : sample;
        }
        return;
    }
    case FrameKind::BulkAbort: {
        const std::optional<BulkAbort> abort = decodeBulkAbort(frame.body);
        if (!abort) {
            close(ByeReason::Malformed, "an unreadable BulkAbort");
            return;
        }
        for (const auto& replica : m_panes) {
            for (const auto& sample : replica.samples) {
                if (sample.incoming && sample.transferID == abort->transfer) {
                    close(ByeReason::TransferRefused, "a required sample transfer was aborted");
                    return;
                }
            }
        }
        if (std::ranges::any_of(m_deferredEffects, [&](const DeferredEffect& held) { return held.clipboardTransfer == abort->transfer; })) {
            close(ByeReason::TransferRefused, "a required clipboard transfer was aborted");
            return;
        }
        ++m_counters.bulkAbortsReceived;
        bool didRetireImage = false;
        for (auto& image : m_incomingImages) {
            if (image && image->transfer == abort->transfer) {
                // The host re-offers after aborting a pixel source. Asking
                // before that new offer can race the old delivery's teardown
                // and leave a deduplicated Want that will never be answered.
                if (auto* replica = pane(image->handle)) {
                    for (auto& offer : replica->imageOffers) {
                        if (offer == std::optional { image->asset })
                            offer.reset();
                    }
                }
                image.reset();
                didRetireImage = true;
                break;
            }
        }
        // The client gave the transfer up first: the abort is the host's
        // word that nothing more of it follows.
        if (m_reassembler.releaseFence(abort->transfer)) {
            if (didRetireImage)
                requestMissingImages();
            return;
        }
        // Only a transfer a head announced and no chunk completed can be
        // aborted; any other is a host naming what it never sent.
        if (!m_reassembler.isExpected(abort->transfer)) {
            close(ByeReason::Malformed, "a BulkAbort for a transfer never announced");
            return;
        }
        // Chunks sent before the abort may still be on their way on a carrier
        // that reorders its lanes; the fence discards them.
        m_reassembler.abandon(abort->transfer);
        if (didRetireImage) {
            requestMissingImages();
            return;
        }
        if (m_historyTransfers.erase(abort->transfer) > 0 || m_resultTransfers.erase(abort->transfer) > 0 || m_structureTransfers.erase(abort->transfer) > 0)
            return;
        m_transferPanes.erase(abort->transfer);
        const auto replica = std::ranges::find_if(m_panes, [&](const PaneReplica& held) { return held.pending && held.pending->transfer == abort->transfer; });
        if (replica == m_panes.end())
            return;
        replica->pending.reset();
        // Updates chained after the abandoned body can never apply.
        replica->buffered.clear();
        replica->bufferedBytes = 0;
        return;
    }
    case FrameKind::Effect:
        if (carrier != Carrier::Stream) {
            close(ByeReason::Malformed, "an Effect outside the negotiated ordered grammar");
            return;
        }
        receiveEffect(frame.channel, frame.body, now);
        return;
    case FrameKind::PaneFacts: {
        std::optional<PaneFacts> facts = decodePaneFacts(frame.body);
        if (ListedPane* listed = facts ? listedPane(frame.channel) : nullptr) {
            listed->facts = std::move(*facts);
            ++m_counters.recordsApplied;
            if (m_delegate)
                m_delegate->listedPaneChanged(listed->paneID);
            return;
        }
        PaneReplica* replica = facts ? pane(frame.channel) : nullptr;
        if (!replica) {
            close(ByeReason::Malformed, "an unreadable PaneFacts or one for an unbound pane");
            return;
        }
        if (replica->terminal)
            applyRecorded(*replica->terminal, *facts);
        replica->facts = std::move(*facts);
        ++m_counters.recordsApplied;
        if (m_delegate)
            m_delegate->paneRecordsChanged(replica->handle, replica->paneID);
        return;
    }
    case FrameKind::ImageHead:
        receiveImageHead(frame.channel, frame.body);
        return;
    case FrameKind::RichTextMeasure: {
        Ubiquitty::Terminal* terminal = paneTerminal(frame.channel);
        if (!terminal) {
            close(ByeReason::UnboundHandle, "rich-text measurement for an unbound pane");
            return;
        }
        WireReader reader(frame.body);
        RichText::MeasureRequest request;
        if (!readMeasureRequest(reader, request) || !reader.isAtEnd()) {
            close(ByeReason::Malformed, "unreadable rich-text measurement request");
            return;
        }
        // The peer can ask under an older publication while a local font
        // rebuild is in flight. Publish the actual configuration and let its
        // epoch change invalidate that request instead of mislabelling geometry.
        const auto* replica = pane(frame.channel);
        if (!terminal->typography.facts || terminal->typography.facts->rendererRevision != request.rendererRevision
            || !replica || !replica->publishedTypography || *replica->publishedTypography != *terminal->typography.facts) {
            if (terminal->typography.facts)
                publishTypography(frame.channel, *terminal->typography.facts);
            return;
        }
        RichText::Measurement measured;
        if (request.fontCatalog)
            measured.fontCatalog = RichText::queryFontCatalog(*request.fontCatalog, terminal->typography);
        else
            measured = RichText::measure(request.document, request.shape, terminal->typography);
        measured.rendererRevision = request.rendererRevision;
        if (!request.needsGeometry)
            measured.geometry = {};
        std::vector<std::uint8_t> body;
        WireWriter writer(body);
        writeMeasurement(writer, measured);
        for (std::size_t offset = 0; offset < body.size();) {
            const auto count = std::min(body.size() - offset, maxFrameBodyBytes - 12);
            std::vector<std::uint8_t> chunk;
            WireWriter part(chunk);
            part.u32(request.token);
            part.u32(static_cast<std::uint32_t>(body.size()));
            part.u32(static_cast<std::uint32_t>(offset));
            part.raw(std::span(body).subspan(offset, count));
            send(FrameKind::RichTextMeasured, frame.channel, chunk);
            offset += count;
        }
        return;
    }
    case FrameKind::ClipboardRequest:
        receiveClipboardRequest(frame.channel, frame.body);
        return;
    case FrameKind::NativeDragCommand: {
        const auto command = decodeNativeDragCommand(frame.body);
        const auto* replica = pane(frame.channel);
        if (!replica)
            return;
        if (!command || !m_welcome || m_identity.kind != ClientKind::Human || !m_policy.capabilities.contains(ClientCapability::NativeDrag)
            || (m_welcome->grantedRole != Role::Owner && m_welcome->grantedRole != Role::Driver)) {
            close(ByeReason::Malformed, "invalid native drag command");
            return;
        }
        auto& epoch = m_nativeDragEpochs[frame.channel];
        if (command->epoch < epoch)
            return;
        if (command->epoch != epoch && command->command.kind != DragDrop::CommandKind::Registration) {
            close(ByeReason::Malformed, "native drag epoch lacks registration");
            return;
        }
        epoch = command->epoch;
        if (m_delegate)
            m_delegate->paneNativeDragCommand(frame.channel, replica->paneID, *command);
        return;
    }
    case FrameKind::SampleHead:
        receiveSampleHead(frame.channel, frame.body, now);
        return;
    case FrameKind::ImagePlacements:
        receiveImagePlacements(frame.channel, frame.body);
        return;
    case FrameKind::StatusRecords: {
        std::optional<StatusFragment> fragment = decodeStatusFragment(frame.body);
        if (ListedPane* listed = fragment ? listedPane(frame.channel) : nullptr) {
            switch (listed->statusAssembly.accept(std::move(*fragment))) {
            case StatusAssembly::Outcome::Partial:
                return;
            case StatusAssembly::Outcome::Refused:
                close(ByeReason::Malformed, "a StatusRecords frame that continues no set");
                return;
            case StatusAssembly::Outcome::Whole:
                break;
            }
            listed->status = listed->statusAssembly.take();
            ++m_counters.recordsApplied;
            if (m_delegate)
                m_delegate->listedPaneChanged(listed->paneID);
            return;
        }
        PaneReplica* replica = fragment ? pane(frame.channel) : nullptr;
        if (!replica) {
            close(ByeReason::Malformed, "an unreadable StatusRecords or one for an unbound pane");
            return;
        }
        switch (replica->statusAssembly.accept(std::move(*fragment))) {
        case StatusAssembly::Outcome::Partial:
            return;
        case StatusAssembly::Outcome::Refused:
            close(ByeReason::Malformed, "a StatusRecords frame that continues no set");
            return;
        case StatusAssembly::Outcome::Whole:
            break;
        }
        replica->status = replica->statusAssembly.take();
        if (replica->view != ViewState::Live)
            standStatusSetInTerminal(*replica);
        ++m_counters.recordsApplied;
        if (m_delegate)
            m_delegate->paneRecordsChanged(replica->handle, replica->paneID);
        return;
    }
    case FrameKind::PaneDirty: {
        PaneReplica* replica = pane(frame.channel);
        if (!replica || !frame.body.empty()) {
            close(replica ? ByeReason::Malformed : ByeReason::UnboundHandle, "an unreadable PaneDirty or one for an unbound pane");
            return;
        }
        ++m_counters.paneDirtiesReceived;
        if (m_delegate)
            m_delegate->paneDirty(replica->handle, replica->paneID);
        return;
    }
    case FrameKind::PanePalette: {
        PaneReplica* replica = pane(frame.channel);
        if (!replica) {
            close(ByeReason::UnboundHandle, "a PanePalette for an unbound pane");
            return;
        }
        switch (applyPaletteState(*replica->terminal, frame.body, replica->replica)) {
        case ApplyOutcome::Applied:
            break;
        case ApplyOutcome::AllocationRefused:
            // The host counts the block as held; a checkpoint is what makes
            // the two agree again.
            requestResync(*replica, ResyncReason::DecodeFailed);
            return;
        case ApplyOutcome::Malformed:
        case ApplyOutcome::DigestMismatch:
        case ApplyOutcome::BaseMissing:
            close(ByeReason::Malformed, "an unreadable PanePalette");
            return;
        }
        ++m_counters.palettesApplied;
        // The pane's colours moved, which whatever draws it outside its
        // screen reads as it reads any other change of the pane.
        if (m_delegate)
            m_delegate->paneChanged(replica->handle, replica->paneID);
        return;
    }
    case FrameKind::Bye: {
        const std::optional<Bye> bye = decodeBye(frame.body);
        if (m_state == ConnectionState::AwaitingConsent && m_ownConsentPhase
            && (*m_ownConsentPhase == ConsentStatusPhase::Denied || *m_ownConsentPhase == ConsentStatusPhase::Expired)
            && (!bye || bye->reason != ByeReason::NoConsent)) {
            close(ByeReason::Malformed, "a terminal consent status without its NoConsent Bye");
            return;
        }
        m_receivedBye = bye;
        m_byeReason = bye ? bye->reason : ByeReason::Malformed;
        m_state = ConnectionState::Closed;
        for (const auto& replica : m_panes) {
            abandonImages(replica.handle);
            abandonEffects(replica.handle);
        }
        return;
    }
    case FrameKind::HistoryHead: {
        const std::optional<HistoryHead> head = decodeHistoryHead(frame.body);
        if (!head || !pane(frame.channel) || !m_reassembler.expect(head->transfer, maxHistoryTransferBytes)) {
            close(head && !pane(frame.channel) ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable or misplaced HistoryHead");
            return;
        }
        m_historyTransfers[head->transfer] = { .handle = frame.channel, .head = *head };
        // A transfer of an epoch the cache does not hold is fenced at once:
        // its chunks are discarded as they come (Docs/sync.md §6.6).
        if (head->historyEpoch != pane(frame.channel)->history->key().epoch) {
            m_reassembler.abandon(head->transfer);
            m_historyTransfers.erase(head->transfer);
        }
        return;
    }
    case FrameKind::HistoryInline: {
        const std::optional<DecodedHistoryInlineHeader> header = decodeHistoryInlineHeader(frame.body);
        PaneReplica* replica = pane(frame.channel);
        if (!header || !replica) {
            close(header ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable HistoryInline or one for an unbound pane");
            return;
        }
        if (!storeHistory(*replica, header->header.historyEpoch, frame.body.subspan(header->bytes), header->header.firstLine, HistoryOrder::Ascending))
            close(ByeReason::Malformed, "a HistoryInline whose lines do not decode");
        return;
    }
    case FrameKind::HistoryReset: {
        const std::optional<HistoryReset> reset = decodeHistoryReset(frame.body);
        PaneReplica* replica = pane(frame.channel);
        if (!reset || !replica) {
            close(reset ? ByeReason::UnboundHandle : ByeReason::Malformed, "an unreadable HistoryReset or one for an unbound pane");
            return;
        }
        // An epoch the cache already holds, or one it has moved past, retires
        // nothing.
        if (reset->newEpoch <= replica->history->key().epoch)
            return;
        if (retireHistory(*replica, *reset) != HistoryPurgeOutcome::Purged)
            return;
        purgeHistory(*replica, *reset);
        return;
    }
    case FrameKind::Result: {
        std::optional<Result> result = decodeResult(frame.body);
        if (!result) {
            close(ByeReason::Malformed, "an unreadable Result");
            return;
        }
        if (result->transfer == 0) {
            if (!m_delegate || !m_delegate->consumeControlResult(result->text))
                m_results.push_back(std::move(result->text));
            return;
        }
        if (!m_reassembler.expect(result->transfer, m_policy.maxResultBytes)) {
            close(ByeReason::Malformed, "a Result naming a transfer already announced");
            return;
        }
        m_resultTransfers.insert(result->transfer);
        return;
    }
    case FrameKind::Notify: {
        const std::optional<NotifyHead> head = decodeNotifyHead(frame.body);
        if (!head) {
            close(ByeReason::Malformed, "a Notify without a kind");
            return;
        }
        const std::optional<NotifyKind> kind = notifyKindOf(head->kind);
        std::optional<ConsentStatus> ownStatus;
        if (kind == NotifyKind::ConsentStatus) {
            ownStatus = decodeConsentStatus(head->record);
            if (!ownStatus) {
                close(ByeReason::Malformed, "a malformed own-consent status");
                return;
            }
        }
        if (m_state == ConnectionState::AwaitingConsent) {
            if (!ownStatus || !m_welcome || ownStatus->question != m_welcome->consent
                || (ownStatus->phase == ConsentStatusPhase::Pending ? m_ownConsentPhase.has_value()
                                                                    : m_ownConsentPhase != ConsentStatusPhase::Pending)) {
                close(ByeReason::Malformed, "an unsolicited or mismatched own-consent transition");
                return;
            }
            m_ownConsentPhase = ownStatus->phase;
            if (ownStatus->phase == ConsentStatusPhase::Approved) {
                Welcome admitted = *m_welcome;
                admitted.admission = AttachmentAdmission::Granted;
                admitted.consent = {};
                if (m_welcomeSink && !m_welcomeSink->acceptWelcome(admitted)) {
                    close(ByeReason::TransferRefused, "protected approved identity was refused");
                    return;
                }
                if (m_replicas)
                    (void)m_replicas->noteHostInstance(admitted.hostInstanceID, admitted.predecessorHostInstanceID);
                m_welcome = std::move(admitted);
                m_state = ConnectionState::Attached;
                if (m_identity.facts.themePalette)
                    sendFacts();
                sendProportionalFaces();
                sendProbe(now);
                // Publish the bounded status before the observer callback:
                // that callback may replace or destroy this connection.
                if (m_notifies.size() < maxUnreadNotifies)
                    m_notifies.push_back(Notify { .kind = NotifyKind::ConsentStatus, .record = { head->record.begin(), head->record.end() } });
                if (m_delegate)
                    m_delegate->hostWelcomed(*m_welcome);
                return;
            }
        } else if (ownStatus) {
            close(ByeReason::Malformed, "an own-consent status outside a pending attachment");
            return;
        }
        // A kind this build does not know is skipped by the frame's length: a
        // newer host may tell a client something it has no reader for.
        if (!kind) {
            ++m_counters.notifiesSkipped;
            return;
        }
        if (m_notifies.size() >= maxUnreadNotifies) {
            ++m_counters.notifiesDropped;
            return;
        }
        m_notifies.push_back(Notify { .kind = *kind, .record = { head->record.begin(), head->record.end() } });
        if (ownStatus && (ownStatus->phase == ConsentStatusPhase::Denied || ownStatus->phase == ConsentStatusPhase::Expired) && m_delegate)
            m_delegate->consentRefused(*ownStatus);
        return;
    }
    // The client's own kinds, refused by their direction before this switch.
    case FrameKind::Call:
    case FrameKind::Greet:
    case FrameKind::Facts:
    case FrameKind::TypographyFacts:
    case FrameKind::ProportionalFaces:
    case FrameKind::Ack:
    case FrameKind::Resync:
    case FrameKind::View:
    case FrameKind::HistoryWant:
    case FrameKind::ImageWant:
    case FrameKind::RichTextMeasured:
    case FrameKind::NativeDragEvent:
    case FrameKind::ClipboardData:
    case FrameKind::Keys:
    case FrameKind::Pointer:
    case FrameKind::Paste:
    case FrameKind::PasteAbort:
    case FrameKind::Raw:
    case FrameKind::Focus:
    case FrameKind::Viewport:
        break;
    }
    close(ByeReason::Malformed, "a frame the client does not accept");
}

void ClientConnection::receiveEffect(std::uint32_t handle, std::span<const std::uint8_t> body, std::uint64_t now)
{
    const auto* replica = pane(handle);
    if (!replica || !replica->effects) {
        close(ByeReason::UnboundHandle, "an Effect for an unbound pane");
        return;
    }
    const auto packet = decodeEffectPacket(replica->paneID, body);
    if (!packet || (packet->record && !decodeEffectBody(packet->record->kind, packet->record->payload))) {
        close(ByeReason::Malformed, "an unreadable or unsupported Effect packet");
        return;
    }
    std::optional<ClipboardWriteEffect> clipboard;
    bool needsClipboard = false;
    if (packet->record && packet->record->kind == EffectKind::ClipboardWrite) {
        const auto decoded = decodeEffectBody(packet->record->kind, packet->record->payload);
        clipboard = std::get<ClipboardWriteEffect>(*decoded);
        if (effectDisposition(*packet->record, m_identity.clientInstance, m_identity.kind,
                m_welcome ? m_welcome->grantedRole : Role::Viewer)
            != EffectDisposition::Post) {
            close(ByeReason::Malformed, "a clipboard effect was disclosed to another recipient");
            return;
        }
        needsClipboard = clipboard->payload.transfer && packet->record->sequence > effectReceiptForPane(replica->paneID).consumed;
    }
    const auto retained = inboundImageBytes() + inboundEffectBytes();
    const auto payloadBytes = needsClipboard ? clipboard->payload.encodedBytes : 0;
    const auto required = body.size() + payloadBytes;
    if (m_deferredEffects.size() >= maxEffectRecords || retained > maxCatalogMessageBytes
        || required > catalogRoom(retained)) {
        close(ByeReason::TransferRefused, "deferred effect capacity is exhausted");
        return;
    }
    std::unique_ptr<std::uint8_t[]> copy(new (std::nothrow) std::uint8_t[body.size()]);
    if (!copy) {
        close(ByeReason::TransferRefused, "an effect packet allocation was refused");
        return;
    }
    std::copy(body.begin(), body.end(), copy.get());
    DeferredEffect queued { .handle = handle, .paneID = replica->paneID, .bytes = std::move(copy), .size = body.size(), .clipboard = {} };
    if (clipboard && clipboard->payload.transfer) {
        if (!m_reassembler.expect(clipboard->payload.transfer, 0)) {
            close(ByeReason::Malformed, "a clipboard effect reused a transfer identity");
            return;
        }
        if (needsClipboard) {
            queued.clipboard.bytes.reset(new (std::nothrow) std::uint8_t[payloadBytes]);
            if (!queued.clipboard.bytes) {
                close(ByeReason::TransferRefused, "a clipboard payload allocation was refused");
                return;
            }
            queued.clipboard.size = payloadBytes;
            queued.clipboardTransfer = clipboard->payload.transfer;
        } else {
            m_reassembler.abandon(clipboard->payload.transfer);
        }
    }
    m_deferredEffects.push_back(std::move(queued));
    m_deferredEffectBytes += required;
    drainEffects(now);
}

void ClientConnection::drainEffects(std::uint64_t now)
{
    if (m_isDrainingEffects || m_state != ConnectionState::Attached)
        return;
    m_isDrainingEffects = true;
    while (m_state == ConnectionState::Attached) {
        bool didConsume = false;
        for (auto entry = m_deferredEffects.begin(); entry != m_deferredEffects.end(); ++entry) {
            // A different pane can proceed while this pane's first payload
            // waits for its bulk lane. No later packet overtakes that prefix.
            if (std::any_of(m_deferredEffects.begin(), entry, [&](const DeferredEffect& earlier) { return earlier.handle == entry->handle; }))
                continue;
            auto* replica = pane(entry->handle);
            if (!replica || replica->paneID != entry->paneID || !replica->effects) {
                close(ByeReason::UnboundHandle, "a deferred effect lost its exact pane binding");
                break;
            }
            const auto packet = decodeEffectPacket(entry->paneID, { entry->bytes.get(), entry->size });
            if (!packet) {
                close(ByeReason::Malformed, "an unreadable deferred Effect");
                break;
            }
            // A pane shown live consumes an effect only once it has applied
            // the effect's picture. One the host sent under the view the pane
            // held before, such as a fresh reader's completion at bind, waits
            // here for the picture that showing the pane asks for.
            if (replica->view == ViewState::Live && packet->generation > replica->replica.generation)
                continue;
            std::optional<EffectBody> body;
            PreparedEffect prepared;
            const auto previousReceipt = effectReceiptForPane(replica->paneID);
            if (packet->record) {
                const auto decoded = decodeEffectBody(packet->record->kind, packet->record->payload);
                if (!decoded) {
                    close(ByeReason::Malformed, "an unreadable deferred Effect body");
                    break;
                }
                body = *decoded;
                if (packet->record->sequence > previousReceipt.consumed) {
                    const auto readiness = prepareEffectPayload(*replica, *packet->record, *body, prepared);
                    if (readiness == EffectPayloadReadiness::Pending)
                        continue;
                    if (readiness == EffectPayloadReadiness::Refused) {
                        close(ByeReason::Malformed, "an Effect has no authorized exact payload");
                        break;
                    }
                }
            }
            const auto accepted = replica->effects->accept(*packet, { .view = replica->view, .appliedGeneration = replica->replica.generation });
            if (!accepted) {
                close(ByeReason::Malformed, "an Effect outside its batch or picture dependency");
                m_deliveringSampleBytes = 0;
                m_deliveringClipboardBytes = 0;
                break;
            }
            const auto handleReceipt = replica->effects->state();
            const EffectReceiptState sharedReceipt {
                .acknowledged = std::max(previousReceipt.acknowledged, handleReceipt.acknowledged),
                .consumed = std::max(previousReceipt.consumed, handleReceipt.consumed),
            };
            if (m_effectSink && (sharedReceipt.acknowledged != previousReceipt.acknowledged || sharedReceipt.consumed != previousReceipt.consumed)
                && !m_effectSink->commitEffectReceipt(replica->paneID, sharedReceipt)) {
                close(ByeReason::Suspending, "effect receipt persistence was refused");
                m_deliveringSampleBytes = 0;
                m_deliveringClipboardBytes = 0;
                break;
            }
            if (m_replicas && !m_replicas->keepEffectReceipt(replica->paneID, sharedReceipt)) {
                close(ByeReason::TransferRefused, "effect receipt storage refused");
                m_deliveringSampleBytes = 0;
                m_deliveringClipboardBytes = 0;
                break;
            }
            for (auto& alias : m_panes) {
                if (alias.paneID == replica->paneID)
                    alias.effectReceipt = sharedReceipt;
            }
            if (!packet->remainingRecords)
                owesAck(*replica, now);
            const auto handle = replica->handle;
            const auto paneID = replica->paneID;
            // The packet and its PCM remain owned and charged after removing
            // the queue entry. A callback may close or abandon the connection.
            DeferredEffect delivering = std::move(*entry);
            const auto deliveringBytes = delivering.size + delivering.clipboard.size;
            m_deferredEffectBytes -= deliveringBytes;
            m_deliveringEffectBytes = deliveringBytes;
            m_deferredEffects.erase(entry);
            if (*accepted == EffectReception::Record && packet->record->sequence > previousReceipt.consumed && m_delegate) {
                const auto disposition = effectDisposition(*packet->record, m_identity.clientInstance, m_identity.kind,
                    m_welcome ? m_welcome->grantedRole : Role::Viewer);
                m_delegate->paneEffect(handle, *packet->record, *body, disposition);
            }
            if (prepared.sample && m_state == ConnectionState::Attached) {
                if (auto* current = pane(handle); current && current->paneID == paneID) {
                    auto& slot = current->samples[prepared.sampleSlot - 1];
                    if (slot.identity == prepared.sample->sample && !slot.incoming && !slot.pcm)
                        slot.pcm = std::move(prepared.sample);
                }
            }
            m_deliveringSampleBytes = 0;
            m_deliveringClipboardBytes = 0;
            m_deliveringEffectBytes = 0;
            didConsume = true;
            break; // All pane and queue references are reacquired after callback.
        }
        if (!didConsume)
            break;
    }
    m_isDrainingEffects = false;
}

std::size_t ClientConnection::inboundEffectBytes() const
{
    std::size_t bytes = m_deferredEffectBytes + m_deliveringEffectBytes + m_deliveringSampleBytes + m_deliveringClipboardBytes;
    for (const auto& replica : m_panes) {
        for (const auto& sample : replica.samples) {
            if (sample.incoming)
                bytes += sample.incoming->heldBytes();
            if (sample.pcm)
                bytes += sample.pcm->view().size_bytes();
        }
    }
    return bytes;
}

void ClientConnection::abandonEffects(std::uint32_t handle)
{
    if (auto* replica = pane(handle)) {
        for (auto& sample : replica->samples) {
            if (sample.transferID)
                m_reassembler.abandon(sample.transferID);
            sample = {};
        }
    }
    std::erase_if(m_deferredEffects, [&](const DeferredEffect& effect) {
        if (effect.handle != handle)
            return false;
        if (effect.clipboardTransfer)
            m_reassembler.abandon(effect.clipboardTransfer);
        m_deferredEffectBytes -= effect.size + effect.clipboard.size;
        return true;
    });
}

void ClientConnection::receiveSampleHead(std::uint32_t handle, std::span<const std::uint8_t> body, std::uint64_t now)
{
    const auto head = decodeSampleHead(body);
    PaneReplica* replica = pane(handle);
    if (!head || !replica) {
        close(replica ? ByeReason::Malformed : ByeReason::UnboundHandle, "an unreadable SampleHead or an unbound pane");
        return;
    }
    auto& sample = replica->samples[head->slot - 1];
    if (sample.incoming) {
        close(ByeReason::Malformed, "a SampleHead replaced a sample still in transfer");
        return;
    }
    // A host cannot replace PCM still named by a queued sound. A completed
    // batch ACK is its permission to replace that slot, not a screen ACK.
    for (const auto& held : m_deferredEffects) {
        if (held.handle != handle)
            continue;
        const auto packet = decodeEffectPacket(held.paneID, { held.bytes.get(), held.size });
        if (!packet || !packet->record || packet->record->kind != EffectKind::Sound)
            continue;
        const auto decoded = decodeEffectBody(EffectKind::Sound, packet->record->payload);
        const auto* sound = decoded ? std::get_if<SoundEffect>(&*decoded) : nullptr;
        if (sound && sound->request.wave == 6 && sound->request.sampleSlot == head->slot
            && sound->sample != head->sample) {
            close(ByeReason::Malformed, "a SampleHead replaced PCM owed to an unconsumed sound");
            return;
        }
    }
    sample = {};
    sample.identity = head->sample;
    if (!head->sample.frames) {
        drainEffects(now);
        return;
    }
    if (!m_reassembler.expect(head->transfer, 0)) {
        close(ByeReason::Malformed, "a SampleHead reused a transfer identity");
        return;
    }
    const auto retained = inboundImageBytes() + inboundEffectBytes();
    if (retained > maxCatalogMessageBytes) {
        close(ByeReason::TransferRefused, "sample receive capacity is exhausted");
        return;
    }
    auto incoming = SampleTransfer::begin(*head, catalogRoom(retained));
    if (!incoming) {
        close(ByeReason::TransferRefused, "a sample allocation exceeded receive capacity");
        return;
    }
    sample.transferID = head->transfer;
    sample.incoming = std::move(*incoming);
}

bool ClientConnection::receiveSampleChunk(const BulkChunk& chunk, std::uint64_t now)
{
    for (auto& replica : m_panes) {
        for (auto& sample : replica.samples) {
            if (!sample.incoming || sample.transferID != chunk.transfer)
                continue;
            const auto result = sample.incoming->accept(chunk);
            if (result == SampleChunkResult::Refused) {
                close(ByeReason::TransferRefused, "a sample failed its exact size, ordering or digest check");
                return true;
            }
            if (result == SampleChunkResult::Pending)
                return true;
            sample.pcm = sample.incoming->take();
            sample.incoming.reset();
            sample.transferID = 0;
            m_reassembler.abandon(chunk.transfer);
            (void)m_reassembler.releaseFence(chunk.transfer);
            if (!sample.pcm) {
                close(ByeReason::Malformed, "a completed sample has no verified owner");
                return true;
            }
            drainEffects(now);
            return true;
        }
    }
    return false;
}

ClientConnection::EffectPayloadReadiness ClientConnection::prepareEffectPayload(PaneReplica& replica, const EffectView& record, EffectBody& body, PreparedEffect& prepared)
{
    if (auto* clipboard = std::get_if<ClipboardWriteEffect>(&body)) {
        if (!clipboard->payload.transfer)
            return decodeEffectClipboard(clipboard->payload.inlineBytes, maxEffectClipboardWriteBytes)
                ? EffectPayloadReadiness::Ready
                : EffectPayloadReadiness::Refused;
        const auto held = std::ranges::find_if(m_deferredEffects, [&](const DeferredEffect& queued) {
            return queued.handle == replica.handle && queued.paneID == replica.paneID
                && queued.clipboardTransfer == clipboard->payload.transfer;
        });
        if (held == m_deferredEffects.end())
            return EffectPayloadReadiness::Refused;
        if (!held->isClipboardComplete)
            return EffectPayloadReadiness::Pending;
        if (!decodeEffectClipboard(held->clipboard.view(), maxEffectClipboardWriteBytes))
            return EffectPayloadReadiness::Refused;
        m_deliveringClipboardBytes = held->clipboard.size;
        m_deferredEffectBytes -= held->clipboard.size;
        prepared.clipboard = std::move(held->clipboard);
        held->clipboard.size = 0;
        clipboard->payload.inlineBytes = prepared.clipboard.view();
        return EffectPayloadReadiness::Ready;
    }
    auto* sound = std::get_if<SoundEffect>(&body);
    if (!sound || sound->request.wave != 6 || !sound->sample.frames)
        return EffectPayloadReadiness::Ready;
    const auto disposition = effectDisposition(record, m_identity.clientInstance, m_identity.kind,
        m_welcome ? m_welcome->grantedRole : Role::Viewer);
    if (disposition != EffectDisposition::Post)
        return EffectPayloadReadiness::Ready;
    const auto index = sound->request.sampleSlot;
    if (!index || index > replica.samples.size())
        return EffectPayloadReadiness::Refused;
    auto& sample = replica.samples[index - 1];
    if (sample.identity != sound->sample)
        return EffectPayloadReadiness::Refused;
    if (sample.incoming)
        return EffectPayloadReadiness::Pending;
    if (!sample.pcm)
        return EffectPayloadReadiness::Refused;
    prepared.sample = std::move(sample.pcm);
    sample.pcm.reset();
    prepared.sampleSlot = index;
    const auto pcm = prepared.sample->view();
    sound->request.samplePCM = pcm.data();
    sound->request.sampleFrames = static_cast<std::uint32_t>(pcm.size());
    m_deliveringSampleBytes = pcm.size_bytes();
    return EffectPayloadReadiness::Ready;
}

bool ClientConnection::receiveClipboardChunk(const BulkChunk& chunk, std::uint64_t now)
{
    for (auto& held : m_deferredEffects) {
        if (!held.clipboardTransfer || held.clipboardTransfer != chunk.transfer)
            continue;
        if (held.isClipboardComplete || chunk.index != held.clipboardNextIndex || chunk.bytes.empty()
            || chunk.bytes.size() > bulkChunkPayloadBytes
            || chunk.compression != Compression::None || chunk.inflatedBytes
            || chunk.bytes.size() > held.clipboard.size - held.clipboardReceived) {
            close(ByeReason::TransferRefused, "a clipboard chunk violated its exact admitted operation");
            return true;
        }
        std::copy(chunk.bytes.begin(), chunk.bytes.end(), held.clipboard.bytes.get() + held.clipboardReceived);
        held.clipboardReceived += chunk.bytes.size();
        ++held.clipboardNextIndex;
        if (!chunk.isLast && held.clipboardReceived < held.clipboard.size)
            return true;
        if (!chunk.isLast || held.clipboardReceived != held.clipboard.size) {
            close(ByeReason::TransferRefused, "a clipboard final marker disagreed with its admitted length");
            return true;
        }
        std::array<std::uint8_t, 32> digest {};
        Crypto::sha256(held.clipboard.view(), digest);
        if (digest != chunk.sha256
            || !decodeEffectClipboard(held.clipboard.view(), maxEffectClipboardWriteBytes)) {
            close(ByeReason::TransferRefused, "a clipboard failed its complete size, digest or MIME validation");
            return true;
        }
        held.isClipboardComplete = true;
        m_reassembler.abandon(chunk.transfer);
        (void)m_reassembler.releaseFence(chunk.transfer);
        drainEffects(now);
        return true;
    }
    return false;
}

std::size_t ClientConnection::inboundImageBytes() const
{
    std::size_t bytes = m_deliveringImageBytes;
    for (const auto& image : m_incomingImages) {
        if (image && image->pixels)
            bytes += image->pixels->heldBytes();
    }
    return bytes;
}

std::size_t ClientConnection::pendingImagePixelBytes(const Ubiquitty::Terminal& terminal) const
{
    std::size_t bytes = 0;
    for (const auto& image : m_incomingImages) {
        if (image && image->terminal == &terminal)
            bytes += image->destinationBytes;
    }
    return bytes;
}

void ClientConnection::abandonImages(std::uint32_t handle)
{
    for (auto& image : m_incomingImages) {
        if (!image || image->handle != handle)
            continue;
        m_reassembler.abandon(image->transfer);
        image.reset();
    }
    for (auto& wanted : m_wantedImages) {
        if (wanted && wanted->handle == handle)
            wanted.reset();
    }
}

void ClientConnection::requestMissingImages()
{
    if (m_state != ConnectionState::Attached)
        return;
    for (const auto& replica : m_panes) {
        for (const auto& offer : replica.imageOffers) {
            if (!offer)
                continue;
            const auto needed = replica.images->bytesToInstall(*offer);
            if (!needed || !*needed)
                continue;
            const auto pending = [&](const auto& entry) { return entry && entry->handle == replica.handle && entry->asset == *offer; };
            if (std::ranges::any_of(m_wantedImages, pending) || std::ranges::any_of(m_incomingImages, pending))
                continue;
            const auto count = [](const auto& entry) { return entry.has_value(); };
            if (std::ranges::count_if(m_wantedImages, count) + std::ranges::count_if(m_incomingImages, count)
                >= static_cast<std::ptrdiff_t>(m_wantedImages.size()))
                return;
            if (!admitsImageDestination(replica, *needed, pendingImagePixelBytes(*replica.terminal)))
                continue;
            const auto assemblyBytes = ImageTransfer::assemblyBytes(*offer, imageCoding());
            const auto retained = inboundImageBytes() + inboundEffectBytes();
            if (!assemblyBytes || retained > maxCatalogMessageBytes || *assemblyBytes > catalogRoom(retained))
                continue;
            auto free = std::ranges::find_if(m_wantedImages, [](const auto& entry) { return !entry; });
            if (free == m_wantedImages.end())
                return;
            std::vector<std::uint8_t> body;
            encode(body, ImageWant { .hash = offer->hash });
            send(FrameKind::ImageWant, replica.handle, body);
            *free = WantedImage { .handle = replica.handle, .asset = *offer };
        }
    }
}

void ClientConnection::receiveImageHead(std::uint32_t handle, std::span<const std::uint8_t> body)
{
    const auto head = decodeImageHead(body);
    PaneReplica* replica = pane(handle);
    if (!head || !replica) {
        close(replica ? ByeReason::Malformed : ByeReason::UnboundHandle, "an unreadable ImageHead or one for an unbound pane");
        return;
    }
    const auto needed = replica->images->bytesToInstall(head->asset);
    if (!needed) {
        close(ByeReason::Malformed, "an ImageHead whose asset is not in the pane's image bindings");
        return;
    }
    auto offer = std::ranges::find(replica->imageOffers, std::optional { head->asset });
    if (offer == replica->imageOffers.end())
        offer = std::ranges::find_if(replica->imageOffers, [](const auto& entry) { return !entry; });
    if (offer == replica->imageOffers.end()) {
        close(ByeReason::Malformed, "an ImageHead past the pane's image table");
        return;
    }
    *offer = head->asset;
    if (!head->transfer) {
        requestMissingImages();
        return;
    }
    for (auto& wanted : m_wantedImages) {
        if (wanted && wanted->handle == handle && wanted->asset == head->asset)
            wanted.reset();
    }
    // Reserve the shared transfer ID before accepting any bytes. Its empty
    // generic entry prevents collision with checkpoint/history/result heads;
    // image pixels themselves never enter the growing generic byte vector.
    if (!m_reassembler.expect(head->transfer, 0)) {
        close(ByeReason::Malformed, "an ImageHead naming a transfer already announced or fenced");
        return;
    }
    auto slot = std::ranges::find_if(m_incomingImages, [](const auto& entry) { return !entry; });
    const auto count = [](const auto& entry) { return entry.has_value(); };
    if (slot == m_incomingImages.end()
        || std::ranges::count_if(m_wantedImages, count) + std::ranges::count_if(m_incomingImages, count)
            >= static_cast<std::ptrdiff_t>(m_incomingImages.size())) {
        close(ByeReason::TransferRefused, "too many simultaneous image transfers");
        return;
    }
    const auto discard = [&] {
        m_reassembler.abandon(head->transfer);
        slot->emplace(IncomingImage { .handle = handle, .transfer = head->transfer, .asset = head->asset, .terminal = replica->terminal, .destinationBytes = 0, .pixels = std::nullopt });
    };
    if (!*needed || std::ranges::any_of(m_incomingImages, [&](const auto& entry) { return entry && entry->handle == handle && entry->asset == head->asset; })) {
        discard();
        return;
    }
    const auto retained = inboundImageBytes() + inboundEffectBytes();
    if (retained > maxCatalogMessageBytes) {
        discard();
        return;
    }
    if (!admitsImageDestination(*replica, *needed, pendingImagePixelBytes(*replica->terminal))) {
        if (!replica->images->retireRetainedPictures() || !admitsImageDestination(*replica, *needed, pendingImagePixelBytes(*replica->terminal))) {
            discard();
            return;
        }
        if (m_delegate)
            m_delegate->paneChanged(handle, replica->paneID);
    }
    const auto assemblyBytes = ImageTransfer::assemblyBytes(head->asset, imageCoding());
    const auto room = catalogRoom(retained);
    if (!assemblyBytes || *assemblyBytes > room) {
        discard();
        return;
    }
    auto pixels = ImageTransfer::begin(*head, room, imageCoding());
    if (!pixels) {
        close(ByeReason::TransferRefused, "an image pixel allocation was refused");
        return;
    }
    slot->emplace(IncomingImage { .handle = handle, .transfer = head->transfer, .asset = head->asset, .terminal = replica->terminal, .destinationBytes = *needed, .pixels = std::move(*pixels) });
}

void ClientConnection::receiveImagePlacements(std::uint32_t handle, std::span<const std::uint8_t> body)
{
    const auto state = decodeImagePlacements(body);
    PaneReplica* replica = pane(handle);
    if (!state || !replica) {
        close(replica ? ByeReason::Malformed : ByeReason::UnboundHandle, "unreadable ImagePlacements or an unbound pane");
        return;
    }
    if (state->revision <= replica->images->revision())
        return;
    const auto references = [&](const ImageAssetIdentity& asset) {
        return std::ranges::any_of(std::span(state->images).first(state->imageCount), [&](const ImageBinding& binding) { return binding.asset == asset; });
    };
    for (auto& image : m_incomingImages) {
        if (image && image->handle == handle && !references(image->asset)) {
            m_reassembler.abandon(image->transfer);
            image->pixels.reset();
            image->destinationBytes = 0;
        }
    }
    for (auto& wanted : m_wantedImages) {
        if (wanted && wanted->handle == handle && !references(wanted->asset))
            wanted.reset();
    }
    for (auto& offer : replica->imageOffers) {
        if (offer && !references(*offer))
            offer.reset();
    }
    if (!replica->images->replace(*state, pendingImagePixelBytes(*replica->terminal))) {
        close(ByeReason::Malformed, "an invalid image binding or placement set");
        return;
    }
    refreshImageReservations(*replica);
    (void)retireRetainedPicturesForRoom(*replica);
    requestMissingImages();
    if (m_delegate)
        m_delegate->paneChanged(handle, replica->paneID);
}

bool ClientConnection::retireRetainedPicturesForRoom(PaneReplica& replica)
{
    for (const auto& asset : replica.images->missing()) {
        if (std::ranges::any_of(m_incomingImages, [&](const auto& image) { return image && image->handle == replica.handle && image->asset == asset && image->pixels; }))
            continue;
        const auto needed = replica.images->bytesToInstall(asset);
        if (needed && *needed && !admitsImageDestination(replica, *needed, pendingImagePixelBytes(*replica.terminal)))
            return replica.images->retireRetainedPictures();
    }
    return false;
}

void ClientConnection::refreshImageReservations(PaneReplica& replica)
{
    // Changed binding counts or a new terminal budget may change the
    // destination reservation, though the immutable asset in flight is still
    // the right one. A mere move/crop keeps it rather than restarting the
    // pixel transfer.
    for (auto& image : m_incomingImages) {
        if (!image || !image->pixels || image->handle != replica.handle)
            continue;
        const auto needed = replica.images->bytesToInstall(image->asset);
        const auto other = pendingImagePixelBytes(*replica.terminal) - image->destinationBytes;
        // The transfer's own reservation leaves the connection-wide sum while
        // it is reconsidered.
        image->destinationBytes = 0;
        if (!needed || !*needed || !admitsImageDestination(replica, *needed, other)) {
            m_reassembler.abandon(image->transfer);
            image->pixels.reset();
            image->destinationBytes = 0;
        } else {
            image->destinationBytes = *needed;
        }
    }
}

bool ClientConnection::receiveImageChunk(const BulkChunk& chunk)
{
    const auto slot = std::ranges::find_if(m_incomingImages, [&](const auto& entry) { return entry && entry->transfer == chunk.transfer; });
    if (slot == m_incomingImages.end())
        return false;
    if (!(*slot)->pixels) {
        // The owner was refused or retired, but the fixed metadata slot stays
        // until the old stream has really finished. A different completion
        // must not ask for this asset while its old transfer is still queued.
        if (chunk.compression != (chunk.isLast ? imageCoding() : Compression::None) || m_reassembler.accept(chunk) != ChunkOutcome::Discarded) {
            close(ByeReason::Malformed, "an invalid discarded image chunk");
            return true;
        }
        if (chunk.isLast) {
            slot->reset();
            requestMissingImages();
        }
        return true;
    }
    const auto result = (*slot)->pixels->accept(chunk);
    if (result == ImageChunkResult::Pending)
        return true;
    if (result == ImageChunkResult::Refused) {
        close(ByeReason::TransferRefused, "an image transfer failed its size, ordering, inflation or digest check");
        return true;
    }
    const auto handle = (*slot)->handle;
    auto pixels = (*slot)->pixels->take();
    m_deliveringImageBytes = pixels ? pixels->size : 0;
    slot->reset();
    m_reassembler.abandon(chunk.transfer);
    (void)m_reassembler.releaseFence(chunk.transfer);
    PaneReplica* replica = pane(handle);
    if (!pixels || !replica) {
        m_deliveringImageBytes = 0;
        close(ByeReason::Malformed, "a completed image with no owner or bound pane");
        return true;
    }
    const auto asset = pixels->asset;
    const auto applied = replica->images->accept(std::move(*pixels), pendingImagePixelBytes(*replica->terminal));
    m_deliveringImageBytes = 0;
    if (applied == ImagePixelsResult::Malformed) {
        close(ByeReason::Malformed, "image pixels do not match their content identity");
        return true;
    }
    if (applied == ImagePixelsResult::NoSpace) {
        // Allocation failure is not a busy retry loop. A later image record
        // can offer this asset again after the destination budget changes.
        for (auto& offer : replica->imageOffers) {
            if (offer == std::optional { asset })
                offer.reset();
        }
    }
    requestMissingImages();
    if (applied == ImagePixelsResult::Applied && m_delegate)
        m_delegate->paneChanged(handle, replica->paneID);
    return true;
}

void ClientConnection::handleUpdate(PaneReplica& replica, std::span<const std::uint8_t> body, std::uint64_t now)
{
    const std::optional<DecodedUpdateHeader> header = decodeUpdateHeader(body);
    if (!header) {
        close(ByeReason::Malformed, "an unreadable Update");
        return;
    }
    if (replica.pending) {
        if (header->header.generation <= replica.pending->generation) {
            ++m_counters.updatesIgnored;
            return;
        }
        if (replica.bufferedBytes + body.size() > m_policy.maxBufferedUpdateBytes) {
            replica.buffered.clear();
            replica.bufferedBytes = 0;
            requestResync(replica, ResyncReason::BufferOverflow);
            return;
        }
        replica.buffered.emplace_back(body.begin(), body.end());
        replica.bufferedBytes += body.size();
        return;
    }
    if (!replica.hasCheckpoint || replica.isResyncOutstanding) {
        ++m_counters.updatesIgnored;
        return;
    }
    applyUpdate(replica, *header, body, now);
}

void ClientConnection::noteReceipt(PaneReplica& replica, const Receipt& receipt)
{
    replica.receipt.inputWritten = std::max(replica.receipt.inputWritten, receipt.inputWritten);
    replica.receipt.inputSettled = std::max(replica.receipt.inputSettled, receipt.inputSettled);
}

std::optional<std::uint64_t> ClientConnection::sendInput(FrameKind kind, std::uint32_t handle, const std::function<void(std::vector<std::uint8_t>&, std::uint64_t)>& encodeBody)
{
    const PaneReplica* replica = pane(handle);
    if (!replica)
        return std::nullopt;
    const std::uint64_t inputSeq = replica->nextInputSeq;
    if (!sendInputNumbered(kind, handle, inputSeq, encodeBody))
        return std::nullopt;
    return inputSeq;
}

bool ClientConnection::sendInputNumbered(FrameKind kind, std::uint32_t handle, std::uint64_t inputSeq, const std::function<void(std::vector<std::uint8_t>&, std::uint64_t)>& encodeBody)
{
    PaneReplica* replica = pane(handle);
    if (!replica || inputSeq == 0 || m_state != ConnectionState::Attached)
        return false;
    std::vector<std::uint8_t> body;
    encodeBody(body, inputSeq);
    if (body.size() > maxFrameBodyBytes)
        return false;
    send(kind, handle, body);
    replica->nextInputSeq = std::max(replica->nextInputSeq, inputSeq + 1);
    return true;
}

std::optional<EchoContext> ClientConnection::echoContext(std::uint32_t handle) const
{
    const auto found = std::ranges::find(m_panes, handle, &PaneReplica::handle);
    return found == m_panes.end() ? std::nullopt : found->replica.echoContext;
}

std::vector<std::pair<std::uint32_t, ID128>> ClientConnection::boundPanes() const
{
    std::vector<std::pair<std::uint32_t, ID128>> bound;
    bound.reserve(m_panes.size());
    for (const PaneReplica& replica : m_panes)
        bound.emplace_back(replica.handle, replica.paneID);
    return bound;
}

bool ClientConnection::sendNumbered(std::uint32_t handle, std::uint64_t inputSeq, const InputIntent& intent)
{
    // A whole paste, its one piece final from offset 0, is sent from its
    // start, and the host takes what it has not.
    if (const auto* paste = std::get_if<PasteInput>(&intent))
        return paste->end == PasteEnd::Final && paste->offset == 0 && queuePaste(handle, inputSeq, { reinterpret_cast<const char*>(paste->text.data()), paste->text.size() });
    if (const auto* raw = std::get_if<RawInput>(&intent))
        return !raw->bytes.empty() && sendInputNumbered(FrameKind::Raw, handle, inputSeq, [&](std::vector<std::uint8_t>& body, std::uint64_t seq) { encode(body, Raw { .inputSeq = seq, .bytes = raw->bytes }); });
    if (const auto* keys = std::get_if<KeyInput>(&intent)) {
        if (keys->events.empty() || keys->events.size() > maxInputEventsPerFrame)
            return false;
        return sendInputNumbered(FrameKind::Keys, handle, inputSeq, [&](std::vector<std::uint8_t>& body, std::uint64_t seq) { encode(body, Keys { .inputSeq = seq, .events = keys->events }); });
    }
    if (const auto* pointer = std::get_if<PointerInput>(&intent)) {
        if (pointer->events.empty() || pointer->events.size() > maxInputEventsPerFrame)
            return false;
        return sendInputNumbered(FrameKind::Pointer, handle, inputSeq, [&](std::vector<std::uint8_t>& body, std::uint64_t seq) { encode(body, Pointer { .inputSeq = seq, .events = pointer->events }); });
    }
    return false;
}

std::optional<std::uint64_t> ClientConnection::sendRaw(std::uint32_t handle, std::span<const std::uint8_t> bytes)
{
    // Nothing to write is nothing to send: the host refuses an empty Raw as
    // malformed, so none leaves here and no number is spent on it.
    if (bytes.empty())
        return std::nullopt;
    if (isPasting(handle) && std::ranges::any_of(bytes, [characters = interruptCharacters(handle)](std::uint8_t byte) { return characters.contains(byte); }))
        (void)abortPaste(handle);
    return sendInput(FrameKind::Raw, handle, [&](std::vector<std::uint8_t>& body, std::uint64_t inputSeq) { encode(body, Raw { .inputSeq = inputSeq, .bytes = bytes }); });
}

std::optional<std::uint64_t> ClientConnection::sendKeys(std::uint32_t handle, std::span<const KeyEvent> events)
{
    if (events.empty() || events.size() > maxInputEventsPerFrame)
        return std::nullopt;
    if (isPasting(handle) && std::ranges::any_of(events, [characters = interruptCharacters(handle)](const KeyEvent& event) { return isInterruptKey(event, characters); }))
        (void)abortPaste(handle);
    return sendInput(FrameKind::Keys, handle, [&](std::vector<std::uint8_t>& body, std::uint64_t inputSeq) { encode(body, Keys { .inputSeq = inputSeq, .events = { events.begin(), events.end() } }); });
}

std::optional<std::uint64_t> ClientConnection::sendPointer(std::uint32_t handle, std::span<const PointerEvent> events)
{
    if (events.empty() || events.size() > maxInputEventsPerFrame)
        return std::nullopt;
    return sendInput(FrameKind::Pointer, handle, [&](std::vector<std::uint8_t>& body, std::uint64_t inputSeq) { encode(body, Pointer { .inputSeq = inputSeq, .events = { events.begin(), events.end() } }); });
}

bool ClientConnection::sendNativeDragEvent(std::uint32_t handle, const DragDrop::NativeEvent& event, std::span<const char> payload)
{
    const auto epoch = m_nativeDragEpochs.find(handle);
    if ((m_output.size() > (8u << 20) && event.kind != DragDrop::EventKind::Cancel) || epoch == m_nativeDragEpochs.end() || !pane(handle) || m_state != ConnectionState::Attached
        || m_identity.kind != ClientKind::Human || !m_welcome
        || !admitsSender(FrameKind::NativeDragEvent, m_welcome->grantedRole))
        return false;
    std::vector<std::uint8_t> bytes;
    if (!encodeNativeDragEvent(bytes, epoch->second, event, payload))
        return false;
    send(FrameKind::NativeDragEvent, handle, bytes);
    return true;
}

bool ClientConnection::sendFocus(std::uint32_t handle, Ubiquitty::Modes::FocusState focus)
{
    if (!pane(handle) || m_state != ConnectionState::Attached)
        return false;
    std::vector<std::uint8_t> body;
    encode(body, Focus { .isFocused = focus == Ubiquitty::Modes::FocusState::Focused });
    send(FrameKind::Focus, handle, body);
    return true;
}

bool ClientConnection::sendViewport(std::uint32_t handle, const Viewport& viewport)
{
    if (!pane(handle) || m_state != ConnectionState::Attached || m_identity.kind != ClientKind::Human
        || !m_welcome || !admitsSender(FrameKind::Viewport, m_welcome->grantedRole)
        || viewport.rows == 0 || viewport.columns == 0)
        return false;
    std::vector<std::uint8_t> body;
    encode(body, viewport);
    send(FrameKind::Viewport, handle, body);
    return true;
}

std::vector<InputDiscarded> ClientConnection::takeInputDiscards(std::uint32_t handle)
{
    const auto found = std::ranges::find(m_panes, handle, &PaneReplica::handle);
    return found == m_panes.end() ? std::vector<InputDiscarded> {} : std::exchange(found->discards, {});
}

Receipt ClientConnection::receipt(std::uint32_t handle) const
{
    const auto found = std::ranges::find(m_panes, handle, &PaneReplica::handle);
    return found == m_panes.end() ? Receipt {} : found->receipt;
}

void ClientConnection::applyUpdate(PaneReplica& replica, const DecodedUpdateHeader& decoded, std::span<const std::uint8_t> body, std::uint64_t now)
{
    const UpdateHeader& header = decoded.header;
    // The report is the host's whatever becomes of the picture.
    noteReceipt(replica, { .inputWritten = header.inputWritten, .inputSettled = header.inputSettled });
    const std::uint64_t held = replica.replica.generation;
    if (header.generation <= held || replica.isResyncOutstanding) {
        ++m_counters.updatesIgnored;
        return;
    }
    if (header.mode == UpdateMode::Chain ? header.baseGeneration != held : header.baseGeneration > held) {
        requestResync(replica, ResyncReason::BaseMissing);
        return;
    }
    std::vector<std::uint64_t> refusedRowHashes;
    const std::span<const std::uint8_t> rowUpdate = body.subspan(decoded.bytes);
    const std::uint64_t activityGeneration = replica.terminal->activityGeneration;
    const ApplyOutcome outcome = applyRowUpdate(*replica.terminal, rowUpdate, replica.replica, &refusedRowHashes);
    if (replica.fixture && !replica.fixture->hasDiverged && (outcome == ApplyOutcome::Applied || outcome == ApplyOutcome::DigestMismatch)) {
        DivergenceFixture& fixture = *replica.fixture;
        // The update that diverged is kept past the bound: without it the
        // fixture replays nothing worth having.
        if (outcome == ApplyOutcome::DigestMismatch || fixture.updateBytes + rowUpdate.size() <= m_policy.maxBufferedUpdateBytes) {
            fixture.updates.emplace_back(rowUpdate.begin(), rowUpdate.end());
            fixture.updateBytes += rowUpdate.size();
        } else {
            fixture.updates.clear();
            fixture.updateBytes = 0;
            fixture.isIncomplete = true;
        }
        fixture.hasDiverged = outcome == ApplyOutcome::DigestMismatch;
    }
    switch (outcome) {
    case ApplyOutcome::Applied:
        // An update that carried the status put the screen's in the terminal.
        if (replica.terminal->activityGeneration != activityGeneration)
            replica.screenStatus.reset();
        replica.replica.generation = header.generation;
        replica.replica.historyHead = header.historyHead;
        replica.lagMicroseconds = header.lagMicroseconds;
        replica.frameClass = header.frameClass;
        // The update names the head its appends reached; the lines below it
        // arrived ahead of it on the stream (Docs/sync.md §7.8).
        replica.history->setHead(replica.history->key().epoch, header.historyHead);
        replica.history->project(*replica.terminal, replica.terminal->columns);
        ++m_counters.updatesApplied;
        noteHostTime(header.hostTime, now);
        owesAck(replica, now);
        if (m_delegate)
            m_delegate->paneChanged(replica.handle, replica.paneID);
        return;
    case ApplyOutcome::DigestMismatch:
        ++m_counters.digestMismatches;
        requestResync(replica, ResyncReason::DigestMismatch, header.generation, refusedRowHashes);
        return;
    case ApplyOutcome::Malformed:
    case ApplyOutcome::AllocationRefused:
        requestResync(replica, ResyncReason::DecodeFailed);
        return;
    case ApplyOutcome::BaseMissing:
        requestResync(replica, ResyncReason::BaseMissing);
        return;
    }
}

void ClientConnection::applyCheckpointBody(PaneReplica& replica, std::span<const std::uint8_t> body, std::uint64_t now)
{
    const PendingCheckpoint pending = *replica.pending;
    replica.pending.reset();
    // Validation and external history retirement precede any destination
    // mutation. A body that is not the state its head named cannot retire
    // the old epoch or replace the visible picture.
    const bool isHeadsBody = leadingGeneration(body) == pending.generation && trailingDigest(body) == pending.stateDigest;
    std::expected<PreparedCheckpoint, ApplyOutcome> prepared = std::unexpected(ApplyOutcome::Malformed);
    // Preparation is charged at its whole allowance before it allocates: the
    // owner then cannot be overcommitted by what the plan has yet to count.
    refreshReceiveCharges();
    MemoryCharge preparation(m_receiveMemory);
    const bool isPreparationAdmitted = preparation.resize(m_policy.maxCheckpointStorageCost);
    if (isHeadsBody && !isPreparationAdmitted)
        prepared = std::unexpected(ApplyOutcome::AllocationRefused);
    else if (isHeadsBody)
        prepared = prepareCheckpoint(body, *replica.terminal,
            { .maxDecodedScreenCellBytes = m_policy.maxDecodedScreenCellBytes, .maxStorageCost = m_policy.maxCheckpointStorageCost });
    if (prepared && (prepared->generation() != pending.generation || prepared->stateDigest() != pending.stateDigest))
        prepared = std::unexpected(ApplyOutcome::Malformed);
    // The picture's grid may grow to the checkpoint's geometry only while
    // every retained replica still fits the client's replica allowance.
    bool isReplicaAdmitted = true;
    if (prepared && replica.terminal->grid) {
        const auto grown = replica.terminal->grid->backingStoragePlan(prepared->rows(), prepared->columns());
        const auto current = replica.terminal->grid->backingStoragePlan(replica.terminal->rows, replica.terminal->columns);
        if (grown && current && grown->retainedBytes > current->retainedBytes) {
            const std::size_t growth = grown->retainedBytes - current->retainedBytes;
            const std::size_t held = retainedReplicaBytes();
            isReplicaAdmitted = held <= m_policy.maxRetainedReplicaBytes && growth <= m_policy.maxRetainedReplicaBytes - held;
        }
        if (!grown || !isReplicaAdmitted)
            prepared = std::unexpected(ApplyOutcome::AllocationRefused);
    }
    if (prepared) {
        const auto reserved = reserveCheckpointDestination(*replica.terminal, *prepared);
        if (reserved != ApplyOutcome::Applied)
            prepared = std::unexpected(reserved);
    }
    if (prepared && prepared->historyEpoch() > replica.history->key().epoch
        && retireHistory(replica, HistoryReset { .newEpoch = prepared->historyEpoch(), .clearedThrough = 0, .historyHead = prepared->historyHead() }) != HistoryPurgeOutcome::Purged) {
        replica.buffered.clear();
        replica.bufferedBytes = 0;
        return;
    }
    ScreenReplica candidate;
    const ApplyOutcome outcome = prepared ? commitCheckpoint(*replica.terminal, std::move(*prepared), candidate) : prepared.error();
    // The host answers every program query (Docs/sync.md §3); the terminal
    // holding its picture answers none.
    if (outcome == ApplyOutcome::Applied)
        replica.terminal->replyAuthority = ReplyAuthority::Elsewhere;
    preparation.settle(0);
    if (outcome != ApplyOutcome::Applied || candidate.generation != pending.generation || candidate.stateDigest != pending.stateDigest) {
        if (outcome == ApplyOutcome::DigestMismatch)
            ++m_counters.digestMismatches;
        // What the pane says once the refusals pass their cap. The handshake
        // admitted only this build's layouts, so a body that does not decode
        // or does not hash to its head was damaged on the way, and says so
        // apart from a body that does not fit.
        const std::string_view reason = !isHeadsBody     ? paneContentsMismatchedNotice
            : outcome == ApplyOutcome::DigestMismatch    ? paneContentsDamagedNotice
            : !isPreparationAdmitted                     ? "This pane is too large to prepare within this connection’s memory limit, so it stopped updating."
            : !isReplicaAdmitted                         ? "This pane is too large to keep within this app’s memory limit, so it stopped updating."
            : outcome == ApplyOutcome::AllocationRefused ? "This app ran out of memory showing this pane, so it stopped updating. Reconnect to try again."
                                                         : paneContentsUnreadableNotice;
        refuseCheckpoint(replica, reason, now);
        return;
    }
    replica.refusedCheckpoints = 0;
    replica.resyncDueAt.reset();
    // The checkpoint names the history's epoch and head. One of a later epoch
    // than the cache's completes the retirement confirmed before commit;
    // one of an earlier epoch was taken before a clear the client has
    // already applied, and leaves the epoch where it is.
    if (candidate.historyEpoch > replica.history->key().epoch)
        purgeHistory(replica, HistoryReset { .newEpoch = candidate.historyEpoch, .clearedThrough = 0, .historyHead = candidate.historyHead });
    replica.history->setHead(candidate.historyEpoch, candidate.historyHead);
    replica.history->invalidateProjection();
    replica.history->project(*replica.terminal, replica.terminal->columns);
    replica.replica = std::move(candidate);
    replica.screenStatus.reset();
    replica.hasCheckpoint = true;
    replica.isResyncOutstanding = false;
    replica.frameClass = FrameClass::Structural;
    replica.lagMicroseconds = 0;
    ++m_counters.checkpointsApplied;
    if (m_policy.shouldRetainDivergenceFixture)
        replica.fixture = DivergenceFixture { .checkpoint = std::vector<std::uint8_t>(body.begin(), body.end()), .updates = {}, .updateBytes = 0, .hasDiverged = false, .isIncomplete = false };
    owesAck(replica, now);
    m_isAckImmediate = true;
    if (m_delegate)
        m_delegate->paneChanged(replica.handle, replica.paneID);
    if (m_state == ConnectionState::Closed)
        return;
    std::deque<std::vector<std::uint8_t>> buffered = std::exchange(replica.buffered, {});
    replica.bufferedBytes = 0;
    for (const std::vector<std::uint8_t>& update : buffered) {
        const std::optional<DecodedUpdateHeader> header = decodeUpdateHeader(update);
        if (header)
            applyUpdate(replica, *header, update, now);
        if (m_state == ConnectionState::Closed)
            return;
    }
}

void ClientConnection::refuseCheckpoint(PaneReplica& replica, std::string_view reason, std::uint64_t now)
{
    replica.buffered.clear();
    replica.bufferedBytes = 0;
    // A refused checkpoint answers any Resync outstanding, so the next
    // one is asked for anew: at once the first time, then after a wait
    // that doubles, and not at all past the cap, so a body this client
    // cannot take is never a loop.
    replica.isResyncOutstanding = false;
    ++replica.refusedCheckpoints;
    ++m_counters.checkpointsRefused;
    if (replica.refusedCheckpoints >= m_policy.maxCheckpointRefusals) {
        failPane(replica, reason);
        return;
    }
    if (replica.refusedCheckpoints == 1) {
        requestResync(replica, ResyncReason::CheckpointRefused);
        return;
    }
    const std::uint32_t doublings = std::min<std::uint32_t>(replica.refusedCheckpoints - 2, 32);
    const std::uint64_t wait = std::min(m_policy.checkpointRetryMicroseconds << doublings, m_policy.maxCheckpointRetryMicroseconds);
    replica.resyncDueAt = now + wait;
}

void ClientConnection::refuseOverBudget(std::uint32_t transfer, std::uint64_t now)
{
    ++m_counters.transfersOverBudget;
    if (const auto owner = m_transferPanes.find(transfer); owner != m_transferPanes.end()) {
        PaneReplica* replica = pane(owner->second);
        m_transferPanes.erase(owner);
        if (replica && replica->pending && replica->pending->transfer == transfer) {
            replica->pending.reset();
            refuseCheckpoint(*replica, "This pane is too large to receive within this connection’s memory limit, so it stopped updating.", now);
        }
        return;
    }
    const std::string_view what = m_historyTransfers.contains(transfer) ? "a history transfer"
        : m_resultTransfers.contains(transfer)                          ? "a Result"
        : m_structureTransfers.contains(transfer)                       ? "a workspace structure"
                                                                        : "a bulk transfer";
    close(ByeReason::TransferRefused, std::string(what) + " did not fit the connection's receive memory limit");
}

std::size_t ClientConnection::installedImageBytes()
{
    m_distinctTerminals.clear();
    for (const PaneReplica& replica : m_panes) {
        if (replica.terminal)
            m_distinctTerminals.push_back(replica.terminal);
    }
    std::ranges::sort(m_distinctTerminals);
    const auto [first, last] = std::ranges::unique(m_distinctTerminals);
    m_distinctTerminals.erase(first, last);
    std::uint64_t bytes = 0;
    for (const Ubiquitty::Terminal* terminal : m_distinctTerminals)
        bytes += Image::totalBytes(terminal);
    for (const auto& image : m_incomingImages) {
        if (image)
            bytes += image->destinationBytes;
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(bytes, std::numeric_limits<std::size_t>::max()));
}

std::size_t ClientConnection::retainedReplicaBytes()
{
    if (m_replicas)
        return m_replicas->retainedBytes();
    m_distinctTerminals.clear();
    std::size_t bytes = 0;
    for (const PaneReplica& replica : m_panes) {
        if (replica.terminal)
            m_distinctTerminals.push_back(replica.terminal);
        // Without a set, each binding owns its cache.
        if (replica.history)
            bytes += replica.history->retainedStorageBytes();
    }
    std::ranges::sort(m_distinctTerminals);
    const auto [first, last] = std::ranges::unique(m_distinctTerminals);
    m_distinctTerminals.erase(first, last);
    for (const Ubiquitty::Terminal* terminal : m_distinctTerminals)
        bytes += retainedPictureBytes(*terminal);
    return bytes;
}

void ClientConnection::refreshReceiveCharges()
{
    m_installedImageCharge.settle(installedImageBytes());
    m_catalogCharge.settle(inboundImageBytes() + inboundEffectBytes());
}

std::size_t ClientConnection::catalogRoom(std::size_t retained)
{
    refreshReceiveCharges();
    if (retained > maxCatalogMessageBytes)
        return 0;
    return std::min(maxCatalogMessageBytes - retained, m_receiveMemory->availableBytes());
}

bool ClientConnection::admitsImageDestination(const PaneReplica& replica, std::size_t needed, std::size_t otherReserved)
{
    const auto held = Image::totalBytes(replica.terminal);
    const auto budget = replica.terminal->images.kittyImageBytesBudget;
    if (held > budget || otherReserved > budget - held || needed > budget - held - otherReserved)
        return false;
    // Summed over every pane, the reservation included once.
    const std::size_t installed = installedImageBytes();
    return installed <= m_policy.maxInstalledImageBytes && needed <= m_policy.maxInstalledImageBytes - installed;
}

std::optional<DivergenceFixture> ClientConnection::takeDivergenceFixture(std::uint32_t handle)
{
    PaneReplica* replica = pane(handle);
    if (!replica || !replica->fixture || !replica->fixture->hasDiverged)
        return std::nullopt;
    return std::exchange(replica->fixture, std::nullopt);
}

std::size_t ClientConnection::divergenceFixtureBytes() const
{
    std::size_t bytes = 0;
    for (const PaneReplica& replica : m_panes) {
        if (replica.fixture)
            bytes += replica.fixture->checkpoint.size() + replica.fixture->updateBytes;
    }
    return bytes;
}

void ClientConnection::requestResync(PaneReplica& replica, ResyncReason reason)
{
    const bool isAlternate = replica.terminal->screens.isUsingAlternateScreen;
    requestResync(replica, reason, replica.replica.generation, replica.replica.rowHashes()[isAlternate ? 1 : 0]);
}

void ClientConnection::failPane(PaneReplica& replica, std::string_view reason)
{
    replica.failure = std::string(reason);
    replica.resyncDueAt.reset();
    ++m_counters.panesFailed;
    // Nothing the screen plane sends the pane can apply now, so it is not
    // sent; the pane's records still are.
    setView(replica.handle, ViewState::RecordsOnly);
    if (m_delegate)
        m_delegate->paneFailed(replica.handle, replica.paneID, reason);
}

void ClientConnection::requestResync(PaneReplica& replica, ResyncReason reason, std::uint64_t generation, std::span<const std::uint64_t> rowHashes)
{
    if (replica.isResyncOutstanding || replica.failure || replica.resyncDueAt)
        return;
    replica.isResyncOutstanding = true;
    ++m_counters.resyncsSent;
    Resync resync;
    resync.reason = reason;
    resync.generation = generation;
    const std::span<const std::uint64_t> carried = rowHashes.first(std::min(rowHashes.size(), maxSectionHashes));
    resync.sectionHashes.assign(carried.begin(), carried.end());
    std::vector<std::uint8_t> body;
    encode(body, resync);
    send(FrameKind::Resync, replica.handle, body);
}

void ClientConnection::noteHostTime(std::uint64_t hostTime, std::uint64_t now)
{
    if (hostTime <= m_latestHostTime)
        return;
    m_latestHostTime = hostTime;
    m_latestHostTimeReceivedAt = now;
}

void ClientConnection::owesAck(PaneReplica& replica, std::uint64_t now)
{
    replica.isAckOwed = true;
    if (!m_ackOwedSince)
        m_ackOwedSince = now;
}

void ClientConnection::tick(std::uint64_t now)
{
    if (m_state != ConnectionState::Attached)
        return;
    drainEffects(now);
    if (m_state != ConnectionState::Attached)
        return;
    // A probe unanswered for the stall bound is a dead path. Input that no
    // receipt has covered for the bound draws one.
    if (m_stallProbeSentAt) {
        if (now - *m_stallProbeSentAt >= stallBoundMicroseconds()) {
            m_state = ConnectionState::Lost;
            for (auto& request : m_clipboardReads)
                request.reset();
            for (const auto& replica : m_panes) {
                abandonImages(replica.handle);
                abandonEffects(replica.handle);
            }
            return;
        }
    } else if (m_inputWaitingSince && now - *m_inputWaitingSince >= stallBoundMicroseconds()) {
        sendProbe(now);
        m_stallProbeSentAt = now;
    }
    for (PaneReplica& replica : m_panes) {
        if (replica.resyncDueAt && now >= *replica.resyncDueAt) {
            replica.resyncDueAt.reset();
            requestResync(replica, ResyncReason::CheckpointRefused);
        }
    }
    if (!m_ackOwedSince)
        return;
    if (!m_isAckImmediate && now - *m_ackOwedSince < ackIntervalMicroseconds())
        return;
    sendAck(now);
}

void ClientConnection::sendAck(std::uint64_t now)
{
    if (!m_ackOwedSince)
        return;
    Ack ack;
    ack.hostTimeEcho = m_latestHostTime;
    ack.holdMicroseconds = m_latestHostTime == 0 ? 0 : static_cast<std::uint32_t>(std::min<std::uint64_t>(now - m_latestHostTimeReceivedAt, 0xFFFF'FFFF));
    ack.bytesReceived = m_streamBytesReceived;
    m_streamBytesAcknowledged = m_streamBytesReceived;
    for (PaneReplica& replica : m_panes) {
        if (!replica.isAckOwed)
            continue;
        ack.panes.push_back({ .handle = replica.handle, .generation = replica.replica.generation, .historyAcked = replica.history->contiguousHead(), .effectsAcknowledged = replica.effects ? replica.effects->state().acknowledged : 0 });
        replica.isAckOwed = false;
    }
    std::vector<std::uint8_t> body;
    encode(body, ack);
    send(FrameKind::Ack, 0, body);
    ++m_counters.acksSent;
    m_ackOwedSince.reset();
    m_isAckImmediate = false;
}

std::optional<std::uint64_t> ClientConnection::nextTickMicroseconds() const
{
    if (m_state != ConnectionState::Attached)
        return std::nullopt;
    if (std::ranges::any_of(m_clipboardReads, [](const auto& held) { return held && held->hasAnswer && held->transfer; }))
        return 0; // One bounded chunk per owner drain, behind interactive output.
    std::optional<std::uint64_t> due;
    if (m_ackOwedSince)
        due = m_isAckImmediate ? *m_ackOwedSince : *m_ackOwedSince + ackIntervalMicroseconds();
    if (const std::optional<std::uint64_t> since = m_stallProbeSentAt ? m_stallProbeSentAt : m_inputWaitingSince) {
        const std::uint64_t stall = *since + stallBoundMicroseconds();
        due = due ? std::min(*due, stall) : stall;
    }
    for (const PaneReplica& replica : m_panes) {
        if (replica.resyncDueAt)
            due = due ? std::min(*due, *replica.resyncDueAt) : *replica.resyncDueAt;
    }
    return due;
}

void ClientConnection::noteInputSent(std::uint64_t now)
{
    if (m_state != ConnectionState::Attached)
        return;
    // The owed Ack goes with the input rather than on a timer of its own.
    sendAck(now);
    if (!m_inputWaitingSince)
        m_inputWaitingSince = now;
    if (!m_unreceiptedSince)
        m_unreceiptedSince = now;
}

void ClientConnection::checkLiveness(std::uint64_t now)
{
    // One at a time: a second suspicion while the first is unanswered is
    // answered by the first's verdict, and must not restart its owner's wait.
    if (m_state != ConnectionState::Attached || m_livenessProbe)
        return;
    std::vector<std::uint8_t> body;
    encode(body, Probe { .time = now });
    send(FrameKind::Probe, 0, body);
    m_livenessProbe = now;
    ++m_counters.probesSent;
}

void ClientConnection::settleInputWait()
{
    if (!m_unreceiptedSince && !m_inputWaitingSince)
        return;
    const bool isCovered = std::ranges::all_of(m_panes, [](const PaneReplica& replica) { return replica.nextInputSeq - 1 <= replica.receipt.inputWritten; });
    if (!isCovered)
        return;
    m_inputWaitingSince.reset();
    m_unreceiptedSince.reset();
}

LinkState ClientConnection::linkState(std::uint64_t now) const
{
    return judgeLink(linkFacts(), now);
}

std::optional<std::uint64_t> ClientConnection::nextLinkStallMicroseconds(std::uint64_t now) const
{
    return m_state == ConnectionState::Attached ? nextStallMicroseconds(linkFacts(), now) : std::nullopt;
}

LinkFacts ClientConnection::linkFacts() const
{
    LinkFacts facts { .lastHeardMicroseconds = m_lastHeardAt, .unreceiptedSinceMicroseconds = m_unreceiptedSince, .silenceBoundMicroseconds = std::nullopt, .stallBoundMicroseconds = stallBoundMicroseconds() };
    // The host probes a focused client with a Live pane on its schedule, so
    // a quiet host is silent at most the step it is at, the last repeating.
    const bool hasLivePane = std::ranges::any_of(m_panes, [](const PaneReplica& replica) { return replica.view == ViewState::Live; });
    if (m_welcome && m_identity.facts.isFocused && hasLivePane && m_welcome->limits.probeSteps > 0) {
        const std::size_t steps = std::min<std::size_t>(m_welcome->limits.probeSteps, maxProbeSteps);
        const std::uint64_t step = m_welcome->limits.probeScheduleMicroseconds[std::min(m_hostProbesSinceData, steps - 1)];
        facts.silenceBoundMicroseconds = step + stallBoundMicroseconds();
    }
    return facts;
}

std::uint64_t ClientConnection::ackIntervalMicroseconds() const
{
    const Facts& facts = m_identity.facts;
    const bool isMetered = facts.link != LinkCost::Unmetered;
    const std::uint64_t roundTrip = m_smoothedRoundTrip.value_or(0);
    std::uint64_t interval = isMetered
        ? std::clamp(roundTrip / 2, m_policy.minMeteredAckIntervalMicroseconds, std::max(m_policy.minMeteredAckIntervalMicroseconds, m_policy.maxMeteredAckIntervalMicroseconds))
        : std::clamp(roundTrip / 4, m_policy.minAckIntervalMicroseconds, std::max(m_policy.minAckIntervalMicroseconds, m_policy.maxAckIntervalMicroseconds));
    if (facts.power == PowerState::Saver)
        interval *= 2;
    return interval;
}

bool ClientConnection::sendControl(FrameKind kind, std::string_view text)
{
    if (!m_isGreetSent || isConsentPending(m_state) || m_state == ConnectionState::Closed || m_state == ConnectionState::Lost || text.size() > maxFrameBodyBytes
        || !isValidUTF8(std::span { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }))
        return false;
    send(kind, 0, std::span { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    return true;
}

bool ClientConnection::call(std::string_view request)
{
    return sendControl(FrameKind::Call, request);
}

bool ClientConnection::sendControlUploadChunk(const BulkChunk& chunk)
{
    if (m_state != ConnectionState::Attached || !chunk.transfer
        || chunk.bytes.empty() || chunk.bytes.size() > bulkChunkPayloadBytes
        || chunk.compression != Compression::None || chunk.inflatedBytes)
        return false;
    std::vector<std::uint8_t> body;
    encode(body, chunk);
    if (body.size() > maxFrameBodyBytes)
        return false;
    send(FrameKind::BulkChunk, 0, body);
    return true;
}

void ClientConnection::setFocus(Ubiquitty::Modes::FocusState focus)
{
    const bool isFocused = focus == Ubiquitty::Modes::FocusState::Focused;
    if (m_identity.facts.isFocused == isFocused)
        return;
    m_identity.facts.isFocused = isFocused;
    // The stream is ordered, so a `Facts` written after the `Greet` reaches the
    // host after its welcome, whether or not the welcome has come back.
    if (!m_isGreetSent || m_state == ConnectionState::Closed || m_state == ConnectionState::Lost)
        return;
    sendFacts();
}

void ClientConnection::setThemePalette(std::optional<ThemePalette> palette)
{
    if (m_identity.facts.themePalette == palette)
        return;
    m_identity.facts.themePalette = std::move(palette);
    // The base facts in Greet omit the palette; send it after admission.
    if (m_state == ConnectionState::Attached)
        sendFacts();
}

void ClientConnection::setWindowViewport(const Viewport& viewport)
{
    Facts& facts = m_identity.facts;
    if (facts.viewportRows == viewport.rows && facts.viewportColumns == viewport.columns && facts.cellPixelWidth == viewport.cellPixelWidth && facts.cellPixelHeight == viewport.cellPixelHeight
        && facts.textAreaPixelWidth == viewport.textAreaPixelWidth && facts.textAreaPixelHeight == viewport.textAreaPixelHeight)
        return;
    facts.viewportRows = viewport.rows;
    facts.viewportColumns = viewport.columns;
    facts.cellPixelWidth = viewport.cellPixelWidth;
    facts.cellPixelHeight = viewport.cellPixelHeight;
    facts.textAreaPixelWidth = viewport.textAreaPixelWidth;
    facts.textAreaPixelHeight = viewport.textAreaPixelHeight;
    if (!m_isGreetSent || m_state == ConnectionState::Closed || m_state == ConnectionState::Lost)
        return;
    sendFacts();
}

void ClientConnection::setLinkCost(LinkCost link)
{
    if (m_identity.facts.link == link)
        return;
    m_identity.facts.link = link;
    if (!m_isGreetSent || m_state == ConnectionState::Closed || m_state == ConnectionState::Lost)
        return;
    sendFacts();
}

void ClientConnection::sendFacts()
{
    std::vector<std::uint8_t> body;
    encode(body, m_identity.facts);
    send(FrameKind::Facts, 0, body);
}

void ClientConnection::sendProbe(std::uint64_t now)
{
    std::vector<std::uint8_t> body;
    encode(body, Probe { .time = now });
    std::vector<std::uint8_t> datagram;
    if (m_policy.datagramMaxBytes > 0 && appendFrame(datagram, FrameKind::Probe, 0, body) && datagram.size() <= m_policy.datagramMaxBytes)
        m_datagrams.push_back(std::move(datagram));
    else
        send(FrameKind::Probe, 0, body);
    m_outstandingProbe = now;
    ++m_counters.probesSent;
}

std::uint64_t ClientConnection::stallBoundMicroseconds() const
{
    return std::max(m_policy.stallFloorMicroseconds, 4 * m_smoothedRoundTrip.value_or(0));
}

void ClientConnection::requestCheckpoint(std::uint32_t handle)
{
    if (m_state != ConnectionState::Attached)
        return;
    if (PaneReplica* replica = pane(handle); replica && replica->hasCheckpoint)
        requestResync(*replica, ResyncReason::BaseMissing);
}

void ClientConnection::setPaneTerminal(std::uint32_t handle, Ubiquitty::Terminal* terminal)
{
    PaneReplica* replica = pane(handle);
    if (!replica || replica->terminal == terminal)
        return;
    auto ownedTerminal = terminal ? nullptr : std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::Terminal* const destination = terminal ? terminal : ownedTerminal.get();
    if (ownedTerminal && m_policy.historyCacheLines > 0)
        Ubiquitty::setScrollbackLimit(destination, replicaScrollbackLines(m_policy));
    Ubiquitty::Terminal* const previous = replica->terminal;
    auto images = std::make_unique<ImageReplica>(*destination);
    // Every alias borrows the same picture. Rebind their metadata first, then
    // transfer physical pixels exactly once while both terminals still exist.
    for (auto& alias : m_panes) {
        if (&alias != replica && alias.terminal == previous)
            alias.images = std::make_unique<ImageReplica>(*destination, *alias.images);
    }
    replica->images->moveTo(*images);
    replica->images = std::move(images);
    for (auto& image : m_incomingImages) {
        if (image && image->terminal == previous)
            image->terminal = destination;
    }
    // What the set knew of the pane was known of the terminal it had.
    if (m_replicas) {
        if (terminal)
            (void)m_replicas->holdLent(replica->paneID, *terminal);
        else
            (void)m_replicas->hold(replica->paneID, std::move(ownedTerminal));
    }
    // With no retained set the handles share a single owner, so unbinding the
    // handle that initiated the move cannot destroy another alias's picture.
    std::shared_ptr<Ubiquitty::Terminal> sharedTerminal = std::move(ownedTerminal);
    for (auto& alias : m_panes) {
        if (alias.terminal != previous)
            continue;
        alias.ownedTerminal = sharedTerminal;
        alias.terminal = destination;
    }
    for (auto& alias : m_panes) {
        if (alias.terminal != destination)
            continue;
        refreshImageReservations(alias);
        alias.history->invalidateProjection();
        // Screen state is replenished by a pending or newly requested checkpoint.
        if (alias.hasCheckpoint && !alias.pending)
            requestResync(alias, ResyncReason::BaseMissing);
    }
    requestMissingImages();
}

void ClientConnection::releaseTerminal(const Ubiquitty::Terminal& terminal)
{
    // setPaneTerminal moves every alias of the picture at once and rebinds
    // the panes, so the scan restarts after it; each pass moves one pane at
    // least, which bounds the passes by the panes.
    for (std::size_t pass = 0; pass < m_panes.size(); ++pass) {
        const auto lent = std::ranges::find(m_panes, &terminal, [](const PaneReplica& pane) -> const Ubiquitty::Terminal* { return pane.terminal; });
        if (lent == m_panes.end())
            return;
        setPaneTerminal(lent->handle, nullptr);
    }
    assert(std::ranges::none_of(m_panes, [&](const PaneReplica& pane) { return pane.terminal == &terminal; }));
}

std::vector<std::uint8_t> ClientConnection::takeStreamOutput()
{
    // A paste's piece goes behind everything interactive, one per take, so
    // a key typed during it waits behind one piece at most.
    appendPastePiece();
    appendClipboardReplyChunk();
    return std::exchange(m_output, {});
}

void ClientConnection::receiveClipboardRequest(std::uint32_t handle, std::span<const std::uint8_t> body)
{
    const auto request = decodeClipboardRead(body);
    const PaneReplica* replica = pane(handle);
    if (!request || !replica || !m_welcome
        || (m_welcome->grantedRole != Role::Owner && m_welcome->grantedRole != Role::Driver)
        || std::ranges::any_of(m_clipboardReads, [&](const auto& held) { return held && held->requestID == request->requestID; })) {
        close(ByeReason::Malformed, "an invalid or repeated clipboard request");
        return;
    }
    const auto slot = std::ranges::find_if(m_clipboardReads, [](const auto& held) { return !held; });
    if (slot == m_clipboardReads.end()) {
        close(ByeReason::TransferRefused, "too many pending clipboard requests");
        return;
    }
    std::unique_ptr<std::uint8_t[]> copy(new (std::nothrow) std::uint8_t[body.size()]);
    if (!copy) {
        close(ByeReason::TransferRefused, "clipboard request allocation refused");
        return;
    }
    std::copy(body.begin(), body.end(), copy.get());
    slot->emplace();
    (*slot)->handle = handle;
    (*slot)->requestID = request->requestID;
    (*slot)->request = { .bytes = std::move(copy), .size = body.size() };
    // The notification borrows the active frame, not the stored request owner:
    // a callback may answer, unbind or abandon this connection synchronously.
    if (!m_delegate || !m_delegate->paneClipboardRequest(handle, replica->paneID, *request))
        (void)answerClipboard(request->requestID, EffectClipboardStatus::Refused);
}

bool ClientConnection::answerClipboard(std::uint32_t requestID, EffectClipboardStatus status, const EffectClipboardBundle& bundle)
{
    if (m_state != ConnectionState::Attached)
        return false;
    const auto slot = std::ranges::find_if(m_clipboardReads, [&](const auto& held) { return held && held->requestID == requestID && !held->hasAnswer; });
    if (slot == m_clipboardReads.end() || !pane((*slot)->handle))
        return false;
    ClipboardReadState& held = **slot;
    ClipboardData response { .requestID = requestID, .status = status, .payload = {} };
    EffectCodecBytes data;
    if (status == EffectClipboardStatus::OK) {
        const auto request = decodeClipboardRead(held.request.view());
        if (!request || !matchesClipboardRead(*request, bundle))
            return false;
        const auto size = effectClipboardEncodedBytes(bundle, maxEffectClipboardReadBytes);
        const std::size_t retained = clipboardReadBytes(m_clipboardReads);
        if (!size || retained > maxCatalogMessageBytes || *size > maxCatalogMessageBytes - retained)
            return false;
        auto encoded = encodeEffectClipboard(bundle, maxEffectClipboardReadBytes);
        if (!encoded)
            return false;
        data = std::move(*encoded);
        response.payload.encodedBytes = static_cast<std::uint32_t>(data.size);
        if (data.size <= maxFrameBodyBytes - 13) {
            response.payload.inlineBytes = data.view();
        } else {
            if (!m_nextClipboardTransfer)
                return false;
            response.payload.transfer = m_nextClipboardTransfer;
        }
    } else if (bundle.count) {
        return false;
    }
    const auto encoded = encodeClipboardData(response);
    if (!encoded)
        return false;
    send(FrameKind::ClipboardData, held.handle, encoded->view());
    if (!response.payload.transfer) {
        slot->reset();
        return true;
    }
    held.data = std::move(data);
    held.status = status;
    held.transfer = m_nextClipboardTransfer++;
    Crypto::sha256(held.data.view(), held.sha256);
    held.hasAnswer = true;
    return true;
}

void ClientConnection::appendClipboardReplyChunk()
{
    if (m_state != ConnectionState::Attached)
        return;
    for (auto& slot : m_clipboardReads) {
        if (!slot || !slot->hasAnswer || !slot->transfer)
            continue;
        if (!pane(slot->handle)) {
            slot.reset();
            continue;
        }
        ClipboardReadState& held = *slot;
        // The final raw trailer is32 digest bytes plus its coding byte.
        const std::size_t length = std::min(bulkChunkPayloadBytes, held.data.size - held.offset);
        const bool isLast = length == held.data.size - held.offset;
        std::vector<std::uint8_t> body;
        encode(body, BulkChunk { .transfer = held.transfer, .index = held.nextIndex, .isLast = isLast, .bytes = held.data.view().subspan(held.offset, length), .sha256 = held.sha256 });
        send(FrameKind::BulkChunk, 0, body);
        held.offset += length;
        ++held.nextIndex;
        if (isLast)
            slot.reset();
        return;
    }
}

std::optional<std::uint64_t> ClientConnection::sendPaste(std::uint32_t handle, std::string_view text)
{
    const PaneReplica* replica = pane(handle);
    if (!replica)
        return std::nullopt;
    const std::uint64_t inputSeq = replica->nextInputSeq;
    if (!queuePaste(handle, inputSeq, text))
        return std::nullopt;
    return inputSeq;
}

bool ClientConnection::queuePaste(std::uint32_t handle, std::uint64_t inputSeq, std::string_view text)
{
    PaneReplica* replica = pane(handle);
    if (!replica || inputSeq == 0 || m_state != ConnectionState::Attached || !m_welcome || !m_welcome->pasteLimits)
        return false;
    if (text.size() > std::min<std::uint64_t>(m_welcome->pasteLimits->pasteBytes, maxPasteBytes) || !isWholeScalars({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }))
        return false;
    m_pastes.push_back({ .handle = handle, .inputSeq = inputSeq, .text = std::string(text), .sent = 0, .written = 0 });
    replica->nextInputSeq = std::max(replica->nextInputSeq, inputSeq + 1);
    return true;
}

std::optional<std::uint64_t> ClientConnection::abortPaste(std::uint32_t handle)
{
    const auto found = std::ranges::find(m_pastes, handle, &OutgoingPaste::handle);
    if (found == m_pastes.end())
        return std::nullopt;
    const std::uint64_t inputSeq = found->inputSeq;
    // A paste nothing of which left is dropped without a word; one that
    // began is ended on the interactive lane, ahead of what follows.
    if (found->sent > 0 && m_state == ConnectionState::Attached) {
        std::vector<std::uint8_t> body;
        encode(body, PasteAbort { .inputSeq = inputSeq });
        send(FrameKind::PasteAbort, handle, body);
    }
    m_pastes.erase(found);
    return inputSeq;
}

bool ClientConnection::sendPasteAbort(std::uint32_t handle, std::uint64_t inputSeq)
{
    PaneReplica* replica = pane(handle);
    if (!replica || inputSeq == 0 || m_state != ConnectionState::Attached)
        return false;
    std::vector<std::uint8_t> body;
    encode(body, PasteAbort { .inputSeq = inputSeq });
    send(FrameKind::PasteAbort, handle, body);
    replica->nextInputSeq = std::max(replica->nextInputSeq, inputSeq + 1);
    return true;
}

InterruptCharacters ClientConnection::interruptCharacters(std::uint32_t handle) const
{
    const std::optional<EchoContext> context = echoContext(handle);
    return context ? context->interruptCharacters : InterruptCharacters::terminalDefaults();
}

bool ClientConnection::isPasting(std::uint32_t handle) const
{
    return std::ranges::any_of(m_pastes, [&](const OutgoingPaste& paste) { return paste.handle == handle; });
}

void ClientConnection::appendPastePiece()
{
    if (m_pastes.empty() || m_state != ConnectionState::Attached || !m_welcome || !m_welcome->pasteLimits)
        return;
    // A pane unbound under a paste takes none of the rest.
    std::erase_if(m_pastes, [&](const OutgoingPaste& paste) { return !pane(paste.handle); });
    // The oldest paste whose window is open sends: one a program is not
    // reading waits for its receipt, and holds no other pane's paste behind
    // it. A host that states no window is sent the paste as it comes.
    const std::uint64_t window = m_welcome->pasteLimits->windowBytes;
    const auto sendable = std::ranges::find_if(m_pastes, [&](const OutgoingPaste& paste) { return window == 0 || paste.sent - paste.written < window; });
    if (sendable == m_pastes.end())
        return;
    OutgoingPaste& paste = *sendable;
    const std::size_t chunk = std::max<std::size_t>(4, std::min<std::size_t>({ m_welcome->pasteLimits->chunkBytes, maxPasteChunkBytes, clientPastePieceBytes }));
    std::size_t length = std::min(chunk, paste.text.size() - paste.sent);
    // Back off to the start of a scalar, so no piece splits a character.
    while (length > 0 && paste.sent + length < paste.text.size() && (static_cast<std::uint8_t>(paste.text[paste.sent + length]) & 0xC0) == 0x80)
        --length;
    const bool isFinal = paste.sent + length == paste.text.size();
    std::vector<std::uint8_t> body;
    const std::span<const std::uint8_t> text { reinterpret_cast<const std::uint8_t*>(paste.text.data()) + paste.sent, length };
    encode(body, Paste { .inputSeq = paste.inputSeq, .offset = paste.sent, .state = isFinal ? PasteState::Final : PasteState::More, .text = text });
    send(FrameKind::Paste, paste.handle, body);
    paste.sent += length;
    if (isFinal)
        m_pastes.erase(sendable);
}

std::vector<std::vector<std::uint8_t>> ClientConnection::takeDatagrams()
{
    return std::exchange(m_datagrams, {});
}

std::string_view ClientConnection::notice() const
{
    if (m_state != ConnectionState::Closed || !m_byeReason)
        return {};
    return byeNotice(*m_byeReason);
}

const Ubiquitty::Terminal* ClientConnection::paneTerminal(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.terminal;
    }
    return nullptr;
}

const PaneFacts* ClientConnection::paneFacts(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.facts ? &*replica.facts : nullptr;
    }
    return nullptr;
}

ClientConnection::ListedPane* ClientConnection::listedPane(std::uint32_t handle)
{
    const auto found = std::ranges::find(m_listed, handle, &ListedPane::handle);
    return found == m_listed.end() ? nullptr : &*found;
}

std::vector<ID128> ClientConnection::listedPanes() const
{
    std::vector<ID128> panes;
    panes.reserve(m_listed.size());
    for (const ListedPane& listed : m_listed)
        panes.push_back(listed.paneID);
    return panes;
}

const PaneFacts* ClientConnection::listedPaneFacts(ID128 paneID) const
{
    const auto found = std::ranges::find(m_listed, paneID, &ListedPane::paneID);
    return found != m_listed.end() && found->facts ? &*found->facts : nullptr;
}

const StatusRecords* ClientConnection::listedPaneStatus(ID128 paneID) const
{
    const auto found = std::ranges::find(m_listed, paneID, &ListedPane::paneID);
    return found != m_listed.end() && found->status ? &*found->status : nullptr;
}

void ClientConnection::takeStructure(const WorkspaceStructureHead& head, std::span<const std::uint8_t> record)
{
    const auto held = m_structures.find(head.workspaceID);
    // A revision at or below the one held is an older word arriving late.
    if (held != m_structures.end() && head.revision <= held->second.revision)
        return;
    if (head.isGone) {
        if (held == m_structures.end())
            return;
        m_structureBytes -= held->second.record.size();
        m_structures.erase(held);
    } else {
        const std::size_t before = held == m_structures.end() ? 0 : held->second.record.size();
        // A host holds a bounded number of workspaces, each under its
        // record's bound; one that sends more is not one to keep reading.
        if (record.empty() || record.size() > maxWorkspaceStructureRecordBytes || m_structureBytes - before + record.size() > maxHeldStructureBytes
            || (held == m_structures.end() && m_structures.size() >= maxHeldStructures)) {
            close(ByeReason::Malformed, "a workspace structure past what a client holds");
            return;
        }
        HeldStructure& kept = m_structures[head.workspaceID];
        kept.revision = head.revision;
        kept.record.assign(record.begin(), record.end());
        m_structureBytes = m_structureBytes - before + record.size();
    }
    ++m_counters.structuresApplied;
    if (m_delegate)
        m_delegate->workspaceStructureChanged(head.workspaceID);
}

std::optional<StatusRecords> ClientConnection::paneStatus(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle != handle)
            continue;
        if (replica.view != ViewState::Live)
            return replica.status;
        return replica.terminal ? std::optional { statusRecordsOf(*replica.terminal) } : std::nullopt;
    }
    return std::nullopt;
}

Ubiquitty::Terminal* ClientConnection::paneTerminal(std::uint32_t handle)
{
    PaneReplica* replica = pane(handle);
    return replica ? replica->terminal : nullptr;
}

std::uint32_t ClientConnection::handleForPane(ID128 paneID) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.paneID == paneID)
            return replica.handle;
    }
    return 0;
}

bool ClientConnection::isPaneCurrent(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.hasCheckpoint && !replica.isResyncOutstanding;
    }
    return false;
}

std::optional<Digest128> ClientConnection::paneDigest(std::uint32_t handle) const
{
    const auto found = std::ranges::find(m_panes, handle, &PaneReplica::handle);
    if (found == m_panes.end() || !found->hasCheckpoint)
        return std::nullopt;
    return replicaDigest(found->replica);
}

std::uint64_t ClientConnection::paneGeneration(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.replica.generation;
    }
    return 0;
}

std::optional<FrameClass> ClientConnection::paneFrameClass(std::uint32_t handle) const
{
    const auto found = std::ranges::find(m_panes, handle, &PaneReplica::handle);
    return found == m_panes.end() || !found->hasCheckpoint ? std::nullopt : std::optional(found->frameClass);
}

void ClientConnection::send(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body)
{
    (void)appendFrame(m_output, kind, channel, body);
}

void ClientConnection::close(ByeReason reason, std::string_view detail)
{
    constexpr std::size_t maxLocalCloseDetailBytes = 512;
    m_localCloseDetail.assign(detail.substr(0, maxLocalCloseDetailBytes));
    for (auto& request : m_clipboardReads)
        request.reset();
    for (const auto& replica : m_panes) {
        abandonImages(replica.handle);
        abandonEffects(replica.handle);
    }
    std::vector<std::uint8_t> body;
    encode(body, spokenBye(m_identity.wireVersion, m_identity.appVersion, m_identity.platform, reason, detail));
    send(FrameKind::Bye, 0, body);
    m_byeReason = reason;
    m_state = ConnectionState::Closed;
}

void ClientConnection::leave(ByeReason reason)
{
    if (m_state != ConnectionState::Attached || (reason != ByeReason::Normal && reason != ByeReason::Suspending))
        return;
    close(reason, {});
}

void ClientConnection::setView(std::uint32_t handle, ViewState state)
{
    PaneReplica* replica = pane(handle);
    // A failed pane takes no screen, whatever its frontend shows.
    if (m_state != ConnectionState::Attached || !replica || replica->view == state || (replica->failure && state != ViewState::RecordsOnly))
        return;
    replica->view = state;
    // A pane received live has its status in its screen; the set a host sent
    // for another view is not kept beside it, and the screen's status stands
    // in the terminal again. A set still arriving stays in assembly: the host
    // stops it where it stands when it learns the pane is shown, and begins
    // again from the first record when it is hidden.
    bool didRestoreScreenStatus = false;
    if (state == ViewState::Live) {
        replica->status.reset();
        didRestoreScreenStatus = restoreScreenStatus(*replica);
    }
    const ID128 paneID = replica->paneID;
    sendView(handle, state);
    if (didRestoreScreenStatus && m_delegate)
        m_delegate->paneChanged(handle, paneID);
}

void ClientConnection::standStatusSetInTerminal(PaneReplica& replica)
{
    if (!replica.status)
        return;
    if (!replica.screenStatus)
        replica.screenStatus = replica.terminal->programStatus;
    if (replica.terminal->programStatus == replica.status->store)
        return;
    replica.terminal->programStatus = replica.status->store;
    OSC::activityChanged(*replica.terminal);
}

bool ClientConnection::restoreScreenStatus(PaneReplica& replica)
{
    std::optional<OSC::ProgramStatusStore> screen = std::exchange(replica.screenStatus, std::nullopt);
    if (!screen || replica.terminal->programStatus == *screen)
        return false;
    replica.terminal->programStatus = std::move(*screen);
    OSC::activityChanged(*replica.terminal);
    return true;
}

void ClientConnection::sendView(std::uint32_t handle, ViewState state)
{
    std::vector<std::uint8_t> body;
    encode(body, View { .state = state });
    send(FrameKind::View, handle, body);
}

std::optional<ViewState> ClientConnection::paneView(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.view;
    }
    return std::nullopt;
}

std::optional<std::string> ClientConnection::paneFailure(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.failure;
    }
    return std::nullopt;
}

const HistoryCache* ClientConnection::historyCache(std::uint32_t handle) const
{
    const auto found = std::ranges::find_if(m_panes, [&](const PaneReplica& replica) { return replica.handle == handle; });
    return found == m_panes.end() || !found->history ? nullptr : &*found->history;
}

HistoryPurgeOutcome ClientConnection::retireHistory(PaneReplica& replica, const HistoryReset& reset)
{
    if (m_purgeSink && m_purgeSink->purge(replica.history->key(), reset.clearedThrough) != HistoryPurgeOutcome::Purged) {
        close(ByeReason::Suspending, "external history retirement was refused");
        return HistoryPurgeOutcome::Refused;
    }
    return HistoryPurgeOutcome::Purged;
}

void ClientConnection::purgeHistory(PaneReplica& replica, const HistoryReset& reset)
{
    (void)replica.history->reset(reset);
    // A later diff resume must name the accepted clear, not the epoch of the
    // last full checkpoint (which may precede many resets and row updates).
    replica.replica.historyEpoch = reset.newEpoch;
    replica.replica.historyHead = reset.historyHead;
    // A want names lines of the epoch it was sent in.
    replica.wantedHistory = {};
    // What of the old epoch is still arriving is fenced: a late chunk can never
    // repopulate the purged cache.
    std::erase_if(m_historyTransfers, [&](const auto& entry) {
        if (entry.second.handle != replica.handle || entry.second.head.historyEpoch == reset.newEpoch)
            return false;
        m_reassembler.abandon(entry.first);
        return true;
    });
    Ubiquitty::Screen::resetScrollbackStore(replica.terminal);
    replica.history->invalidateProjection();
    ++m_counters.historyResetsApplied;
}

bool ClientConnection::wantHistory(std::uint32_t handle, HistoryPriority priority, std::span<const LineRange> ranges)
{
    PaneReplica* replica = pane(handle);
    if (m_state != ConnectionState::Attached || !m_welcome || !replica || !replica->hasCheckpoint
        || std::to_underlying(priority) > std::to_underlying(HistoryPriority::Backfill) || ranges.size() > maxHistoryWantRanges)
        return false;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        if (ranges[index].from >= ranges[index].to || (index > 0 && ranges[index].from < ranges[index - 1].to))
            return false;
    }
    // What the other priorities still miss counts against the cap: the host
    // takes each line out of its sets as it sends it.
    std::size_t outstanding = ranges.size();
    for (std::size_t other = 0; other < replica->wantedHistory.size(); ++other) {
        if (other == static_cast<std::size_t>(priority))
            continue;
        for (const LineRange& range : replica->wantedHistory[other])
            outstanding += replica->history->gaps(range).size();
    }
    if (outstanding > m_welcome->limits.maxPendingRanges)
        return false;
    replica->wantedHistory[static_cast<std::size_t>(priority)].assign(ranges.begin(), ranges.end());
    std::vector<std::uint8_t> body;
    encode(body, HistoryWant { .historyEpoch = replica->history->key().epoch, .ranges = { ranges.begin(), ranges.end() }, .priority = priority });
    send(FrameKind::HistoryWant, handle, body);
    return true;
}

bool ClientConnection::storeHistory(PaneReplica& replica, std::uint32_t epoch, std::span<const std::uint8_t> body, std::uint64_t firstLine, HistoryOrder order)
{
    std::vector<DecodedLine> lines;
    if (!decodeHistoryBody(body, firstLine, order, historyBodyLimits, lines))
        return false;
    // The open line travels alone.
    const bool hasOpenLine = std::ranges::any_of(lines, [](const DecodedLine& line) { return (line.flags & historyLineOpen) != 0; });
    if (hasOpenLine && lines.size() != 1)
        return false;
    // Lines of another epoch, or below a clear, are not this cache's to hold.
    const CacheInsert stored = hasOpenLine ? replica.history->setOpenLine(epoch, std::move(lines.front())) : replica.history->insert(epoch, lines);
    // Past the replica allowance the pane that grew gives up its oldest
    // lines, as its own caps would have it; the host still holds them.
    if (stored == CacheInsert::Stored) {
        const std::size_t held = retainedReplicaBytes();
        if (held > m_policy.maxRetainedReplicaBytes) {
            const std::size_t cache = replica.history->retainedStorageBytes();
            const std::size_t excess = held - m_policy.maxRetainedReplicaBytes;
            replica.history->trimToBytes(cache > excess ? cache - excess : 0);
            ++m_counters.historyTrimsForReplicas;
        }
    }
    if (stored == CacheInsert::Stored && replica.hasCheckpoint) {
        replica.history->project(*replica.terminal, replica.terminal->columns);
        // A backfill that lands on a quiet pane has no update behind it to
        // say the scrollback moved.
        if (m_delegate)
            m_delegate->paneChanged(replica.handle, replica.paneID);
    }
    return true;
}

std::uint64_t ClientConnection::paneLagMicroseconds(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.lagMicroseconds;
    }
    return 0;
}

std::uint64_t ClientConnection::paneBytesReceived(std::uint32_t handle) const
{
    for (const PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return replica.bytesReceived;
    }
    return 0;
}

ClientConnection::PaneReplica* ClientConnection::pane(std::uint32_t handle)
{
    for (PaneReplica& replica : m_panes) {
        if (replica.handle == handle)
            return &replica;
    }
    return nullptr;
}

EffectReceiptState ClientConnection::effectReceiptForPane(ID128 paneID) const
{
    auto receipt = m_replicas ? m_replicas->effectReceipt(paneID) : EffectReceiptState {};
    for (const auto& replica : m_panes) {
        if (replica.paneID == paneID) {
            receipt.acknowledged = std::max(receipt.acknowledged, replica.effectReceipt.acknowledged);
            receipt.consumed = std::max(receipt.consumed, replica.effectReceipt.consumed);
        }
    }
    return receipt;
}

} // namespace Ubiquitty::Protocol::Sync
