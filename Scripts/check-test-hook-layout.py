#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-test-hook-layout.py — a test hook never changes a type's layout.

`UBIQUITTY_ENABLE_TEST_HOOKS` is defined per test target, not for the library
the target links. A data member declared under it therefore gives its class two
layouts in one binary: the library's translation units see the smaller one and
the test's see the larger. When library code allocates the object and the test
binary's inline constructor initialises it, the constructor writes past the
end. `TerminalFrameInputs` did exactly this: the portable snapshot render held
one on its stack, and in a hooked test binary every render overran it — an
abort on the stack check, or silently overwritten neighbours with the check off.
Nothing in a passing build says so, because each translation unit is
internally consistent.

So a header may put functions, statics, externs and nested types under the
hook, but not an instance member of the enclosing class. The member belongs in
every build; only what counts into it and reads it is hooked.

The scan is textual and keys on the naming rule: an instance member is spelled
`m_name`, so a declaration under the hook whose declared name is `m_…` is
reported. Brace depth alone cannot tell a member from a local, because a hooked
block can sit inside a function body opened before it; the prefix can. A
nested type is not a member, but a member of that type declared after its
closing brace is.

    python3 Scripts/check-test-hook-layout.py
    python3 Scripts/check-test-hook-layout.py --self-test
"""

import re
import subprocess
import sys

HOOK_OPEN = re.compile(r'^\s*#\s*(?:if|ifdef)\b.*\bUBIQUITTY_ENABLE_TEST_HOOKS\b')
PREPROCESSOR_OPEN = re.compile(r'^\s*#\s*if')
PREPROCESSOR_CLOSE = re.compile(r'^\s*#\s*endif\b')
NOT_A_MEMBER = re.compile(r'^(?:static|extern|using|friend|typedef|return|template|enum|struct|class|union|namespace)\b')
# The declared name of an instance member, by the `m_name` rule: the last
# identifier before an initialiser or the terminating semicolon.
# A type precedes it — an identifier, a closing template bracket or a pointer
# or reference mark — where an assignment starts with the name or reaches it
# through `.` or `->`.
MEMBER_NAME = re.compile(r'^[A-Za-z_][\w:<>,\s]*[\w>][\s*&]+(m_[A-Za-z0-9_]+)\s*(?:=[^;]*|\{[^;]*\})?\s*;\s*$')


def hooked_members(text):
    """The instance data members a header declares under the test hook.

    - Parameter text: the header's contents.
    - Returns: (line number, declaration) for each finding.
    """
    findings = []
    depth = 0
    for number, line in enumerate(text.split("\n"), 1):
        if depth == 0:
            if HOOK_OPEN.match(line):
                depth = 1
            continue
        if PREPROCESSOR_OPEN.match(line):
            depth += 1
            continue
        if PREPROCESSOR_CLOSE.match(line):
            depth -= 1
            continue
        code = line.split("//", 1)[0].strip()
        if not code or not code.endswith(";"):
            continue
        # A closing brace ends a nested type, and the name after it, if any,
        # is a member of that type's kind: `} m_hook;` declares one.
        closes_type = code.startswith("}")
        if closes_type:
            code = code.lstrip("}").strip()
        head = code.split("=", 1)[0]
        if "(" in head or NOT_A_MEMBER.match(code) or "static" in code.split():
            continue
        if MEMBER_NAME.search(code) or (closes_type and re.match(r'^m_[A-Za-z0-9_]+\s*;$', code)):
            findings.append((number, line.strip()))
    return findings


def self_test():
    """Runs the rule against a member it must catch and constructs it must not."""
    planted = "\n".join([
        "class A {",
        "#if UBIQUITTY_ENABLE_TEST_HOOKS",
        "    std::size_t m_countForTest = 0;",
        "#endif",
        "};",
    ])
    allowed = "\n".join([
        "class B {",
        "#if UBIQUITTY_ENABLE_TEST_HOOKS",
        "    [[nodiscard]] std::size_t countForTest() const { return m_count; }",
        "    static inline bool s_failNextForTest = false;",
        "    void reset()",
        "    {",
        "        Value v;",
        "        v.m_type = 1;",
        "    }",
        "    void grow()",
        "    {",
        "#if UBIQUITTY_ENABLE_TEST_HOOKS",
        "        char* const grown = static_cast<char*>(std::realloc(data, size));",
        "#endif",
        "    }",
        "    struct Probe {",
        "        int calls = 0;",
        "    };",
        "#endif",
        "    std::size_t m_count = 0;",
        "};",
        "#if UBIQUITTY_ENABLE_TEST_HOOKS",
        "extern std::uint64_t cellsCopiedForTest;",
        "#endif",
    ])
    nested = "\n".join([
        "class C {",
        "#ifdef UBIQUITTY_ENABLE_TEST_HOOKS",
        "    struct Probe {",
        "        int calls = 0;",
        "    } m_probe;",
        "#endif",
        "};",
    ])
    failures = []
    if len(hooked_members(planted)) != 1:
        failures.append("missed a hooked instance member")
    if hooked_members(allowed):
        failures.append("reported a function, a static, a local or an extern: %r" % hooked_members(allowed))
    if len(hooked_members(nested)) != 1:
        failures.append("missed a member of a nested type declared after its closing brace")
    for failure in failures:
        print("check-test-hook-layout --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-test-hook-layout --self-test: ok (3 cases; a member, the constructs a hook may hold, a nested-type member)")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()
    listed = subprocess.run(["git", "ls-files", "--", "Source/*.h"], capture_output=True, text=True)
    headers = listed.stdout.split()
    if listed.returncode != 0 or not headers:
        print("check-test-hook-layout: REFUSED — git listed no header under Source/, so nothing was checked",
              file=sys.stderr)
        return 2
    findings = []
    for header in headers:
        try:
            with open(header, "r", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        if "UBIQUITTY_ENABLE_TEST_HOOKS" not in text:
            continue
        findings.extend((header, number, line) for number, line in hooked_members(text))
    if findings:
        print("check-test-hook-layout: an instance member declared under UBIQUITTY_ENABLE_TEST_HOOKS gives its class"
              " two layouts in one test binary; declare it in every build and hook only its use:", file=sys.stderr)
        for header, number, line in findings:
            print("  %s:%d: %s" % (header, number, line), file=sys.stderr)
        return 1
    print("check-test-hook-layout: ok (%d header(s); no member changes a layout under the test hook)" % len(headers))
    return 0


if __name__ == "__main__":
    sys.exit(main())
