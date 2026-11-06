// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Standalone Facts always includes a palette presence flag. Its theme
// round-trips, precedes typography after Welcome, and is refused whole when
// damaged. The embedded base facts in Greet have a distinct fixed layout.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/RichText/TypographyFacts.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

ThemePalette theme(std::uint16_t background)
{
    ThemePalette palette;
    palette.live.seedDefaults();
    palette.live.defaultBackground = { background, background, background };
    palette.live.indexed[1] = { 0xaaaa, 0, 0 };
    palette.baseline = palette.live;
    palette.baseline.defaultForeground = { 0x1111, 0x2222, 0x3333 };
    return palette;
}

/// Typography a client publishes, which travels after its theme.
Ubiquitty::RichText::TypographyFacts typography()
{
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = 7 * 64;
    facts.cellHeight = 15 * 64;
    facts.faceCount = 1;
    Ubiquitty::RichText::FaceMetrics& face = facts.faces[0];
    face.lineHeight = 2400;
    face.ascent = 1900;
    face.descent = 500;
    face.averageAdvance = 1100;
    face.advances.fill(1100);
    return facts;
}

/// The frames a client sends, read back.
std::vector<FrameKind> kindsOf(std::span<const std::uint8_t> stream)
{
    std::vector<FrameKind> kinds;
    FrameDecoder decoder;
    decoder.append(stream);
    while (const std::optional<Frame> frame = decoder.next())
        kinds.push_back(frame->kind);
    CHECK(decoder.error() == EnvelopeError::None);
    CHECK_EQ(decoder.bufferedBytes(), 0u);
    return kinds;
}

/// A host's identity.
HostIdentity hostIdentity()
{
    HostIdentity identity;
    identity.hostInstanceID = { 1, 2 };
    identity.attachmentID = { 3, 4 };
    identity.resumeSecret = { 5, 6 };
    identity.verifiedKind = ClientKind::Human;
    identity.mayDrive = true;
    identity.selection = { .target = AdmissionTarget::Session, .sessionID = 7, .workspaceID = 0, .daemonInstanceID = 17, .socketName = "palette-fixture.sock", .buildFlags = 0, .hostPlatform = "fixture" };
    return identity;
}

ClientIdentity fixtureClientIdentity()
{
    ClientIdentity identity;
    identity.clientInstance = { 7, 8 };
    identity.kind = ClientKind::Human;
    identity.facts = UbiquittyTest::syncDefaultFacts();
    identity.request = { .target = AdmissionTarget::Session, .sessionID = 7, .expectedDaemonInstanceID = 17, .isAttachOnly = true };
    return identity;
}

} // namespace

TEST(aFactsCarriesTheThemeFromVersionTwelve)
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    facts.themePalette = theme(0x2020);
    std::vector<std::uint8_t> carried;
    encode(carried, facts);
    const std::optional<Facts> decoded = decodeFacts(carried);
    REQUIRE(decoded.has_value());
    CHECK(decoded->themePalette == facts.themePalette);
    CHECK(*decoded == facts);
}

TEST(aStandaloneFactsRequiresItsPresenceFlagEvenWithoutATheme)
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    std::vector<std::uint8_t> body;
    encode(body, facts);
    const std::optional<Facts> decoded = decodeFacts(body);
    REQUIRE(decoded.has_value());
    CHECK(!decoded->themePalette.has_value());
    REQUIRE(!body.empty() && body.back() == 0);
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!decodeFacts(std::span(body).first(length)));
    body.back() = 2;
    CHECK(!decodeFacts(body));
    body.back() = 0;
    body.push_back(0);
    CHECK(!decodeFacts(body));
}

// A theme cut short, or a byte past it, is refused whole.
TEST(aDamagedThemeIsRefused)
{
    Facts facts = UbiquittyTest::syncDefaultFacts();
    facts.themePalette = theme(0x2020);
    std::vector<std::uint8_t> body;
    encode(body, facts);
    for (std::size_t length = 0; length < body.size(); ++length)
        CHECK(!decodeFacts(std::span(body).first(length)));
    std::vector<std::uint8_t> longer = body;
    longer.push_back(0);
    CHECK(!decodeFacts(longer).has_value());
}

// The client's theme is the first thing it sends after the Welcome, ahead of
// the probe, so no colour query is answered from an older
// theme; the host holds it.
TEST(theThemeIsTheFirstFrameAfterTheWelcome)
{
    HostConnection host(UbiquittyTest::syncHostPolicy(0), hostIdentity());
    ClientConnection client(UbiquittyTest::syncClientPolicy, fixtureClientIdentity());
    client.setThemePalette(theme(0x3030));
    client.publishTypography(1, typography());
    client.start();
    host.receiveStream(client.takeStreamOutput(), 1);
    host.service(1);
    std::vector<std::uint8_t> down;
    while (host.popStreamFrame(down, 1)) {
    }
    client.receiveStream(down, 1);
    const std::vector<std::uint8_t> up = client.takeStreamOutput();
    const std::vector<FrameKind> kinds = kindsOf(up);
    REQUIRE(kinds.size() >= 2);
    CHECK(kinds.front() == FrameKind::Facts);
    CHECK(std::ranges::find(kinds, FrameKind::TypographyFacts) == kinds.end());
    host.receiveStream(up, 2);
    CHECK(host.facts().themePalette == std::optional { theme(0x3030) });
    // A change of theme is told at once.
    client.setThemePalette(theme(0x4040));
    host.receiveStream(client.takeStreamOutput(), 3);
    CHECK(host.facts().themePalette == std::optional { theme(0x4040) });
}

UBIQUITTY_TEST_MAIN()
