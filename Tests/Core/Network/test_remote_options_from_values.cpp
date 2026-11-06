// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_remote_options_from_values — the [network] block as an ssh/mosh launch
// reads it.
//
// The interesting half is agent forwarding, which is three-valued: a connect
// window with a forward-agent control speaks for the launch either way, while a
// menu or a URL says nothing and the configured default stands. A two-valued
// parameter cannot carry the third case, so the cases below check each of the
// three against each setting of the config — a resolver that ignored the
// caller, and one that ignored the config, each pass half of that grid.
//
// The proxy half is a name-to-enum mapping and nothing else: the configured
// proxy is carried across as written, and whether it is usable is answered by
// `enabled()` and `isUsableProxy`, which the argv builders already consult. Two
// places answering that can answer it differently, so the cases pin that an
// unusable proxy survives resolution and dies at the gate rather than being
// refused earlier.

#include "Harness.h"

#include "Core/Network/RemoteOptionsFromValues.h"
#include "Core/Preferences/Preferences.h"

#include <algorithm>
#include <string>

namespace {

namespace Net = Ubiquitty::Network;
namespace Cfg = Ubiquitty::Preferences;

Cfg::Values withProxy(const char* kind, const char* host, int port)
{
    Cfg::Values config;
    config.proxyKind = kind;
    config.proxyHost = host;
    config.proxyPort = port;
    return config;
}

} // namespace

TEST(testEveryProxyNameMapsAndAnythingElseIsNone)
{
    CHECK(Net::proxyKindFromName("socks5") == Net::ProxyKind::SOCKS5);
    CHECK(Net::proxyKindFromName("socks4") == Net::ProxyKind::SOCKS4);
    CHECK(Net::proxyKindFromName("http") == Net::ProxyKind::HTTP);

    // The empty name is the one a config that sets no proxy leaves behind, and
    // "none" is the spelling the config comment offers for turning one off.
    CHECK(Net::proxyKindFromName("") == Net::ProxyKind::None);
    CHECK(Net::proxyKindFromName("none") == Net::ProxyKind::None);
    CHECK(Net::proxyKindFromName("SOCKS5") == Net::ProxyKind::None); // the config value is lowercase
    CHECK(Net::proxyKindFromName("socks") == Net::ProxyKind::None);
}

// The whole grid: three callers against both configured defaults. Checking one
// column would pass for a resolver that read only the config, and checking one
// row would pass for one that read only the caller.
TEST(testAgentForwardingIsThreeValuedAgainstBothDefaults)
{
    Cfg::Values forwards;
    forwards.sshForwardAgent = true;
    Cfg::Values withholds;
    withholds.sshForwardAgent = false;
    CHECK(Net::remoteOptionsFromValues(forwards, Net::AgentForwarding::FromConfig).forwardAgent);
    CHECK(!Net::remoteOptionsFromValues(withholds, Net::AgentForwarding::FromConfig).forwardAgent);

    CHECK(Net::remoteOptionsFromValues(forwards, Net::AgentForwarding::Forward).forwardAgent);
    CHECK(Net::remoteOptionsFromValues(withholds, Net::AgentForwarding::Forward).forwardAgent);

    CHECK(!Net::remoteOptionsFromValues(forwards, Net::AgentForwarding::Withhold).forwardAgent);
    CHECK(!Net::remoteOptionsFromValues(withholds, Net::AgentForwarding::Withhold).forwardAgent);
}

// The default argument is what a caller with nothing to say gets, and it must be
// the config's answer rather than a fixed one.
TEST(testTheDefaultCallerDefersToConfig)
{
    Cfg::Values forwards;
    forwards.sshForwardAgent = true;
    CHECK(Net::remoteOptionsFromValues(forwards).forwardAgent);
}

TEST(testAConfiguredProxyArrivesUsable)
{
    const Net::RemoteOptions options = Net::remoteOptionsFromValues(withProxy("socks5", "proxy.example", 1080));

    CHECK(options.proxy.kind == Net::ProxyKind::SOCKS5);
    CHECK(options.proxy.host == "proxy.example");
    CHECK(options.proxy.port == 1080);
    CHECK(options.proxy.enabled());
    CHECK(Net::isUsableProxy(options.proxy));
}

// A port past the range, a missing host, an unnamed kind: each resolves without
// complaint and each fails the gate the builders consult. Pinned because the
// alternative — refusing them during resolution — is a second screen that can
// drift out of step with the first.
TEST(testAnUnusableProxyResolvesAndThenFailsTheGate)
{
    const Net::RemoteOptions outOfRange = Net::remoteOptionsFromValues(withProxy("socks5", "proxy.example", 99999));
    CHECK(outOfRange.proxy.port == 99999);
    CHECK(!outOfRange.proxy.enabled());

    const Net::RemoteOptions noHost = Net::remoteOptionsFromValues(withProxy("http", "", 3128));
    CHECK(noHost.proxy.kind == Net::ProxyKind::HTTP);
    CHECK(!noHost.proxy.enabled());

    const Net::RemoteOptions noKind = Net::remoteOptionsFromValues(withProxy("", "proxy.example", 1080));
    CHECK(!noKind.proxy.enabled());

    // A host that would smuggle an ssh option passes enabled() and is stopped by
    // the validating gate instead, which is the one the builders call.
    const Net::RemoteOptions injected = Net::remoteOptionsFromValues(withProxy("socks5", "-oProxyCommand=id", 1080));
    CHECK(injected.proxy.enabled());
    CHECK(!Net::isUsableProxy(injected.proxy));
}

TEST(testAConfigWithNoNetworkBlockAsksForNothing)
{
    const Net::RemoteOptions options = Net::remoteOptionsFromValues(Cfg::Values {});

    CHECK(!options.forwardAgent);
    CHECK(options.proxy.kind == Net::ProxyKind::None);
    CHECK(!options.proxy.enabled());
}

// A shell launch reads the configuration and the launch's own word on the
// agent together, and refuses a destination ssh would read as an option.
TEST(testAShellLaunchCarriesTheOptionsAndRefusesAnOption)
{
    namespace Net = Ubiquitty::Network;
    Ubiquitty::Preferences::Values config;
    const auto forwarded = Net::buildShellLaunchArguments(config, Net::ShellTool::SSH, "/usr/bin/ssh", "user@build.example", Net::AgentForwarding::Forward);
    REQUIRE(!forwarded.empty());
    CHECK_STR_EQ(forwarded.front(), "/usr/bin/ssh");
    CHECK_STR_EQ(forwarded.back(), "user@build.example");
    CHECK(std::find(forwarded.begin(), forwarded.end(), std::string("-A")) != forwarded.end());
    const auto withheld = Net::buildShellLaunchArguments(config, Net::ShellTool::SSH, "/usr/bin/ssh", "user@build.example", Net::AgentForwarding::Withhold);
    CHECK(std::find(withheld.begin(), withheld.end(), std::string("-A")) == withheld.end());
    const auto mosh = Net::buildShellLaunchArguments(config, Net::ShellTool::Mosh, "/opt/homebrew/bin/mosh", "build.example", Net::AgentForwarding::Withhold);
    REQUIRE(!mosh.empty());
    CHECK_STR_EQ(mosh.front(), "/opt/homebrew/bin/mosh");
    CHECK(Net::buildShellLaunchArguments(config, Net::ShellTool::SSH, "/usr/bin/ssh", "-oProxyCommand=evil", Net::AgentForwarding::Withhold).empty());
}

// A profile's startup_host launches under the user's own [network] options,
// and the value itself names a host and nothing that could run a program: a
// profile is shareable, so an option carried in it would run on whichever
// machine opened the profile.
TEST(testAStartupHostLaunchesUnderTheConfigAndRefusesAProgram)
{
    Cfg::Values config;
    config.sshForwardAgent = true;
    const Net::StartupHostLaunch launch = Net::buildStartupHostLaunch(config, "/usr/bin/ssh", "/opt/bin/mosh", "prod.example");
    REQUIRE(launch.accepted());
    CHECK_STR_EQ(launch.arguments.front(), "/usr/bin/ssh");
    CHECK_STR_EQ(launch.arguments.back(), "prod.example");
    CHECK_MSG(std::find(launch.arguments.begin(), launch.arguments.end(), std::string("-A")) != launch.arguments.end(),
        "the configured agent forwarding reaches the startup host's launch");

    config.sshForwardAgent = false;
    const Net::StartupHostLaunch withheld = Net::buildStartupHostLaunch(config, "/usr/bin/ssh", "/opt/bin/mosh", "prod.example");
    CHECK(std::find(withheld.arguments.begin(), withheld.arguments.end(), std::string("-A")) == withheld.arguments.end());

    for (const char* hostile : { "-oProxyCommand=touch /tmp/owned", "ssh -oProxyCommand=evil prod.example", "/bin/sh -c id", "prod.example; id" }) {
        const Net::StartupHostLaunch refused = Net::buildStartupHostLaunch(config, "/usr/bin/ssh", "/opt/bin/mosh", hostile);
        CHECK_MSG(!refused.accepted(), hostile);
        CHECK_MSG(refused.arguments.empty(), "a refusal launches nothing, so the local shell opens instead");
    }
}

UBIQUITTY_TEST_MAIN()
