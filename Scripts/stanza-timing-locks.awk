# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Names the tests and shell stanzas a CMake call holds under the `timing`
# resource lock. Each input line is one call, joined onto a line by the caller:
#   set_tests_properties(<test>... PROPERTIES <PROPERTY> <value>... ...)
#   set(shell_<stanza>_RESOURCE_LOCK <lock>...) or list(APPEND ... ...)
# A value is a CMake list, so `a;timing` holds it as `timing` does.

function holdsTiming(value, parts, count, i) {
    gsub(/"/, "", value)
    count = split(value, parts, ";")
    for (i = 1; i <= count; ++i)
        if (parts[i] == "timing") return 1
    return 0
}

{
    line = $0
    sub(/^[^(]*\(/, "", line)
    sub(/\)[[:space:]]*$/, "", line)
    count = split(line, tokens, /[[:space:]]+/)
    if ($0 ~ /^set_tests_properties/) {
        names = ""
        i = 1
        while (i <= count && tokens[i] != "PROPERTIES") {
            if (tokens[i] != "") names = names " " tokens[i]
            ++i
        }
        locked = 0
        for (++i; i <= count; ++i) {
            if (tokens[i] != "RESOURCE_LOCK") continue
            for (j = i + 1; j <= count && tokens[j] !~ /^[A-Z][A-Z_]*$/; ++j)
                if (holdsTiming(tokens[j])) locked = 1
        }
        if (locked) {
            n = split(names, list, " ")
            for (k = 1; k <= n; ++k) if (list[k] != "") print list[k]
        }
        next
    }
    if (!match($0, /shell_[A-Za-z0-9_]+_RESOURCE_LOCK/)) next
    stanza = substr($0, RSTART + 6, RLENGTH - 6 - 14)
    for (i = 1; i <= count; ++i) {
        if (tokens[i] ~ /_RESOURCE_LOCK$/ || tokens[i] == "APPEND") continue
        if (holdsTiming(tokens[i])) { print stanza; next }
    }
}
