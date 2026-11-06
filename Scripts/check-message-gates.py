#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks canonical frame admission and its actual Core/host dispatch owners.

This is a source-structure tripwire, not a proof of authorization behavior.
The role, method, principal, scope, consent, and local-descriptor matrices
exercise the executable policies independently.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = "check-message-gates"
PATHS = {
    "envelope": "Source/Core/Protocol/Sync/Envelope.h",
    "directions": "Source/Core/Protocol/Sync/Envelope.cpp",
    "rows": "Source/Core/Protocol/Sync/FrameGates.h",
    "connection": "Source/Core/Protocol/Sync/Connection.cpp",
    "daemon": "Source/Platform/Host/Daemon.cpp",
}


def codeOnly(text):
    """Masks comments and literals without changing brace positions or lines."""
    pattern = (r'//[^\n]*|/\*[\s\S]*?\*/|'
               r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)"|'
               r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    return re.sub(pattern, lambda match: re.sub(r"[^\n]", " ", match.group()), text)


def blockAfter(text, pattern):
    """Returns one balanced declaration/function body, failing on a missed parse."""
    source = codeOnly(text)
    starts = list(re.finditer(pattern, source))
    if len(starts) != 1:
        raise ValueError("expected one declaration matching %s, found %d" % (pattern, len(starts)))
    opening = source.find("{", starts[0].end())
    if opening < 0:
        raise ValueError("declaration has no body: %s" % pattern)
    depth = 1
    for offset in range(opening + 1, len(source)):
        depth += (source[offset] == "{") - (source[offset] == "}")
        if not depth:
            return source[opening + 1:offset]
    raise ValueError("unterminated body: %s" % pattern)


def frameKinds(text):
    """Reads every explicit canonical wire kind, refusing duplicates or drift."""
    body = blockAfter(text, r"\benum\s+class\s+FrameKind\s*:\s*std::uint8_t")
    kinds = {}
    values = set()
    for item in body.split(","):
        if not item.strip():
            continue
        match = re.fullmatch(r"\s*(\w+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)\s*", item)
        if not match:
            raise ValueError("unparsed FrameKind declaration: %s" % item.strip())
        name, literal = match.groups()
        value = int(literal, 0)
        if name in kinds or value in values or value > 255:
            raise ValueError("duplicate or out-of-range FrameKind: %s" % name)
        kinds[name] = value
        values.add(value)
    if not kinds:
        raise ValueError("no FrameKind declarations")
    return kinds


def frameRows(text):
    """Reads the executable role rows, refusing partial/duplicate/empty rows."""
    body = blockAfter(text, r"\binline\s+constexpr\s+FrameGate\s+frameGates\s*\[\s*\]\s*(?:=)?")
    pattern = re.compile(r"\{\s*FrameKind::(\w+)\s*,\s*\{([^{}]*)\}\s*\}")
    rows = {}
    end = 0
    for match in pattern.finditer(body):
        if body[end:match.start()].strip(" \t\r\n,"):
            raise ValueError("unparsed tokens between FrameGate rows")
        kind, spelling = match.groups()
        roles = []
        for item in spelling.split(","):
            parsed = re.fullmatch(r"\s*Role::(Owner|Driver|Viewer|Agent)\s*", item)
            if not parsed:
                raise ValueError("unparsed or empty sender in %s" % kind)
            roles.append(parsed.group(1))
        if kind in rows or len(set(roles)) != len(roles):
            raise ValueError("duplicate FrameGate kind or sender: %s" % kind)
        rows[kind] = set(roles)
        end = match.end()
    if body[end:].strip(" \t\r\n,") or not rows:
        raise ValueError("unparsed or empty FrameGate table")
    return rows


def frameDirections(text):
    """Reads grouped direction cases rather than guessing from enum names."""
    body = blockAfter(text, r"\bDirection\s+directionOf\s*\(\s*FrameKind\s+kind\s*\)")
    body = blockAfter(body, r"\bswitch\s*\(\s*kind\s*\)")
    directions = {}
    pending = []
    tokens = re.finditer(r"case\s+FrameKind::(\w+)\s*:|return\s+Direction::(\w+)\s*;", body)
    for token in tokens:
        kind, direction = token.groups()
        if kind:
            pending.append(kind)
            continue
        if direction not in ("ClientToHost", "HostToClient", "Both"):
            raise ValueError("unknown direction: %s" % direction)
        for name in pending:
            if name in directions:
                raise ValueError("duplicate direction case: %s" % name)
            directions[name] = direction
        pending.clear()
    if pending or not directions:
        raise ValueError("unresolved or empty direction cases")
    return directions


def policyFindings(kinds, rows, directions, dispatch):
    """Checks coverage against the executable wire direction and dispatch."""
    failures = []
    for label, names in (("direction", directions), ("dispatch", dispatch)):
        failures += ["%s kind %s is missing from %s" % (label, name, label)
                     for name in sorted(set(kinds) - set(names))]
        failures += ["stale %s kind %s" % (label, name)
                     for name in sorted(set(names) - set(kinds))]
    clients = {name for name, direction in directions.items() if direction != "HostToClient"}
    failures += ["client kind %s has no role row" % name for name in sorted(clients - set(rows))]
    failures += ["kind %s has a role row but is not client-to-host" % name
                 for name in sorted(set(rows) - clients)]
    order = [kinds[name] for name in rows if name in kinds]
    if order != sorted(order):
        failures.append("FrameGate rows are not in wire order")
    for name, roles in rows.items():
        if not {"Owner", "Driver"} <= roles:
            failures.append("%s lacks its Owner/Driver senders" % name)
    return failures


def requires(body, pattern, what):
    """Checks an executable boundary, never a mention in prose or a literal."""
    return [] if re.search(pattern, body, re.S) else [what]


def ownershipFindings(connection, daemon):
    """Checks the actual sender, handshake, and authenticated-provider seams."""
    receive = blockAfter(connection, r"\bvoid\s+HostConnection::receiveStream\s*\(")
    dispatch = blockAfter(connection, r"\bvoid\s+HostConnection::handleFrame\s*\(")
    greet = blockAfter(dispatch, r"\bcase\s+FrameKind::Greet\s*:")
    hostReceive = blockAfter(daemon, r"\bvoid\s+Daemon::receiveScreenBytes\s*\(")
    admission = blockAfter(daemon, r"\bDaemon::admitGreeting\s*\(")
    failures = []
    beforeSwitch = dispatch.split("switch (frame.kind)", 1)[0]
    roleGuard = blockAfter(beforeSwitch, r"\bif\s*\(\s*!admitsSender\(frame.kind,\s*m_grantedRole\)\s*\)")
    failures += requires(roleGuard, r"\bclose\([^;]+;\s*return\s*;",
                         "HostConnection does not refuse the role before dispatch")
    failures += requires(receive, r"directionOf\(header.kind\)\s*==\s*Direction::HostToClient",
                         "host ingress has no header direction refusal")
    failures += requires(receive, r"m_state\s*==\s*ConnectionState::Greeting\s*&&\s*header.kind\s*!=\s*FrameKind::Greet\s*&&\s*header.kind\s*!=\s*FrameKind::Bye",
                         "host ingress has no header-first pre-Greet boundary")
    failures += requires(receive, r"isConsentPending\(m_state\)\s*&&\s*!isPendingTransportFrame\(header.kind\)",
                         "host ingress no longer isolates pending-consent traffic")
    failures += requires(greet, r"decodeGreet\(frame.body\)[\s\S]*m_greetingAdmission\(\*greet\)",
                         "the decoded Greet does not reach the host admission provider")
    failures += requires(greet, r"!isValidSelection\(m_identity.selection\)[\s\S]*m_identity.selection.target\s*!=\s*greet->request.target",
                         "Greet does not verify the authenticated serving selection")
    failures += requires(hostReceive, r"setGreetingAdmission\(\[this,\s*clientID\][\s\S]*return\s+admitGreeting\(clientID,\s*greet\)",
                         "Daemon does not install its exact-client admission provider")
    failures += requires(hostReceive, r"syncConnection->receiveStream\(bytes,",
                         "Daemon does not deliver canonical bytes to HostConnection")
    failures += requires(admission, r"tokenScopeAllows\(client,\s*requiredScope\)",
                         "authenticated target admission has no credential-scope gate")
    failures += requires(admission, r"identity.mayDrive\s*=\s*tokenScopeAllows\(client,\s*Protocol::Sync::Scope::Drive\)",
                         "the admitted drive authority is not derived from its credential")
    failures += requires(admission, r"identity.verifiedKind\s*=\s*isAgentClient\(client\)",
                         "verified human/agent standing is not derived by the daemon")
    return failures


def examine(sources):
    """Returns source findings, including a refusal when any parser loses its subject."""
    try:
        kinds = frameKinds(sources["envelope"])
        rows = frameRows(sources["rows"])
        directions = frameDirections(sources["directions"])
        dispatchBody = blockAfter(sources["connection"], r"\bvoid\s+HostConnection::handleFrame\s*\(")
        dispatch = re.findall(r"\bcase\s+FrameKind::(\w+)\s*:", dispatchBody)
        if len(dispatch) != len(set(dispatch)):
            raise ValueError("duplicate HostConnection dispatch case")
        return policyFindings(kinds, rows, directions, set(dispatch)) + ownershipFindings(
            sources["connection"], sources["daemon"])
    except ValueError as error:
        return [str(error)]


def selftest(verbose):
    """Exercises successful parsing and planted omissions without subprocesses."""
    table = """inline constexpr FrameGate frameGates[] {
        { FrameKind::Greet, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
        { FrameKind::Call, { Role::Owner, Role::Driver } },
    };"""
    cases = [
        ("valid role rows", lambda: len(frameRows(table)) != 2, False),
        ("empty table", lambda: not frameRows("inline constexpr FrameGate frameGates[] {};"), True),
        ("unparsed row", lambda: not frameRows(table.replace("FrameKind::Call", "Other::Call")), True),
        ("duplicate kind", lambda: not frameRows(table.replace("FrameKind::Call", "FrameKind::Greet")), True),
        ("duplicate role", lambda: not frameRows(table.replace("Role::Viewer", "Role::Owner")), True),
        ("unknown role", lambda: not frameRows(table.replace("Role::Viewer", "Role::Unknown")), True),
        ("empty role set", lambda: not frameRows(table.replace("{ Role::Owner, Role::Driver }", "{}")), True),
    ]
    kinds = {"Greet": 1, "Welcome": 2, "Call": 13}
    rows = frameRows(table)
    directions = {"Greet": "ClientToHost", "Welcome": "HostToClient", "Call": "ClientToHost"}
    dispatch = set(kinds)
    cases += [
        ("complete executable coverage", lambda: bool(policyFindings(kinds, rows, directions, dispatch)), False),
        ("missing role row", lambda: bool(policyFindings(kinds, {"Greet": rows["Greet"]}, directions, dispatch)), True),
        ("host-only row", lambda: bool(policyFindings(kinds, dict(rows, Welcome={"Owner", "Driver"}), directions, dispatch)), True),
        ("missing dispatch", lambda: bool(policyFindings(kinds, rows, directions, {"Greet", "Welcome"})), True),
        ("missing direction", lambda: bool(policyFindings(kinds, rows, {"Greet": "ClientToHost"}, dispatch)), True),
        ("stale dispatcher", lambda: bool(policyFindings(kinds, rows, directions, dispatch | {"Gone"})), True),
        ("missing owner", lambda: bool(policyFindings(kinds, dict(rows, Call={"Driver"}), directions, dispatch)), True),
        ("wrong row order", lambda: bool(policyFindings(kinds, dict(reversed(list(rows.items()))), directions, dispatch)), True),
        ("braces in comments and strings", lambda: "return;" not in blockAfter('void f() { "}"; /* } */ return; }', r"void f\(\)"), False),
        ("missing subject", lambda: not blockAfter("void other() {}", r"void missing\(\)"), True),
    ]
    enum = "enum class FrameKind : std::uint8_t { Greet = 1, Welcome = 2, Call = 13 };"
    directionSource = """Direction directionOf(FrameKind kind) {
        switch (kind) {
        case FrameKind::Greet:
        case FrameKind::Call: return Direction::ClientToHost;
        case FrameKind::Welcome: return Direction::HostToClient;
        }
        return Direction::Both;
    }"""
    cases += [
        ("complete wire kinds", lambda: frameKinds(enum) != kinds, False),
        ("duplicate wire value", lambda: not frameKinds(enum.replace("Call = 13", "Call = 2")), True),
        ("unparsed wire value", lambda: not frameKinds(enum.replace("Call = 13", "Call = expression()")), True),
        ("grouped direction cases", lambda: frameDirections(directionSource) != directions, False),
        ("unresolved direction case", lambda: not frameDirections(directionSource.replace("return Direction::HostToClient;", "break;")), True),
        ("unknown direction", lambda: not frameDirections(directionSource.replace("Direction::HostToClient", "Direction::Unknown")), True),
    ]
    connection = """void HostConnection::receiveStream(Bytes bytes) {
        if (directionOf(header.kind) == Direction::HostToClient) return;
        if (m_state == ConnectionState::Greeting && header.kind != FrameKind::Greet && header.kind != FrameKind::Bye) return;
        if (isConsentPending(m_state) && !isPendingTransportFrame(header.kind)) return;
    }
    void HostConnection::handleFrame(Frame frame) {
        if (!admitsSender(frame.kind, m_grantedRole)) { close(ByeReason::Malformed, detail); return; }
        switch (frame.kind) {
        case FrameKind::Greet: {
            auto greet = decodeGreet(frame.body);
            auto identity = m_greetingAdmission(*greet);
            if (!isValidSelection(m_identity.selection) || m_identity.selection.target != greet->request.target) return;
        }
        }
    }"""
    daemon = """void Daemon::receiveScreenBytes(Index index, Bytes bytes) {
        syncConnection->setGreetingAdmission([this, clientID](Greet greet) { return admitGreeting(clientID, greet); });
        syncConnection->receiveStream(bytes, now);
    }
    Identity Daemon::admitGreeting(ID clientID, Greet greet) {
        if (!tokenScopeAllows(client, requiredScope)) return refusal;
        identity.mayDrive = tokenScopeAllows(client, Protocol::Sync::Scope::Drive);
        identity.verifiedKind = isAgentClient(client) ? Agent : Human;
    }"""
    cases += [
        ("live owner boundaries", lambda: bool(ownershipFindings(connection, daemon)), False),
        ("role guard with nested initializer", lambda: bool(ownershipFindings(connection.replace("{ close(ByeReason::Malformed, detail);", "{ const auto detail = std::string_view {}; close(ByeReason::Malformed, detail);"), daemon)), False),
        ("missing role guard", lambda: bool(ownershipFindings(connection.replace("admitsSender", "unguardedSender"), daemon)), True),
        ("missing pending isolation", lambda: bool(ownershipFindings(connection.replace("isPendingTransportFrame", "allFrames"), daemon)), True),
        ("missing selection validation", lambda: bool(ownershipFindings(connection.replace("isValidSelection", "assumedSelection"), daemon)), True),
        ("missing exact-client provider", lambda: bool(ownershipFindings(connection, daemon.replace("admitGreeting(clientID, greet)", "admitGreeting(otherID, greet)"))), True),
        ("missing credential scope", lambda: bool(ownershipFindings(connection, daemon.replace("tokenScopeAllows(client, requiredScope)", "true"))), True),
        ("missing verified standing", lambda: bool(ownershipFindings(connection, daemon.replace("isAgentClient(client)", "claimedAgent"))), True),
    ]
    failures = 0
    for title, operation, expected in cases:
        try:
            reported = operation()
        except ValueError:
            reported = True
        if bool(reported) != expected:
            failures += 1
            print("  FAIL: %s" % title, file=sys.stderr)
        elif verbose:
            print("  PASS: %s" % title)
    print("selftest: %d cases, %d failures" % (len(cases), failures))
    return failures


def main():
    if "--selftest" in sys.argv[1:]:
        return 1 if selftest(True) else 0
    if selftest(False):
        return 2
    sources = {}
    for name, path in PATHS.items():
        with open(os.path.join(ROOT, path), encoding="utf-8") as source:
            sources[name] = source.read()
    failures = examine(sources)
    if failures:
        print("%s: canonical admission is incomplete" % GATE, file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1
    print("%s: ok (canonical direction, role rows, dispatch, and authenticated host owner)" % GATE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
