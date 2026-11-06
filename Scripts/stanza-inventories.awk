# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Inventories derived from the driver's stanza declarations. The shared
# declaration functions come from stanza-declarations.awk before this program.
BEGIN {
    if (mode != "list" && mode != "serial" && mode != "host" &&
        mode != "agent" && mode != "quic" && mode != "shape")
        exit 2
}

mode == "list" {
    name = stanzaName($0)
    if (name != "" && stanzaIsAvailable($0, platform)) print name
    next
}

mode == "serial" {
    # Only ROOT/build and named BUILD_ROOT trees count; ROOT/Source is not a
    # shared build directory. Resolve assignments before scanning the body.
    if ($0 ~ /^[A-Za-z_][A-Za-z0-9_]*="\$\{?([A-Za-z_][A-Za-z0-9_]*BUILD_ROOT\}?\/|ROOT\}?\/build\/)[A-Za-z0-9_-]+"/) {
        split($0, parts, "=")
        match($0, /\/[A-Za-z0-9_-]+"$/)
        treeForVariable[parts[1]] = substr($0, RSTART + 1, RLENGTH - 2)
    }
    if (stanzaName($0) != "") {
        for (d in seen) print name, d
        delete seen
        name = stanzaName($0)
    }
    if (name != "" && match($0, /(BUILD_ROOT\}?\/|ROOT\}?\/build\/)[A-Za-z0-9_-]+/)) {
        tree = substr($0, RSTART, RLENGTH)
        sub(/^.*\//, "", tree)
        seen[tree] = 1
    }
    if (name != "") {
        # The boundary excludes HOST_TEST_BUILD_QUIC from HOST_TEST_BUILD.
        for (v in treeForVariable) {
            if ($0 ~ ("\\$\\{?" v "\\}?([^A-Za-z0-9_]|$)"))
                seen[treeForVariable[v]] = 1
        }
    }
    next
}

mode == "host" || mode == "agent" || mode == "quic" {
    if (stanzaName($0) != "") {
        if (needs && name != excludedStanza()) print name
        needs = 0
        name = stanzaName($0)
    }
    if (name != "" && mode == "host" && $0 ~ /ubq_(agent_)?host_binary/) needs = 1
    if (name != "" && mode == "agent" && $0 ~ /ubq_agent_binary/) needs = 1
    if (name != "" && mode == "quic" && $0 ~ /quic_deps_present/) needs = 1
    next
}

mode == "shape" {
    declaration = stanzaName($0)
    if (declaration != "") {
        name = declaration
        if (!stanzaIsAvailable($0, platform) || name ~ /_fixture$/) { name = ""; next }
        if (want == "@inline" && $0 ~ /^[ \t]*build_and_run /) { print name; exit }
        next
    }
    if (name != "" && want != "@inline" && $0 ~ want) { print name; exit }
}

END {
    if (mode == "serial")
        for (d in seen) print name, d
    if ((mode == "host" || mode == "agent" || mode == "quic") && needs && name != excludedStanza())
        print name
}

function excludedStanza() {
    if (mode == "host") return "host_binary"
    if (mode == "agent") return "agent_binary"
    return "quic_deps_fixture"
}
