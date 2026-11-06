// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/test_peer_process_instance.cpp — a process id is read together
// with something that changes when it is reused.
//
// The daemon decides a peer is local by identifying the binary the peer's
// process id is running. A process id is a number the kernel hands out again,
// so anything derived from one describes whoever holds it at the moment of the
// call, and two calls a syscall apart can describe two different programs. The
// instance value is what makes the pair comparable.
//
// On Linux it comes from field 22 of /proc/<pid>/stat, and the first
// twenty-one fields cannot be found by counting spaces from the left. Field 2
// is the executable's name in parentheses, the kernel does not escape it, and
// a process is free to call itself `sh) 1 2 3 4`. A reader counting from the
// left then takes its number from a string the process chose — which for a
// value whose whole job is detecting substitution is itself the substitution.
//
// What the kernel does guarantee is that the name's closing parenthesis is the
// last one on the line, so the scan starts there. That is the property this
// pins, on inputs a hostile process can actually produce.
//
// Exit 0 on success.
//

#include "Platform/Host/PeerProcessInstance.h"

#include <cstdio>
#include <string>

using Ubiquitty::Host::parseLinuxProcessStartTime;

namespace {

int parseFailures = 0;

void check(bool isSo, const std::string& what) // bool-ok: every line below is one condition, and the message names it
{
    if (!isSo) {
        std::fprintf(stderr, "peer_process_instance: FAIL — %s\n", what.c_str());
        ++parseFailures;
    }
}

// A /proc/<pid>/stat line whose executable name is `name`, with the start time
// in field 22 and every other field distinct so a miscount lands on a value
// this test can recognise as wrong.
std::string statLine(const std::string& name, std::uint64_t startTime)
{
    std::string line = "1234 (" + name + ") S";
    for (int field = 4; field <= 21; ++field)
        line += " " + std::to_string(field);
    line += " " + std::to_string(startTime);
    for (int field = 23; field <= 30; ++field)
        line += " " + std::to_string(field);
    return line + "\n";
}

} // namespace

int main()
{
    std::uint64_t value = 0;

    // The ordinary line.
    check(parseLinuxProcessStartTime(statLine("bash", 8675309), value), "an ordinary line parses");
    check(value == 8675309, "and yields field 22 rather than a neighbour");

    // A name with a space. Counting fields from the left shifts by one here.
    value = 0;
    check(parseLinuxProcessStartTime(statLine("my program", 4242), value), "a name containing a space parses");
    check(value == 4242, "and the space does not shift the field");

    // A name that closes its own parenthesis and writes plausible fields after
    // it. The scan must find the LAST parenthesis, not the first.
    value = 0;
    check(parseLinuxProcessStartTime(statLine("sh) S 1 2 3 4 5 6 7 8 9", 99), value),
        "a name containing a closing parenthesis parses");
    check(value == 99, "and the fields the name supplied are not mistaken for the kernel's");

    // A name that is entirely parentheses and spaces — nothing outside the
    // last one is trusted.
    value = 0;
    check(parseLinuxProcessStartTime(statLine(") ) ) (", 7), value), "a name of parentheses parses");
    check(value == 7, "and still yields the kernel's field");

    // Malformed input is refused rather than guessed at. A caller that cannot
    // read an instance keeps what it had; one handed a wrong number would
    // compare two unrelated values and call a substitution a match.
    value = 0;
    check(!parseLinuxProcessStartTime("1234 no parenthesis here at all", value), "a line with no parenthesis is refused");
    check(!parseLinuxProcessStartTime("", value), "an empty line is refused");
    check(!parseLinuxProcessStartTime("1234 (sh) S 1 2 3", value), "a line that ends before field 22 is refused");
    check(!parseLinuxProcessStartTime(statLine("sh", 0).substr(0, 12), value), "a truncated line is refused");

    // A non-numeric field 22 is refused rather than read as zero: zero is a
    // value two reads could agree on.
    {
        std::string line = "1234 (sh) S";
        for (int field = 4; field <= 21; ++field)
            line += " " + std::to_string(field);
        line += " notanumber 23 24\n";
        value = 12345;
        check(!parseLinuxProcessStartTime(line, value), "a non-numeric start time is refused");
        check(value == 12345, "and the caller's value is left alone");
    }

    if (parseFailures == 0) {
        std::printf("peer_process_instance: a process id is paired with a value that changes when it is reused, "
                    "read from the last parenthesis rather than by counting fields a process can choose, "
                    "and refused rather than guessed when the line does not carry one\n");
        return 0;
    }
    std::fprintf(stderr, "peer_process_instance: %d failure(s)\n", parseFailures);
    return 1;
}
