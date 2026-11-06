// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sshhost.cpp — portable SSH-host validation + /bin/sh single-quoting
// (Transport/SSHHost.h). Pins
// the argv-injection guard (leading '-' on the whole string and each '@'-part),
// the conservative characterSet (admits IPv6 ':[]%'), and the shell single-quote
// escape of embedded quotes. Header-only, links standalone.

#include "Core/Network/RemoteLaunch.h"
#include "Core/Transport/SSHHost.h"

#include <cstdio>
#include <string>

using Ubiquitty::Transport::shellSingleQuote;
using Ubiquitty::Transport::validateSSHHost;

TEST(sshhostCases)
{

    // ---- valid plain + user@host forms ----
    CHECK(validateSSHHost("example.com"));
    CHECK(validateSSHHost("host"));
    CHECK(validateSSHHost("user@example.com"));
    CHECK(validateSSHHost("user@host.local"));
    CHECK(validateSSHHost("u_ser-1@my-host_2.example.com"));
    CHECK(validateSSHHost("192.168.1.10"));
    CHECK(validateSSHHost("user@192.168.1.10"));

    // ---- valid IPv6 forms: characterSet admits ':' '[' ']' '%' ----
    CHECK(validateSSHHost("[::1]"));
    CHECK(validateSSHHost("[fe80::1]"));
    CHECK(validateSSHHost("fe80::1%en0")); // scoped link-local
    CHECK(validateSSHHost("host:22")); // host:port admitted (ssh ignores; mid-string ':' safe)
    CHECK(validateSSHHost("user@[2001:db8::1]"));

    // ---- argv-injection guard: leading '-' on the WHOLE string ----
    CHECK(!validateSSHHost("-oProxyCommand=evil"));
    CHECK(!validateSSHHost("-l"));
    CHECK(!validateSSHHost("-")); // bare dash

    // ---- argv-injection guard: leading '-' on an '@'-PART ----
    CHECK(!validateSSHHost("user@-oProxyCommand=evil")); // host half is an option
    CHECK(!validateSSHHost("-user@host")); // user half is an option (also caught by whole-string)
    CHECK(!validateSSHHost("a@-b"));

    // ---- empty parts rejected (empty whole, leading/trailing/double '@') ----
    CHECK(!validateSSHHost(""));
    CHECK(!validateSSHHost("@host")); // empty user part
    CHECK(!validateSSHHost("user@")); // empty host part
    CHECK(!validateSSHHost("a@@b")); // empty middle part

    // ---- a string is no longer than a host's records keep of one ----
    CHECK(validateSSHHost(std::string(Ubiquitty::Transport::maxSSHHostLength, 'h')));
    CHECK(!validateSSHHost(std::string(Ubiquitty::Transport::maxSSHHostLength + 1, 'h')));
    CHECK(!validateSSHHost("user@" + std::string(Ubiquitty::Transport::maxSSHHostLength, 'h')));

    // ---- characterSet rejects shell/space metacharacters (defense in depth) ----
    CHECK(!validateSSHHost("host;rm -rf")); // ';' and space out of characterSet
    CHECK(!validateSSHHost("host name")); // space
    CHECK(!validateSSHHost("host`whoami`")); // backtick
    CHECK(!validateSSHHost("host$(id)")); // '$' '(' ')'
    CHECK(!validateSSHHost("host/path")); // '/'
    CHECK(!validateSSHHost("host\nx")); // newline

    // ---- shell single-quote: wraps in '...' and escapes embedded quotes ----
    CHECK(shellSingleQuote("host") == "'host'");
    CHECK(shellSingleQuote("") == "''"); // empty → one empty shell word
    CHECK(shellSingleQuote("a b") == "'a b'"); // spaces stay inside the quotes
    CHECK(shellSingleQuote("it's") == "'it'\\''s'"); // ' → '\''
    CHECK(shellSingleQuote("'")
        == "''\\''"
           "'"); // single quote alone → '' \' '
    CHECK(shellSingleQuote("a'b'c") == "'a'\\''b'\\''c'"); // two embedded quotes
    // $HOME stays literal — single quotes suppress expansion, only ' is special.
    CHECK(shellSingleQuote("$HOME/.ubiquitty") == "'$HOME/.ubiquitty'");
}

// startup_host names a HOST, and everything except the implementation already
// said so: the field's name, the Values label "Startup Host", its placeholder
// "e.g. ssh host", and the shipped config template ("opens the session against
// that host instead of the local shell", the startup_host comment in
// PreferencesFileTemplate.cpp).
//
// The implementation whitespace-tokenized the string into argv and execve()'d
// argument zero, so it ran any program with any arguments. That matters because a
// profile is a SHAREABLE artifact: the same template documents profiles as
// "named presentation bundles" and gives `startup_host = "ssh prod.example.com"`
// as the example to copy, the config watcher re-reads the file when a synced
// dotfiles checkout writes it, and profile inheritance means a user's own trusted
// profile with `parent = "shared-base"` picks up an ancestor's startup_host
// without them ever selecting the poisoned profile. The menu shows a bare name.
//
// So this pins the shape: a bare host, or the documented ssh/mosh prefix, and
// nothing else. Options are refused rather than forwarded — `-oProxyCommand=…` is
// an ssh option that names a program to run, so a startup_host allowed to carry
// flags is a startup_host allowed to run anything, which is the whole defect.
//
// Fails-without-it: every REFUSED case below returns a populated argv whose first
// element is the attacker's program.
TEST(startupHostAcceptsAHostAndRefusesACommand)
{
    using Ubiquitty::Network::buildStartupHostArguments;
    using Ubiquitty::Network::StartupHostRejection;

    const std::string ssh = "/usr/bin/ssh";
    const std::string mosh = "/opt/homebrew/bin/mosh";

    // ---- accepted: a bare host ----
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "prod.example.com");
        CHECK(launch.accepted());
        CHECK(launch.arguments.size() >= 2);
        CHECK(launch.arguments.front() == ssh);
        CHECK(launch.arguments.back() == "prod.example.com");
    }

    // ---- accepted: the form the config template documents ----
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "ssh prod.example.com");
        CHECK(launch.accepted());
        CHECK(launch.arguments.front() == ssh);
        CHECK(launch.arguments.back() == "prod.example.com");
    }

    // ---- accepted: user@host, and mosh ----
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "deploy@10.0.0.4");
        CHECK(launch.accepted());
        CHECK(launch.arguments.back() == "deploy@10.0.0.4");

        auto viaMosh = buildStartupHostArguments(ssh, mosh, "mosh deploy@10.0.0.4");
        CHECK(viaMosh.accepted());
        CHECK(viaMosh.arguments.front() == mosh);
    }

    // ---- accepted: surrounding whitespace is not a shape ----
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "   ssh  host   ");
        CHECK(launch.accepted());
        CHECK(launch.arguments.back() == "host");
    }

    // ---- REFUSED: an arbitrary program ----
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "/tmp/payload --exfiltrate");
        CHECK(!launch.accepted());
        CHECK(launch.arguments.empty());
        CHECK(launch.rejection == StartupHostRejection::UnknownProgram);
    }
    {
        // A bare name PATH-walks in the POSIX spawn path, so it needs refusing too.
        auto launch = buildStartupHostArguments(ssh, mosh, "curl evil.example.com/x");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::UnknownProgram);
    }

    // ---- REFUSED: ssh with options. -oProxyCommand names a program to run, so
    // permitting flags here would reopen the hole through the allowed program.
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "ssh -oProxyCommand=/tmp/payload host");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::ExtraArguments);
    }
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "ssh host -- /tmp/payload");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::ExtraArguments);
    }

    // ---- REFUSED: a host that is really an ssh option (the argv-injection guard
    // validateSSHHost already carries, reached through this path too) ----
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "-oProxyCommand=/tmp/payload");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::InvalidHost);
    }
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "ssh -badhost");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::InvalidHost);
    }

    // ---- REFUSED: shell metacharacters. Nothing runs a shell on this path, so
    // these are not injection today — they are refused because they are not a host,
    // and because the next consumer might.
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "host;/tmp/payload");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::InvalidHost);
    }
    {
        auto launch = buildStartupHostArguments(ssh, mosh, "host$(id)");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::InvalidHost);
    }

    // ---- REFUSED: empty / whitespace ----
    {
        CHECK(buildStartupHostArguments(ssh, mosh, "").rejection == StartupHostRejection::Empty);
        CHECK(buildStartupHostArguments(ssh, mosh, "   ").rejection == StartupHostRejection::Empty);
    }

    // ---- REFUSED: the binary is missing, so nothing is launched rather than a
    // bare host being handed to whatever the spawn path resolves.
    {
        auto launch = buildStartupHostArguments("", mosh, "prod.example.com");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::MissingBinary);
    }
    {
        auto launch = buildStartupHostArguments(ssh, "", "mosh prod.example.com");
        CHECK(!launch.accepted());
        CHECK(launch.rejection == StartupHostRejection::MissingBinary);
    }
}

// One parse answers every surface that reads a typed address, so each accepted
// shape is pinned field by field: a parser that kept the port in the host, or
// read a tmux session name as a port, would still accept the string.
TEST(parseConnectAddressReadsEveryShapeItAccepts)
{
    using Ubiquitty::Transport::ConnectKindHint;
    using Ubiquitty::Transport::parseConnectAddress;

    const auto plain = parseConnectAddress("  user@build.example  ");
    CHECK(plain.isValid);
    CHECK(plain.reason.empty());
    CHECK(plain.kindHint == ConnectKindHint::None);
    CHECK_STR_EQ(plain.user, "user");
    CHECK_STR_EQ(plain.host, "build.example");
    CHECK_EQ(plain.port, 0);
    CHECK_STR_EQ(plain.destination(), "user@build.example");

    const auto ported = parseConnectAddress("build.example:2222");
    CHECK(ported.isValid);
    CHECK_STR_EQ(ported.host, "build.example");
    CHECK_EQ(ported.port, 2222);

    const auto bracketed = parseConnectAddress("user@[2001:db8::1]:22");
    CHECK(bracketed.isValid);
    CHECK_STR_EQ(bracketed.host, "2001:db8::1");
    CHECK_EQ(bracketed.port, 22);
    CHECK_STR_EQ(bracketed.destination(), "user@[2001:db8::1]");

    const auto bare = parseConnectAddress("fe80::1");
    CHECK(bare.isValid);
    CHECK_STR_EQ(bare.host, "fe80::1");
    CHECK_EQ(bare.port, 0);

    const auto device = parseConnectAddress("/dev/cu.usbserial-1410");
    CHECK(device.isValid);
    CHECK(device.kindHint == ConnectKindHint::Serial);
    CHECK_STR_EQ(device.devicePath, "/dev/cu.usbserial-1410");
    CHECK(device.host.empty());

    const auto tmux = parseConnectAddress("tmux build.example:main");
    CHECK(tmux.isValid);
    CHECK(tmux.kindHint == ConnectKindHint::Tmux);
    CHECK_STR_EQ(tmux.host, "build.example");
    CHECK_STR_EQ(tmux.tmuxSession, "main");
    CHECK_EQ(tmux.port, 0);

    const auto mosh = parseConnectAddress("mosh user@build.example");
    CHECK(mosh.isValid);
    CHECK(mosh.kindHint == ConnectKindHint::Mosh);
    CHECK_STR_EQ(mosh.user, "user");

    const auto ssh = parseConnectAddress("ssh build.example");
    CHECK(ssh.isValid);
    CHECK(ssh.kindHint == ConnectKindHint::SSH);
}

// Every refusal carries a sentence, because the sentence is what a person sees
// under the field instead of an alert. Each negative is one ssh would read as
// something other than a host: an option, a second argument, a shell command,
// or a port it cannot dial.
TEST(parseConnectAddressRefusesWithAReason)
{
    using Ubiquitty::Transport::parseConnectAddress;

    const auto expectRefused = [](std::string_view text, std::string_view reasonFragment) {
        const auto address = parseConnectAddress(text);
        const std::string accepted = "accepted: " + std::string(text);
        CHECK_MSG(!address.isValid, accepted.c_str());
        const std::string wrongReason = "reason for " + std::string(text) + ": " + address.reason;
        CHECK_MSG(address.reason.find(reasonFragment) != std::string::npos, wrongReason.c_str());
    };
    expectRefused("-oProxyCommand=evil", "option");
    expectRefused("user@-oProxyCommand=evil", "option");
    expectRefused("build example", "spaces");
    expectRefused("build.example;reboot", "“;”");
    expectRefused("host$(id)", "“$”");
    expectRefused("build.example:0", "1 to 65535");
    expectRefused("build.example:65536", "1 to 65535");
    expectRefused("build.example:ssh", "1 to 65535");
    expectRefused("", "Enter an address");
    expectRefused("user@", "after “@”");
    expectRefused("[2001:db8::1", "closing “]”");
    expectRefused("/dev/cu usbserial", "spaces");
    expectRefused("ssh /dev/cu.usbserial", "serial port");
}

UBIQUITTY_TEST_MAIN()
