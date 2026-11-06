// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Pins deterministic byte encodings independently of the round-trip tests.
// A symmetric encoder/decoder change passes a round trip but moves these
// digests. Review the layout change before updating the affected corpus pin
// and Sync::wireFingerprint together with a wire-version increment, then
// regenerate the editor wire tables.
//
// The handshake compares both wireVersion and wireFingerprint. As specified
// in Docs/sync.md, every wire change increments the protocol version; the
// fingerprint also refuses a build that changed layouts without doing so.
// Local host handover has its own
// versioned format and frozen fixtures in test_sync_handover_body.cpp.

#include "Core/CellGrid.h"
#include "Core/Codec/ZlibDeflate.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Image/TerminalImage.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/Images.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/NativeDrag.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/RichText/RTFReader.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include <algorithm>
#include <array>
#include <cstdio>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

// FNV-1a over the corpus. A digest, not a checksum for a channel: it needs to
// move when any byte or any LENGTH moves, and to be reproducible by hand from
// the printed bytes. Written here rather than borrowed from GlyphKey.h, whose
// fnv1aCluster hashes uint32 code points for an atlas key and would tie this
// file's expected value to a rendering decision that has no reason to hold still
// for it.
static std::uint64_t wireDigest(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t hash = 1469598103934665603ull; // FNV offset basis
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull; // FNV prime
    }
    return hash;
}

// Every frame the catalog defines, from inputs that never vary. Values are
// arbitrary and must stay arbitrary: they are here to be encoded, and changing
// one is changing the fixture, which moves the digest for a reason that has
// nothing to do with the wire.
static void buildCorpus(std::vector<std::uint8_t>& wire)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    // The app version is fixed here, not this build's: the digest pins the
    // wire's layout, and a VERSION bump is not a wire change.
    Sync::Greet hello;
    hello.wireVersion = Sync::wireVersion;
    hello.appVersion = "1.2.3";
    // Fixed, not this build's: the corpus is what the fingerprint digests.
    hello.engineFingerprint = {};
    hello.clientInstance = { 3, 4 };
    hello.facts.viewportColumns = 120;
    hello.facts.viewportRows = 40;
    // Every bit a client may name. A host ignores a bit it does not know, so
    // only the fingerprint can tell a newer client's set from an older host's.
    hello.capabilities = Sync::ClientCapabilities::fromRaw(Sync::knownClientCapabilityBits);
    std::vector<std::uint8_t> body;
    Sync::encode(body, hello);
    REQUIRE(Sync::decodeGreet(body));
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, body));

    Sync::Welcome ack;
    ack.wireVersion = Sync::wireVersion;
    ack.appVersion = "1.2.3";
    ack.engineFingerprint = {};
    ack.hostInstanceID = { 9, 10 };
    ack.attachmentID = { 11, 12 };
    ack.resumeSecret = { 13, 14 };
    ack.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 0x12345678u, .workspaceID = 0, .daemonInstanceID = 9, .socketName = "fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    body.clear();
    Sync::encode(body, ack);
    REQUIRE(Sync::decodeWelcome(body));
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::Welcome, 0, body));

    // Multi-byte UTF-8 on purpose: a length written in characters rather than
    // bytes is a classic wire change, and an ASCII-only fixture cannot see it.
    // PaneFacts, the whole record every attachment of a pane is sent. Each
    // field distinct, so two swapped in the layout move the digest.
    Ubiquitty::Protocol::Sync::PaneFacts facts;
    facts.revision = 0x01020304u;
    facts.programTitle = "~/work — zsh";
    facts.currentDirectory = "/srv/work";
    facts.processChain = "vim ◂ -zsh";
    facts.processChainWithArguments = "vim notes.txt ◂ -zsh";
    facts.foreground.name = "vim";
    facts.shellCommandName = "zsh";
    facts.terminalName = "ttys003";
    facts.hasMarkedPrompt = true;
    std::vector<std::uint8_t> recordBody;
    Ubiquitty::Protocol::Sync::encodePaneFacts(recordBody, facts);
    std::vector<std::uint8_t> record;
    CHECK(Ubiquitty::Protocol::Sync::appendFrame(record, Ubiquitty::Protocol::Sync::FrameKind::PaneFacts, 1, recordBody));
    wire.insert(wire.end(), record.begin(), record.end());

    body.clear();
    Sync::encode(body, Sync::Bye {});
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::Bye, 0, body));

    // The faces a client draws proportional runs in: every advance distinct
    // from its neighbours and across faces, so a swapped face or a narrowed
    // field moves the digest.
    Ubiquitty::OSC::TextSizing::ProportionalFaceFacts faces;
    faces.cellAdvance = 0x00002A03u;
    faces.untabledAdvances = { 0x0405, 0x0607, 0x0809, 0x0A0B };
    for (std::size_t face = 0; face < faces.advances.size(); ++face)
        for (std::size_t index = 0; index < faces.advances[face].size(); ++index) {
            faces.advances[face][index] = static_cast<std::uint16_t>(0x0100 * (face + 1) + index);
            faces.weightedAdvances[face][index] = static_cast<std::uint8_t>((17 * face + index) % 129);
        }
    body.clear();
    Sync::encode(body, faces);
    REQUIRE(Sync::decodeProportionalFaces(body));
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::ProportionalFaces, 0, body));

    // A typed sampled sound, its exact announcement and raw PCM transfer.
    // The one wire range covers all three bodies and the effect's identity.
    //
    // Every field is set to something other than its default, and to a value
    // distinct from its neighbours' where the widths allow: a fixture that left
    // them zero would digest the same after two adjacent fields swapped places.
    Ubiquitty::PAL::PlayTonesRequest tones;
    tones.volume = 5;
    tones.wave = 6;
    tones.channel = 3;
    tones.attackMilliseconds = 11;
    tones.releaseMilliseconds = 22;
    tones.semitonesDown = 4;
    tones.durationMilliseconds = 333;
    tones.gapMilliseconds = 44;
    tones.sampleSlot = 6;
    tones.decayMilliseconds = 55;
    tones.sustainLevel = 7;
    tones.reverbSend = 1;
    tones.gridUnits = 8;
    tones.lowpassHertz = 660;
    tones.highpassHertz = 77;
    tones.pan = 9;
    tones.repeat = 10;
    tones.count = 2;
    tones.freqs[0] = 440;
    tones.freqs[1] = 880;
    tones.noteVolumes[0] = 6;
    tones.noteVolumes[1] = 7;
    tones.noteEffects[0] = 14;
    tones.noteEffects[1] = 15;
    tones.noteParameters[0] = 16;
    tones.noteParameters[1] = 17;
    constexpr std::array<std::int16_t, 5> pcm { -32768, -1, 0, 1, 32767 };
    const auto sample = Sync::SampleBytes::capture(9, pcm, pcm.size() * 2);
    REQUIRE(sample);
    std::vector<std::uint8_t> sampleHead;
    REQUIRE(Sync::encodeSampleHead(sampleHead, { .slot = tones.sampleSlot, .sample = sample->sample, .transfer = 7 }));
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::SampleHead, 1, sampleHead));
    wire.insert(wire.end(), record.begin(), record.end());
    std::vector<std::uint8_t> sampleChunk;
    Sync::encode(sampleChunk, Sync::BulkChunk { .transfer = 7, .index = 0, .isLast = true, .bytes = sample->view(), .sha256 = sample->sample.sha256 });
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::BulkChunk, 0, sampleChunk));
    wire.insert(wire.end(), record.begin(), record.end());
    const auto soundBody = Sync::encodeEffectBody(Sync::SoundEffect { .request = tones, .sample = sample->sample });
    REQUIRE(soundBody);
    constexpr Sync::ID128 pane { 11, 12 };
    const auto sound = Sync::encodeEffect({ .paneID = pane, .generation = 3, .sequence = 4, .primary = { 13, 14 }, .kind = Sync::EffectKind::Sound, .payload = soundBody->view() });
    REQUIRE(sound);
    const auto packet = Sync::encodeEffectPacket(pane, 3, 4, 0, sound->view());
    REQUIRE(packet);
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::Effect, 1, packet->view()));
    wire.insert(wire.end(), record.begin(), record.end());

    // Consent has one canonical grammar: requested/resolved human questions,
    // a strictly bound pending Welcome/own status, and correlated decisions.
    // Each body travels directly in its canonical envelope.
    const auto appendControlText = [&](Sync::FrameKind kind, const std::string& text) {
        REQUIRE(!text.empty());
        if (kind == Sync::FrameKind::Call) {
            const auto request = Sync::decodeCallRequest(text);
            REQUIRE(request.outcome == Ubiquitty::Protocol::Control::RequestOutcome::Dispatch);
            if (request.method == "consent.respond")
                REQUIRE(Ubiquitty::Protocol::Control::readConsentResponse(request.params));
            else {
                REQUIRE(request.method == "consent.revoke");
                REQUIRE(Ubiquitty::Protocol::Control::readConsentRevocation(request.params));
            }
        }
        record.clear();
        CHECK(Sync::appendFrame(record, kind, 0, { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }));
        wire.insert(wire.end(), record.begin(), record.end());
    };
    // A Notify is a kind and its record; each is read back through its kind's
    // decoder before it joins the corpus.
    const auto appendNotify = [&](const std::optional<std::vector<std::uint8_t>>& body, Sync::NotifyKind kind) {
        REQUIRE(body);
        const auto head = Sync::decodeNotifyHead(*body);
        REQUIRE(head && head->kind == std::to_underlying(kind));
        record.clear();
        CHECK(Sync::appendFrame(record, Sync::FrameKind::Notify, 0, *body));
        wire.insert(wire.end(), record.begin(), record.end());
    };
    Sync::ConsentQuestion consent;
    consent.requestID = 0x0A0B0C0Du;
    consent.sessionID = 0x01020304u;
    consent.peerProcessID = 4242;
    consent.peerProcess = "/usr/bin/agent";
    consent.agentLabel = "claimed agent — λ";
    consent.action = 2;
    consent.capabilityCode = 0x000B;
    consent.capabilityOrigin = 2;
    consent.isRecipientsOwnRequest = true;
    const auto requested = Sync::consentQuestionNotify(consent);
    REQUIRE(requested && Sync::decodeConsentQuestion(Sync::decodeNotifyHead(*requested)->record));
    appendNotify(requested, Sync::NotifyKind::ConsentQuestion);
    const auto resolved = Sync::consentResolvedNotify({ Sync::ConsentKind::Capability, consent.requestID, consent.sessionID });
    REQUIRE(resolved && Sync::decodeConsentResolved(Sync::decodeNotifyHead(*resolved)->record));
    appendNotify(resolved, Sync::NotifyKind::ConsentResolved);
    const auto structure = Sync::structureConsentNotify({ .requestID = 19, .sessionID = consent.sessionID, .workspaceID = 23, .revision = 0x0102030405060708, .workspaceName = "work — λ", .method = "workspace.rename", .parameters = R"({"workspace":23,"name":"renamed"})", .agentLabel = "claimed builder", .peerProcess = "/usr/bin/verified", .peerProcessID = 29 });
    REQUIRE(structure && Sync::decodeStructureConsent(Sync::decodeNotifyHead(*structure)->record));
    appendNotify(structure, Sync::NotifyKind::StructureConsent);
    appendControlText(Sync::FrameKind::Call, Ubiquitty::Protocol::Control::consentResponseRequest({ consent.requestID, Sync::ConsentKind::Capability, Sync::ConsentChoice::AllowSession }, "consent.respond.7"));
    appendControlText(Sync::FrameKind::Call, Ubiquitty::Protocol::Control::consentRevocationRequest({ .sessionID = consent.sessionID, .action = Sync::ConsentRevocationKind::SessionGrant, .attachmentID = {}, .identity = "token:verified-grant-key" }, "consent.revoke.8"));
    const Sync::ConsentBinding question { .requestID = 31, .sessionID = consent.sessionID };
    Sync::Welcome pending;
    pending.wireVersion = Sync::wireVersion;
    pending.engineFingerprint = {};
    pending.hostInstanceID = { 37, 41 };
    pending.attachmentID = { 43, 47 };
    pending.resumeSecret = { 53, 59 };
    pending.grantedRole = Sync::Role::Agent;
    pending.admission = Sync::AttachmentAdmission::AwaitingConsent;
    pending.consent = question;
    pending.principal = "token:verified-principal";
    pending.limits = Sync::provisionalLimits;
    pending.hostTime = 0x0102030405060708;
    pending.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = consent.sessionID, .workspaceID = 0, .daemonInstanceID = 37, .socketName = "pending.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    recordBody.clear();
    Sync::encode(recordBody, pending);
    REQUIRE(Sync::decodeWelcome(recordBody));
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::Welcome, 0, recordBody));
    wire.insert(wire.end(), record.begin(), record.end());
    for (const auto phase : { Sync::ConsentStatusPhase::Pending, Sync::ConsentStatusPhase::Approved }) {
        const auto status = Sync::consentStatusNotify({ .question = question, .phase = phase });
        REQUIRE(status && Sync::decodeConsentStatus(Sync::decodeNotifyHead(*status)->record));
        appendNotify(status, Sync::NotifyKind::ConsentStatus);
    }
    const auto attachments = Sync::attachmentsNotify({ .revision = 0x0102030405060708,
        .attachments = { { .attachmentID = { 61, 67 }, .role = Sync::AttachmentRole::Driver, .clientKind = Sync::ClientKind::Human, .state = Sync::AttachmentState::Live, .isGeometryOwner = true, .isPrimary = true, .panes = { pane, { 17, 19 } }, .isSelf = true, .principal = "owner:verified-principal", .presence = Sync::AttachmentPresence { .clientID = 71, .processID = 73, .process = "/usr/bin/verified", .claimedLabel = "claimed — λ", .grantIdentity = "token:verified-grant-key", .isQUIC = true, .sharedPanes = { { pane, Sync::PresenceGrant::Session }, { { 17, 19 }, Sync::PresenceGrant::Always } } } } } });
    REQUIRE(attachments && Sync::decodeAttachments(Sync::decodeNotifyHead(*attachments)->record));
    appendNotify(attachments, Sync::NotifyKind::Attachments);

    const auto appendEffect = [&](Sync::EffectKind kind, std::uint64_t sequence, const Sync::EffectBody& body, std::uint8_t target) {
        const auto encodedBody = Sync::encodeEffectBody(body);
        REQUIRE(encodedBody);
        const auto effect = Sync::encodeEffect({ .paneID = pane, .generation = 3, .sequence = sequence, .primary = { 13, 14 }, .kind = kind, .target = target, .payload = encodedBody->view() });
        REQUIRE(effect);
        const auto packet = Sync::encodeEffectPacket(pane, 3, sequence, 0, effect->view());
        REQUIRE(packet);
        record.clear();
        CHECK(Sync::appendFrame(record, Sync::FrameKind::Effect, 1, packet->view()));
        wire.insert(wire.end(), record.begin(), record.end());
    };
    constexpr std::array<std::uint8_t, 4> plain { 'c', 'o', 'p', 'y' };
    constexpr std::array<std::uint8_t, 3> html { '<', 'b', '>' };
    Sync::EffectClipboardBundle bundle;
    bundle.count = 2;
    bundle.items[0] = { "text/plain", plain };
    bundle.items[1] = { "text/html", html };
    const auto clipboard = Sync::encodeEffectClipboard(bundle, Sync::maxEffectClipboardWriteBytes);
    REQUIRE(clipboard);
    appendEffect(Sync::EffectKind::ClipboardWrite, 5, Sync::ClipboardWriteEffect { Ubiquitty::ClipboardBuffer::Primary, { .transfer = 0, .encodedBytes = static_cast<std::uint32_t>(clipboard->size), .inlineBytes = clipboard->view() } }, std::to_underlying(Ubiquitty::ClipboardBuffer::Primary));
    appendEffect(Sync::EffectKind::Notification, 6, Sync::NotificationEffect { "Build", "finished — ok", "build-7", Ubiquitty::NotificationOccasion::Invisible }, 0);

    // The event stream is one correlated Call and bounded `Events` notifies;
    // a cursor in the Call is a decimal string, never a floating-point value.
    const auto subscription = Ubiquitty::Protocol::Control::eventSubscriptionRequest({ .after = (std::uint64_t { 1 } << 53) + 17, .isFromLatest = false }, "events.7");
    REQUIRE(!subscription.empty());
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::Call, 0, { reinterpret_cast<const std::uint8_t*>(subscription.data()), subscription.size() }));
    wire.insert(wire.end(), record.begin(), record.end());
    const std::array events { Ubiquitty::Session::Event { .cursor = 0x0102030405060708, .kind = Ubiquitty::Session::EventKind::CommandFinished, .sessionID = 7, .paneID = 8, .exitStatus = -17, .state = Ubiquitty::Session::EventProgramState::Blocked, .isAtPrompt = true, .count = std::numeric_limits<std::uint64_t>::max(), .text = std::string("a\0b", 3), .detail = "/work/\xCE\xBB" } };
    appendNotify(Ubiquitty::Protocol::Control::eventsNotify(events), Sync::NotifyKind::Events);

    // A pasted file has one manifest, exact upload credit, raw verified bytes
    // and a final Result, all in direct canonical envelopes.
    constexpr std::array<std::uint8_t, 4> pasted { 0x89, 'P', 0, 0xFF };
    Ubiquitty::Protocol::Control::PastePushRequest paste { .pane = 7, .mime = "image/png", .bytes = pasted.size() };
    Ubiquitty::Crypto::sha256(pasted, paste.sha256);
    const auto pasteCall = Ubiquitty::Protocol::Control::pastePushRequest(paste, "paste.7");
    REQUIRE(!pasteCall.empty());
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::Call, 0, { reinterpret_cast<const std::uint8_t*>(pasteCall.data()), pasteCall.size() }));
    wire.insert(wire.end(), record.begin(), record.end());
    appendNotify(Ubiquitty::Protocol::Control::pasteReadyNotify({ "paste.7", 7, 0x01020304 }), Sync::NotifyKind::PasteReady);
    recordBody.clear();
    Sync::encode(recordBody, Sync::BulkChunk { .transfer = 0x01020304, .index = 0, .isLast = true, .bytes = pasted, .sha256 = paste.sha256, .compression = Sync::Compression::None, .inflatedBytes = 0 });
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::BulkChunk, 0, recordBody));
    wire.insert(wire.end(), record.begin(), record.end());
    recordBody.clear();
    Sync::encode(recordBody, Sync::Result { .transfer = 0, .text = R"({"jsonrpc":"2.0","id":"paste.7","result":{"pane":7,"status":"busy","path":"/tmp/ubiquitty-paste-X.png"}})" });
    record.clear();
    CHECK(Sync::appendFrame(record, Sync::FrameKind::Result, 0, recordBody));
    wire.insert(wire.end(), record.begin(), record.end());

    // A typed refusal is the frame two builds that disagree still exchange, so
    // its body moving without the version is the skew that cannot report
    // itself: the refusal meant to say "different builds" is the frame misread.
    const Sync::Bye denied { .reason = Sync::ByeReason::VersionMismatch,
        .detail = "Different wire versions.",
        .peer = Sync::PeerMetadata { .build = { .isKnown = true, .wireVersion = 0x0203, .appVersion = "1.2.3" }, .platform = "Linux aarch64" } };
    body.clear();
    Sync::encode(body, denied);
    REQUIRE(Sync::decodeBye(body));
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::Bye, 0, body));

    // Clearing a pane is a correlated Call with a named command, not an enum
    // ordinal carried outside the current grammar.
    const Sync::Call clear { .text = R"({"jsonrpc":"2.0","id":"clear.9","method":"pane.clear","params":{"pane":7,"command":"previous-mark"}})" };
    body.clear();
    Sync::encode(body, clear);
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::Call, 0, body));

    const Ubiquitty::DragDrop::Command dragCommand {
        .kind = Ubiquitty::DragDrop::CommandKind::RemoteFileData, .sessionID = 17, .requestID = 29, .metadata = { .type = Ubiquitty::DragDrop::Type::File, .x = -1, .y = 2, .pixelX = 31, .pixelY = 37, .identity = 41, .operation = 3, .hasType = true, .hasMore = true, .hasMoreChunks = true }, .payload = "native file", .isFinal = false
    };
    body.clear();
    REQUIRE(Sync::encodeNativeDragCommand(body, 43, dragCommand));
    REQUIRE(Sync::decodeNativeDragCommand(body));
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::NativeDragCommand, 7, body));
    const Ubiquitty::DragDrop::NativeEvent dragEvent {
        .kind = Ubiquitty::DragDrop::EventKind::ReadData, .sessionID = 47, .requestID = 53, .x = -1, .y = 2, .pixelX = 3, .pixelY = 5, .operation = 1, .error = Ubiquitty::DragDrop::Error::InputOutput, .isFinal = true, .isSameWindow = true
    };
    body.clear();
    REQUIRE(Sync::encodeNativeDragEvent(body, 59, dragEvent, {}));
    REQUIRE(Sync::decodeNativeDragEvent(body));
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::NativeDragEvent, 7, body));

    // The palette block a Dormant pane is sent when a theme moves its colours.
    auto themed = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*themed, 2, 4);
    Ubiquitty::processBytes(themed.get(), "\033]11;rgb:fa/fa/f0\033\\\033]4;1;rgb:c0/20/30\033\\");
    body.clear();
    Sync::encodePaletteState(*themed, body);
    CHECK(Sync::appendFrame(wire, Sync::FrameKind::PanePalette, 7, body));
}

// Each body records the wire version and exact bytes used by its corpus.
struct Fingerprint {
    const char* body; // the constant's name, as the reader will grep for it
    std::uint32_t recordedVersion;
    std::size_t recordedLength;
    std::uint64_t recordedDigest;
};

static void checkFingerprint(const Fingerprint& recorded, std::uint32_t liveVersion,
    const std::vector<std::uint8_t>& bytes)
{
    const std::uint64_t digest = wireDigest(bytes);

    if (liveVersion != recorded.recordedVersion) {
        std::printf("%s: moved %u -> %u; re-mint with length=%zu digest=0x%016llx\n",
            recorded.body, static_cast<unsigned>(recorded.recordedVersion),
            static_cast<unsigned>(liveVersion), bytes.size(),
            static_cast<unsigned long long>(digest));
        CHECK(false); // fails so the pair is re-minted in the same edit
        return;
    }

    if (bytes.size() != recorded.recordedLength || digest != recorded.recordedDigest) {
        std::printf("%s: the body changed but the version is still %u.\n",
            recorded.body, static_cast<unsigned>(liveVersion));
        std::printf("  recorded: length=%zu digest=0x%016llx\n",
            recorded.recordedLength, static_cast<unsigned long long>(recorded.recordedDigest));
        std::printf("  now:      length=%zu digest=0x%016llx\n",
            bytes.size(), static_cast<unsigned long long>(digest));
        std::printf("  Increment wireVersion, then update this pin and wireFingerprint\n"
                    "  and regenerate the editor wire tables (Docs/sync.md).\n");
    }
    CHECK(bytes.size() == recorded.recordedLength);
    CHECK(digest == recorded.recordedDigest);
}

// A body encoded twice from one source, and once more from a separately built
// twin. The first comparison catches an encoder that is not a function of its
// inputs; the second catches a field carrying an address, an allocation counter
// or a per-instance id, which compares equal to itself and differs between two
// builds of the same state. Without the second, a digest can pass here and flap
// in another checkout.
static void checkDeterminism(const std::vector<std::uint8_t>& first,
    const std::vector<std::uint8_t>& second, const std::vector<std::uint8_t>& fromTwin)
{
    CHECK(first.size() == second.size());
    CHECK(first == second);
    CHECK(fromTwin == first);
}

// A grid whose every cell differs from its neighbours, so a field that moves
// within a cell shows up rather than hiding behind repeated values. Small on
// purpose: the digest has to be reproducible by hand from a hexdump when someone
// disputes it, and 4x6 fits on a screen.
static std::unique_ptr<Ubiquitty::Terminal> paintedTerminal()
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 4, 6);
    for (int row = 0; row < terminal->rows; ++row) {
        for (int column = 0; column < terminal->columns; ++column) {
            Ubiquitty::Cell cell {};
            cell.codePoint = static_cast<std::uint32_t>('A' + (row * terminal->columns + column));
            cell.pen.attributes = Ubiquitty::VT::AttributeSet::fromRaw(static_cast<std::uint16_t>(row));
            cell.pen.foregroundIndex = static_cast<std::uint16_t>(column);
            cell.pen.backgroundIndex = Ubiquitty::colorDefault;
            cell.pen.underlineIndex = Ubiquitty::colorDefault;
            cell.pen.semanticRole = static_cast<Ubiquitty::SemanticRole>((row + column) % 3);
            cell.pen.isForegroundAssignedDefault = (row + column) % 2;
            cell.pen.foregroundRed = static_cast<std::uint8_t>(row * 16);
            cell.pen.foregroundGreen = static_cast<std::uint8_t>(column * 8);
            cell.pen.foregroundBlue = 0x33;
            cell.pen.underlineStyle = 0;
            cell.pen.extendedAttributes = {};
            if (row == 1 && column == 2) {
                cell.combining[0] = 0x0301; // one combining mark, for the cluster path
                cell.setCombiningLength(1);
            }
            terminal->grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
        }
    }
    terminal->screens.active->cursorY = 2;
    terminal->screens.active->cursorX = 3;
    terminal->modes.isCursorVisible = true;
    // The pinned body was minted with the CRT saver set; the mode starts reset.
    terminal->modes.shouldSaveCRT = true;
    // A steady block (DECSCUSR 2). The body carries the cursor's resolved blink
    // alpha, and a blinking style resolves it from the blink model at tick 0,
    // so a digest taken with one moves with that model rather than with the
    // encoding; a steady block resolves to a full alpha under any model.
    terminal->cursorStyle = Ubiquitty::Cursor::Style::SteadyBlock;
    terminal->titleUTF8 = "~/work — zsh";
    Ubiquitty::processBytes(terminal.get(),
        "\033]3008;start=shell;type=shell;user=alice;hostname=local;machineid=0123456789abcdef0123456789abcdef;bootid=fedcba9876543210fedcba9876543210;pid=42;pidfdid=43;comm=zsh;cwd=/work;cmdline=make;vm=guest;container=build;targetuser=root;targethost=remote;sessionid=login\a"
        "\033]3008;start=job;type=command\a\033]3008;end=job;exit=failure;status=3;signal=SIGTERM\a"
        "\033]7501;state=working:app=build:title=VGl0bGU=:msg=TWVzc2FnZQ==:progress=37\a"
        "\033]7501;state=blocked:id=worker:kind=permission\a");
    return terminal;
}

/// The screen plane's checkpoint body has not moved under a standing
/// `wireVersion`.
///
/// The body carries the grid, the history, the state blocks and the
/// dictionaries; a silent change to any is what an attaching client refuses as
/// malformed, or decodes into the wrong state.
TEST(testTheCheckpointBodyCannotMoveWhileTheWireVersionStandsStill)
{
    auto host = paintedTerminal();
    const std::vector<std::uint8_t> first = UbiquittyTest::checkpointBody(*host);
    const std::vector<std::uint8_t> second = UbiquittyTest::checkpointBody(*host);
    auto twin = paintedTerminal();
    const std::vector<std::uint8_t> fromTwin = UbiquittyTest::checkpointBody(*twin);
    CHECK(!first.empty());
    checkDeterminism(first, second, fromTwin);

    checkFingerprint({ "wireVersion", 2, 2041, 0xf36b398ae67f02b8ull }, Ubiquitty::Protocol::Sync::wireVersion, first);
}

/// The frame catalog's encoding has not moved under a standing version.
///
/// Fails on any change to a frame header, a field order, a width or an
/// endianness. A round trip sees only the subset that also changes a decoded
/// value; this sees the bytes, so a change applied to both ends at once still
/// fails here.
TEST(testTheWireFormatCannotMoveWhileItsVersionStandsStill)
{
    // Two encodings of one fixture, compared before anything is claimed about
    // the digest. A digest over a nondeterministic encoder would fail on an
    // innocent day and be re-minted until it happened to stick, which is a worse
    // state than no gate: it would still be here, still green, and pinning
    // nothing. Uninitialised padding written into the wire is the usual cause
    // and is a defect in its own right.
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    buildCorpus(first);
    buildCorpus(second);
    CHECK(first.size() == second.size());
    CHECK(first == second); // the encoder is a function of its inputs

    const std::uint64_t digest = wireDigest(first);

    // This corpus is encoded with Sync::wireVersion == 2. The full SHA-256
    // below is the fingerprint checked by the handshake.
    constexpr std::uint16_t fingerprintedVersion = 2;
    constexpr std::uint64_t fingerprintedDigest = 0xcd3fce1a76c9c8c6ull;
    constexpr std::size_t fingerprintedLength = 15669;

    if (Ubiquitty::Protocol::Sync::wireVersion != fingerprintedVersion) {
        std::printf("wire fingerprint: Ubiquitty::Protocol::Sync::wireVersion moved %u -> %u; "
                    "re-mint with length=%zu digest=0x%016llx\n",
            static_cast<unsigned>(fingerprintedVersion), static_cast<unsigned>(Ubiquitty::Protocol::Sync::wireVersion),
            first.size(), static_cast<unsigned long long>(digest));
        CHECK(false); // fails so the digest below is re-minted in the same edit
        return;
    }

    if (first.size() != fingerprintedLength || digest != fingerprintedDigest) {
        std::printf("wire fingerprint: the encoded corpus changed but "
                    "Ubiquitty::Protocol::Sync::wireVersion is still %u.\n",
            static_cast<unsigned>(Ubiquitty::Protocol::Sync::wireVersion));
        std::printf("  recorded: length=%zu digest=0x%016llx\n",
            fingerprintedLength, static_cast<unsigned long long>(fingerprintedDigest));
        std::printf("  now:      length=%zu digest=0x%016llx\n",
            first.size(), static_cast<unsigned long long>(digest));
        std::printf("  Increment wireVersion, then update this pin and wireFingerprint\n"
                    "  and regenerate the editor wire tables (Docs/sync.md).\n");
    }
    CHECK(first.size() == fingerprintedLength);
    CHECK(digest == fingerprintedDigest);
}

/// A complete typed ImagePlacements body, including the asset identity,
/// virtual metadata and every placement geometry field, stays versioned.
static Ubiquitty::Protocol::Sync::ImagePlacements placementsFixture()
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return [] {
        Sync::ImagePlacements state;
        state.revision = 0x01020304;
        state.imageCount = 1;
        state.images[0] = { .imageID = 17, .imageNumber = 19, .asset = { .hash = { 0x0102030405060708, 0x1112131415161718 }, .format = 24, .width = 8, .height = 4 }, .isVirtual = true, .virtualColumns = 2, .virtualRows = 3, .virtualPlacementID = 23 };
        state.placementCount = 1;
        state.placements[0] = { .hash = state.images[0].asset.hash,
            .geometry = { .used = true, .imageID = 17, .placementID = 29, .hasPlacementID = true, .cursorX = 5, .cursorY = -6, .cellColumns = 7, .cellRows = 8, .pixelOffsetX = 9, .pixelOffsetY = 10, .sourceX = 1, .sourceY = 2, .sourceWidth = 3, .sourceHeight = 1, .drawnPixelWidth = 11, .drawnPixelHeight = 12, .zIndex = -31, .cursorPolicy = 1, .onAlternateScreen = 1 } };
        return state;
    }();
}

TEST(testTheImagePlacementsBodyCannotMoveWhileItsVersionStandsStill)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto host = placementsFixture();
    const auto twin = placementsFixture();
    REQUIRE(Sync::isValid(host));
    REQUIRE(Sync::isValid(twin));
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    std::vector<std::uint8_t> fromTwin;
    Sync::encode(first, host);
    Sync::encode(second, host);
    Sync::encode(fromTwin, twin);
    checkDeterminism(first, second, fromTwin);

    checkFingerprint({ "wireVersion", 2, 103, 0x2493352285e2eeabull }, Ubiquitty::Protocol::Sync::wireVersion, first);
}

// A small image whose bytes vary, filled into the Terminal's own slot.
//
// The slot's `pixels` is a raw owned pointer, so the buffer is the caller's and
// is unhooked again before the Terminal is destroyed, since the Terminal
// would otherwise free a buffer it does not own.
//
// Varying rather than flat: every pixel byte participates in the asset's
// content identity and the exact raw BulkChunk payload.
//
// Non-square for the same class of reason the config fixture gives every field a
// distinct value: width and height sit next to each other on the wire, and a
// square image encodes the same either way round. Measured — swapping the two in
// the encoder and the decoder moves this digest, and over an 8x8 image it would
// not have.
struct ImageFixture {
    std::vector<std::uint8_t> pixels;
    std::unique_ptr<Ubiquitty::Terminal> terminal;
};

static ImageFixture terminalWithImage()
{
    ImageFixture fixture;
    fixture.terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*fixture.terminal, 4, 6);

    constexpr int imageWidth = 8;
    constexpr int imageHeight = 4;
    fixture.pixels.resize(static_cast<std::size_t>(imageWidth) * imageHeight * 3);
    for (std::size_t i = 0; i < fixture.pixels.size(); ++i)
        fixture.pixels[i] = static_cast<std::uint8_t>(i * 7 + (i % 5));

    Ubiquitty::Image::TerminalImage& stored = fixture.terminal->images.stored[0];
    stored.used = true;
    stored.imageID = 40;
    stored.width = imageWidth;
    stored.height = imageHeight;
    stored.format = 24; // RGB
    stored.pixelLength = static_cast<std::uint32_t>(fixture.pixels.size());
    stored.pixels = fixture.pixels.data();
    return fixture;
}

static void releaseImage(ImageFixture& fixture)
{
    Ubiquitty::Image::TerminalImage& stored = fixture.terminal->images.stored[0];
    stored.used = false;
    stored.pixels = nullptr; // the buffer is this fixture's, not the Terminal's
}

/// A typed ImageHead and its exact BulkChunk stay versioned together: the
/// raw pixels an editor client takes, then the zlib stream of the same pixels
/// a client naming `DeflateImages` takes. All bodies use direct canonical
/// envelopes.
static std::vector<std::uint8_t> encodeImage(const ImageFixture& fixture)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return [&fixture] {
        std::vector<std::uint8_t> wire;
        Sync::ImagePublisher publisher(*fixture.terminal);
        if (!publisher.capture() || publisher.state().imageCount != 1) {
            CHECK(false);
            return wire;
        }
        const auto send = [&](std::uint32_t transfer, std::span<const std::uint8_t> bytes, Sync::Compression coding) {
            std::vector<std::uint8_t> head;
            Sync::encode(head, Sync::ImageHead { .asset = publisher.state().images[0].asset, .transfer = transfer });
            std::vector<std::uint8_t> frame;
            CHECK(Sync::appendFrame(frame, Sync::FrameKind::ImageHead, 1, head));
            wire.insert(wire.end(), frame.begin(), frame.end());
            Sync::BulkChunk chunk { .transfer = transfer, .index = 0, .isLast = true, .bytes = bytes };
            Ubiquitty::Crypto::sha256(bytes, chunk.sha256);
            if (coding == Sync::Compression::Deflate) {
                chunk.compression = coding;
                chunk.inflatedBytes = static_cast<std::uint32_t>(fixture.pixels.size());
            }
            std::vector<std::uint8_t> body;
            Sync::encode(body, chunk);
            frame.clear();
            CHECK(Sync::appendFrame(frame, Sync::FrameKind::BulkChunk, 0, body));
            wire.insert(wire.end(), frame.begin(), frame.end());
        };
        send(7, fixture.pixels, Sync::Compression::None);
        std::vector<std::uint8_t> stream(Ubiquitty::Codec::Zlib::compressBound(fixture.pixels.size()));
        stream.resize(Ubiquitty::Codec::Zlib::compressInto(fixture.pixels, stream));
        send(8, stream, Sync::Compression::Deflate);
        return wire;
    }();
}

TEST(testTheImageBodyCannotMoveWhileItsVersionStandsStill)
{
    ImageFixture host = terminalWithImage();
    ImageFixture twin = terminalWithImage();
    const auto first = encodeImage(host);
    const auto second = encodeImage(host);
    const auto fromTwin = encodeImage(twin);
    releaseImage(host);
    releaseImage(twin);

    CHECK(!first.empty()); // the fixture really produced a body
    checkDeterminism(first, second, fromTwin);

    checkFingerprint({ "wireVersion", 2, 364, 0x9bf427e46d5eb09dull }, Ubiquitty::Protocol::Sync::wireVersion, first);
}

// Retained source, pane-scoped renderer identity and native measurement travel
// in distinct records. Pin all of them, including nonzero source revision and
// viewport anchor, so a symmetric encoder/decoder change still rejects peers.
static void buildRichTextCorpus(std::vector<std::uint8_t>& wire)
{
    namespace RichText = Ubiquitty::RichText;
    constexpr std::string_view body = R"({\rtf1{\fonttbl{\f0\fswiss Fixture;}}\deff0\fs22 A\b BC\par D})";
    auto document = RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    REQUIRE(document);
    RichText::TypographyFacts facts;
    facts.rendererRevision = 0x0102030405060708;
    facts.cellWidth = 513;
    facts.cellHeight = 1025;
    facts.baseline = 769;
    facts.proportionalSize = 705;
    facts.monospaceSize = 801;
    facts.rendersBlocks = 1;
    facts.shaping = 1;
    facts.fontCatalogScope = RichText::FontCatalogScope::System;
    facts.faceCount = 1;
    auto& face = facts.faces[0];
    constexpr std::string_view name = "Fixture";
    std::ranges::copy(name, face.name.begin());
    face.nameLength = static_cast<std::uint8_t>(name.size());
    face.ascent = 1701;
    face.descent = 403;
    face.lineHeight = 2205;
    face.averageAdvance = 907;
    for (std::size_t index = 0; index < face.advances.size(); ++index)
        face.advances[index] = static_cast<std::uint16_t>(301 + index);
    std::vector<std::uint8_t> encoded;
    encode(encoded, facts);
    REQUIRE(decodeTypographyFacts(encoded));
    CHECK(appendFrame(wire, FrameKind::TypographyFacts, 7, encoded));

    RichText::MeasureRequest request { 0x11223344, 0x5566,
        { 19, 3, RichText::Overflow::Ellipsis, RichText::Alignment::Right, RichText::LineMode::Terminal, { 1, 2, 3, 4 }, 2 },
        {}, true, facts.rendererRevision };
    REQUIRE(request.document.tryAssign(*document));
    encoded.clear();
    WireWriter requestWriter(encoded);
    writeMeasureRequest(requestWriter, request);
    CHECK(appendFrame(wire, FrameKind::RichTextMeasure, 7, encoded));

    RichText::Measurement measured { .width = 3, .height = 1, .shown = 5, .capacity = 7, .lineCapacity = 3, .epoch = 0x10203, .cellWidth = facts.cellWidth, .cellHeight = facts.cellHeight, .baseline = facts.baseline, .fontSize = facts.proportionalSize, .lineHeight = 1025, .lineBaseline = 769, .minWidth = 2, .isFitting = false, .isExact = true, .isShaped = true, .lineEnds = { 5 }, .geometry = {}, .rendererRevision = facts.rendererRevision };
    measured.geometry.lines.push_back({ 0, 5, 0, 1025, 769, 128, 1792 });
    for (std::uint32_t index = 0; index < 5; ++index)
        measured.geometry.spans.push_back({ index, index + 1, 0, static_cast<std::int32_t>(128 + index * 64), static_cast<std::int32_t>(192 + index * 64) });
    measured.geometry.elision = std::make_shared<RichText::ShapedText>(measured.geometry);
    measured.geometry.elisionEnd = 4;
    encoded.clear();
    WireWriter measurementWriter(encoded);
    writeMeasurement(measurementWriter, measured);
    WireReader measurementReader(encoded);
    RichText::Measurement decoded;
    REQUIRE(readMeasurement(measurementReader, decoded));
    std::vector<std::uint8_t> chunk;
    WireWriter chunkWriter(chunk);
    chunkWriter.u32(request.token);
    chunkWriter.u32(static_cast<std::uint32_t>(encoded.size()));
    chunkWriter.u32(0);
    chunkWriter.raw(encoded);
    CHECK(appendFrame(wire, FrameKind::RichTextMeasured, 7, chunk));

    request.document = {};
    request.fontCatalog = RichText::FontCatalogQuery { 2, 3, facts.rendererRevision };
    ++request.token;
    encoded.clear();
    WireWriter catalogRequestWriter(encoded);
    writeMeasureRequest(catalogRequestWriter, request);
    CHECK(appendFrame(wire, FrameKind::RichTextMeasure, 7, encoded));
    measured = {};
    measured.rendererRevision = facts.rendererRevision;
    measured.fontCatalog = RichText::FontCatalogPage {};
    auto& page = *measured.fontCatalog;
    page.status = RichText::FontCatalogStatus::Ready;
    page.scope = RichText::FontCatalogScope::System;
    page.offset = 2;
    page.total = 5;
    page.rendererRevision = facts.rendererRevision;
    REQUIRE(page.appendFamily("Fixture"));
    REQUIRE(page.appendFamily("Other;{}\\"));
    REQUIRE(page.appendFamily("字体"));
    encoded.clear();
    WireWriter catalogPageWriter(encoded);
    writeMeasurement(catalogPageWriter, measured);
    chunk.clear();
    WireWriter catalogChunkWriter(chunk);
    catalogChunkWriter.u32(request.token);
    catalogChunkWriter.u32(static_cast<std::uint32_t>(encoded.size()));
    catalogChunkWriter.u32(0);
    catalogChunkWriter.raw(encoded);
    CHECK(appendFrame(wire, FrameKind::RichTextMeasured, 7, chunk));

    RichText::Block block { .serial = 3, .id = 0x5566, .documentRevision = 0x12345678, .row = 1, .column = 2, .width = 19, .height = 3, .overflow = RichText::Overflow::Ellipsis, .alignment = RichText::Alignment::Right, .lineMode = RichText::LineMode::Terminal, .pointerMode = RichText::PointerMode::Characters, .lifetime = RichText::Lifetime::Overlay, .caret = 3, .viewportTop = 2, .padding = { 1, 2, 3, 4 }, .verticalPlacement = RichText::VerticalPlacement::Center, .document = std::move(*document) };
    REQUIRE(block.visibleRects.tryPushBack({ 1, 0, 17, 2 }));
    RichText::BlockStore blocks;
    blocks.restore(Ubiquitty::ScreenMode::Primary, std::move(block));
    encoded.clear();
    WireWriter placementWriter(encoded);
    writeRichTextBlock(placementWriter, blocks);
    WireWriter corpusWriter(wire);
    corpusWriter.bytes(encoded);
}

TEST(theRichTextWireCorpusIsDeterministic)
{
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    buildRichTextCorpus(first);
    buildRichTextCorpus(second);
    CHECK(!first.empty());
    CHECK(first == second);
}

/// The fingerprint a handshake carries is the digest of every corpus above,
/// so it moves whenever any body layout does, whatever `wireVersion` says.
///
/// A peer compares it in Greet and Welcome and refuses another, which is what
/// keeps two builds whose layouts differ from reaching a body one of them
/// reads as something else. Re-mint by pasting the printed initializer into
/// `wireFingerprint` (Envelope.h) and the editor clients' copies, which
/// `Scripts/check-wire-version-pins.py` compares with it.
TEST(testTheHandshakeFingerprintIsTheDigestOfEveryBodyLayout)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto painted = paintedTerminal();
    std::vector<std::uint8_t> corpus = UbiquittyTest::checkpointBody(*painted);
    std::vector<std::uint8_t> catalog;
    buildCorpus(catalog);
    corpus.insert(corpus.end(), catalog.begin(), catalog.end());
    std::vector<std::uint8_t> placements;
    Sync::encode(placements, placementsFixture());
    corpus.insert(corpus.end(), placements.begin(), placements.end());
    ImageFixture image = terminalWithImage();
    const std::vector<std::uint8_t> imageBody = encodeImage(image);
    releaseImage(image);
    REQUIRE(!imageBody.empty());
    corpus.insert(corpus.end(), imageBody.begin(), imageBody.end());
    buildRichTextCorpus(corpus);

    std::array<std::uint8_t, 32> digest {};
    Ubiquitty::Crypto::sha256(corpus, digest);
    if (digest != Sync::wireFingerprint) {
        std::printf("wire fingerprint: a body layout moved; re-mint Sync::wireFingerprint (Envelope.h) and the editor clients' copies with:\n   ");
        for (std::size_t index = 0; index < digest.size(); ++index)
            std::printf(" 0x%02x,%s", digest[index], index % 16 == 15 ? "\n   " : "");
        std::printf("\n");
    }
    CHECK(digest == Sync::wireFingerprint);
}

TEST(aChangedPublishedFingerprintRequiresANewerWireVersion)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    CHECK(Sync::wireVersion >= Sync::lastPublishedWireVersion);
    if (Sync::wireFingerprint != Sync::lastPublishedWireFingerprint)
        CHECK(Sync::wireVersion > Sync::lastPublishedWireVersion);
}

UBIQUITTY_TEST_MAIN()
