// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_wire_version.cpp — the one wire version two builds share, and
// how a peer at another is told apart.
//
// - The older side of a refusal is the one with the lower version; equal or
//   unknown versions name no side.
// - The handshake prefix and the refusal carry the version and the app
//   version, and an app version that is not one reads as empty.
// - Greet and Welcome have exactly one complete body. Truncation or an
//   unknown suffix is refused; only the version and build readers stop at a
//   prefix.

#include "Harness.h"

#include "Core/Protocol/Sync/AppVersion.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Protocol/Sync/WireVersion.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol;
TEST(theOlderSideIsTheOneWithTheLowerVersion)
{
    CHECK(Sync::olderSideOfVersions(2, 1) == Sync::OlderSide::Host);
    CHECK(Sync::olderSideOfVersions(1, 2) == Sync::OlderSide::Client);
    // The same version, or one that is not known, names no side.
    CHECK(Sync::olderSideOfVersions(1, 1) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfVersions(1, 0) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfVersions(0, 1) == Sync::OlderSide::Unknown);
    // A refusal not about the version, or from a host that stated nothing,
    // names no side.
    const Sync::PeerBuild newer { .isKnown = true, .wireVersion = Sync::wireVersion + 1, .appVersion = "0.3.0" };
    CHECK(Sync::olderSideOfRefusal(Sync::ByeReason::VersionMismatch, newer) == Sync::OlderSide::Client);
    CHECK(Sync::olderSideOfRefusal(Sync::ByeReason::VersionMismatch, newer, Sync::PeerBuild { .isKnown = true, .wireVersion = Sync::wireVersion + 2, .appVersion = {} }) == Sync::OlderSide::Host);
    CHECK(Sync::olderSideOfRefusal(Sync::ByeReason::OutsideScope, newer) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfRefusal(Sync::ByeReason::VersionMismatch, Sync::PeerBuild {}) == Sync::OlderSide::Unknown);
}

// A fingerprint refusal at one wire version names no older side. An app
// version describes a build but never orders its protocol.
TEST(buildsOfOneWireVersionNameNoOlderSide)
{
    const Sync::PeerBuild newer { .isKnown = true, .wireVersion = 1, .appVersion = "0.2.0.7" };
    const Sync::PeerBuild older { .isKnown = true, .wireVersion = 1, .appVersion = "0.2.0.3" };
    CHECK(Sync::olderSideOfBuilds(newer, older) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfBuilds(older, newer) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfBuilds(newer, newer) == Sync::OlderSide::Unknown);
    const Sync::PeerBuild unversioned { .isKnown = true, .wireVersion = 1, .appVersion = std::string(Sync::unversionedAppVersion) };
    CHECK(Sync::olderSideOfBuilds(newer, unversioned) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfBuilds(newer, Sync::PeerBuild {}) == Sync::OlderSide::Unknown);
    CHECK(Sync::olderSideOfBuilds(Sync::PeerBuild {}, newer) == Sync::OlderSide::Unknown);
    // Another wire version orders by the version, whatever the app versions.
    const Sync::PeerBuild nextWire { .isKnown = true, .wireVersion = 2, .appVersion = "0.1.0" };
    CHECK(Sync::olderSideOfBuilds(newer, nextWire) == Sync::OlderSide::Client);
    CHECK(Sync::olderSideOfRefusal(Sync::ByeReason::VersionMismatch, older, newer) == Sync::OlderSide::Unknown);
}

// The prefix a handshake opens with names the build's fingerprint after its
// versions, and a prefix cut short of it names none.
TEST(theHandshakePrefixEndsWithTheFingerprint)
{
    Sync::Greet greet;
    greet.wireVersion = Sync::wireVersion;
    greet.appVersion = "0.2.0.3";
    greet.engineFingerprint.fill(0x5A);
    std::vector<std::uint8_t> body;
    Sync::encode(body, greet);
    CHECK(Sync::decodeHandshakeFingerprint(body) == std::optional { greet.engineFingerprint });
    const std::size_t prefix = 2 + 1 + greet.appVersion.size() + greet.engineFingerprint.size();
    CHECK(Sync::decodeHandshakeFingerprint(std::span(body).first(prefix)));
    CHECK(!Sync::decodeHandshakeFingerprint(std::span(body).first(prefix - 1)));
    CHECK(Sync::Greet {}.engineFingerprint == Sync::wireFingerprint);
}

TEST(theHandshakeAndTheRefusalCarryTheVersionAndTheAppVersion)
{
    Sync::Greet greeting;
    greeting.wireVersion = 9;
    greeting.appVersion = "1.2.3";
    greeting.clientInstance = { 11, 13 };
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    const auto build = Sync::decodeHandshakeBuild(body);
    REQUIRE(build);
    CHECK(build->isKnown);
    CHECK_EQ(build->wireVersion, 9);
    CHECK(build->appVersion == "1.2.3");
    const auto decoded = Sync::decodeGreet(body);
    REQUIRE(decoded);
    CHECK_EQ(decoded->wireVersion, 9);
    CHECK(decoded->appVersion == "1.2.3");
    CHECK_EQ(decoded->facts.viewportColumns, 80);

    // The version needs only the first two bytes, independently of the
    // remainder's grammar. Full admission still requires every field.
    CHECK(Sync::decodeHandshakeVersion(std::span(body).first(2)) == std::optional<std::uint16_t> { 9 });
    CHECK(!Sync::decodeHandshakeVersion(std::span(body).first(1)));
    const std::array<std::uint8_t, 2> unversioned {};
    CHECK(!Sync::decodeHandshakeVersion(unversioned));
    Sync::WireReader prefixReader(body);
    CHECK_EQ(prefixReader.u16(), 9);
    CHECK(prefixReader.utf8(Sync::maxAppVersionBytes) == greeting.appVersion);
    REQUIRE(prefixReader.ok());
    const std::size_t prefixBytes = prefixReader.offset();
    REQUIRE(body.size() > prefixBytes);
    std::vector<std::uint8_t> prefixOnly(body.begin(), body.begin() + prefixBytes);
    prefixOnly.push_back(0xFF);
    CHECK(Sync::decodeHandshakeBuild(prefixOnly));
    CHECK(!Sync::decodeGreet(prefixOnly));
    CHECK(!Sync::decodeHandshakeBuild(std::span(prefixOnly).first(1)));

    Sync::Welcome welcome;
    welcome.wireVersion = 6;
    welcome.appVersion = "0.2.2";
    welcome.hostInstanceID = { 17, 19 };
    welcome.attachmentID = { 23, 29 };
    welcome.resumeSecret = { 31, 37 };
    welcome.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 7, .workspaceID = 0, .daemonInstanceID = 17, .socketName = "version-fixture.sock", .buildFlags = 0, .hostPlatform = "Darwin arm64" };
    body.clear();
    Sync::encode(body, welcome);
    const auto decodedWelcome = Sync::decodeWelcome(body);
    REQUIRE(decodedWelcome);
    CHECK_EQ(decodedWelcome->wireVersion, 6);
    CHECK(decodedWelcome->appVersion == "0.2.2");
    CHECK_EQ(decodedWelcome->selection.sessionID, 7u);

    const Sync::Bye refusal { .reason = Sync::ByeReason::VersionMismatch, .detail = "mismatch", .peer = Sync::PeerMetadata { .build = { .isKnown = true, .wireVersion = 6, .appVersion = "0.2.2" }, .platform = "Darwin arm64" } };
    body.clear();
    Sync::encode(body, refusal);
    const auto decodedRefusal = Sync::decodeBye(body);
    REQUIRE(decodedRefusal && decodedRefusal->peer);
    CHECK(decodedRefusal->reason == Sync::ByeReason::VersionMismatch);
    CHECK_EQ(decodedRefusal->peer->build.wireVersion, 6);
    CHECK(decodedRefusal->peer->build.appVersion == "0.2.2");
    CHECK(decodedRefusal->peer->platform == "Darwin arm64");
}

TEST(anAppVersionThatIsNotOneReadsAsEmpty)
{
    const auto read = [](std::string_view text) {
        return Sync::appVersionFromPeer({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    };
    CHECK(read("0.2.0") == "0.2.0");
    CHECK(read("0.2.0.3") == "0.2.0.3");
    CHECK(read("999.99.99.99999") == "999.99.99.99999");
    // What a build stated before versions counted commits, and what a test
    // binary states, are still shown as they were sent.
    CHECK(read("0.9.1") == "0.9.1");
    CHECK(read("1.0.0-beta.2+build.7") == "1.0.0-beta.2+build.7");
    CHECK(read("").empty());
    CHECK(read("0.2.0.3\n\x1b[2J").empty());
    CHECK(read("0.2.0 3").empty());
    CHECK(read("0.2.0.3 (1a2b3c4d)").empty());
    CHECK(read(std::string(Sync::maxAppVersionBytes + 1, '1')).empty());
    CHECK(read(std::string(Sync::maxAppVersionBytes, '1')).size() == Sync::maxAppVersionBytes);
}

// The handshake carries the version and never the build identity: the
// identity is for a person reading this machine's own surfaces.
TEST(thisBuildStatesAVersionAndAnIdentity)
{
    // A test binary compiled outside the build system states no version; one
    // the build system compiled states a canonical one.
    const std::string_view version = Sync::appVersion();
    CHECK(!version.empty());
    CHECK(version.size() <= Sync::maxAppVersionBytes);
    CHECK(Sync::parseAppVersion(version) || version == Sync::unversionedAppVersion);
    CHECK(!Sync::buildIdentity().empty());
    CHECK(Sync::buildIdentity().size() <= Sync::maxBuildIdentityBytes);
    CHECK(Sync::appVersionWithIdentity() == std::string(version) + " (" + std::string(Sync::buildIdentity()) + ")");

    Sync::ClientIdentity identity;
    CHECK(identity.appVersion == version);
    identity.clientInstance = { 11, 13 };
    identity.request.target = Sync::AdmissionTarget::Control;
    Sync::ClientConnection connection(Sync::measuredClientPolicy(), std::move(identity));
    connection.start();
    const auto wire = connection.takeStreamOutput();
    Sync::FrameHeader header;
    Sync::EnvelopeError error = Sync::EnvelopeError::None;
    REQUIRE(Sync::readFrameHeader(wire, header, error) == Sync::FrameHeaderState::Complete);
    REQUIRE(header.kind == Sync::FrameKind::Greet && header.channel == 0);
    REQUIRE(wire.size() == Sync::frameBytes(header.channel, header.bodyBytes));
    const auto greeting = Sync::decodeGreet(std::span(wire).last(header.bodyBytes));
    REQUIRE(greeting);
    CHECK(greeting->appVersion == version);
    const auto localBuild = Sync::localPeerBuild();
    CHECK(localBuild.appVersion == version);
}

// Full admission has one grammar, independently of the version-only readers
// above. No unknown suffix is an optional handshake extension.
TEST(everyCanonicalHandshakeIsExactAndRefusesEveryTruncation)
{
    const std::array<std::uint8_t, 8> appended { 1, 2, 3, 4, 5, 6, 7, 8 };
    Sync::Greet greet;
    greet.wireVersion = Sync::wireVersion;
    greet.clientInstance = { 11, 12 };
    greet.clientKind = Sync::ClientKind::Agent;
    greet.requestedRole = Sync::Role::Viewer;
    greet.facts.viewportRows = 24;
    greet.facts.viewportColumns = 80;
    greet.facts.isFocused = true;
    greet.historyCacheLines = 1000;
    greet.backfillDepthLines = 250;
    std::vector<std::uint8_t> greetBody;
    Sync::encode(greetBody, greet);
    const auto readGreet = Sync::decodeGreet(greetBody);
    REQUIRE(readGreet);
    CHECK_EQ(readGreet->wireVersion, Sync::wireVersion);
    CHECK(readGreet->clientInstance == greet.clientInstance);
    CHECK(readGreet->clientKind == Sync::ClientKind::Agent);
    CHECK(readGreet->requestedRole == Sync::Role::Viewer);
    CHECK(readGreet->facts == greet.facts);
    CHECK_EQ(readGreet->historyCacheLines, 1000u);
    CHECK_EQ(readGreet->backfillDepthLines, 250u);
    for (std::size_t cut = 0; cut < greetBody.size(); ++cut)
        CHECK(!Sync::decodeGreet(std::span(greetBody).first(cut)));
    greetBody.insert(greetBody.end(), appended.begin(), appended.end());
    CHECK(!Sync::decodeGreet(greetBody));

    Sync::Welcome welcome;
    welcome.wireVersion = Sync::wireVersion;
    welcome.hostInstanceID = { 21, 22 };
    welcome.attachmentID = { 23, 24 };
    welcome.resumeSecret = { 25, 26 };
    welcome.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 7, .workspaceID = 0, .daemonInstanceID = 21, .socketName = "strict-welcome.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    welcome.grantedRole = Sync::Role::Driver;
    welcome.principal = "agent";
    welcome.limits.probeSteps = 2;
    welcome.limits.probeScheduleMicroseconds[0] = 2'000'000;
    welcome.limits.probeScheduleMicroseconds[1] = 4'000'000;
    welcome.hostTime = 99;
    std::vector<std::uint8_t> welcomeBody;
    Sync::encode(welcomeBody, welcome);
    const auto readWelcome = Sync::decodeWelcome(welcomeBody);
    REQUIRE(readWelcome);
    CHECK(readWelcome->hostInstanceID == welcome.hostInstanceID);
    CHECK(readWelcome->principal == "agent");
    CHECK_EQ(readWelcome->limits.probeSteps, 2);
    CHECK_EQ(readWelcome->limits.probeScheduleMicroseconds[1], 4'000'000u);
    CHECK_EQ(readWelcome->hostTime, 99u);
    for (std::size_t cut = 0; cut < welcomeBody.size(); ++cut)
        CHECK(!Sync::decodeWelcome(std::span(welcomeBody).first(cut)));
    welcomeBody.insert(welcomeBody.end(), appended.begin(), appended.end());
    CHECK(!Sync::decodeWelcome(welcomeBody));
}

// Caps apply before a full handshake decode, and typed bodies refuse bytes
// outside the fields their current grammar names.
TEST(allCanonicalBodiesKeepTheirOwnLimitsAndExactEnds)
{
    const std::vector<std::uint8_t> oversized(Sync::maxFrameBodyBytes + 1, 0);
    CHECK(!Sync::decodeGreet(oversized));
    CHECK(!Sync::decodeWelcome(oversized));
    std::vector<std::uint8_t> facts;
    Sync::encode(facts, Sync::Facts { .viewportRows = 24, .viewportColumns = 80 });
    REQUIRE(Sync::decodeFacts(facts));
    facts.push_back(0);
    CHECK(!Sync::decodeFacts(facts));
    std::vector<std::uint8_t> ack;
    Sync::encode(ack, Sync::Ack { .hostTimeEcho = 3 });
    REQUIRE(Sync::decodeAck(ack));
    ack.push_back(0);
    CHECK(!Sync::decodeAck(ack));
    std::vector<std::uint8_t> bye;
    Sync::encode(bye, Sync::Bye {});
    REQUIRE(Sync::decodeBye(bye));
    bye.push_back(0);
    CHECK(!Sync::decodeBye(bye));
}

UBIQUITTY_TEST_MAIN()
