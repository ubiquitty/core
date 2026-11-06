// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Every door a non-human principal drives a session through evaluates here.
//
// The agent surface (the `ubq` CLI and its MCP server) and the
// macOS control socket inject text that reaches the PTY as typed input, and
// read the screen back. One rule file in the control directory decides what
// such a principal may do, and one audit log beside it records every verdict;
// a door that does not evaluate here does not act.
//
// The gate screens the principal at the door, not the session's capability
// origin: a frontend Terminal a control-socket client types into keeps
// `CapabilityOrigin::Local`. The GUI capability policies hold no consent
// question, so an Agent origin on a GUI pane would refuse the host-escaping
// effects of the program the local user runs there, a clipboard write or a
// notification among them. The bytes are screened before they join the
// keyboard route; what the program then does with them is the local user's own
// program doing it.

#include "Core/Security/AgentPolicy.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Security {

/// What a `Confirm` verdict becomes at a door that can or cannot ask.
///
/// The CLI's `--yes` and nothing else turns it into an approval; the MCP
/// server and the control socket have no human on the line, so they refuse.
enum class ApprovalMode : uint8_t {
    RequireConfirmation,
    AutoApprove
};

/// Who records the audit line for an action the policy permits.
///
/// Most actions have nothing further to report, so the gate is the only place
/// that knows the action happened. An action the host can still refuse —
/// terminating a session — audits its own outcome instead, and a line here as
/// well would put a permitted request in the log in the same words as a
/// completed one.
enum class AuditOnAllow : uint8_t {
    Record,
    CallerRecords
};

/// The gate's answer. Anything but `Allowed` has already been audited.
enum class InjectionVerdict : uint8_t {
    Allowed,
    /// A deny rule matched; no approval lifts it.
    DeniedByRule,
    /// The policy says confirm and the door could not.
    NeedsApproval
};

/// Screens one action a principal asks of a session and records the outcome.
///
/// The verdict is the policy's first matching rule, else its default, with a
/// `Confirm` resolved by `approval`. Every refusal is audited; an allowance is
/// audited unless the caller says it will record the outcome itself.
///
/// - Parameters:
///   - policy: The rule set the principal is held to.
///   - sessionID: The session the audit line names; 0 for a session the host
///     has not numbered.
///   - kind: The verb, as the audit line names it (`send`, `run`, `resize`).
///   - detail: What the rules match against — the injected text, or a fixed
///     description of an action that carries none.
///   - approval: Whether a `Confirm` verdict counts as approved.
///   - auditOnAllow: Who records a permitted action.
///   - subject: Whether the principal wrote `detail`; only a fixed string this
///     code wrote may skip the shell-indirection downgrade.
/// - Returns: The verdict.
[[nodiscard]] InjectionVerdict evaluateAction(const AgentPolicy& policy, uint32_t sessionID, const char* kind, const std::string& detail, ApprovalMode approval, AuditOnAllow auditOnAllow = AuditOnAllow::Record, AgentPolicy::Subject subject = AgentPolicy::Subject::AgentSupplied);

/// Screens text a principal wants typed into a session, and records the outcome.
///
/// The text is the principal's own, so a matched `allow` still downgrades to
/// `Confirm` when the line carries shell indirection the token match never
/// vetted.
///
/// - Parameters:
///   - policy: The rule set the principal is held to.
///   - sessionID: The session the audit line names.
///   - kind: The verb, as the audit line names it.
///   - text: The bytes that would reach the PTY.
///   - approval: Whether a `Confirm` verdict counts as approved.
/// - Returns: The verdict.
[[nodiscard]] InjectionVerdict evaluateInjection(const AgentPolicy& policy, uint32_t sessionID, const char* kind, const std::string& text, ApprovalMode approval);

/// Why a door with no person on the line refuses a line its policy allows.
///
/// A rule vets only the words it names, so a line that pipes, chains,
/// redirects, substitutes or submits more than one command never runs
/// unattended. A refusal for that reason says so instead of pointing at a
/// rule or a default that cannot lift it.
inline constexpr std::string_view shellIndirectionHold
    = "the line pipes, chains, redirects or substitutes commands, or types more than one line, and a rule approves only the words it names, so neither an 'allow' rule nor 'default allow' runs it unattended";

/// Whether the policy holds `detail` for confirmation only because of its
/// shell indirection.
///
/// - Parameters:
///   - policy: The rule set the principal is held to.
///   - detail: The principal's own line, as the gate screened it.
/// - Returns: True when the policy would allow the line's words and its
///   indirection alone turned that into a confirmation.
[[nodiscard]] bool isHeldForShellIndirection(const AgentPolicy& policy, const std::string& detail);

/// The text a typed line is screened as: `text` without the one terminator that
/// runs it.
///
/// A door that types verbatim bytes ends a command with the newline the agent's
/// `run` verb appends after screening, so screening the line without that one
/// terminator holds the two doors to the same rule for the same command. A
/// second terminator stays: it runs a second line, which is the indirection the
/// matcher downgrades.
///
/// - Parameters:
///   - text: The bytes that would reach the PTY.
/// - Returns: `text` less one trailing `\n`, `\r`, or `\r\n`.
[[nodiscard]] std::string screenedLine(std::string_view text);

/// The rule token that names a screen read: `allow <read screen>`.
inline constexpr const char* screenReadRuleToken = "<read screen>";

/// Screens a principal's read of a session's screen text, and records it.
///
/// The read is `Capability::ScreenContentRead`, the read-exfiltration tier: a
/// screen holds whatever the user last had on it, so a non-human principal
/// never reads one for free. The rule line that grants it is
/// `allow <read screen>`, the detail the log records is that token followed
/// by `target`.
///
/// - Parameters:
///   - policy: The rule set the principal is held to.
///   - sessionID: The session the audit line names.
///   - kind: The verb, as the audit line names it.
///   - target: Which screen, in the door's own words; may be empty.
///   - approval: Whether a `Confirm` verdict counts as approved.
/// - Returns: The verdict.
[[nodiscard]] InjectionVerdict evaluateScreenRead(const AgentPolicy& policy, uint32_t sessionID, const char* kind, const std::string& target, ApprovalMode approval);

} // namespace Ubiquitty::Security
