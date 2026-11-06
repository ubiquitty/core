// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_remotelaunch.cpp — the ssh/mosh argv assembly (Core/Network/
// RemoteLaunch.h): the proxy command a validated config expands to, the agent
// and proxy arms of both builders, and the startup_host parser whose whole job
// is refusing anything that is not one host. The argv is what execs, so every
// case asserts the exact vector, not a substring of it.

#include "Core/Network/RemoteLaunch.h"

#include <string>
#include <vector>

using namespace Ubiquitty::Network;

namespace {

ProxyConfig socks5Proxy()
{
    ProxyConfig proxy;
    proxy.kind = ProxyKind::SOCKS5;
    proxy.host = "proxy.example";
    proxy.port = 1080;
    return proxy;
}

} // namespace

TEST(testProxyCommandPerKind)
{
    // Each kind maps to nc's -X value; the command is one ssh -o argument the
    // shell never re-splits, so the whole string is the assertion.
    ProxyConfig proxy = socks5Proxy();
    CHECK(detail::proxyCommandFor(proxy) == "nc -X 5 -x proxy.example:1080 %h %p");
    proxy.kind = ProxyKind::SOCKS4;
    CHECK(detail::proxyCommandFor(proxy) == "nc -X 4 -x proxy.example:1080 %h %p");
    proxy.kind = ProxyKind::HTTP;
    CHECK(detail::proxyCommandFor(proxy) == "nc -X connect -x proxy.example:1080 %h %p");

    // A proxy is usable only when enabled and only when its host passes the same
    // injection guard as the destination.
    CHECK(isUsableProxy(socks5Proxy()));
    ProxyConfig disabled;
    CHECK(!isUsableProxy(disabled));
    ProxyConfig badPort = socks5Proxy();
    badPort.port = 0;
    CHECK(!isUsableProxy(badPort));
    badPort.port = 65536;
    CHECK(!isUsableProxy(badPort));
    ProxyConfig hostileHost = socks5Proxy();
    hostileHost.host = "-oProxyCommand=evil";
    CHECK(!isUsableProxy(hostileHost));
}

TEST(testSSHArgumentAssembly)
{
    using Arguments = std::vector<std::string>;

    // The bare launch: path, forced pty, host.
    CHECK(buildSSHArguments("/usr/bin/ssh", "user@box") == (Arguments { "/usr/bin/ssh", "-t", "user@box" }));

    // Agent forwarding and a usable proxy each add their argument in place.
    RemoteOptions options;
    options.forwardAgent = true;
    CHECK(buildSSHArguments("/usr/bin/ssh", "box", options) == (Arguments { "/usr/bin/ssh", "-A", "-t", "box" }));
    options.proxy = socks5Proxy();
    CHECK(buildSSHArguments("/usr/bin/ssh", "box", options)
        == (Arguments { "/usr/bin/ssh", "-A", "-o", "ProxyCommand=nc -X 5 -x proxy.example:1080 %h %p", "-t", "box" }));

    // No path or an unsafe host builds nothing.
    CHECK(buildSSHArguments("", "box").empty());
    CHECK(buildSSHArguments("/usr/bin/ssh", "-oProxyCommand=evil").empty());
}

TEST(testMoshArgumentAssembly)
{
    using Arguments = std::vector<std::string>;

    // Plain mosh: no --ssh override when nothing differs from its default.
    CHECK(buildMoshArguments("/opt/bin/mosh", "user@box") == (Arguments { "/opt/bin/mosh", "user@box" }));

    // The agent and proxy ride mosh's ssh bootstrap, proxy quoted so mosh's
    // space-tokenised --ssh keeps the -o value as one argument.
    RemoteOptions options;
    options.forwardAgent = true;
    CHECK(buildMoshArguments("/opt/bin/mosh", "box", options) == (Arguments { "/opt/bin/mosh", "--ssh=ssh -A", "box" }));
    options.forwardAgent = false;
    options.proxy = socks5Proxy();
    CHECK(buildMoshArguments("/opt/bin/mosh", "box", options)
        == (Arguments { "/opt/bin/mosh", "--ssh=ssh -o \"ProxyCommand=nc -X 5 -x proxy.example:1080 %h %p\"", "box" }));

    CHECK(buildMoshArguments("", "box").empty());
    CHECK(buildMoshArguments("/opt/bin/mosh", "bad host").empty());
}

TEST(testStartupHostParsing)
{
    // A bare host takes ssh; the documented prefixed forms pick their program.
    StartupHostLaunch launch = buildStartupHostArguments("/usr/bin/ssh", "/opt/bin/mosh", "prod.example");
    CHECK(launch.accepted());
    CHECK(launch.arguments[0] == "/usr/bin/ssh");

    launch = buildStartupHostArguments("/usr/bin/ssh", "/opt/bin/mosh", "  ssh user@prod.example  ");
    CHECK(launch.accepted());
    CHECK(launch.arguments.back() == "user@prod.example");

    launch = buildStartupHostArguments("/usr/bin/ssh", "/opt/bin/mosh", "mosh prod.example");
    CHECK(launch.accepted());
    CHECK(launch.arguments[0] == "/opt/bin/mosh");

    // Every refusal names itself, and a refused launch builds nothing.
    struct Refusal {
        const char* startupHost;
        StartupHostRejection rejection;
    };
    static constexpr Refusal refusals[] = {
        { "", StartupHostRejection::Empty },
        { "   \t ", StartupHostRejection::Empty },
        { "telnet prod.example", StartupHostRejection::UnknownProgram },
        { "ssh prod.example -A", StartupHostRejection::ExtraArguments },
        { "ssh -oProxyCommand=evil", StartupHostRejection::InvalidHost },
        { "-oProxyCommand=evil", StartupHostRejection::InvalidHost },
    };
    for (const Refusal& refusal : refusals) {
        const StartupHostLaunch refused = buildStartupHostArguments("/usr/bin/ssh", "/opt/bin/mosh", refusal.startupHost);
        CHECK_MSG(!refused.accepted(), refusal.startupHost);
        CHECK_MSG(refused.rejection == refusal.rejection, refusal.startupHost);
        CHECK_MSG(refused.arguments.empty(), refusal.startupHost);
    }

    // A host whose program cannot be resolved is refused as missing, per side.
    launch = buildStartupHostArguments("", "/opt/bin/mosh", "prod.example");
    CHECK(launch.rejection == StartupHostRejection::MissingBinary);
    launch = buildStartupHostArguments("/usr/bin/ssh", "", "mosh prod.example");
    CHECK(launch.rejection == StartupHostRejection::MissingBinary);

    // Every rejection reason is a distinct non-empty sentence.
    static constexpr StartupHostRejection rejections[] = { StartupHostRejection::None, StartupHostRejection::Empty,
        StartupHostRejection::UnknownProgram, StartupHostRejection::ExtraArguments, StartupHostRejection::InvalidHost,
        StartupHostRejection::MissingBinary };
    for (std::size_t i = 0; i < sizeof(rejections) / sizeof(rejections[0]); ++i) {
        const char* a = startupHostRejectionReason(rejections[i]);
        CHECK(a != nullptr && a[0] != '\0');
        for (std::size_t j = i + 1; j < sizeof(rejections) / sizeof(rejections[0]); ++j)
            CHECK(std::string(a) != startupHostRejectionReason(rejections[j]));
    }
}

UBIQUITTY_TEST_MAIN()
