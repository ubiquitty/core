// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_daemon_session_configuration.cpp — a daemon session runs
// under the configuration it was seeded with, not the process-wide instance
// (Platform/Host/Daemon.cpp, createSession).
//
// Two sessions are created in one daemon under two configurations. Each
// child prints the TERM_PROGRAM it was launched with back through its PTY, and
// each Terminal reports the scrollback limit it was sized with, so both facts
// are read from what the session actually did. Against a process-wide read
// both sessions would answer with the same identity and limit, and the
// divergence asserted here cannot hold.
//
// The [security] sequence gate is a third such fact: each session's Terminal
// parses under the lists its own configuration named, so an OSC one tenant
// denies still runs for the other. The OSC 52 clipboard policy is a fourth:
// the write one tenant's configuration denies still lands for the other. The
// `[capabilities.*]` verdicts are a fifth: each session's policy decides by the
// table resolved from the configuration current when it was created.
//

#include "Harness.h"

#include "Core/OSC/Dispatch.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/Themes.h"
#include "PAL/Stream.h"
#include "Platform/Host/CapabilityGrantFile.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace Ubiquitty::Host {

// The seam names the private Session type so the case outside it need not.
struct SessionConfigurationAccess {
    static Daemon::Session* createSession(Daemon& daemon, std::span<const std::string> command)
    {
        return daemon.createSession(80, 24, command);
    }
    static int scrollbackLimitOf(const Daemon::Session& session) { return session.term->scrollbackLimit; }
    static Ubiquitty::Terminal& terminalOf(Daemon::Session& session) { return *session.term; }
    static Ubiquitty::PAL::Stream& streamOf(Daemon::Session& session) { return *session.stream; }
    static Ubiquitty::HostCapabilityPolicy& policyOf(Daemon::Session& session) { return session.capabilityPolicy; }
    static void setSocketPath(Daemon& daemon, const std::string& path) { daemon.m_socketPath = path; }
    static bool persistAlways(Daemon& daemon, Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin origin, const std::string& identity)
    {
        return daemon.editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation::Grant, { capability, origin, identity });
    }
    static void loadAlwaysGrants(Daemon& daemon) { daemon.loadAlwaysAllowedCapabilityGrants(); }
    static bool isGranted(const Daemon& daemon, Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin origin, const std::string& identity)
    {
        return daemon.m_grantStore.isGranted(capability, origin, identity);
    }
    static std::size_t newlyPendingCount(Daemon& daemon) { return daemon.m_consentQueue.takeNewlyPending().size(); }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::SessionConfigurationAccess;

namespace {

// The child echoes the identity it was launched under, then holds the PTY
// open long enough for the read below to land the bytes.
const std::array<std::string, 3> echoTerminalProgram = { "/bin/sh", "-c", "printf '%s' \"$TERM_PROGRAM\"; sleep 2" };

// Reads the child's output until `expected` arrives or the budget runs out.
bool childPrinted(Ubiquitty::PAL::Stream& stream, const std::string& expected, int budgetMilliseconds)
{
    std::string received;
    for (int waited = 0; waited < budgetMilliseconds; waited += 10) {
        std::array<char, 256> buffer {};
        const int count = stream.read(buffer);
        if (count > 0)
            received.append(buffer.data(), static_cast<std::size_t>(count));
        if (received.find(expected) != std::string::npos)
            return true;
        ::usleep(10 * 1000);
    }
    return false;
}

} // namespace

TEST(twoDaemonSessionsHoldDivergentIdentityAndScrollback)
{
    Ubiquitty::Preferences::Values alpha;
    alpha.termProgram = "alpha-host";
    alpha.scrollbackLines = 500;
    Ubiquitty::Preferences::Values beta;
    beta.termProgram = "beta-host";
    beta.scrollbackLines = 2000;

    Daemon daemon;
    daemon.setConfiguration(alpha);
    auto* alphaSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(alphaSession != nullptr);
    daemon.setConfiguration(beta);
    auto* betaSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(betaSession != nullptr);
    if (alphaSession == nullptr || betaSession == nullptr)
        return;

    // Both limits are read while both sessions are alive: the point is that
    // they hold at the same time, in the same daemon.
    CHECK_EQ(SessionConfigurationAccess::scrollbackLimitOf(*alphaSession), 500);
    CHECK_EQ(SessionConfigurationAccess::scrollbackLimitOf(*betaSession), 2000);

    CHECK_MSG(childPrinted(SessionConfigurationAccess::streamOf(*alphaSession), "alpha-host", 5000),
        "the alpha child was launched under alpha's identity");
    CHECK_MSG(childPrinted(SessionConfigurationAccess::streamOf(*betaSession), "beta-host", 5000),
        "the beta child was launched under beta's identity");
}

TEST(twoDaemonSessionsHoldDivergentSequenceGates)
{
    Ubiquitty::Preferences::Values hardened;
    hardened.oscDeny = "2";
    Ubiquitty::Preferences::Values permissive;

    Daemon daemon;
    daemon.setConfiguration(hardened);
    auto* hardenedSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(hardenedSession != nullptr);
    daemon.setConfiguration(permissive);
    auto* permissiveSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(permissiveSession != nullptr);
    if (hardenedSession == nullptr || permissiveSession == nullptr)
        return;

    Ubiquitty::Terminal& hardenedTerminal = SessionConfigurationAccess::terminalOf(*hardenedSession);
    Ubiquitty::Terminal& permissiveTerminal = SessionConfigurationAccess::terminalOf(*permissiveSession);
    CHECK(!Ubiquitty::OSC::oscAllowed(hardenedTerminal, 2));
    CHECK(Ubiquitty::OSC::oscAllowed(permissiveTerminal, 2));

    // The same title sequence through both parsers: only the permissive
    // tenant takes it.
    const std::string_view title = "\033]2;tenant\033\\";
    Ubiquitty::processBytes(&hardenedTerminal, title);
    Ubiquitty::processBytes(&permissiveTerminal, title);
    CHECK(hardenedTerminal.titleUTF8.empty());
    CHECK(permissiveTerminal.titleUTF8 == "tenant");
}

TEST(twoDaemonSessionsHoldDivergentOSC52Policies)
{
    Ubiquitty::Preferences::Values hardened;
    hardened.clipboardWrite = "deny";
    hardened.clipboardRead = "deny";
    Ubiquitty::Preferences::Values permissive;

    Daemon daemon;
    daemon.setConfiguration(hardened);
    auto* hardenedSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(hardenedSession != nullptr);
    daemon.setConfiguration(permissive);
    auto* permissiveSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(permissiveSession != nullptr);
    if (hardenedSession == nullptr || permissiveSession == nullptr)
        return;

    Ubiquitty::Terminal& hardenedTerminal = SessionConfigurationAccess::terminalOf(*hardenedSession);
    Ubiquitty::Terminal& permissiveTerminal = SessionConfigurationAccess::terminalOf(*permissiveSession);
    CHECK(!Ubiquitty::OSC::osc52WriteAllowed(hardenedTerminal));
    CHECK(Ubiquitty::OSC::osc52WriteAllowed(permissiveTerminal));
    CHECK(hardenedTerminal.osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Deny);
    CHECK(permissiveTerminal.osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Prompt);
}

TEST(twoDaemonSessionsHoldDivergentCapabilityDecisions)
{
    Ubiquitty::Preferences::Values restricted;
    restricted.remoteCapabilities.informational = Ubiquitty::CapabilityDecision::Deny;
    Ubiquitty::Preferences::Values defaulted;

    Daemon daemon;
    daemon.setConfiguration(restricted);
    auto* restrictedSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(restrictedSession != nullptr);
    daemon.setConfiguration(defaulted);
    auto* defaultedSession = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(defaultedSession != nullptr);
    if (restrictedSession == nullptr || defaultedSession == nullptr)
        return;

    // The restricted session keeps its table after the daemon's configuration
    // moved on, and the later session takes the default.
    auto& restrictedPolicy = SessionConfigurationAccess::policyOf(*restrictedSession);
    auto& defaultedPolicy = SessionConfigurationAccess::policyOf(*defaultedSession);
    CHECK(restrictedPolicy.check(Ubiquitty::Capability::SetHyperlink, Ubiquitty::CapabilityOrigin::Remote, "") == Ubiquitty::CapabilityDecision::Deny);
    CHECK(defaultedPolicy.check(Ubiquitty::Capability::SetHyperlink, Ubiquitty::CapabilityOrigin::Remote, "") == Ubiquitty::CapabilityDecision::Allow);

    // And it is the policy the session's Terminal consults: a remote OSC 8 arms
    // a link only in the session whose table allows it.
    Ubiquitty::Terminal& restrictedTerminal = SessionConfigurationAccess::terminalOf(*restrictedSession);
    Ubiquitty::Terminal& defaultedTerminal = SessionConfigurationAccess::terminalOf(*defaultedSession);
    restrictedTerminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;
    defaultedTerminal.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;
    const std::string_view link = "\033]8;;https://example.test\033\\";
    Ubiquitty::processBytes(&restrictedTerminal, link);
    Ubiquitty::processBytes(&defaultedTerminal, link);
    CHECK(restrictedTerminal.currentLinkID == 0);
    CHECK(defaultedTerminal.currentLinkID != 0);
}

// A configured `deny` outranks every grant. The grant here is the strongest kind
// there is — Always, token-bound, persisted to disk under a laxer policy and
// loaded by the next daemon — and the owner's written restriction still wins:
// the check is Deny and nobody is asked. The control is the same persisted
// grant under `prompt`, where it answers Allow, so the grant is shown to be
// loaded and live rather than merely absent.
TEST(aConfiguredDenyOutranksAPersistedAlwaysGrant)
{
    char directoryTemplate[] = "/tmp/ubiquitty-hard-deny.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    CHECK(directory != nullptr);
    if (directory == nullptr)
        return;
    const std::string socketPath = std::string(directory) + "/host.sock";
    const std::string identity = "token:owner-phone";
    constexpr auto capability = Ubiquitty::Capability::SetHyperlink;
    constexpr auto origin = Ubiquitty::CapabilityOrigin::Remote;

    {
        Daemon granting;
        SessionConfigurationAccess::setSocketPath(granting, socketPath);
        CHECK(SessionConfigurationAccess::persistAlways(granting, capability, origin, identity));
    }

    const auto checkUnder = [&](Ubiquitty::CapabilityDecision configured) {
        Ubiquitty::Preferences::Values configuration;
        configuration.remoteCapabilities.informational = configured;
        Daemon daemon;
        SessionConfigurationAccess::setSocketPath(daemon, socketPath);
        SessionConfigurationAccess::loadAlwaysGrants(daemon);
        CHECK(SessionConfigurationAccess::isGranted(daemon, capability, origin, identity));
        daemon.setConfiguration(configuration);
        auto* session = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
        CHECK(session != nullptr);
        if (session == nullptr)
            return std::pair { Ubiquitty::CapabilityDecision::Allow, std::size_t { 0 } };
        auto& policy = SessionConfigurationAccess::policyOf(*session);
        policy.setCurrentIdentity(identity);
        const Ubiquitty::CapabilityDecision decision = policy.check(capability, origin, "");
        return std::pair { decision, SessionConfigurationAccess::newlyPendingCount(daemon) };
    };

    const auto [denied, deniedAsked] = checkUnder(Ubiquitty::CapabilityDecision::Deny);
    CHECK(denied == Ubiquitty::CapabilityDecision::Deny);
    CHECK_EQ(deniedAsked, std::size_t { 0 });

    const auto [prompted, promptedAsked] = checkUnder(Ubiquitty::CapabilityDecision::Prompt);
    CHECK(prompted == Ubiquitty::CapabilityDecision::Allow);
    CHECK_EQ(promptedAsked, std::size_t { 0 });

    ::unlink((std::string(directory) + "/capability_grants").c_str());
    ::unlink((std::string(directory) + "/capability_grants.lock").c_str());
    ::rmdir(directory);
}

TEST(daemonSessionSeedsConfiguredThemeAndPreservesExistingPalette)
{
    Ubiquitty::Preferences::Values configuration;
    Ubiquitty::Preferences::ThemeEntry theme;
    theme.name = "Host session theme";
    theme.background = { 234, 225, 216, 255, true };
    theme.foreground = { 45, 56, 67, 255, true };
    configuration.themes.push_back(theme);
    configuration.themeName = theme.name;
    Daemon daemon;
    daemon.setConfiguration(configuration);
    auto* first = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(first != nullptr);
    if (!first)
        return;
    auto& terminal = SessionConfigurationAccess::terminalOf(*first);
    CHECK_EQ(Ubiquitty::channel16To8(terminal.palette.defaultBackground.red), 234);
    CHECK_EQ(Ubiquitty::channel16To8(terminal.palette.defaultForeground.green), 56);
    CHECK_EQ(Ubiquitty::channel16To8(terminal.paletteBaseline.defaultBackground.red), 234);
    Ubiquitty::processBytes(&terminal, "\033]11;#112233\007");
    CHECK_EQ(Ubiquitty::channel16To8(terminal.palette.defaultBackground.red), 17);

    configuration.themes.clear();
    configuration.themeName = Ubiquitty::Themes::get(0)->name;
    daemon.setConfiguration(configuration);
    auto* second = SessionConfigurationAccess::createSession(daemon, echoTerminalProgram);
    CHECK(second != nullptr);
    if (second) {
        const auto& secondTerminal = SessionConfigurationAccess::terminalOf(*second);
        CHECK_EQ(Ubiquitty::channel16To8(secondTerminal.palette.defaultBackground.red), Ubiquitty::Themes::get(0)->defaultBackground.red);
    }
    CHECK_EQ(Ubiquitty::channel16To8(terminal.palette.defaultBackground.red), 17);
    Ubiquitty::processBytes(&terminal, "\033]111\007");
    CHECK_EQ(Ubiquitty::channel16To8(terminal.palette.defaultBackground.red), 234);
}

UBIQUITTY_TEST_MAIN()
