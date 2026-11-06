// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Where a session actually is. The table below is the specification rather
// than an illustration of it: a shape this parser claims to know is a row
// here, and adding a tool to the table without adding a row leaves the claim
// unchecked.
//
// The rows that earn the parser are the ones where the destination is
// somewhere other than argv[1] — an option that swallows the token after it,
// a bundle, an attached value, a subcommand, a transparent prefix — because
// "take the second word" passes every simple case and fails all of those.

#include "PAL/RemoteTarget.h"

#include <cstdio>
#include <string>
#include <string_view>

using namespace Ubiquitty::PAL::Process;

namespace {

struct Row {
    std::string_view commandLine;
    RemoteKind kind;
    std::string_view name;
    std::string_view user;
    std::string_view via;
};

std::string_view spell(RemoteKind kind)
{
    switch (kind) {
    case RemoteKind::None:
        return "None";
    case RemoteKind::Host:
        return "Host";
    case RemoteKind::Container:
        return "Container";
    case RemoteKind::VirtualMachine:
        return "VirtualMachine";
    case RemoteKind::Serial:
        return "Serial";
    }
    return "?";
}

bool check(const Row& row)
{
    const RemoteTarget target = remoteTargetForCommand(row.commandLine);
    if (target.kind == row.kind && target.name == row.name && target.user == row.user
        && target.via == row.via)
        return true;
    std::printf("    %.*s\n      wanted %.*s name=%.*s user=%.*s via=%.*s\n"
                "      got    %.*s name=%s user=%s via=%s\n",
        static_cast<int>(row.commandLine.size()), row.commandLine.data(),
        static_cast<int>(spell(row.kind).size()), spell(row.kind).data(),
        static_cast<int>(row.name.size()), row.name.data(),
        static_cast<int>(row.user.size()), row.user.data(),
        static_cast<int>(row.via.size()), row.via.data(),
        static_cast<int>(spell(target.kind).size()), spell(target.kind).data(),
        target.name.c_str(), target.user.c_str(), target.via.c_str());
    return false;
}

bool checkAll(std::span<const Row> rows)
{
    bool didAllMatch = true;
    for (const Row& row : rows)
        didAllMatch = check(row) && didAllMatch;
    return didAllMatch;
}

} // namespace

// ssh's own grammar, which is the one every other shape borrows. A destination
// is the first token that is not an option and not an option's value, and
// everything after it belongs to the remote host rather than to ssh.
TEST(testSSHNamesItsDestinationWhereverItSits)
{
    static constexpr Row rows[] {
        { "ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh ada@example-host", RemoteKind::Host, "example-host", "ada", "ssh" },
        { "/usr/bin/ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        // An option that takes a value eats the token after it, so the
        // destination is argv[3] here and argv[1] would be the port.
        { "ssh -p 2222 example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh -l ada example-host", RemoteKind::Host, "example-host", "ada", "ssh" },
        // Attached values and bundles: no separate token to skip.
        { "ssh -p2222 example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh -tt example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh -4v example-host", RemoteKind::Host, "example-host", "", "ssh" },
        // A bundle ENDING in a value-taking letter still eats the next token.
        { "ssh -vp 2222 example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh -o ConnectTimeout=90 ada@example-host", RemoteKind::Host, "example-host", "ada", "ssh" },
        { "ssh -oConnectTimeout=90 example-host", RemoteKind::Host, "example-host", "", "ssh" },
        // What follows the destination is the REMOTE command, not ssh's.
        { "ssh example-host uptime", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh example-host -p 22", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh -- example-host", RemoteKind::Host, "example-host", "", "ssh" },
        // The URI form carries a scheme and may carry a port; neither is the name.
        { "ssh ssh://ada@example-host:2222", RemoteKind::Host, "example-host", "ada", "ssh" },
        { "ssh ssh://example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "ssh ssh://[::1]:22", RemoteKind::Host, "::1", "", "ssh" },
        // A bare IPv6 literal is unbracketed outside the URI form.
        { "ssh ::1", RemoteKind::Host, "::1", "", "ssh" },
    };
    CHECK(checkAll(rows));
}

// No destination means no claim. A session that has not been told where to go
// is not somewhere else, and saying it is would tag a window with a lie.
TEST(testAnSSHWithNoDestinationNamesNowhere)
{
    static constexpr Row rows[] {
        { "ssh", RemoteKind::None, "", "", "" },
        { "ssh -V", RemoteKind::None, "", "", "" },
        { "ssh -p 2222", RemoteKind::None, "", "", "" },
        // A different program whose name merely starts the same way.
        { "ssh-keygen -t ed25519", RemoteKind::None, "", "", "" },
        { "ssh-add -l", RemoteKind::None, "", "", "" },
        { "vim ssh.c", RemoteKind::None, "", "", "" },
        { "", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

// The other ways onto a machine. Each has its own option spelling — mosh's
// flags are not ssh's — so each is a row rather than an assumption.
TEST(testTheOtherRoutesToAHostNameTheirDestination)
{
    static constexpr Row rows[] {
        { "mosh example-host", RemoteKind::Host, "example-host", "", "mosh" },
        { "mosh ada@example-host", RemoteKind::Host, "example-host", "ada", "mosh" },
        { "mosh --ssh=ssh -p 60000 ada@example-host", RemoteKind::Host, "example-host", "ada", "mosh" },
        { "et ada@example-host", RemoteKind::Host, "example-host", "ada", "et" },
        { "telnet example-host", RemoteKind::Host, "example-host", "", "telnet" },
        // telnet's second operand is a port, not another destination.
        { "telnet example-host 23", RemoteKind::Host, "example-host", "", "telnet" },
        { "autossh -M 0 ada@example-host", RemoteKind::Host, "example-host", "ada", "autossh" },
        { "slogin example-host", RemoteKind::Host, "example-host", "", "slogin" },
    };
    CHECK(checkAll(rows));
}

// A tool whose own flags come ahead of the subcommand that does the connecting.
// `via` spells the pair, because "tailscale" alone does not say what happened.
TEST(testASubcommandCarriesTheDestination)
{
    static constexpr Row rows[] {
        { "tailscale ssh example-host", RemoteKind::Host, "example-host", "", "tailscale ssh" },
        { "tailscale ssh ada@example-host", RemoteKind::Host, "example-host", "ada", "tailscale ssh" },
        { "tailscale --socket=/var/run/x ssh example-host", RemoteKind::Host, "example-host", "", "tailscale ssh" },
        { "tsh ssh ada@example-host", RemoteKind::Host, "example-host", "ada", "tsh ssh" },
        { "gcloud compute ssh vm-1", RemoteKind::Host, "vm-1", "", "gcloud compute ssh" },
        { "kitten ssh example-host", RemoteKind::Host, "example-host", "", "kitten ssh" },
        // The same tool doing something that goes nowhere.
        { "tailscale status", RemoteKind::None, "", "", "" },
        { "gcloud compute instances list", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

// A container is a place too, and `web-1` means something different from a
// hostname `web-1` — which is why the kind travels with the name.
TEST(testAContainerIsAPlaceWithItsOwnKind)
{
    static constexpr Row rows[] {
        { "docker exec -it web-1 bash", RemoteKind::Container, "web-1", "", "docker exec" },
        { "docker exec -i -t web-1 sh", RemoteKind::Container, "web-1", "", "docker exec" },
        { "docker exec -u root web-1 sh", RemoteKind::Container, "web-1", "root", "docker exec" },
        { "docker attach web-1", RemoteKind::Container, "web-1", "", "docker attach" },
        { "podman exec -it api sh", RemoteKind::Container, "api", "", "podman exec" },
        { "kubectl exec -it pod-7 -- sh", RemoteKind::Container, "pod-7", "", "kubectl exec" },
        { "kubectl exec -n prod pod-7 -c app -- bash", RemoteKind::Container, "pod-7", "", "kubectl exec" },
        { "lxc exec c1 -- bash", RemoteKind::Container, "c1", "", "lxc exec" },
        { "incus exec c1 -- bash", RemoteKind::Container, "c1", "", "incus exec" },
        { "distrobox enter fedora", RemoteKind::Container, "fedora", "", "distrobox enter" },
        { "toolbox enter rhel", RemoteKind::Container, "rhel", "", "toolbox enter" },
        { "docker ps", RemoteKind::None, "", "", "" },
        { "kubectl get pods", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

TEST(testAContainerNamedByAnOptionIsNotDiscarded)
{
    static constexpr Row rows[] {
        { "distrobox enter -n fedora", RemoteKind::Container, "fedora", "", "distrobox enter" },
        { "distrobox enter --name fedora", RemoteKind::Container, "fedora", "", "distrobox enter" },
        { "distrobox enter --name=fedora", RemoteKind::Container, "fedora", "", "distrobox enter" },
        { "toolbox enter -c rhel", RemoteKind::Container, "rhel", "", "toolbox enter" },
        { "toolbox enter --container rhel", RemoteKind::Container, "rhel", "", "toolbox enter" },
        { "toolbox enter --container=rhel", RemoteKind::Container, "rhel", "", "toolbox enter" },
        { "distrobox enter --name fedora -- bash", RemoteKind::Container, "fedora", "", "distrobox enter" },
        { "toolbox enter --container=rhel -- bash", RemoteKind::Container, "rhel", "", "toolbox enter" },
        { "distrobox enter -nfedora bash", RemoteKind::Container, "fedora", "", "distrobox enter" },
        { "toolbox enter --container=", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

// A local virtual machine. `vagrant ssh` with no name still goes somewhere —
// the box this directory owns — so the kind stands even where the name cannot.
TEST(testALocalVirtualMachineIsNamedWhereItCanBe)
{
    static constexpr Row rows[] {
        { "vagrant ssh web", RemoteKind::VirtualMachine, "web", "", "vagrant ssh" },
        { "vagrant ssh", RemoteKind::VirtualMachine, "", "", "vagrant ssh" },
        { "limactl shell default", RemoteKind::VirtualMachine, "default", "", "limactl shell" },
        { "multipass shell primary", RemoteKind::VirtualMachine, "primary", "", "multipass shell" },
        { "virsh console fedora", RemoteKind::VirtualMachine, "fedora", "", "virsh console" },
        { "vagrant up", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

// A serial line is hardware, and `screen` is only serial when its operand is a
// device — `screen -r` is a multiplexer and goes nowhere.
TEST(testASerialLineIsRecognisedByItsDeviceNotItsTool)
{
    static constexpr Row rows[] {
        { "screen /dev/tty.usbserial-1420 115200", RemoteKind::Serial, "/dev/tty.usbserial-1420", "", "screen" },
        { "picocom /dev/ttyUSB0", RemoteKind::Serial, "/dev/ttyUSB0", "", "picocom" },
        { "minicom -D /dev/ttyUSB0", RemoteKind::Serial, "/dev/ttyUSB0", "", "minicom" },
        { "minicom /dev/ttyUSB0", RemoteKind::None, "", "", "" },
        { "screen -r", RemoteKind::None, "", "", "" },
        { "screen", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

// A prefix that runs something else is not the destination, and the tool it
// wraps has to be found behind it — including prefixes with options of their
// own, which is what makes this more than dropping the first token.
TEST(testATransparentPrefixIsLookedThrough)
{
    static constexpr Row rows[] {
        { "sudo ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "doas ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "env FOO=1 ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "env -i ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "nice -n 5 ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        { "sshpass -p secret ssh ada@example-host", RemoteKind::Host, "example-host", "ada", "ssh" },
        { "time ssh example-host", RemoteKind::Host, "example-host", "", "ssh" },
        // A prefix wrapping something that goes nowhere is still nowhere.
        { "sudo ls", RemoteKind::None, "", "", "" },
        { "env FOO=1", RemoteKind::None, "", "", "" },
    };
    CHECK(checkAll(rows));
}

// The lookup is by basename and is case-insensitive, because a process can
// report a name the reader never typed — macOS names python3 `Python`.
TEST(testTheToolIsFoundByBasenameWhateverItsCase)
{
    static constexpr Row rows[] {
        { "/opt/homebrew/bin/mosh example-host", RemoteKind::Host, "example-host", "", "mosh" },
        { "SSH example-host", RemoteKind::Host, "example-host", "", "SSH" },
        { "/usr/local/bin/tailscale ssh example-host", RemoteKind::Host, "example-host", "", "tailscale ssh" },
    };
    CHECK(checkAll(rows));
}

UBIQUITTY_TEST_MAIN()
