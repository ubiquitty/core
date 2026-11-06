// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_ssh_reach — the address a QUIC carrier dials is the one ssh says it
// connected to (Docs/sync.md §6.9): read from the attempt that was
// established, told apart from a proxied run and one over a shared
// connection, and never taken from a line that is not ssh's own or an address
// that is not numeric.

#include "Harness.h"

#include "Core/Session/SSHReach.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

using namespace Ubiquitty::Session;

namespace {

/// A direct connect as OpenSSH 10 prints it under -v, cut to what bears.
constexpr std::string_view directTranscript = "debug1: OpenSSH_10.3p1, LibreSSL 3.3.6\n"
                                              "debug1: Reading configuration data /Users/user/.ssh/config\n"
                                              "debug1: Connecting to build.example.org [192.0.2.1] port 22.\n"
                                              "debug1: Connection established.\n"
                                              "debug1: identity file /Users/user/.ssh/id_ed25519 type 3\n"
                                              "debug1: Remote protocol version 2.0, remote software version OpenSSH_9.6\n"
                                              "Authenticated to build.example.org ([192.0.2.1]:22) using \"publickey\".\n";

/// What `strings` finds of a format in a binary, or whether it could be asked.
enum class Found : std::uint8_t {
    Yes,
    No,
    Unreadable,
};

Found binaryHolds(const std::string& path, const char* format)
{
    const std::string command = "strings '" + path + "' 2>/dev/null | grep -F -q -- '" + format + "'";
    const int status = std::system(command.c_str());
    if (status == -1)
        return Found::Unreadable;
    return status == 0 ? Found::Yes : Found::No;
}

} // namespace

// The address is the one in ssh's own connect line, for the attempt that was
// established; the port beside it is ssh's.
TEST(theAddressIsTheOneSSHConnectedTo)
{
    const SSHReach direct { .kind = SSHReachKind::Direct, .address = "192.0.2.1", .port = 22 };
    CHECK(readSSHReach(directTranscript) == direct);
    CHECK(sshReachRefusal(SSHReachKind::Direct).empty());
    // A transcript whose lines end CR LF reads the same.
    std::string carriageReturns;
    for (const char c : directTranscript) {
        if (c == '\n')
            carriageReturns += '\r';
        carriageReturns += c;
    }
    CHECK(readSSHReach(carriageReturns) == direct);
}

// ssh tries each address a name resolves to and prints a connect line for
// each: the one that counts is the one `Connection established.` follows.
TEST(anAddressThatWasTriedAndFailedIsNotTheOneReached)
{
    constexpr std::string_view transcript = "debug1: Connecting to dual.example.org [2001:db8::1] port 2222.\n"
                                            "debug1: connect to address 2001:db8::1 port 2222: No route to host\n"
                                            "debug1: Connecting to dual.example.org [198.51.100.7] port 2222.\n"
                                            "debug1: Connection established.\n";
    const SSHReach reached { .kind = SSHReachKind::Direct, .address = "198.51.100.7", .port = 2222 };
    CHECK(readSSHReach(transcript) == reached);
    // Every attempt failed: no address was reached.
    constexpr std::string_view failed = "debug1: Connecting to dual.example.org [2001:db8::1] port 22.\n"
                                        "debug1: connect to address 2001:db8::1 port 22: No route to host\n"
                                        "debug1: Connecting to dual.example.org [198.51.100.7] port 22.\n"
                                        "debug1: connect to address 198.51.100.7 port 22: Connection refused\n"
                                        "ssh: connect to host dual.example.org port 22: Connection refused\n";
    CHECK(readSSHReach(failed).kind == SSHReachKind::Unrecognised);
    // A transcript cut before the line that says it connected names nothing.
    CHECK(readSSHReach("debug1: Connecting to build.example.org [192.0.2.1] port 22.\n").kind == SSHReachKind::Unrecognised);
    CHECK(readSSHReach("debug1: Connecting to build.example.org [192.0.2.1] port 22.\ndebug1: Connection establ").kind == SSHReachKind::Unrecognised);
}

// IPv6 literals, with a zone or an IPv4 tail, are addresses.
TEST(anIPv6AddressIsReadWhole)
{
    const SSHReach global { .kind = SSHReachKind::Direct, .address = "2001:db8::42", .port = 22 };
    CHECK(readSSHReach("debug1: Connecting to six.example.org [2001:db8::42] port 22.\ndebug1: Connection established.\n") == global);
    const SSHReach scoped { .kind = SSHReachKind::Direct, .address = "fe80::1c2d:3eff:fe4f:5a6b%en0", .port = 22 };
    CHECK(readSSHReach("debug1: Connecting to printer.local [fe80::1c2d:3eff:fe4f:5a6b%en0] port 22.\ndebug1: Connection established.\n") == scoped);
    const SSHReach loopback { .kind = SSHReachKind::Direct, .address = "::1", .port = 22 };
    CHECK(readSSHReach("debug1: Connecting to localhost [::1] port 22.\ndebug1: Connection established.\n") == loopback);
    CHECK(isNumericAddress("::ffff:192.0.2.1"));
    CHECK(isNumericAddress("192.0.2.1"));
    CHECK(isNumericAddress("fe80::1%eth0.100"));
}

// What is not a numeric address is never dialled: a name, a number out of
// range, a zone with shell text, an empty bracket.
TEST(onlyANumericAddressIsTaken)
{
    for (const std::string_view notAnAddress : { std::string_view { "" }, std::string_view { "build.example.org" }, std::string_view { "192.0.2" }, std::string_view { "192.0.2.256" },
             std::string_view { "192.0.2.1.5" }, std::string_view { "192.0..1" }, std::string_view { "1.2.3.4%en0" }, std::string_view { "fe80::1%" }, std::string_view { "fe80::1%en0;id" },
             std::string_view { "fe80::1 %en0" }, std::string_view { ":" }, std::string_view { "2001:db8::g" }, std::string_view { "$(id)" }, std::string_view { "1234" } })
        CHECK_MSG(!isNumericAddress(notAnAddress), std::string(notAnAddress).c_str());
    // Nothing ssh prints as an address is this long, and nothing longer is
    // taken for one however it is spelled.
    std::string longest;
    while (longest.size() < 100)
        longest += "1:";
    CHECK(isNumericAddress(longest));
    CHECK(!isNumericAddress(longest + "1"));
    for (const std::string_view line : { std::string_view { "debug1: Connecting to host [build.example.org] port 22.\n" }, std::string_view { "debug1: Connecting to host [] port 22.\n" },
             std::string_view { "debug1: Connecting to host [192.0.2.1] port 0.\n" }, std::string_view { "debug1: Connecting to host [192.0.2.1] port 70000.\n" },
             std::string_view { "debug1: Connecting to host [192.0.2.1] port 22\n" }, std::string_view { "debug1: Connecting to host [192.0.2.1] port x.\n" },
             std::string_view { "debug1: Connecting to host 192.0.2.1 port 22.\n" }, std::string_view { "debug1: Connecting to host [192.0.2.1] port .\n" } })
        CHECK_MSG(readSSHReach(std::string(line) + "debug1: Connection established.\n").kind == SSHReachKind::Unrecognised, std::string(line).c_str());
    // A connect line that names no address ends the attempt before it: the
    // established line that follows is not the earlier address's.
    constexpr std::string_view superseded = "debug1: Connecting to host [192.0.2.1] port 22.\n"
                                            "debug1: Connecting to host [not-an-address] port 22.\n"
                                            "debug1: Connection established.\n";
    CHECK(readSSHReach(superseded).kind == SSHReachKind::Unrecognised);
}

// Only ssh's own lines are read: text the remote end chose, a banner shown
// before the login, is not a connect line however it is worded.
TEST(aBannerCannotNameTheAddress)
{
    constexpr std::string_view banner = "debug1: Connecting to build.example.org [192.0.2.1] port 22.\n"
                                        "debug1: Connection established.\n"
                                        "Welcome. debug1: Connecting to evil [203.0.113.9] port 22.\n"
                                        "  debug1: Connecting to evil [203.0.113.9] port 22.\n"
                                        "debug1: Connection established. \n";
    const SSHReach reached { .kind = SSHReachKind::Direct, .address = "192.0.2.1", .port = 22 };
    CHECK(readSSHReach(banner) == reached);
    // Text worded exactly as ssh's own lines, shown after ssh connected,
    // changes nothing: the first established attempt stands.
    constexpr std::string_view forged = "debug1: Connecting to build.example.org [192.0.2.1] port 22.\n"
                                        "debug1: Connection established.\n"
                                        "debug1: Connecting to evil [203.0.113.9] port 22.\n"
                                        "debug1: Connection established.\n";
    CHECK(readSSHReach(forged) == reached);
    constexpr std::string_view onlyBanner = "Welcome. debug1: Connecting to evil [203.0.113.9] port 22.\n"
                                            " debug1: Connection established.\n";
    CHECK(readSSHReach(onlyBanner).kind == SSHReachKind::Unrecognised);
    // Each line has to be ssh's own, whole: a connect line with text ahead
    // of it is not one though the established line after it is exact, and
    // an exact connect line is not established by a line that only holds
    // the words.
    CHECK(readSSHReach("Welcome. debug1: Connecting to evil [203.0.113.9] port 22.\ndebug1: Connection established.\n").kind == SSHReachKind::Unrecognised);
    CHECK(readSSHReach("debug1: Connecting to evil [203.0.113.9] port 22.\nNote: debug1: Connection established.\n").kind == SSHReachKind::Unrecognised);
    CHECK(readSSHReach("debug1: Connecting to evil [203.0.113.9] port 22.\ndebug1: Connection established. Really.\n").kind == SSHReachKind::Unrecognised);
}

// A run through a proxy reaches the host at no address this machine can
// dial, and says so; no carrier is offered.
TEST(aProxiedRouteNamesNoAddress)
{
    constexpr std::string_view jump = "debug1: Setting implicit ProxyCommand from ProxyJump: ssh -l alice -v -W '[%h]:%p' bastion.example.org\n"
                                      "debug1: Executing proxy command: exec ssh -l alice -v -W '[inner.example.org]:22' bastion.example.org\n"
                                      "debug1: identity file /Users/user/.ssh/id_ed25519 type 3\n"
                                      "debug1: Connecting to bastion.example.org [192.0.2.9] port 22.\n"
                                      "debug1: Connection established.\n"
                                      "Authenticated to inner.example.org (via proxy) using \"publickey\".\n";
    // The connect line is the inner ssh's, to the bastion: it comes first in
    // order of arrival here, and is still not the host's address.
    constexpr std::string_view command = "debug1: Executing proxy command: exec nc -X connect -x proxy:3128 inner.example.org 22\n";
    CHECK(readSSHReach(command).kind == SSHReachKind::Proxied);
    constexpr std::string_view jumpThenQuiet = "debug1: Setting implicit ProxyCommand from ProxyJump: ssh -W '[%h]:%p' bastion\n"
                                               "debug1: Executing proxy command: exec ssh -W '[inner]:22' bastion\n";
    CHECK(readSSHReach(jumpThenQuiet).kind == SSHReachKind::Proxied);
    CHECK(readSSHReach(jumpThenQuiet).address.empty());
    CHECK(!sshReachRefusal(SSHReachKind::Proxied).empty());
    // The inner ssh's lines share the transcript when it is verbose too: its
    // established attempt is the bastion's. The proxy lines come first, so
    // the run is known proxied before any address is read.
    CHECK(readSSHReach(jump).kind == SSHReachKind::Proxied);
}

// A run over a connection another ssh holds makes none of its own: it is
// told apart from a transcript that says nothing, because a direct probe can
// still learn the address.
TEST(aSharedConnectionIsToldApart)
{
    constexpr std::string_view shared = "debug1: OpenSSH_10.3p1, LibreSSL 3.3.6\n"
                                        "debug1: auto-mux: Trying existing master at '/Users/user/.ssh/cm-alice@build.example.org:22'\n"
                                        "debug1: mux_client_request_session: master session id: 2\n";
    CHECK(readSSHReach(shared).kind == SSHReachKind::Multiplexed);
    // The control socket's path, as ssh expanded it, names the master.
    CHECK_STR_EQ(readSSHReach(shared).sharedConnectionPath, std::string("/Users/user/.ssh/cm-alice@build.example.org:22"));
    CHECK(readSSHReach("debug1: auto-mux: Trying existing master at ''\n").kind == SSHReachKind::Unrecognised);
    CHECK(readSSHReach("debug1: auto-mux: Trying existing master at '/tmp/cm\n").kind == SSHReachKind::Unrecognised);
    CHECK(readSSHReach("debug1: auto-mux: Trying existing master at '/tmp/first'\ndebug1: auto-mux: Trying existing master at '/tmp/second'\n").sharedConnectionPath == "/tmp/first");
    CHECK(!sshReachRefusal(SSHReachKind::Multiplexed).empty());
    // The master was stale and ssh connected itself after all.
    constexpr std::string_view stale = "debug1: auto-mux: Trying existing master at '/Users/user/.ssh/cm-alice@build.example.org:22'\n"
                                       "debug1: Control socket \"/Users/user/.ssh/cm-alice@build.example.org:22\" does not exist\n"
                                       "debug1: Connecting to build.example.org [192.0.2.1] port 22.\n"
                                       "debug1: Connection established.\n";
    const SSHReach reached { .kind = SSHReachKind::Direct, .address = "192.0.2.1", .port = 22 };
    CHECK(readSSHReach(stale) == reached);
    CHECK(readSSHReach("").kind == SSHReachKind::Unrecognised);
    CHECK(readSSHReach("Permission denied (publickey).\n").kind == SSHReachKind::Unrecognised);
    CHECK(!sshReachRefusal(SSHReachKind::Unrecognised).empty());
    CHECK(sshReachRefusal(SSHReachKind::Unrecognised) != sshReachRefusal(SSHReachKind::Multiplexed));
    CHECK(sshReachRefusal(SSHReachKind::Proxied) != sshReachRefusal(SSHReachKind::Multiplexed));
}

// The lines read are this machine's ssh's own formats. A different OpenSSH
// that words them otherwise turns this red; a machine with no ssh, or whose
// binary cannot be read, is skipped and says so.
TEST(theLocalSSHPrintsTheLinesThatAreRead)
{
    std::string path;
    if (std::FILE* which = ::popen("command -v ssh 2>/dev/null", "r")) {
        char buffer[512] {};
        if (std::fgets(buffer, sizeof(buffer), which))
            path = buffer;
        ::pclose(which);
    }
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
    if (path.empty() || binaryHolds(path, "OpenSSH") == Found::Unreadable) {
        UbiquittyTest::skip("no ssh on PATH, or strings(1) could not read it");
        return;
    }
    const Found connecting = binaryHolds(path, "Connecting to %.200s [%.100s] port %s.");
    const Found established = binaryHolds(path, "Connection established.");
    if (connecting == Found::No && established == Found::No && binaryHolds(path, "Executing proxy command: %.500s") == Found::No) {
        UbiquittyTest::skip((path + " holds none of OpenSSH's diagnostics as plain strings").c_str());
        return;
    }
    CHECK_MSG(connecting == Found::Yes, "this ssh words its connect line otherwise: readSSHReach would offer no QUIC carrier");
    CHECK_MSG(established == Found::Yes, "this ssh words its established line otherwise");
    CHECK_MSG(binaryHolds(path, "Executing proxy command: %.500s") == Found::Yes, "this ssh words its proxy line otherwise");
    CHECK_MSG(binaryHolds(path, "Setting implicit ProxyCommand from ProxyJump: %s") == Found::Yes, "this ssh words its ProxyJump line otherwise");
    CHECK_MSG(binaryHolds(path, "auto-mux: Trying existing master at ") == Found::Yes, "this ssh words its shared-connection line otherwise");
}

// What is known of a shared connection is kept for that master and no
// other: another master at the same path, or another path, is not it, and
// only so many are remembered.
TEST(aSharedConnectionsReachIsKeptForThatMaster)
{
    SharedConnectionReaches reaches;
    const SharedConnectionIdentity master { .controlPath = "/tmp/cm-a", .madeAt = 100 };
    const SharedConnectionIdentity renewed { .controlPath = "/tmp/cm-a", .madeAt = 200 };
    const SharedConnectionIdentity other { .controlPath = "/tmp/cm-b", .madeAt = 100 };
    CHECK(!reaches.find(master).has_value());
    reaches.note(master, "192.0.2.1");
    reaches.note(other, "192.0.2.2");
    CHECK(reaches.find(master) == std::optional<std::string> { "192.0.2.1" });
    CHECK(reaches.find(other) == std::optional<std::string> { "192.0.2.2" });
    CHECK(!reaches.find(renewed).has_value());
    // The master made anew replaces what was kept for its path.
    reaches.note(renewed, "192.0.2.9");
    CHECK(reaches.find(renewed) == std::optional<std::string> { "192.0.2.9" });
    CHECK(!reaches.find(master).has_value());
    CHECK(reaches.find(other) == std::optional<std::string> { "192.0.2.2" });
    for (std::size_t index = 0; index < SharedConnectionReaches::maxReaches; ++index)
        reaches.note({ .controlPath = "/tmp/many-" + std::to_string(index), .madeAt = 1 }, "198.51.100.1");
    CHECK(!reaches.find(renewed).has_value());
    CHECK(!reaches.find(other).has_value());
    CHECK(reaches.find({ .controlPath = "/tmp/many-0", .madeAt = 1 }).has_value());
    // Nothing at a path is no master.
    CHECK(!identifySharedConnection("").has_value());
    CHECK(!identifySharedConnection("/nonexistent/ubiquitty/control-socket").has_value());
}

UBIQUITTY_TEST_MAIN()
