# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The dispatcher's supported declaration forms, shared by every inventory.
function stanzaName(line, declaration) {
    if (match(line, /^[ \t]*build_and_run [A-Za-z0-9_-]+/)) {
        declaration = substr(line, RSTART, RLENGTH)
        sub(/^[ \t]*build_and_run /, "", declaration)
        return declaration
    }
    if (match(line, /^[ \t]*if wanted [A-Za-z0-9_-]+/) ||
        match(line, /^[ \t]*if \[ "\$\(uname -s\)" = "Darwin" \] && wanted [A-Za-z0-9_-]+/)) {
        declaration = substr(line, RSTART, RLENGTH)
        sub(/^.*wanted /, "", declaration)
        return declaration
    }
    return ""
}

function stanzaIsAvailable(line, platform) {
    return platform == "Darwin" || line !~ /^[ \t]*if \[ "\$\(uname -s\)" = "Darwin" \] && wanted /
}
