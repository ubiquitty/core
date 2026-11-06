// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Security/InjectionGate.h"

#include "PAL/Capability.h"

namespace Ubiquitty::Security {

InjectionVerdict evaluateAction(const AgentPolicy& policy, uint32_t sessionID, const char* kind, const std::string& detail, ApprovalMode approval, AuditOnAllow auditOnAllow, AgentPolicy::Subject subject)
{
    const PolicyDecision decision = policy.evaluate(detail, subject);
    if (decision == PolicyDecision::Deny) {
        auditRecord(sessionID, kind, detail, decision);
        return InjectionVerdict::DeniedByRule;
    }
    if (decision == PolicyDecision::Confirm && approval == ApprovalMode::RequireConfirmation) {
        auditRecord(sessionID, kind, detail, decision);
        return InjectionVerdict::NeedsApproval;
    }
    if (auditOnAllow == AuditOnAllow::Record)
        auditRecord(sessionID, kind, decision == PolicyDecision::Confirm ? "[approved] " + detail : detail, decision);
    return InjectionVerdict::Allowed;
}

InjectionVerdict evaluateInjection(const AgentPolicy& policy, uint32_t sessionID, const char* kind, const std::string& text, ApprovalMode approval)
{
    return evaluateAction(policy, sessionID, kind, text, approval, AuditOnAllow::Record, AgentPolicy::Subject::AgentSupplied);
}

bool isHeldForShellIndirection(const AgentPolicy& policy, const std::string& detail)
{
    return policy.evaluate(detail, AgentPolicy::Subject::AgentSupplied) == PolicyDecision::Confirm && policy.evaluate(detail, AgentPolicy::Subject::Description) == PolicyDecision::Allow;
}

std::string screenedLine(std::string_view text)
{
    if (text.ends_with("\r\n"))
        text.remove_suffix(2);
    else if (text.ends_with('\n') || text.ends_with('\r'))
        text.remove_suffix(1);
    return std::string(text);
}

InjectionVerdict evaluateScreenRead(const AgentPolicy& policy, uint32_t sessionID, const char* kind, const std::string& target, ApprovalMode approval)
{
    static_assert(tierOf(Capability::ScreenContentRead) == CapabilityTier::ReadExfil, "a screen read is never free for a non-human principal");
    std::string detail = screenReadRuleToken;
    if (!target.empty())
        detail += " " + target;
    return evaluateAction(policy, sessionID, kind, detail, approval, AuditOnAllow::Record, AgentPolicy::Subject::Description);
}

} // namespace Ubiquitty::Security
