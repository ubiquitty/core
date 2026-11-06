// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Messages.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/NameLimits.h"
#include "Core/Transport/SSHHost.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

template<typename Writer>
void putID(Writer& writer, const ID128& value)
{
    writer.u64(value.low);
    writer.u64(value.high);
}

ID128 takeID(WireReader& reader)
{
    ID128 value;
    value.low = reader.u64();
    value.high = reader.u64();
    return value;
}

template<typename Writer>
void putDigest(Writer& writer, const Digest128& value)
{
    writer.u64(value.low);
    writer.u64(value.high);
}

Digest128 takeDigest(WireReader& reader)
{
    Digest128 value;
    value.low = reader.u64();
    value.high = reader.u64();
    return value;
}

template<std::size_t count>
void takeArray(WireReader& reader, std::array<std::uint8_t, count>& value)
{
    const std::span<const std::uint8_t> bytes = reader.take(count);
    if (reader.ok())
        std::copy(bytes.begin(), bytes.end(), value.begin());
}

/// Reads an enumeration byte, failing the reader on a value past `last`.
template<typename Enumeration>
Enumeration takeEnumeration(WireReader& reader, Enumeration last)
{
    const std::uint8_t value = reader.u8();
    if (value > std::to_underlying(last))
        reader.fail();
    return static_cast<Enumeration>(reader.ok() ? value : 0);
}

bool takeBool(WireReader& reader)
{
    const std::uint8_t value = reader.u8();
    if (value > 1)
        reader.fail();
    return value == 1;
}

/// The last `FrameClass` value, which the wire's `frameClass` byte is checked against.
constexpr FrameClass lastFrameClass = FrameClass::Structural;

template<typename Writer>
void putFacts(Writer& writer, const Facts& value)
{
    writer.u16(value.viewportRows);
    writer.u16(value.viewportColumns);
    writer.u16(value.cellPixelWidth);
    writer.u16(value.cellPixelHeight);
    writer.u16(value.textAreaPixelWidth);
    writer.u16(value.textAreaPixelHeight);
    writer.u32(value.refreshIntervalMicroseconds);
    writer.u8(std::to_underlying(value.link));
    writer.u8(std::to_underlying(value.power));
    writer.u8(value.isFocused ? 1 : 0);
}

Facts takeFacts(WireReader& reader)
{
    Facts value;
    value.viewportRows = reader.u16();
    value.viewportColumns = reader.u16();
    value.cellPixelWidth = reader.u16();
    value.cellPixelHeight = reader.u16();
    value.textAreaPixelWidth = reader.u16();
    value.textAreaPixelHeight = reader.u16();
    value.refreshIntervalMicroseconds = reader.u32();
    value.link = takeEnumeration(reader, LinkCost::Constrained);
    value.power = takeEnumeration(reader, PowerState::Saver);
    value.isFocused = takeBool(reader);
    return value;
}

/// Runs a decoder over a whole body.
///
/// - Returns: The value when the reader stayed well formed and consumed every byte.
template<typename Message, typename Decode>
std::optional<Message> decodeWhole(std::span<const std::uint8_t> body, Decode&& decode)
{
    WireReader reader(body);
    Message message = decode(reader);
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    return message;
}

} // namespace

namespace {

template<typename Writer>
void putResumeRequest(Writer& writer, const ResumeRequest& resume)
{
    putID(writer, resume.attachmentID);
    writer.u8(resume.resumeSecret ? 1 : 0);
    if (resume.resumeSecret)
        putID(writer, *resume.resumeSecret);
    writer.varint(resume.panes.size());
    for (const PaneResume& pane : resume.panes) {
        putID(writer, pane.paneID);
        writer.u64(pane.ackedGeneration);
        putDigest(writer, pane.ackedRowHashesDigest);
        writer.u32(pane.historyEpoch);
        writer.varint(pane.cachedRanges.size());
        for (const LineRange& range : pane.cachedRanges) {
            writer.u64(range.from);
            writer.u64(range.to);
        }
        writer.u64(pane.lastInputSeqSent);
        writer.u8(std::to_underlying(pane.view));
        writer.u64(pane.effectsAcknowledged);
    }
}

std::optional<std::uint64_t> takeOptionalU64(WireReader& reader)
{
    return takeBool(reader) ? std::optional { reader.u64() } : std::nullopt;
}

ResumeRequest takeResumeRequest(WireReader& reader)
{
    ResumeRequest resume;
    resume.attachmentID = takeID(reader);
    if (takeBool(reader))
        resume.resumeSecret = takeID(reader);
    // The smallest pane: its identity, generation, digest, epoch, an empty
    // range list, its last input number, view and effect cursor.
    constexpr std::size_t minPaneBytes = 16 + 8 + 16 + 4 + 1 + 8 + 1 + 8;
    const std::size_t panes = reader.count(maxPanesPerConnection, minPaneBytes);
    resume.panes.reserve(reader.ok() ? panes : 0);
    for (std::size_t index = 0; index < panes && reader.ok(); ++index) {
        PaneResume pane;
        pane.paneID = takeID(reader);
        pane.ackedGeneration = reader.u64();
        pane.ackedRowHashesDigest = takeDigest(reader);
        pane.historyEpoch = reader.u32();
        const std::size_t ranges = reader.count(maxResumeCachedRanges, 16);
        pane.cachedRanges.reserve(reader.ok() ? ranges : 0);
        for (std::size_t range = 0; range < ranges && reader.ok(); ++range) {
            const LineRange line { .from = reader.u64(), .to = reader.u64() };
            if (line.from >= line.to || (!pane.cachedRanges.empty() && line.from < pane.cachedRanges.back().to))
                reader.fail();
            pane.cachedRanges.push_back(line);
        }
        pane.lastInputSeqSent = reader.u64();
        pane.view = takeEnumeration(reader, ViewState::RecordsOnly);
        pane.effectsAcknowledged = reader.u64();
        resume.panes.push_back(std::move(pane));
    }
    return resume;
}

/// Measures the same body walk as the writer without copying any payload.
class HandshakeSize {
public:
    void u8(std::uint8_t) { add(1); }
    void u16(std::uint16_t) { add(2); }
    void u32(std::uint32_t) { add(4); }
    void u64(std::uint64_t) { add(8); }
    void varint(std::uint64_t value)
    {
        do {
            add(1);
            value >>= 7;
        } while (value);
    }
    void raw(std::span<const std::uint8_t> value) { add(value.size()); }
    void bytes(std::span<const std::uint8_t> value)
    {
        varint(value.size());
        raw(value);
    }
    void utf8(std::string_view value)
    {
        varint(value.size());
        add(value.size());
    }
    [[nodiscard]] bool fits() const { return m_fits; }

private:
    void add(std::size_t bytes)
    {
        if (!m_fits || bytes > maxFrameBodyBytes - m_bytes) {
            m_fits = false;
            return;
        }
        m_bytes += bytes;
    }
    std::size_t m_bytes = 0;
    bool m_fits = true;
};

bool isText(std::string_view text, std::size_t maximum)
{
    return text.size() <= maximum && text.find('\0') == std::string_view::npos
        && isValidUTF8({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

bool isValidRequest(const AdmissionRequest& request)
{
    if (request.target > AdmissionTarget::Management
        || !isText(request.workspaceName, Session::maxWorkspaceNameBytes)
        || !isText(request.agentLabel, maxFrameBodyBytes)
        || !isText(request.sshHost, Transport::maxSSHHostLength)
        || ((request.sessionID || request.workspaceID) && !request.isAttachOnly)
        || (!request.sshHost.empty() && (request.sessionID || request.workspaceID)))
        return false;
    if (request.target == AdmissionTarget::Session)
        return !request.workspaceID;
    if (request.target == AdmissionTarget::Workspace)
        return true;
    return !request.sessionID && !request.workspaceID && request.workspaceName.empty()
        && request.sshHost.empty() && !request.isAttachOnly && !request.endsWhenDetached;
}

} // namespace

bool isValidSelection(const ServingSelection& selection)
{
    if (selection.target > AdmissionTarget::Management || !selection.daemonInstanceID
        || selection.socketName.empty() || selection.socketName.size() > maxDaemonSocketNameBytes
        || (selection.buildFlags & ~knownServingBuildFlags) || !isText(selection.hostPlatform, maxHostPlatformBytes))
        return false;
    for (const char character : selection.socketName) {
        if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-'))
            return false;
    }
    if (selection.restoredFromDaemonInstanceID
        && (selection.target != AdmissionTarget::Workspace || selection.restoredFromDaemonInstanceID == selection.daemonInstanceID))
        return false;
    if (selection.target == AdmissionTarget::Session)
        return selection.sessionID != 0;
    if (selection.target == AdmissionTarget::Workspace)
        return selection.sessionID && selection.workspaceID;
    return !selection.sessionID && !selection.workspaceID;
}

namespace {

std::string_view presentedLabel(std::string_view label)
{
    const auto prefix = label.substr(0, maxAgentLabelBytes);
    return prefix.substr(0, Unicode::wellFormedUTF8PrefixLength(prefix));
}

template<typename Writer>
void putRequest(Writer& writer, const AdmissionRequest& request)
{
    writer.u8(std::to_underlying(request.target));
    writer.u32(request.sessionID);
    writer.u32(request.workspaceID);
    writer.u64(request.expectedDaemonInstanceID);
    writer.utf8(request.workspaceName);
    writer.utf8(presentedLabel(request.agentLabel));
    writer.utf8(request.sshHost);
    writer.u8(request.isAttachOnly ? 1 : 0);
    writer.u8(request.endsWhenDetached ? 1 : 0);
    writer.u8(request.servesClipboard ? 1 : 0);
}

AdmissionRequest takeRequest(WireReader& reader)
{
    AdmissionRequest request;
    request.target = takeEnumeration(reader, AdmissionTarget::Management);
    request.sessionID = reader.u32();
    request.workspaceID = reader.u32();
    request.expectedDaemonInstanceID = reader.u64();
    request.workspaceName = reader.utf8(Session::maxWorkspaceNameBytes);
    const auto label = reader.utf8(maxFrameBodyBytes);
    if (!isText(label, maxFrameBodyBytes))
        reader.fail();
    request.agentLabel = presentedLabel(label);
    request.sshHost = reader.utf8(Transport::maxSSHHostLength);
    request.isAttachOnly = takeBool(reader);
    request.endsWhenDetached = takeBool(reader);
    request.servesClipboard = takeBool(reader);
    if (!isValidRequest(request))
        reader.fail();
    return request;
}

template<typename Writer>
void putSelection(Writer& writer, const ServingSelection& selection)
{
    writer.u8(std::to_underlying(selection.target));
    writer.u32(selection.sessionID);
    writer.u32(selection.workspaceID);
    writer.u64(selection.daemonInstanceID);
    writer.utf8(selection.socketName);
    writer.u8(selection.buildFlags);
    writer.utf8(selection.hostPlatform);
    writer.u64(selection.restoredFromDaemonInstanceID);
}

ServingSelection takeSelection(WireReader& reader)
{
    ServingSelection selection;
    selection.target = takeEnumeration(reader, AdmissionTarget::Management);
    selection.sessionID = reader.u32();
    selection.workspaceID = reader.u32();
    selection.daemonInstanceID = reader.u64();
    selection.socketName = reader.utf8(maxDaemonSocketNameBytes);
    selection.buildFlags = reader.u8();
    selection.hostPlatform = reader.utf8(maxHostPlatformBytes);
    selection.restoredFromDaemonInstanceID = reader.u64();
    if (!isValidSelection(selection))
        reader.fail();
    return selection;
}

template<typename Writer>
void putGreet(Writer& writer, const Greet& message)
{
    writer.u16(message.wireVersion);
    writer.utf8(message.appVersion);
    writer.raw(message.engineFingerprint);
    putID(writer, message.clientInstance);
    writer.u8(std::to_underlying(message.clientKind));
    writer.u8(std::to_underlying(message.requestedRole));
    putRequest(writer, message.request);
    writer.u8(message.credential ? 1 : 0);
    if (message.credential)
        writer.bytes(*message.credential);
    putFacts(writer, message.facts);
    writer.u32(message.historyCacheLines);
    writer.u32(message.backfillDepthLines);
    writer.varint(message.capabilities.toRaw());
    writer.u8(message.resume ? 1 : 0);
    if (message.resume)
        putResumeRequest(writer, *message.resume);
}

template<typename Writer>
void putWelcome(Writer& writer, const Welcome& message)
{
    writer.u16(message.wireVersion);
    writer.utf8(message.appVersion);
    writer.raw(message.engineFingerprint);
    putID(writer, message.hostInstanceID);
    putID(writer, message.attachmentID);
    putID(writer, message.resumeSecret);
    writer.u8(std::to_underlying(message.grantedRole));
    putSelection(writer, message.selection);
    writer.u8(std::to_underlying(message.admission));
    writer.u32(message.consent.requestID);
    writer.u32(message.consent.sessionID);
    writer.utf8(message.principal);
    writer.u16(message.limits.maxOutstandingCalls);
    writer.u32(message.limits.maxQueuedBulkBytes);
    writer.u16(message.limits.maxPendingRanges);
    writer.varint(message.limits.probeSteps);
    for (std::size_t step = 0; step < message.limits.probeSteps && step < maxProbeSteps; ++step)
        writer.u32(message.limits.probeScheduleMicroseconds[step]);
    writer.u64(message.hostTime);
    writer.u8(message.predecessorHostInstanceID ? 1 : 0);
    if (message.predecessorHostInstanceID)
        putID(writer, *message.predecessorHostInstanceID);
    writer.varint(message.resumed.size());
    for (const PaneResumed& pane : message.resumed) {
        putID(writer, pane.paneID);
        writer.u8(std::to_underlying(pane.outcome));
        writer.u8(pane.inputWritten ? 1 : 0);
        if (pane.inputWritten)
            writer.u64(*pane.inputWritten);
    }
    writer.u8(message.pasteLimits ? 1 : 0);
    if (!message.pasteLimits)
        return;
    writer.u32(message.pasteLimits->chunkBytes);
    writer.u32(message.pasteLimits->pasteBytes);
    writer.u32(message.pasteLimits->windowBytes);
}

template<typename Writer>
void putBye(Writer& writer, const Bye& message)
{
    writer.u8(std::to_underlying(message.reason));
    writer.utf8(message.detail);
    writer.u8(message.peer ? 1 : 0);
    if (message.peer) {
        writer.u16(message.peer->build.wireVersion);
        writer.utf8(message.peer->build.appVersion);
        writer.utf8(message.peer->platform);
    }
}

} // namespace

void encode(std::vector<std::uint8_t>& output, const Greet& message)
{
    if (!isValidRequest(message.request) || !isText(message.appVersion, maxAppVersionBytes)
        || message.clientKind > ClientKind::Viewer || message.requestedRole > lastRole
        || message.facts.link > LinkCost::Constrained || message.facts.power > PowerState::Saver
        || (message.capabilities.toRaw() & ~knownClientCapabilityBits)
        || (message.credential && (message.credential->empty() || message.credential->size() > maxCredentialBytes))
        || (isUnboundTarget(message.request.target) && message.resume)
        || (message.resume && message.resume->panes.size() > maxPanesPerConnection))
        return;
    if (message.resume) {
        for (const auto& pane : message.resume->panes) {
            if (pane.cachedRanges.size() > maxResumeCachedRanges || pane.view > ViewState::RecordsOnly)
                return;
            std::uint64_t end = 0;
            for (const auto& range : pane.cachedRanges) {
                if (range.from >= range.to || range.from < end)
                    return;
                end = range.to;
            }
        }
    }
    HandshakeSize size;
    putGreet(size, message);
    if (!size.fits())
        return;
    WireWriter writer(output);
    putGreet(writer, message);
}

void encodeGreetResume(std::vector<std::uint8_t>& output, const ResumeRequest& resume)
{
    if (resume.panes.size() > maxPanesPerConnection)
        return;
    for (const auto& pane : resume.panes) {
        if (pane.cachedRanges.size() > maxResumeCachedRanges || pane.view > ViewState::RecordsOnly)
            return;
        std::uint64_t end = 0;
        for (const auto& range : pane.cachedRanges) {
            if (range.from >= range.to || range.from < end)
                return;
            end = range.to;
        }
    }
    WireWriter writer(output);
    writer.u8(1);
    putResumeRequest(writer, resume);
}

void encode(std::vector<std::uint8_t>& output, const Welcome& message)
{
    if (!isValidSelection(message.selection) || !isText(message.appVersion, maxAppVersionBytes)
        || !isText(message.principal, maxFrameBodyBytes) || message.grantedRole > lastRole
        || message.admission > AttachmentAdmission::Granted || message.limits.probeSteps > maxProbeSteps
        || message.resumed.size() > maxPanesPerConnection
        || (isUnboundTarget(message.selection.target) && !message.resumed.empty())
        || (message.admission == AttachmentAdmission::AwaitingConsent
                ? !message.consent.requestID || !message.consent.sessionID || message.consent.sessionID != message.selection.sessionID || !message.resumed.empty()
                : message.consent.requestID || message.consent.sessionID))
        return;
    for (const auto& pane : message.resumed)
        if (pane.outcome > ResumeOutcome::Gone)
            return;
    HandshakeSize size;
    putWelcome(size, message);
    if (!size.fits())
        return;
    WireWriter writer(output);
    putWelcome(writer, message);
}

void encode(std::vector<std::uint8_t>& output, const Facts& message)
{
    WireWriter writer(output);
    putFacts(writer, message);
    writer.u8(message.themePalette ? 1 : 0);
    if (message.themePalette)
        putPaletteBlock(writer, message.themePalette->live, message.themePalette->baseline);
}

void encode(std::vector<std::uint8_t>& output, const RichText::TypographyFacts& message)
{
    WireWriter writer(output);
    writer.u64(message.rendererRevision);
    writer.u16(message.cellWidth);
    writer.u16(message.cellHeight);
    writer.u16(message.baseline);
    writer.u16(message.proportionalSize);
    writer.u16(message.monospaceSize);
    writer.u8(message.rendersBlocks);
    writer.u8(message.shaping);
    writer.u8(std::to_underlying(message.fontCatalogScope));
    writer.u8(message.faceCount);
    for (const RichText::FaceMetrics& face : message.faceTable()) {
        writer.utf8(face.familyName());
        writer.u8(std::to_underlying(face.familyClass));
        writer.u8(face.styles);
        writer.u16(face.ascent);
        writer.u16(face.descent);
        writer.u16(face.lineHeight);
        writer.u16(face.averageAdvance);
        for (const std::uint16_t advance : face.advances)
            writer.u16(advance);
    }
}

void encode(std::vector<std::uint8_t>& output, const OSC::TextSizing::ProportionalFaceFacts& message)
{
    static_assert(OSC::TextSizing::publishedFaceBodyBytes <= maxFrameBodyBytes);
    WireWriter writer(output);
    writer.u32(message.cellAdvance);
    for (const std::uint16_t advance : message.untabledAdvances)
        writer.u16(advance);
    for (const auto& face : message.advances)
        for (const std::uint16_t advance : face)
            writer.u16(advance);
    for (const auto& face : message.weightedAdvances)
        for (const std::uint8_t advance : face)
            writer.u8(advance);
}

void encode(std::vector<std::uint8_t>& output, const Ack& message)
{
    WireWriter writer(output);
    writer.u64(message.hostTimeEcho);
    writer.u32(message.holdMicroseconds);
    writer.u64(message.bytesReceived);
    writer.varint(message.panes.size());
    for (const PaneAck& pane : message.panes) {
        writer.varint(pane.handle);
        writer.u64(pane.generation);
        writer.u64(pane.historyAcked);
        writer.u64(pane.effectsAcknowledged);
    }
}

void encode(std::vector<std::uint8_t>& output, const Probe& message)
{
    WireWriter writer(output);
    writer.u64(message.time);
}

void encode(std::vector<std::uint8_t>& output, const Echo& message)
{
    WireWriter writer(output);
    writer.u64(message.timeEcho);
    writer.u32(message.holdMicroseconds);
}

void encode(std::vector<std::uint8_t>& output, const PaneBind& message)
{
    WireWriter writer(output);
    writer.varint(message.handle);
    putID(writer, message.paneID);
}

void encode(std::vector<std::uint8_t>& output, const PaneUnbind& message)
{
    WireWriter writer(output);
    writer.varint(message.handle);
    writer.u8(message.reason);
}

void encode(std::vector<std::uint8_t>& output, const BulkChunk& message)
{
    WireWriter writer(output);
    writer.u32(message.transfer);
    writer.u32(message.index);
    writer.u8(message.isLast ? 1 : 0);
    writer.bytes(message.bytes);
    if (message.isLast) {
        writer.raw(message.sha256);
        // An undeflated tail ends at the digest, version 6's layout, so only a
        // client that named DeflateCheckpoints meets the coding byte.
        if (message.compression == Compression::Deflate) {
            writer.u8(std::to_underlying(message.compression));
            writer.u32(message.inflatedBytes);
        }
    }
}

void encode(std::vector<std::uint8_t>& output, const Bye& message)
{
    if (message.reason > lastByeReasonOnWire || !isText(message.detail, maxFrameBodyBytes)
        || (message.peer && (!message.peer->build.isKnown || !message.peer->build.wireVersion || !isText(message.peer->build.appVersion, maxAppVersionBytes) || !isText(message.peer->platform, maxHostPlatformBytes))))
        return;
    HandshakeSize size;
    putBye(size, message);
    if (!size.fits())
        return;
    WireWriter writer(output);
    putBye(writer, message);
}

void encode(std::vector<std::uint8_t>& output, const CheckpointHead& message)
{
    WireWriter writer(output);
    writer.u64(message.generation);
    writer.u32(message.transfer);
    writer.u8(std::to_underlying(message.frameClass));
    putDigest(writer, message.stateDigest);
}

void encode(std::vector<std::uint8_t>& output, const UpdateHeader& message)
{
    WireWriter writer(output);
    writer.u8(std::to_underlying(message.mode));
    writer.u64(message.baseGeneration);
    writer.u64(message.generation);
    writer.u8(std::to_underlying(message.frameClass));
    writer.u64(message.hostTime);
    writer.varint(message.lagMicroseconds);
    writer.u64(message.inputWritten);
    writer.u64(message.inputSettled);
    writer.u64(message.historyHead);
    writer.u64(message.effectsThrough);
}

void encode(std::vector<std::uint8_t>& output, const Resync& message)
{
    WireWriter writer(output);
    writer.u8(std::to_underlying(message.reason));
    writer.u64(message.generation);
    writer.varint(message.sectionHashes.size());
    for (const std::uint64_t hash : message.sectionHashes)
        writer.u64(hash);
}

void encode(std::vector<std::uint8_t>& output, const View& message)
{
    WireWriter writer(output);
    writer.u8(std::to_underlying(message.state));
}

void encode(std::vector<std::uint8_t>& output, const BulkAbort& message)
{
    WireWriter writer(output);
    writer.u32(message.transfer);
}

void encode(std::vector<std::uint8_t>& output, const Call& message)
{
    output.insert(output.end(), message.text.begin(), message.text.end());
}

void encode(std::vector<std::uint8_t>& output, const Result& message)
{
    WireWriter writer(output);
    writer.varint(message.transfer);
    if (message.transfer == 0)
        output.insert(output.end(), message.text.begin(), message.text.end());
}

void encode(std::vector<std::uint8_t>& output, const WorkspaceStructureHead& message)
{
    WireWriter writer(output);
    writer.u32(message.workspaceID);
    writer.u64(message.revision);
    writer.u8(message.isGone ? 1 : 0);
    writer.varint(message.transfer);
    if (message.transfer == 0 && !message.isGone)
        output.insert(output.end(), message.record.begin(), message.record.end());
}

void encode(std::vector<std::uint8_t>& output, const Notify& message)
{
    WireWriter writer(output);
    writer.varint(std::to_underlying(message.kind));
    writer.raw(message.record);
}

void encode(std::vector<std::uint8_t>& output, const Receipt& message)
{
    WireWriter writer(output);
    writer.u64(message.inputWritten);
    writer.u64(message.inputSettled);
}

void encode(std::vector<std::uint8_t>& output, const Raw& message)
{
    WireWriter writer(output);
    writer.u64(message.inputSeq);
    writer.bytes(message.bytes);
}

void encode(std::vector<std::uint8_t>& output, const Paste& message)
{
    WireWriter writer(output);
    writer.u64(message.inputSeq);
    writer.u64(message.offset);
    writer.u8(std::to_underlying(message.state));
    writer.bytes(message.text);
}

void encode(std::vector<std::uint8_t>& output, const PasteReceipt& message)
{
    WireWriter writer(output);
    writer.u64(message.inputSeq);
    writer.u64(message.bytesWritten);
}

void encode(std::vector<std::uint8_t>& output, const PasteAbort& message)
{
    WireWriter writer(output);
    writer.u64(message.inputSeq);
}

void encode(std::vector<std::uint8_t>& output, const Keys& message)
{
    WireWriter writer(output);
    writer.u64(message.inputSeq);
    writer.varint(message.events.size());
    for (const KeyEvent& event : message.events) {
        writer.u32(event.key);
        writer.u16(event.modifiers);
        writer.u8(std::to_underlying(event.kind));
        writer.utf8(event.text);
        writer.u32(event.unshiftedCodePoint);
        writer.u32(event.shiftedCodePoint);
        writer.u32(event.baseLayoutCodePoint);
        writer.u8(event.wasModified ? 1 : 0);
    }
}

void encode(std::vector<std::uint8_t>& output, const Pointer& message)
{
    WireWriter writer(output);
    writer.u64(message.inputSeq);
    writer.varint(message.events.size());
    for (const PointerEvent& event : message.events) {
        writer.u8(std::to_underlying(event.kind));
        writer.u8(event.button);
        writer.u16(event.modifiers);
        writer.u16(event.row);
        writer.u16(event.column);
        writer.u16(event.pixelX);
        writer.u16(event.pixelY);
    }
}

void encode(std::vector<std::uint8_t>& output, const Focus& message)
{
    WireWriter writer(output);
    writer.u8(message.isFocused ? 1 : 0);
}

void encode(std::vector<std::uint8_t>& output, const Viewport& message)
{
    WireWriter writer(output);
    writer.u16(message.rows);
    writer.u16(message.columns);
    writer.u16(message.cellPixelWidth);
    writer.u16(message.cellPixelHeight);
    writer.u16(message.textAreaPixelWidth);
    writer.u16(message.textAreaPixelHeight);
    writer.u16(message.drawablePixelWidth);
    writer.u16(message.drawablePixelHeight);
}

void encode(std::vector<std::uint8_t>& output, const HistoryInlineHeader& message)
{
    WireWriter writer(output);
    writer.u32(message.historyEpoch);
    writer.u64(message.firstLine);
}

void encode(std::vector<std::uint8_t>& output, const HistoryHead& message)
{
    WireWriter writer(output);
    writer.u32(message.transfer);
    writer.u32(message.historyEpoch);
    writer.u64(message.firstLine);
    writer.u16(message.count);
    writer.u8(std::to_underlying(message.order));
}

void encode(std::vector<std::uint8_t>& output, const HistoryReset& message)
{
    WireWriter writer(output);
    writer.u32(message.newEpoch);
    writer.u64(message.clearedThrough);
    writer.u64(message.historyHead);
}

void encode(std::vector<std::uint8_t>& output, const HistoryWant& message)
{
    WireWriter writer(output);
    writer.u32(message.historyEpoch);
    writer.varint(message.ranges.size());
    for (const LineRange& range : message.ranges) {
        writer.u64(range.from);
        writer.u64(range.to);
    }
    writer.u8(std::to_underlying(message.priority));
}

std::optional<std::uint16_t> decodeHandshakeVersion(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    const std::uint16_t version = reader.u16();
    return reader.ok() && version ? std::optional { version } : std::nullopt;
}

std::optional<PeerBuild> decodeHandshakeBuild(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    PeerBuild build;
    build.wireVersion = reader.u16();
    build.appVersion = reader.utf8(maxAppVersionBytes);
    if (!reader.ok() || !isText(build.appVersion, maxAppVersionBytes))
        return std::nullopt;
    build.isKnown = true;
    return build;
}

std::optional<EngineFingerprint> decodeHandshakeFingerprint(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    (void)reader.u16();
    (void)reader.utf8(maxAppVersionBytes);
    EngineFingerprint fingerprint {};
    takeArray(reader, fingerprint);
    return reader.ok() ? std::optional { fingerprint } : std::nullopt;
}

std::optional<Greet> decodeGreet(std::span<const std::uint8_t> body)
{
    if (body.size() > maxFrameBodyBytes)
        return std::nullopt;
    return decodeWhole<Greet>(body, [](WireReader& reader) {
        Greet message;
        message.wireVersion = reader.u16();
        message.appVersion = reader.utf8(maxAppVersionBytes);
        takeArray(reader, message.engineFingerprint);
        message.clientInstance = takeID(reader);
        message.clientKind = takeEnumeration(reader, ClientKind::Viewer);
        message.requestedRole = takeEnumeration(reader, lastRole);
        message.request = takeRequest(reader);
        if (takeBool(reader)) {
            const auto credential = reader.bytes(maxCredentialBytes);
            if (credential.empty())
                reader.fail();
            if (reader.ok())
                message.credential = std::vector<std::uint8_t>(credential.begin(), credential.end());
        }
        message.facts = takeFacts(reader);
        message.historyCacheLines = reader.u32();
        message.backfillDepthLines = reader.u32();
        const auto capabilities = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
        // The bits a client may name are in the wire fingerprint, checked
        // before the Greet is read, so a build that names one this build
        // does not know has already met it as a different version: a bit
        // past them here is malformed, never a request to ignore.
        if (capabilities & ~knownClientCapabilityBits)
            reader.fail();
        message.capabilities = ClientCapabilities::fromRaw(capabilities);
        if (takeBool(reader)) {
            if (isUnboundTarget(message.request.target))
                reader.fail();
            else
                message.resume = takeResumeRequest(reader);
        }
        if (!isText(message.appVersion, maxAppVersionBytes))
            reader.fail();
        return message;
    });
}

std::optional<Welcome> decodeWelcome(std::span<const std::uint8_t> body)
{
    if (body.size() > maxFrameBodyBytes)
        return std::nullopt;
    return decodeWhole<Welcome>(body, [](WireReader& reader) {
        Welcome message;
        message.wireVersion = reader.u16();
        message.appVersion = reader.utf8(maxAppVersionBytes);
        takeArray(reader, message.engineFingerprint);
        message.hostInstanceID = takeID(reader);
        message.attachmentID = takeID(reader);
        message.resumeSecret = takeID(reader);
        message.grantedRole = takeEnumeration(reader, lastRole);
        message.selection = takeSelection(reader);
        message.admission = takeEnumeration(reader, AttachmentAdmission::Granted);
        message.consent.requestID = reader.u32();
        message.consent.sessionID = reader.u32();
        if (message.admission == AttachmentAdmission::AwaitingConsent
                ? !message.consent.requestID || !message.consent.sessionID || message.consent.sessionID != message.selection.sessionID
                : message.consent.requestID || message.consent.sessionID)
            reader.fail();
        message.principal = std::string(reader.utf8(maxFrameBodyBytes));
        if (!isText(message.appVersion, maxAppVersionBytes) || !isText(message.principal, maxFrameBodyBytes))
            reader.fail();
        message.limits.maxOutstandingCalls = reader.u16();
        message.limits.maxQueuedBulkBytes = reader.u32();
        message.limits.maxPendingRanges = reader.u16();
        // A schedule names no more steps than the cap; none says the host
        // does not probe an idle connection.
        const std::size_t steps = reader.count(maxProbeSteps, 4);
        message.limits.probeSteps = static_cast<std::uint8_t>(steps);
        for (std::size_t step = 0; step < steps && reader.ok(); ++step)
            message.limits.probeScheduleMicroseconds[step] = reader.u32();
        message.hostTime = reader.u64();
        if (takeBool(reader))
            message.predecessorHostInstanceID = takeID(reader);
        constexpr std::size_t minPaneBytes = 16 + 1 + 1;
        const std::size_t panes = reader.count(maxPanesPerConnection, minPaneBytes);
        if (panes && isUnboundTarget(message.selection.target))
            reader.fail();
        message.resumed.reserve(reader.ok() ? panes : 0);
        for (std::size_t index = 0; index < panes && reader.ok(); ++index) {
            PaneResumed pane;
            pane.paneID = takeID(reader);
            pane.outcome = takeEnumeration(reader, ResumeOutcome::Gone);
            pane.inputWritten = takeOptionalU64(reader);
            message.resumed.push_back(pane);
        }
        if (message.admission == AttachmentAdmission::AwaitingConsent && !message.resumed.empty())
            reader.fail();
        if (!takeBool(reader))
            return message;
        PasteLimits paste;
        paste.chunkBytes = reader.u32();
        paste.pasteBytes = reader.u32();
        paste.windowBytes = reader.u32();
        message.pasteLimits = paste;
        return message;
    });
}

std::optional<Facts> decodeFacts(std::span<const std::uint8_t> body)
{
    return decodeWhole<Facts>(body, [](WireReader& reader) {
        Facts facts = takeFacts(reader);
        if (!takeBool(reader))
            return facts;
        ThemePalette palette;
        takePaletteBlock(reader, palette.live, palette.baseline);
        facts.themePalette = palette;
        return facts;
    });
}

std::optional<RichText::TypographyFacts> decodeTypographyFacts(std::span<const std::uint8_t> body)
{
    std::optional<RichText::TypographyFacts> facts = decodeWhole<RichText::TypographyFacts>(body, [](WireReader& reader) {
        RichText::TypographyFacts value;
        value.rendererRevision = reader.u64();
        value.cellWidth = reader.u16();
        value.cellHeight = reader.u16();
        value.baseline = reader.u16();
        value.proportionalSize = reader.u16();
        value.monospaceSize = reader.u16();
        value.rendersBlocks = reader.u8();
        value.shaping = reader.u8();
        value.fontCatalogScope = static_cast<RichText::FontCatalogScope>(reader.u8());
        value.faceCount = reader.u8();
        if (!value.rendererRevision || value.faceCount > RichText::maxFaces) {
            reader.fail();
            return value;
        }
        for (RichText::FaceMetrics& face : std::span(value.faces).first(value.faceCount)) {
            const std::string_view name = reader.utf8(RichText::maxFamilyNameBytes);
            std::ranges::copy(name, face.name.begin());
            face.nameLength = static_cast<std::uint8_t>(name.size());
            const std::uint8_t familyClass = reader.u8();
            if (familyClass > std::to_underlying(RichText::FamilyClass::Monospace))
                reader.fail();
            face.familyClass = static_cast<RichText::FamilyClass>(familyClass);
            face.styles = reader.u8();
            face.ascent = reader.u16();
            face.descent = reader.u16();
            face.lineHeight = reader.u16();
            face.averageAdvance = reader.u16();
            for (std::uint16_t& advance : face.advances)
                advance = reader.u16();
        }
        return value;
    });
    if (facts && !RichText::isAdoptable(*facts))
        return std::nullopt;
    return facts;
}

std::optional<OSC::TextSizing::ProportionalFaceFacts> decodeProportionalFaces(std::span<const std::uint8_t> body)
{
    std::optional<OSC::TextSizing::ProportionalFaceFacts> faces = decodeWhole<OSC::TextSizing::ProportionalFaceFacts>(body, [](WireReader& reader) {
        OSC::TextSizing::ProportionalFaceFacts value;
        value.cellAdvance = reader.u32();
        for (std::uint16_t& advance : value.untabledAdvances)
            advance = reader.u16();
        for (auto& face : value.advances)
            for (std::uint16_t& advance : face)
                advance = reader.u16();
        for (auto& face : value.weightedAdvances)
            for (std::uint8_t& advance : face)
                advance = reader.u8();
        return value;
    });
    if (faces && !OSC::TextSizing::isAdoptable(*faces))
        return std::nullopt;
    return faces;
}

std::optional<Ack> decodeAck(std::span<const std::uint8_t> body)
{
    return decodeWhole<Ack>(body, [](WireReader& reader) {
        Ack message;
        message.hostTimeEcho = reader.u64();
        message.holdMicroseconds = reader.u32();
        message.bytesReceived = reader.u64();
        // A handle varint and three u64s: twenty-five bytes at least. A
        // connection holds no more panes than its cap, so an Ack names no more.
        const std::size_t count = reader.count(maxPanesPerConnection, 25);
        message.panes.reserve(count);
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            PaneAck pane;
            pane.handle = static_cast<std::uint32_t>(reader.varint(maxChannel));
            pane.generation = reader.u64();
            pane.historyAcked = reader.u64();
            pane.effectsAcknowledged = reader.u64();
            message.panes.push_back(pane);
        }
        return message;
    });
}

std::optional<Probe> decodeProbe(std::span<const std::uint8_t> body)
{
    return decodeWhole<Probe>(body, [](WireReader& reader) { return Probe { .time = reader.u64() }; });
}

std::optional<Echo> decodeEcho(std::span<const std::uint8_t> body)
{
    return decodeWhole<Echo>(body, [](WireReader& reader) {
        Echo message;
        message.timeEcho = reader.u64();
        message.holdMicroseconds = reader.u32();
        return message;
    });
}

std::optional<PaneBind> decodePaneBind(std::span<const std::uint8_t> body)
{
    return decodeWhole<PaneBind>(body, [](WireReader& reader) {
        PaneBind message;
        message.handle = static_cast<std::uint32_t>(reader.varint(maxChannel));
        if (message.handle == 0)
            reader.fail();
        message.paneID = takeID(reader);
        return message;
    });
}

std::optional<PaneUnbind> decodePaneUnbind(std::span<const std::uint8_t> body)
{
    return decodeWhole<PaneUnbind>(body, [](WireReader& reader) {
        PaneUnbind message;
        message.handle = static_cast<std::uint32_t>(reader.varint(maxChannel));
        if (message.handle == 0)
            reader.fail();
        message.reason = reader.u8();
        return message;
    });
}

std::optional<BulkChunk> decodeBulkChunk(std::span<const std::uint8_t> body)
{
    return decodeWhole<BulkChunk>(body, [](WireReader& reader) {
        BulkChunk message;
        message.transfer = reader.u32();
        message.index = reader.u32();
        message.isLast = takeBool(reader);
        message.bytes = reader.bytes(maxFrameBodyBytes);
        if (message.isLast) {
            takeArray(reader, message.sha256);
            if (!reader.isAtEnd()) {
                // Present only for a coding other than None.
                message.compression = takeEnumeration(reader, Compression::Deflate);
                if (message.compression != Compression::Deflate)
                    reader.fail();
                message.inflatedBytes = reader.u32();
            }
        }
        return message;
    });
}

std::optional<Bye> decodeBye(std::span<const std::uint8_t> body)
{
    if (body.size() > maxFrameBodyBytes)
        return std::nullopt;
    return decodeWhole<Bye>(body, [](WireReader& reader) {
        Bye message;
        message.reason = takeEnumeration(reader, lastByeReasonOnWire);
        message.detail = std::string(reader.utf8(maxFrameBodyBytes));
        if (takeBool(reader)) {
            PeerMetadata peer;
            peer.build.wireVersion = reader.u16();
            peer.build.appVersion = reader.utf8(maxAppVersionBytes);
            peer.platform = reader.utf8(maxHostPlatformBytes);
            if (!peer.build.wireVersion || !isText(peer.build.appVersion, maxAppVersionBytes) || !isText(peer.platform, maxHostPlatformBytes))
                reader.fail();
            peer.build.isKnown = reader.ok();
            message.peer = std::move(peer);
        }
        if (!isText(message.detail, maxFrameBodyBytes))
            reader.fail();
        return message;
    });
}

std::optional<CheckpointHead> decodeCheckpointHead(std::span<const std::uint8_t> body)
{
    return decodeWhole<CheckpointHead>(body, [](WireReader& reader) {
        CheckpointHead message;
        message.generation = reader.u64();
        message.transfer = reader.u32();
        message.frameClass = takeEnumeration(reader, lastFrameClass);
        message.stateDigest = takeDigest(reader);
        return message;
    });
}

std::optional<DecodedUpdateHeader> decodeUpdateHeader(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    UpdateHeader message;
    message.mode = takeEnumeration(reader, UpdateMode::Superset);
    message.baseGeneration = reader.u64();
    message.generation = reader.u64();
    message.frameClass = takeEnumeration(reader, lastFrameClass);
    message.hostTime = reader.u64();
    message.lagMicroseconds = reader.varint();
    message.inputWritten = reader.u64();
    message.inputSettled = reader.u64();
    message.historyHead = reader.u64();
    message.effectsThrough = reader.u64();
    // An update moves forward: its target lies past its base.
    if (!reader.ok() || message.generation <= message.baseGeneration)
        return std::nullopt;
    return DecodedUpdateHeader { .header = message, .bytes = reader.offset() };
}

std::optional<Resync> decodeResync(std::span<const std::uint8_t> body)
{
    return decodeWhole<Resync>(body, [](WireReader& reader) {
        Resync message;
        message.reason = takeEnumeration(reader, ResyncReason::BufferOverflow);
        message.generation = reader.u64();
        const std::size_t count = reader.count(maxSectionHashes, 8);
        message.sectionHashes.reserve(count);
        for (std::size_t index = 0; index < count && reader.ok(); ++index)
            message.sectionHashes.push_back(reader.u64());
        return message;
    });
}

std::optional<View> decodeView(std::span<const std::uint8_t> body)
{
    return decodeWhole<View>(body, [](WireReader& reader) { return View { .state = takeEnumeration(reader, ViewState::RecordsOnly) }; });
}

std::optional<BulkAbort> decodeBulkAbort(std::span<const std::uint8_t> body)
{
    return decodeWhole<BulkAbort>(body, [](WireReader& reader) { return BulkAbort { .transfer = reader.u32() }; });
}

std::optional<Call> decodeCall(std::span<const std::uint8_t> body)
{
    if (!isValidUTF8(body))
        return std::nullopt;
    return Call { .text = std::string(body.begin(), body.end()) };
}

std::optional<Result> decodeResult(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    const std::uint64_t transfer = reader.varint(std::numeric_limits<std::uint32_t>::max());
    if (!reader.ok())
        return std::nullopt;
    const std::span<const std::uint8_t> text = reader.take(reader.remaining());
    if ((transfer != 0 && !text.empty()) || !isValidUTF8(text))
        return std::nullopt;
    return Result { .transfer = static_cast<std::uint32_t>(transfer), .text = std::string(text.begin(), text.end()) };
}

std::optional<WorkspaceStructureHead> decodeWorkspaceStructureHead(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    WorkspaceStructureHead head;
    head.workspaceID = reader.u32();
    head.revision = reader.u64();
    const std::uint8_t flags = reader.u8();
    head.transfer = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
    if (!reader.ok() || flags > 1)
        return std::nullopt;
    head.isGone = flags == 1;
    const std::span<const std::uint8_t> record = reader.take(reader.remaining());
    // Gone carries nothing; otherwise the record is here or in a transfer,
    // and never both.
    if (head.isGone ? (head.transfer != 0 || !record.empty()) : ((head.transfer != 0) == !record.empty()))
        return std::nullopt;
    head.record.assign(record.begin(), record.end());
    return head;
}

std::optional<NotifyHead> decodeNotifyHead(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    const std::uint64_t kind = reader.varint();
    if (!reader.ok())
        return std::nullopt;
    return NotifyHead { .kind = kind, .record = body.subspan(reader.offset()) };
}

std::optional<Receipt> decodeReceipt(std::span<const std::uint8_t> body)
{
    return decodeWhole<Receipt>(body, [](WireReader& reader) {
        Receipt message;
        message.inputWritten = reader.u64();
        message.inputSettled = reader.u64();
        return message;
    });
}

std::optional<Keys> decodeKeys(std::span<const std::uint8_t> body)
{
    return decodeWhole<Keys>(body, [](WireReader& reader) {
        Keys message;
        message.inputSeq = reader.u64();
        // The smallest event: a key, its modifiers and kind, empty text, three
        // code points and its flags.
        constexpr std::size_t minEventBytes = 4 + 2 + 1 + 1 + 12 + 1;
        const std::size_t count = reader.count(maxInputEventsPerFrame, minEventBytes);
        if (message.inputSeq == 0 || count == 0)
            reader.fail();
        message.events.reserve(reader.ok() ? count : 0);
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            KeyEvent event;
            event.key = reader.u32();
            if (event.key > std::numeric_limits<std::uint16_t>::max())
                reader.fail();
            event.modifiers = reader.u16();
            event.kind = takeEnumeration(reader, KeyEventKind::Release);
            event.text = std::string(reader.utf8(maxKeyEventTextBytes));
            event.unshiftedCodePoint = reader.u32();
            event.shiftedCodePoint = reader.u32();
            event.baseLayoutCodePoint = reader.u32();
            event.wasModified = takeBool(reader);
            message.events.push_back(std::move(event));
        }
        return message;
    });
}

std::optional<Pointer> decodePointer(std::span<const std::uint8_t> body)
{
    return decodeWhole<Pointer>(body, [](WireReader& reader) {
        Pointer message;
        message.inputSeq = reader.u64();
        constexpr std::size_t eventBytes = 1 + 1 + 2 + 4 + 4;
        const std::size_t count = reader.count(maxInputEventsPerFrame, eventBytes);
        if (message.inputSeq == 0 || count == 0)
            reader.fail();
        message.events.reserve(reader.ok() ? count : 0);
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            PointerEvent event;
            event.kind = takeEnumeration(reader, PointerEventKind::Motion);
            event.button = reader.u8();
            event.modifiers = reader.u16();
            event.row = reader.u16();
            event.column = reader.u16();
            event.pixelX = reader.u16();
            event.pixelY = reader.u16();
            message.events.push_back(event);
        }
        return message;
    });
}

std::optional<Focus> decodeFocus(std::span<const std::uint8_t> body)
{
    return decodeWhole<Focus>(body, [](WireReader& reader) {
        return Focus { .isFocused = takeBool(reader) };
    });
}

std::optional<Viewport> decodeViewport(std::span<const std::uint8_t> body)
{
    return decodeWhole<Viewport>(body, [](WireReader& reader) {
        Viewport message;
        message.rows = reader.u16();
        message.columns = reader.u16();
        message.cellPixelWidth = reader.u16();
        message.cellPixelHeight = reader.u16();
        message.textAreaPixelWidth = reader.u16();
        message.textAreaPixelHeight = reader.u16();
        message.drawablePixelWidth = reader.u16();
        message.drawablePixelHeight = reader.u16();
        return message;
    });
}

std::optional<Raw> decodeRaw(std::span<const std::uint8_t> body)
{
    return decodeWhole<Raw>(body, [](WireReader& reader) {
        Raw message;
        message.inputSeq = reader.u64();
        message.bytes = reader.bytes(maxFrameBodyBytes);
        // Numbered from 1, and never empty: a Raw that writes nothing is no
        // event, and a queue that held one would be holding its entry for free.
        if (message.inputSeq == 0 || message.bytes.empty())
            reader.fail();
        return message;
    });
}

std::optional<Paste> decodePaste(std::span<const std::uint8_t> body)
{
    return decodeWhole<Paste>(body, [](WireReader& reader) {
        Paste message;
        message.inputSeq = reader.u64();
        message.offset = reader.u64();
        message.state = takeEnumeration(reader, PasteState::Final);
        message.text = reader.bytes(maxPasteChunkBytes);
        if (message.inputSeq == 0 || message.offset > maxPasteBytes || message.text.size() > maxPasteBytes - message.offset)
            reader.fail();
        return message;
    });
}

void encode(std::vector<std::uint8_t>& output, const InputDiscarded& message)
{
    WireWriter writer(output);
    writer.u64(message.inputWritten);
    writer.u64(message.throughSeq);
}

std::optional<InputDiscarded> decodeInputDiscarded(std::span<const std::uint8_t> body)
{
    const std::optional<InputDiscarded> decoded = decodeWhole<InputDiscarded>(body, [](WireReader& reader) {
        InputDiscarded message;
        message.inputWritten = reader.u64();
        message.throughSeq = reader.u64();
        return message;
    });
    return decoded && decoded->throughSeq > decoded->inputWritten ? decoded : std::nullopt;
}

std::optional<PasteReceipt> decodePasteReceipt(std::span<const std::uint8_t> body)
{
    return decodeWhole<PasteReceipt>(body, [](WireReader& reader) {
        PasteReceipt message;
        message.inputSeq = reader.u64();
        message.bytesWritten = reader.u64();
        return message;
    });
}

std::optional<PasteAbort> decodePasteAbort(std::span<const std::uint8_t> body)
{
    return decodeWhole<PasteAbort>(body, [](WireReader& reader) {
        PasteAbort message;
        message.inputSeq = reader.u64();
        if (message.inputSeq == 0)
            reader.fail();
        return message;
    });
}

std::optional<DecodedHistoryInlineHeader> decodeHistoryInlineHeader(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    DecodedHistoryInlineHeader decoded;
    decoded.header.historyEpoch = reader.u32();
    decoded.header.firstLine = reader.u64();
    if (!reader.ok())
        return std::nullopt;
    decoded.bytes = body.size() - reader.remaining();
    return decoded;
}

std::optional<HistoryHead> decodeHistoryHead(std::span<const std::uint8_t> body)
{
    return decodeWhole<HistoryHead>(body, [](WireReader& reader) {
        HistoryHead message;
        message.transfer = reader.u32();
        message.historyEpoch = reader.u32();
        message.firstLine = reader.u64();
        message.count = reader.u16();
        message.order = takeEnumeration(reader, HistoryOrder::Descending);
        if (message.count == 0)
            reader.fail();
        return message;
    });
}

std::optional<HistoryReset> decodeHistoryReset(std::span<const std::uint8_t> body)
{
    return decodeWhole<HistoryReset>(body, [](WireReader& reader) {
        HistoryReset message;
        message.newEpoch = reader.u32();
        message.clearedThrough = reader.u64();
        message.historyHead = reader.u64();
        if (message.historyHead < message.clearedThrough)
            reader.fail();
        return message;
    });
}

std::optional<HistoryWant> decodeHistoryWant(std::span<const std::uint8_t> body)
{
    return decodeWhole<HistoryWant>(body, [](WireReader& reader) {
        HistoryWant message;
        message.historyEpoch = reader.u32();
        const std::size_t count = reader.count(maxHistoryWantRanges, 16);
        message.ranges.reserve(count);
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            LineRange range;
            range.from = reader.u64();
            range.to = reader.u64();
            // Half-open and non-empty, and past the previous range's end: a
            // set the host can use without sorting or merging it.
            if (range.from >= range.to || (!message.ranges.empty() && range.from < message.ranges.back().to))
                reader.fail();
            message.ranges.push_back(range);
        }
        message.priority = takeEnumeration(reader, HistoryPriority::Backfill);
        return message;
    });
}

std::string_view byeNotice(ByeReason reason)
{
    switch (reason) {
    case ByeReason::Normal:
        return "The connection ended.";
    case ByeReason::VersionMismatch:
        return versionMismatchNotice();
    case ByeReason::Malformed:
        return "The connection ended on data that couldn’t be read. Connect again.";
    case ByeReason::Oversize:
        return "The connection ended on a message over the size limit. Connect again.";
    case ByeReason::UnknownKind:
        return "The connection ended on a message this app and the host don’t share. Update both to the same build.";
    case ByeReason::UnboundHandle:
        return "The connection ended on data for a pane it wasn’t showing. Connect again.";
    case ByeReason::TransferRefused:
        return "The connection ended on a transfer that arrived damaged or too large. Connect again.";
    case ByeReason::Superseded:
        return "This workspace moved to another connection.";
    case ByeReason::ResumeInProgress:
        return "This workspace is already reconnecting on another connection.";
    case ByeReason::Suspending:
        return "The connection is paused while this device sleeps.";
    case ByeReason::HandingOver:
        return "The host is updating. Your workspaces are being kept for reconnecting.";
    case ByeReason::TokenMissing:
        return "This host requires an access token.";
    case ByeReason::TokenInvalid:
        return "This host doesn’t recognize the access token.";
    case ByeReason::TokenExpired:
        return "The access token for this host has expired.";
    case ByeReason::TokenRevoked:
        return "The access token for this host was revoked.";
    case ByeReason::OutsideScope:
        return "This access token doesn’t allow this request.";
    case ByeReason::DuplicateHandshake:
        return "The host refused a second sign-in on one connection. Connect again.";
    // Consent is the reason most in need of spelling out: it is not about
    // the token at all, and reading it as one sends a person to reissue
    // credentials that were never the problem.
    case ByeReason::NoConsent:
        return "Nobody on the host could allow this connection. Open this workspace on the host first.";
    case ByeReason::RemoteHostRefused:
        return "The host can’t open that SSH destination. Give a plain host name or user@host.";
    case ByeReason::SessionGone:
        return "This workspace is no longer available on the host. Choose another workspace, or create a new one.";
    case ByeReason::HostFull:
        return "The host has reached its session limit. Close a session on the host, then try again.";
    case ByeReason::OutputBacklog:
        return "This device fell too far behind the host’s output. Reconnecting after a pause.";
    case ByeReason::Silent:
        return "The host heard nothing from this device for too long and dropped the connection. Reconnecting.";
    }
    return "The connection ended.";
}

} // namespace Ubiquitty::Protocol::Sync
