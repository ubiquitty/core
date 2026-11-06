// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_injection_gate — every door a non-human principal drives a session
// through gets the same verdict and the same audit line from the same rules.

#include "Harness.h"

#include "Core/Security/InjectionGate.h"
#include "PAL/Capability.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using Ubiquitty::Security::AgentPolicy;
using Ubiquitty::Security::ApprovalMode;
using Ubiquitty::Security::AuditOnAllow;
using Ubiquitty::Security::evaluateAction;
using Ubiquitty::Security::evaluateInjection;
using Ubiquitty::Security::evaluateScreenRead;
using Ubiquitty::Security::InjectionVerdict;
using Ubiquitty::Security::isHeldForShellIndirection;
using Ubiquitty::Security::screenedLine;
using Ubiquitty::Security::screenReadRuleToken;
using Ubiquitty::Security::shellIndirectionHold;

/// A scratch file the audit log is pointed at for one case, and read back.
class ScratchAuditLog {
public:
    ScratchAuditLog()
    {
        char path[] = "/tmp/ubiquitty-injection-gate-audit-XXXXXX";
        const int fileDescriptor = ::mkstemp(path);
        if (fileDescriptor >= 0)
            ::close(fileDescriptor);
        m_path = path;
        ::setenv("UBIQUITTY_AGENT_AUDIT", m_path.c_str(), 1);
    }

    ~ScratchAuditLog()
    {
        ::unsetenv("UBIQUITTY_AGENT_AUDIT");
        ::unlink(m_path.c_str());
    }

    [[nodiscard]] std::vector<std::string> lines() const
    {
        std::vector<std::string> out;
        std::ifstream in(m_path);
        for (std::string line; std::getline(in, line);)
            out.push_back(line);
        return out;
    }

private:
    std::string m_path;
};

/// A policy read from rule lines, the way both doors read theirs.
AgentPolicy policyFrom(const std::string& rules)
{
    char path[] = "/tmp/ubiquitty-injection-gate-policy-XXXXXX";
    const int fileDescriptor = ::mkstemp(path);
    AgentPolicy policy;
    if (fileDescriptor < 0)
        return policy;
    std::FILE* file = ::fdopen(fileDescriptor, "w");
    if (!file) {
        ::close(fileDescriptor);
        ::unlink(path);
        return policy;
    }
    std::fputs(rules.c_str(), file);
    std::fclose(file);
    policy.loadFromFile(path);
    ::unlink(path);
    return policy;
}

/// The audit line with its timestamp removed: `session=<id> <kind> <decision>: <detail>`.
std::string withoutStamp(const std::string& line)
{
    const std::size_t space = line.find(' ');
    return space == std::string::npos ? line : line.substr(space + 1);
}

} // namespace

// ---- the two doors agree ------------------------------------------------

TEST(testTheControlSocketAndTheAgentGetOneVerdictFromOneRuleFile)
{
    const AgentPolicy policy = policyFrom("default confirm\nallow echo\ndeny rm -rf\n");
    const std::string allowed = "echo hi";
    const std::string denied = "rm -rf /";
    const std::string unruled = "curl example.test";

    ScratchAuditLog audit;
    CHECK_MSG(evaluateInjection(policy, 7, "send", allowed, ApprovalMode::RequireConfirmation) == InjectionVerdict::Allowed, "the agent's send is allowed by the rule");
    CHECK_MSG(evaluateInjection(policy, 7, "control-send", allowed, ApprovalMode::RequireConfirmation) == InjectionVerdict::Allowed, "the control socket's send is allowed by the same rule");
    CHECK_MSG(evaluateInjection(policy, 7, "send", denied, ApprovalMode::RequireConfirmation) == InjectionVerdict::DeniedByRule, "the agent's send is denied by the rule");
    CHECK_MSG(evaluateInjection(policy, 7, "control-send", denied, ApprovalMode::RequireConfirmation) == InjectionVerdict::DeniedByRule, "the control socket's send is denied by the same rule");
    CHECK_MSG(evaluateInjection(policy, 7, "send", unruled, ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "the agent's unruled send needs approval");
    CHECK_MSG(evaluateInjection(policy, 7, "control-send", unruled, ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "the control socket's unruled send needs approval");

    const std::vector<std::string> lines = audit.lines();
    CHECK_MSG(lines.size() == 6, "every evaluation, allowed or not, is one audit line");
    if (lines.size() == 6) {
        CHECK_MSG(withoutStamp(lines[0]) == "session=7 send allow: echo hi", "the agent's allowance is recorded");
        CHECK_MSG(withoutStamp(lines[1]) == "session=7 control-send allow: echo hi", "the control socket's allowance is recorded in the same words, under its own door");
        CHECK_MSG(withoutStamp(lines[2]) == "session=7 send deny: rm -rf /", "the agent's refusal is recorded");
        CHECK_MSG(withoutStamp(lines[3]) == "session=7 control-send deny: rm -rf /", "the control socket's refusal is recorded");
        CHECK_MSG(withoutStamp(lines[4]) == "session=7 send confirm: curl example.test", "the agent's unapproved action is recorded");
        CHECK_MSG(withoutStamp(lines[5]) == "session=7 control-send confirm: curl example.test", "the control socket's unapproved action is recorded");
    }
}

TEST(testNoRuleFileMeansNothingRunsUnattended)
{
    const AgentPolicy policy;
    ScratchAuditLog audit;
    CHECK_MSG(evaluateInjection(policy, 1, "control-send", "ls", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "the built-in default is confirm, and a door with no human on the line refuses");
    CHECK_MSG(evaluateInjection(policy, 1, "control-send", "ls", ApprovalMode::AutoApprove) == InjectionVerdict::Allowed, "a door that can approve turns confirm into allow");
    const std::vector<std::string> lines = audit.lines();
    CHECK_MSG(lines.size() == 2 && withoutStamp(lines[1]) == "session=1 control-send confirm: [approved] ls", "an approved confirm says so in the log");
}

TEST(testInjectedTextCarryingShellIndirectionIsNeverAllowedOutright)
{
    const AgentPolicy policy = policyFrom("default deny\nallow echo\n");
    ScratchAuditLog audit;
    CHECK_MSG(evaluateInjection(policy, 1, "control-send", "echo hi; rm -rf /", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "an allow vets only the tokens it matched; a chained command needs a human");
    CHECK_MSG(evaluateInjection(policy, 1, "control-send", "echo hi\n", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "a newline runs the line, so it counts as indirection");
}

TEST(testAHoldForIndirectionIsToldApartFromAMissingRule)
{
    // Under `default allow` a pipe or a chain still needs a person, and the
    // refusal must say why rather than ask for the `default allow` already set.
    const AgentPolicy permissive = policyFrom("default allow\n");
    ScratchAuditLog audit;
    CHECK(evaluateInjection(permissive, 1, "send", "ls | wc -l", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval);
    CHECK_MSG(isHeldForShellIndirection(permissive, "ls | wc -l"), "a pipe under default allow is held for its indirection");
    CHECK_MSG(isHeldForShellIndirection(permissive, "make && make test"), "a chain is too");
    CHECK_MSG(!isHeldForShellIndirection(permissive, "ls -l"), "a plain line is not held at all");
    const AgentPolicy cautious = policyFrom("default confirm\nallow echo\n");
    CHECK_MSG(isHeldForShellIndirection(cautious, "echo hi; ls"), "an allowed first word chained to more is held for the chain");
    CHECK_MSG(!isHeldForShellIndirection(cautious, "ls | wc -l"), "a line no rule allows is held for the missing rule, which a rule can lift");
    CHECK(shellIndirectionHold.find("default allow") != std::string_view::npos);
}

TEST(testATypedLineIsScreenedAsTheCommandItRuns)
{
    CHECK_MSG(screenedLine("echo hi\n") == "echo hi", "one newline is the terminator that runs the line");
    CHECK_MSG(screenedLine("echo hi\r") == "echo hi", "a carriage return runs it the same way");
    CHECK_MSG(screenedLine("echo hi\r\n") == "echo hi", "a CR LF pair is one terminator");
    CHECK_MSG(screenedLine("echo hi\n\n") == "echo hi\n", "a second terminator stays, and is indirection");
    CHECK_MSG(screenedLine("echo hi") == "echo hi", "an unterminated line is itself");
    CHECK_MSG(screenedLine("") == "", "empty stays empty");

    const AgentPolicy policy = policyFrom("default deny\nallow echo\n");
    ScratchAuditLog audit;
    CHECK_MSG(evaluateInjection(policy, 1, "control-send", screenedLine("echo hi\n"), ApprovalMode::RequireConfirmation) == InjectionVerdict::Allowed, "the socket's typed line and the agent's run verb get one verdict for one command");
    CHECK_MSG(evaluateInjection(policy, 1, "control-send", screenedLine("echo hi\nrm -rf /\n"), ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "a second line is not typed unattended");
}

TEST(testAControlByteInsideTheTextIsAGapAndIndirection)
{
    const AgentPolicy allowing = policyFrom("default confirm\nallow echo\n");
    ScratchAuditLog audit;
    CHECK_MSG(evaluateInjection(allowing, 1, "control-send", "echo hi\rrm -rf /", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "a carriage return submits the first line and starts a second; the allow on echo does not cover it");
    CHECK_MSG(evaluateInjection(allowing, 1, "control-send", "echo hi\x15rm", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "^U discards the matched words and leaves the rest");
    CHECK_MSG(evaluateInjection(allowing, 1, "control-send", "echo hi\x1b[Arm", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "an escape sequence is not text the matcher can read");
    CHECK_MSG(evaluateInjection(allowing, 1, "control-send", "echo hi\x7f", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "delete edits the line");
    CHECK_MSG(evaluateInjection(allowing, 1, "control-send", "echo\thi", ApprovalMode::RequireConfirmation) == InjectionVerdict::Allowed, "a tab is the shell's own separator");

    const AgentPolicy denying = policyFrom("default allow\ndeny rm -rf\n");
    CHECK_MSG(evaluateInjection(denying, 1, "send", "echo hi\rrm -rf /", ApprovalMode::AutoApprove) == InjectionVerdict::DeniedByRule, "the deny rule sees the word after the carriage return");
    CHECK_MSG(evaluateInjection(denying, 1, "send", "echo hi\x15rm -rf /", ApprovalMode::AutoApprove) == InjectionVerdict::DeniedByRule, "and after ^U");
}

// ---- the screen read is a capability, not a courtesy --------------------

TEST(testAScreenReadIsReadExfiltrationAndNeedsARule)
{
    static_assert(Ubiquitty::tierOf(Ubiquitty::Capability::ScreenContentRead) == Ubiquitty::CapabilityTier::ReadExfil);

    const AgentPolicy policy;
    ScratchAuditLog audit;
    CHECK_MSG(evaluateScreenRead(policy, 3, "control-screen", "window=12 pane=4", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "with no rule a screen read is refused, not granted");

    const AgentPolicy granting = policyFrom(std::string("allow ") + screenReadRuleToken + "\n");
    CHECK_MSG(evaluateScreenRead(granting, 3, "control-screen", "window=12 pane=4", ApprovalMode::RequireConfirmation) == InjectionVerdict::Allowed, "the documented rule token grants it");

    const AgentPolicy denying = policyFrom(std::string("default allow\ndeny ") + screenReadRuleToken + "\n");
    CHECK_MSG(evaluateScreenRead(denying, 3, "control-screen", "", ApprovalMode::AutoApprove) == InjectionVerdict::DeniedByRule, "a deny rule on the token holds even for a door that can approve");

    const std::vector<std::string> lines = audit.lines();
    CHECK_MSG(lines.size() == 3, "each read is audited");
    if (lines.size() == 3) {
        CHECK_MSG(withoutStamp(lines[0]) == "session=3 control-screen confirm: <read screen> window=12 pane=4", "the refused read names its target");
        CHECK_MSG(withoutStamp(lines[1]) == "session=3 control-screen allow: <read screen> window=12 pane=4", "the granted read names its target");
        CHECK_MSG(withoutStamp(lines[2]) == "session=3 control-screen deny: <read screen>", "a read with no target is the bare token");
    }
}

TEST(testAnAllowedRuleOnTheScreenTokenDoesNotAllowInjection)
{
    const AgentPolicy policy = policyFrom(std::string("allow ") + screenReadRuleToken + "\n");
    ScratchAuditLog audit;
    CHECK_MSG(evaluateInjection(policy, 3, "control-send", "ls", ApprovalMode::RequireConfirmation) == InjectionVerdict::NeedsApproval, "granting the read grants nothing else");
}

// ---- who records an allowance ----------------------------------------------

TEST(testACallerThatRecordsItsOwnOutcomeGetsNoLineFromTheGate)
{
    const AgentPolicy policy = policyFrom("default allow\n");
    ScratchAuditLog audit;
    CHECK_MSG(evaluateAction(policy, 9, "kill", "<terminate session>", ApprovalMode::RequireConfirmation, AuditOnAllow::CallerRecords, AgentPolicy::Subject::Description) == InjectionVerdict::Allowed, "allowed");
    CHECK_MSG(audit.lines().empty(), "the gate wrote nothing; the caller will");
}

UBIQUITTY_TEST_MAIN()
