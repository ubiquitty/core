// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Command gating + audit for a non-human principal. Every command (and raw input)
// an agent or a control-socket client injects is screened against a policy before
// reaching the PTY and recorded to a per-session audit log; the two doors share
// one rule file and one log.
//
// Policy is an ordered ruleset: the first rule whose pattern tokens appear as a
// contiguous subsequence of the command's argv tokens wins, else the default.
// Tokenization normalises quoting/whitespace (`r''m -rf` == `rm -rf`; `rm` doesn't
// match `perform`). Default is Confirm (fail-safe): with no policy, nothing runs unattended.

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Security {

enum class PolicyDecision {
    Allow, // run without asking
    Confirm, // run only with explicit human approval (CLI --yes)
    Deny // never run
};

const char* policyDecisionName(PolicyDecision);

class AgentPolicy {
public:
    // Load rules from `path`. Each non-blank, non-'#' line is either
    //   allow|confirm|deny <pattern>
    //   default allow|confirm|deny
    // <pattern> is a shell-like token sequence; the rule matches when the
    // command's tokenized argv contains it as a contiguous subsequence.
    // Returns true if the file was read; false (keeping the built-in
    // default) when it is absent or unreadable. Never throws.
    bool loadFromFile(const char* path);

    // Load from $UBIQUITTY_AGENT_POLICY, else <control-directory>/agent-policy.conf
    // (control-directory honors $UBIQUITTY_HOME). Absent file → built-in default.
    void loadDefault();

    // First-match-wins over the rules, else the default. SECURITY: a matched Allow
    // is downgraded to Confirm when the line carries shell indirection (substitution,
    // chaining, pipe, redirection, or a control byte the line discipline acts on)
    // the token match can't vet; a control byte also ends a token, so a deny rule
    // matches across one. An allowed binary that
    // is itself an exec gadget (find -exec, awk system(), xargs) still runs unattended.
    /// Whether the subject is text the agent wrote, or one Ubiquitty wrote about it.
    ///
    /// The shell-indirection downgrade exists because an Allow vets only the tokens
    /// it matched, and a pipe or a substitution runs more than those. That reasoning
    /// is about a command line the agent supplied. A fixed description this code
    /// wrote — the placeholder for a Ctrl-C, or for ending a session — is not a
    /// command line, cannot be influenced by the agent, and runs nothing; putting it
    /// through the downgrade only means a user who writes `default allow` is told to
    /// write `default allow`.
    enum class Subject {
        AgentSupplied,
        Description
    };

    /// Returns the decision for `commandLine`, first match wins, else the default.
    ///
    /// - Parameters:
    ///   - commandLine: The text to match rules against.
    ///   - subject: Whether the agent supplied the text; a `Description` skips the
    ///     shell-indirection downgrade, so pass it only for a fixed string this code
    ///     wrote, never for anything carrying agent input.
    [[nodiscard]] PolicyDecision evaluate(const std::string& commandLine, Subject subject = Subject::AgentSupplied) const;

    void setDefaultDecision(PolicyDecision decision) { m_default = decision; }
    [[nodiscard]] PolicyDecision defaultDecision() const { return m_default; }
    [[nodiscard]] std::size_t ruleCount() const { return m_rules.size(); }

private:
    struct Rule {
        PolicyDecision action;
        std::string pattern;
        // The pattern split once, when the rule is read. A pattern is fixed
        // from that moment, so splitting it again on every gate check rebuilds
        // the same strings for the same answer — and every mutating verb the
        // agent surface offers goes through that check.
        std::vector<std::string> patternTokens;
    };
    std::vector<Rule> m_rules;
    PolicyDecision m_default = PolicyDecision::Confirm;
};

// Append one timestamped line to the agent audit log ($UBIQUITTY_AGENT_AUDIT, else
// <control-directory>/agent-audit.log):
//   <ISO-8601-UTC> session=<id> <kind> <decision>: <detail>
// Best-effort: a failure to open the log warns once on stderr, else silent.
void auditRecord(uint32_t sessionID, const char* kind, const std::string& detail, PolicyDecision);

/// Records an action that could not proceed after its policy check.
///
/// An incomplete action is neither an authorization refusal nor evidence that
/// the action ran. It shares the audit log's escaping and owner-only storage.
///
/// - Parameters:
///   - sessionID: The session the action targeted.
///   - kind: The fixed action name.
///   - detail: The requested action and the reason it could not proceed.
void auditIncompleteAction(uint32_t sessionID, const char* kind, const std::string& detail);

} // namespace Ubiquitty::Security
