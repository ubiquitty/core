// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_remote_terminfo_install.cpp — pins the remote terminfo-install command
// (Network/RemoteTerminfoInstall.h) the `ubiquitty ssh`/`mosh` wrapper runs on
// the remote. This is a remote-execution surface, so the test pins the security
// contract: TERMINFO-only (only tic runs, over our source), IDEMPOTENT (skips
// when the entry already resolves), FAIL-OPEN (the login shell always execs),
// and ARGV/SHELL-SAFE (host floored by validateSSHHost; source single-quoted).

#include "Core/Network/RemoteTerminfoInstall.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;

static bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

TEST(remoteTerminfoInstall)
{

    // A representative terminfo source: multi-line, backslash escapes, %, and no
    // single quote (as the real xterm-ubiquitty source that carries the clipboard cap).
    const std::string_view source = "xterm-ubiquitty|Ubiquitty,\n\tam, Tc,\n\tsetab=\\E[48;5;%p1%dm,\n\tsgr0=\\E(B\\E[m,\n";

    const std::string command = Network::buildRemoteTerminfoInstallCommand(source);
    const std::string_view execTail = "exec \"${SHELL:-/bin/sh}\" -l";

    // (1) IDEMPOTENT + tic-guarded: gated on `! infocmp xterm-ubiquitty` (skip an
    //     already-resolvable entry) and `command -v tic` (skip a tic-less remote).
    CHECK(contains(command, "command -v tic >/dev/null 2>&1"));
    CHECK(contains(command, "! infocmp xterm-ubiquitty >/dev/null 2>&1"));

    // (2) TERMINFO-only: the only compiler invoked is `tic` over our source via
    //     stdin (-x keeps extension caps; the db is the user's own ~/.terminfo),
    //     and the source is passed literally through a single-quoted printf — no
    //     base64, no eval, no fetch, no rc-file mutation: shell startup files are
    //     written only by the local install, behind its own question.
    CHECK(contains(command, "tic -x -o \"$HOME/.terminfo\" -"));
    CHECK(contains(command, "printf %s '"));
    CHECK(contains(command, source)); // the terminfo source embedded verbatim
    CHECK(!contains(command, "eval"));
    CHECK(!contains(command, "curl"));
    CHECK(!contains(command, "wget"));
    CHECK(!contains(command, ".bashrc"));
    CHECK(!contains(command, ".zshrc"));
    CHECK(!contains(command, ".profile"));
    CHECK(!contains(command, "claude"));

    // (3) FAIL-OPEN: the login shell exec is UNCONDITIONAL — it sits after `fi`,
    //     outside the install guard, and is the LAST thing in the command, so a
    //     missing or failing tic still logs the user in. Diagnostics suppressed.
    CHECK(contains(command, std::string("; fi; ") + std::string(execTail)));
    CHECK(command.size() >= execTail.size() && command.compare(command.size() - execTail.size(), execTail.size(), execTail) == 0);
    CHECK(contains(command, ">/dev/null 2>&1; fi"));

    // (4) SHELL-SAFE source quoting: a source that did contain a single quote is
    //     escaped by shellSingleQuote's POSIX '\'' idiom, staying one shell word.
    {
        const std::string quoted = Network::buildRemoteTerminfoInstallCommand("name,\n\tx='y',\n");
        CHECK(contains(quoted, "'\\''"));
    }

    // (5) ARGV-SAFE ssh wiring: the wrapper argv is the ssh argv (host floored by
    //     validateSSHHost) with the install command appended as the LAST element
    //     (the ssh remote command). A well-formed host builds; an argv-injection
    //     host or an empty ssh path builds nothing (caller falls back to plain ssh).
    {
        const std::vector<std::string> argv = Network::buildSSHArgumentsWithTerminfoInstall("/usr/bin/ssh", "user@host.example", source);
        CHECK(!argv.empty());
        CHECK(argv.front() == "/usr/bin/ssh");
        CHECK(argv.back() == Transport::remoteShellCommand(command));
        CHECK(argv.back().starts_with("exec /bin/sh -c '"));
        bool haveForceTty = false, haveHost = false;
        for (const std::string& token : argv) {
            if (token == "-t")
                haveForceTty = true;
            if (token == "user@host.example")
                haveHost = true;
        }
        CHECK(haveForceTty); // -t forces the remote pty for the exec'd login shell
        CHECK(haveHost);
    }
    // (6) The remote's own options come first and the install stays last:
    //     agent forwarding and a SOCKS proxy sit between the program and the
    //     forced tty, so the command ssh runs is still the final argument.
    {
        Network::RemoteOptions options;
        options.forwardAgent = true;
        options.proxy = { Network::ProxyKind::SOCKS5, "proxy.example", 1080 };
        const std::vector<std::string> argv = Network::buildSSHArgumentsWithTerminfoInstall("/usr/bin/ssh", "user@host.example", source, options);
        REQUIRE(argv.size() == 7);
        CHECK(argv[0] == "/usr/bin/ssh");
        CHECK(argv[1] == "-A");
        CHECK(argv[2] == "-o" && argv[3] == "ProxyCommand=nc -X 5 -x proxy.example:1080 %h %p");
        CHECK(argv[4] == "-t");
        CHECK(argv[5] == "user@host.example");
        CHECK(argv[6] == Transport::remoteShellCommand(command));
    }
    {
        CHECK(Network::buildSSHArgumentsWithTerminfoInstall("/usr/bin/ssh", "-oProxyCommand=touch /tmp/pwned", source).empty());
        CHECK(Network::buildSSHArgumentsWithTerminfoInstall("", "user@host", source).empty());
    }
}

UBIQUITTY_TEST_MAIN()
