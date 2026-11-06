// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_roles.cpp — the role a handshake asks for and is granted.
//
// A role crosses as one byte whose values are the four roles; a byte past
// them is a malformed handshake, not a role nobody has heard of. What a host
// grants is never more than the client asked for, never more than its
// credential allows, and an agent is an agent whatever it asked to be. The
// agent role is not a rung on the person ladder: a person who asks for it is
// granted a viewer, and a credential that allows only an agent makes one.

#include "Harness.h"

#include "Core/Protocol/Sync/Authority.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/FrameGates.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Roles.h"
#include "SyncLink.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using Ubiquitty::Protocol::Sync::everyRole;
using Ubiquitty::Protocol::Sync::Role;
using Ubiquitty::Protocol::Sync::roleFromWire;
using Ubiquitty::Protocol::Sync::Roles;

namespace {

/// A bare message's app version on the wire: the length byte of an empty
/// string.
constexpr std::size_t emptyAppVersionBytes = 1;
/// Where a bare Greet's role byte sits: after the version, the app version,
/// the fingerprint, the client instance and the client kind.
constexpr std::size_t greetRoleOffset = 2 + emptyAppVersionBytes + 32 + 16 + 1;
/// Where a bare Welcome's role byte sits: after the version, the app version,
/// the fingerprint and the three identifiers.
constexpr std::size_t welcomeRoleOffset = 2 + emptyAppVersionBytes + 32 + 16 + 16 + 16;

} // namespace

TEST(aRequestedRoleNeverExceedsTheGrant)
{
    // Asked for more than the credential allows, the credential decides.
    CHECK(grantRole(Role::Owner, ClientKind::Human, Role::Driver) == Role::Driver);
    CHECK(grantRole(Role::Owner, ClientKind::Human, Role::Viewer) == Role::Viewer);
    CHECK(grantRole(Role::Driver, ClientKind::Human, Role::Viewer) == Role::Viewer);
    // Asked for less, the client gets what it asked for.
    CHECK(grantRole(Role::Viewer, ClientKind::Human, Role::Owner) == Role::Viewer);
    CHECK(grantRole(Role::Driver, ClientKind::Human, Role::Owner) == Role::Driver);
    CHECK(grantRole(Role::Owner, ClientKind::Human, Role::Owner) == Role::Owner);
    // A person who asks to be an agent is asking to be a program; what they
    // are granted is the role that sends nothing, whatever their credential
    // would have allowed. A credential that allows only an agent makes one.
    CHECK(grantRole(Role::Agent, ClientKind::Human, Role::Owner) == Role::Viewer);
    CHECK(grantRole(Role::Agent, ClientKind::Human, Role::Viewer) == Role::Viewer);
    CHECK(grantRole(Role::Owner, ClientKind::Human, Role::Agent) == Role::Agent);
    // No pair of a request and a ceiling grants a person more than either. The
    // person roles ascend from most privileged, so "no more" is the later
    // enumerator; the agent role is off that ladder and compared by name.
    for (const Role requested : everyRole) {
        for (const Role ceiling : everyRole) {
            const Role granted = grantRole(requested, ClientKind::Human, ceiling);
            if (ceiling == Role::Agent) {
                CHECK(granted == Role::Agent);
                continue;
            }
            CHECK(granted != Role::Agent);
            CHECK(std::to_underlying(granted) >= std::to_underlying(ceiling));
            if (requested == Role::Agent)
                CHECK(granted == Role::Viewer);
            else
                CHECK(std::to_underlying(granted) >= std::to_underlying(requested));
        }
    }
}

TEST(anAgentKindIsAnAgentWhateverItAsks)
{
    for (const Role requested : everyRole) {
        for (const Role ceiling : everyRole)
            CHECK(grantRole(requested, ClientKind::Agent, ceiling) == Role::Agent);
    }
}

TEST(aClientThatOnlyWatchesIsGrantedNothingAboveAViewer)
{
    CHECK(grantRole(Role::Owner, ClientKind::Viewer, Role::Owner) == Role::Viewer);
    CHECK(grantRole(Role::Driver, ClientKind::Viewer, Role::Driver) == Role::Viewer);
    CHECK(grantRole(Role::Viewer, ClientKind::Viewer, Role::Owner) == Role::Viewer);
    CHECK(grantRole(Role::Agent, ClientKind::Viewer, Role::Owner) == Role::Viewer);
}

TEST(aRoleIsItsByteAndAByteBeyondTheLastIsNoRole)
{
    CHECK(roleFromWire(0) == Role::Owner);
    CHECK(roleFromWire(1) == Role::Driver);
    CHECK(roleFromWire(2) == Role::Viewer);
    CHECK(roleFromWire(3) == Role::Agent);
    CHECK(!roleFromWire(4));
    CHECK(!roleFromWire(255));
    // A set keeps a bit per role, so the first role is a member like any other.
    const Roles drivers { Role::Owner, Role::Driver };
    CHECK(drivers.contains(Role::Owner) && drivers.contains(Role::Driver));
    CHECK(!drivers.contains(Role::Viewer) && !drivers.contains(Role::Agent));
    CHECK(Roles {}.isEmpty() && !drivers.isEmpty());
}

TEST(everyClientFrameKindHasExactlyOneRoleGate)
{
    for (std::uint16_t byte = 0; byte <= 255; ++byte) {
        const std::optional<FrameKind> kind = frameKindOf(static_cast<std::uint8_t>(byte));
        if (!kind)
            continue;
        const auto count = std::ranges::count(frameGates, *kind, &FrameGate::kind);
        CHECK_EQ(count, directionOf(*kind) == Direction::HostToClient ? 0 : 1);
    }
    for (const FrameGate& gate : frameGates) {
        CHECK(frameKindOf(std::to_underlying(gate.kind)) == gate.kind);
        CHECK(admitsSender(gate.kind, Role::Owner));
        CHECK(admitsSender(gate.kind, Role::Driver));
    }
    for (const FrameKind kind : { FrameKind::Raw, FrameKind::Keys, FrameKind::Pointer, FrameKind::Paste, FrameKind::PasteAbort }) {
        CHECK(admitsSender(kind, Role::Agent));
        CHECK(!admitsSender(kind, Role::Viewer));
    }
    CHECK(!admitsSender(FrameKind::Viewport, Role::Agent));
    CHECK(!admitsSender(FrameKind::Viewport, Role::Viewer));
}

TEST(anUnknownRoleByteIsMalformed)
{
    Greet greet;
    greet.wireVersion = wireVersion;
    greet.requestedRole = Role::Agent;
    std::vector<std::uint8_t> greetBody;
    encode(greetBody, greet);
    REQUIRE(greetBody.size() > greetRoleOffset);
    CHECK_EQ(static_cast<int>(greetBody[greetRoleOffset]), 3);
    const std::optional<Greet> readGreet = decodeGreet(greetBody);
    REQUIRE(readGreet);
    CHECK(readGreet->requestedRole == Role::Agent);
    greetBody[greetRoleOffset] = 4;
    CHECK(!decodeGreet(greetBody));

    Welcome welcome;
    welcome.selection = UbiquittyTest::syncDefaultSelection();
    welcome.wireVersion = wireVersion;
    welcome.grantedRole = Role::Viewer;
    std::vector<std::uint8_t> welcomeBody;
    encode(welcomeBody, welcome);
    REQUIRE(welcomeBody.size() > welcomeRoleOffset);
    CHECK_EQ(static_cast<int>(welcomeBody[welcomeRoleOffset]), 2);
    const std::optional<Welcome> readWelcome = decodeWelcome(welcomeBody);
    REQUIRE(readWelcome);
    CHECK(readWelcome->grantedRole == Role::Viewer);
    welcomeBody[welcomeRoleOffset] = 4;
    CHECK(!decodeWelcome(welcomeBody));
}

namespace {

/// Greets a host whose credential ceiling is `ceiling` and which verified the
/// client as `verified`, asking for `requested` as a client of `declared`.
struct Greeted {
    HostConnection host;
    ClientConnection client;

    Greeted(Role ceiling, ClientKind verified, Role requested, ClientKind declared)
        : host(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .maxRole = ceiling, .principal = "test", .mayDrive = true, .verifiedKind = verified, .selection = UbiquittyTest::syncDefaultSelection() })
        , client(UbiquittyTest::syncClientPolicy, ClientIdentity { .clientInstance = { 7, 8 }, .kind = declared, .requestedRole = requested, .facts = UbiquittyTest::syncDefaultFacts() })
    {
        client.start();
        std::vector<std::uint8_t> stream;
        for (std::uint64_t now = 1; now < 20 && !client.welcome(); ++now) {
            host.receiveStream(client.takeStreamOutput(), now);
            host.service(now);
            stream.clear();
            while (host.popStreamFrame(stream, now)) {
            }
            client.receiveStream(stream, now);
        }
    }
};

} // namespace

TEST(aClientIsToldTheRoleItsHostGranted)
{
    // Before a Greet the host has granted nothing: a viewer.
    HostConnection idle(UbiquittyTest::syncHostPolicy(0), HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .maxRole = Role::Owner, .principal = "test", .selection = UbiquittyTest::syncDefaultSelection() });
    CHECK(idle.grantedRole() == Role::Viewer);

    // Asked for more than the credential allows, the credential decides, and
    // the host holds the role it told the client.
    Greeted capped(Role::Driver, ClientKind::Human, Role::Owner, ClientKind::Human);
    REQUIRE(capped.client.welcome());
    CHECK(capped.client.welcome()->grantedRole == Role::Driver);
    CHECK(capped.host.grantedRole() == Role::Driver);
    // Asked for less, the client gets what it asked.
    Greeted modest(Role::Owner, ClientKind::Human, Role::Viewer, ClientKind::Human);
    REQUIRE(modest.client.welcome());
    CHECK(modest.client.welcome()->grantedRole == Role::Viewer && modest.host.grantedRole() == Role::Viewer);
    Greeted owner(Role::Owner, ClientKind::Human, Role::Owner, ClientKind::Human);
    CHECK(owner.host.grantedRole() == Role::Owner);
    // A client the host verified as an agent is one, whatever it declares.
    Greeted posing(Role::Owner, ClientKind::Agent, Role::Owner, ClientKind::Human);
    REQUIRE(posing.client.welcome());
    CHECK(posing.client.welcome()->grantedRole == Role::Agent && posing.host.grantedRole() == Role::Agent);
    // What a person's client declares only lowers it.
    Greeted declaredAgent(Role::Owner, ClientKind::Human, Role::Owner, ClientKind::Agent);
    CHECK(declaredAgent.host.grantedRole() == Role::Agent);
    Greeted watching(Role::Owner, ClientKind::Human, Role::Owner, ClientKind::Viewer);
    CHECK(watching.host.grantedRole() == Role::Viewer);
    // A person who asks for the agent role is granted a viewer, not the role
    // that passes the input gates a viewer does not.
    Greeted askingToBeAProgram(Role::Viewer, ClientKind::Human, Role::Agent, ClientKind::Human);
    REQUIRE(askingToBeAProgram.client.welcome());
    CHECK(askingToBeAProgram.client.welcome()->grantedRole == Role::Viewer && askingToBeAProgram.host.grantedRole() == Role::Viewer);
}

TEST(aViewerIsRefusedEveryFrameThatCanDriveAPane)
{
    const std::uint8_t text[] { 'x' };
    for (const FrameKind kind : { FrameKind::Raw, FrameKind::Keys, FrameKind::Pointer, FrameKind::Paste, FrameKind::PasteAbort, FrameKind::Viewport }) {
        UbiquittyTest::OwnedPane pane;
        Greeted viewer(Role::Owner, ClientKind::Human, Role::Viewer, ClientKind::Human);
        REQUIRE(viewer.host.grantedRole() == Role::Viewer);
        const std::uint32_t handle = viewer.host.bindPane(*pane.source);
        REQUIRE(handle);
        std::vector<std::uint8_t> body;
        switch (kind) {
        case FrameKind::Raw:
            encode(body, Raw { .inputSeq = 1, .bytes = text });
            break;
        case FrameKind::Keys: {
            KeyEvent event;
            event.text = "x";
            encode(body, Keys { .inputSeq = 1, .events = { event } });
            break;
        }
        case FrameKind::Pointer:
            encode(body, Pointer { .inputSeq = 1, .events = { PointerEvent {} } });
            break;
        case FrameKind::Paste:
            encode(body, Paste { .inputSeq = 1, .offset = 0, .state = PasteState::Final, .text = text });
            break;
        case FrameKind::PasteAbort:
            encode(body, PasteAbort { .inputSeq = 1 });
            break;
        case FrameKind::Viewport: {
            Viewport viewport;
            viewport.rows = 20;
            viewport.columns = 60;
            encode(body, viewport);
            break;
        }
        default:
            CHECK(false);
            break;
        }
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, kind, handle, body));
        viewer.host.receiveStream(frame, 20);
        CHECK(viewer.host.state() == ConnectionState::Closed);
        CHECK(viewer.host.byeReason() == ByeReason::Malformed);
        CHECK_EQ(pane.source->heldInputBytes(), 0u);
        CHECK(pane.writer.writes().empty());
        CHECK(viewer.host.takeViewportReports().empty());
    }
}

TEST(aViewerFocusDoesNotClaimDriverFocusOrGeometry)
{
    UbiquittyTest::OwnedPane pane;
    Greeted viewer(Role::Owner, ClientKind::Human, Role::Viewer, ClientKind::Human);
    const std::uint32_t handle = viewer.host.bindPane(*pane.source);
    REQUIRE(handle);
    std::vector<std::uint8_t> body;
    encode(body, Focus { .isFocused = true });
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Focus, handle, body));
    viewer.host.receiveStream(frame, 20);
    CHECK(viewer.host.state() == ConnectionState::Attached);
    CHECK_EQ(pane.source->personFocusGains(), 0u);
    CHECK(!pane.source->geometry().status().owner);
}

UBIQUITTY_TEST_MAIN()
