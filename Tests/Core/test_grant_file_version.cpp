// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/test_grant_file_version.cpp — a grant file says which layout it is
// in.
//
// ~/.ubiquitty/consent and ~/.ubiquitty/capability_grants are the daemon's
// record of what a human allowed. Both were newline-delimited text with no
// statement of their own format: capability_grants persists a capability and an
// origin as bare integers, range-checked on load against the enum's current
// last value. So a build that renumbers or extends either enum reads an old
// file's numbers under the new meaning and grants something nobody granted —
// silently, because every value still passes the range check.
//
// A header naming the one layout this build reads is what makes that a refusal
// rather than a reinterpretation: a file that does not open with it is refused
// whole.
//
// Exit 0 on success.
//

#include "Platform/Host/GrantFileHeader.h"

#include <cstdio>
#include <string>

using Ubiquitty::Host::grantFileHeader;
using Ubiquitty::Host::isGrantFileHeader;

namespace {

// What a case found, named rather than positional: every line below asks a
// yes-or-no question and a bare bool at the call site says which by position.
enum class Held {
    No,
    Yes
};

int headerFailures = 0;

Held held(bool isSo) { return isSo ? Held::Yes : Held::No; } // bool-ok: naming the answer is this function's job, so the bool it takes is the thing being named

void check(Held outcome, const std::string& what)
{
    if (outcome == Held::No) {
        std::fprintf(stderr, "grant_file_version: FAIL — %s\n", what.c_str());
        ++headerFailures;
    }
}

} // namespace

int main()
{
    // What the daemon writes, the daemon reads, with or without the line end.
    for (const char* kind : { "consent", "capability-grants" }) {
        const std::string header = grantFileHeader(kind);
        check(held(isGrantFileHeader(header, kind)), std::string(kind) + ": its own header is recognised");
        check(held(isGrantFileHeader(header.substr(0, header.size() - 1), kind)), std::string(kind) + ": without its line end too");
    }

    // One kind's header is not another's. Both files live in one directory and
    // hold different records; reading either as the other would range-check
    // garbage into the grant store.
    check(held(!isGrantFileHeader(grantFileHeader("consent"), "capability-grants")),
        "a consent header is not read as a capability-grants one");
    check(held(!isGrantFileHeader(grantFileHeader("capability-grants"), "consent")),
        "and the reverse");

    // A file whose first line is a record has no header, and the caller
    // refuses it whole.
    check(held(!isGrantFileHeader("token:abc123\n", "consent")),
        "a consent record is not a header");
    check(held(!isGrantFileHeader("3\t1\ttoken:abc123\n", "capability-grants")),
        "a capability-grants record is not a header");
    check(held(!isGrantFileHeader("", "consent")), "an empty line is not a header");

    // Any other layout is not this build's, however its version is spelled.
    for (const char* line : { "# ubiquitty consent v2\n", "# ubiquitty consent v0\n", "# ubiquitty consent v10\n",
             "# ubiquitty consent vX\n", "# ubiquitty consent v1 extra\n", "# ubiquitty consent v\n" })
        check(held(!isGrantFileHeader(line, "consent")), std::string("not this build's header: ") + line);

    if (headerFailures == 0) {
        std::printf("grant_file_version: a grant file opens with its kind's header; its own header is "
                    "recognised, one kind's is not another's, and a record or any other layout is not a header\n");
        return 0;
    }
    std::fprintf(stderr, "grant_file_version: %d failure(s)\n", headerFailures);
    return 1;
}
