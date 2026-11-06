// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Two facts can say where a session is, and they answer different questions:
// the transport knows how Ubiquitty reached the PTY, the process walk knows
// what the reader is typing into. Neither alone covers both a daemon session
// and a locally-typed `ssh`, so the precedence between them is the thing under
// test — a tag rule, the titlebar badge and an identity check all read it, and
// three copies of it would be three chances to disagree.

#include "Core/Session/SessionLocation.h"

#include <cstdio>

using namespace Ubiquitty::Session;
using Ubiquitty::PAL::Process::RemoteKind;
using Ubiquitty::PAL::Process::RemoteTarget;
using Ubiquitty::Transport::Origin;
using Ubiquitty::Transport::OriginKind;

namespace {

bool same(const SessionLocation& got, RemoteKind kind, std::string_view name,
    std::string_view user, std::string_view via, const char* what)
{
    if (got.kind == kind && got.name == name && got.user == user && got.via == via)
        return true;
    std::printf("    %s\n      got kind=%d name=%s user=%s via=%s\n", what,
        static_cast<int>(got.kind), got.name.c_str(), got.user.c_str(), got.via.c_str());
    return false;
}

} // namespace

// A session on this machine, running something local, is nowhere else — and
// saying otherwise would badge a window that has nothing to badge.
TEST(testAPlainLocalSessionIsNowhereElse)
{
    const SessionLocation location = resolveSessionLocation({});
    CHECK(same(location, RemoteKind::None, "", "", "", "a local session names nowhere"));
    CHECK(!location.isRemote());
}

// The walk is what makes a typed `ssh` visible. The transport says Local,
// because Ubiquitty did open a local PTY — the reader went somewhere from it.
TEST(testATypedRemoteIsSeenThroughTheProcessWalk)
{
    const RemoteTarget target { RemoteKind::Host, "example-host", "ada", "ssh" };
    CHECK(same(resolveSessionLocation({ .target = target }), RemoteKind::Host, "example-host", "ada", "ssh",
        "a local PTY running ssh is on the host it reached"));

    const RemoteTarget container { RemoteKind::Container, "web-1", "", "docker exec" };
    CHECK(same(resolveSessionLocation({ .target = container }), RemoteKind::Container, "web-1", "",
        "docker exec", "a container is a place a walk can see"));
}

// The transport is what makes a daemon session visible: nothing in its process
// tree is an `ssh`, so a walk finds nothing and only the stamp knows.
TEST(testATransportOriginAnswersWhereAWalkCannot)
{
    Origin daemon;
    daemon.transport = OriginKind::Daemon;
    daemon.host = "build-1";
    CHECK(same(resolveSessionLocation({ .origin = daemon }), RemoteKind::Host, "build-1", "",
        "daemon", "a daemon session is on its daemon's host"));

    Origin overSSH;
    overSSH.transport = OriginKind::SSH;
    overSSH.host = "ada@example-host";
    // The origin spells the login into its host, and the machine is what a
    // badge and a `match_host` rule are asking about — two logins on one host
    // are one machine.
    CHECK(same(resolveSessionLocation({ .origin = overSSH }), RemoteKind::Host, "example-host", "ada",
        "ssh", "the login is separated from the machine"));
}

// The nearer answer wins. A session Ubiquitty opened on a host, whose reader
// has since stepped into a container, is in the container — that is what they
// are typing into, and what a title naming one place should name.
TEST(testTheNearerPlaceWins)
{
    Origin overSSH;
    overSSH.transport = OriginKind::SSH;
    overSSH.host = "example-host";
    const RemoteTarget container { RemoteKind::Container, "web-1", "", "docker exec" };
    CHECK(same(resolveSessionLocation({ .origin = overSSH, .target = container }), RemoteKind::Container, "web-1", "",
        "docker exec", "the container the reader stepped into wins over the host"));
}

// A target with a kind but no name — `vagrant ssh` with no box named — is not
// a name a badge can show, so the transport still gets its turn.
TEST(testANamelessTargetDoesNotSilenceTheTransport)
{
    Origin daemon;
    daemon.transport = OriginKind::Daemon;
    daemon.host = "build-1";
    const RemoteTarget nameless { RemoteKind::VirtualMachine, "", "", "vagrant ssh" };
    CHECK(same(resolveSessionLocation({ .origin = daemon, .target = nameless }), RemoteKind::Host, "build-1", "", "daemon",
        "a target with no name leaves the transport's answer standing"));
}

// An origin that names no host cannot be one: a stamp with an empty host says
// only that something was intended, and badging it would show a blank.
TEST(testAnOriginWithNoHostNamesNowhere)
{
    Origin blank;
    blank.transport = OriginKind::SSH;
    CHECK(same(resolveSessionLocation({ .origin = blank }), RemoteKind::None, "", "", "",
        "an origin with no host names nowhere"));
}

// A host the far side REPORTED outranks the walk, and this is the case that
// makes it worth keeping: the reader ssh'd to bastion and on from there to example-host, so
// this machine holds one ssh naming bastion, while example-host's own shell says example-host.
TEST(testAReportedHostSeesPastANestedSSH)
{
    const RemoteTarget firstHop { RemoteKind::Host, "bastion.example", "", "ssh" };
    const SessionLocation location
        = resolveSessionLocation({ .target = firstHop, .reportedHost = "ada@example-host" });
    CHECK(same(location, RemoteKind::Host, "example-host", "ada", "ssh",
        "the innermost shell's report wins over the first hop the walk can see"));
}

// A report says "some host" and cannot say "a container". The walk can, and a
// container the reader can name beats a hostname they cannot act on — a shell
// inside one reports a hostname like any other.
TEST(testAWalkOnlyKindOutranksAReportedHost)
{
    const RemoteTarget container { RemoteKind::Container, "web-1", "", "docker exec" };
    CHECK(same(resolveSessionLocation({ .target = container, .reportedHost = "3f2a9c1b" }),
        RemoteKind::Container, "web-1", "", "docker exec",
        "the container outranks the hostname its shell reports"));
}

// A report names where a shell is, never how the reader got there, so the tool
// stays the walk's to name.
TEST(testAReportedHostKeepsTheWalksTool)
{
    const RemoteTarget viaMosh { RemoteKind::Host, "bastion.example", "", "mosh" };
    CHECK(same(resolveSessionLocation({ .target = viaMosh, .reportedHost = "example-host" }),
        RemoteKind::Host, "example-host", "", "mosh",
        "the report supplies the machine and the walk supplies the tool"));
}

UBIQUITTY_TEST_MAIN()
