// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_login_session — the PAL seam a window's shell is registered through.
//
// The two registration kinds are mutually exclusive, and each leaves the other
// kind's call inert. A build that answered one kind while implementing the
// other would register nothing and report nothing.
//
// `registerLoginSession` may refuse: the session table is group-writable by a
// group a desktop process is usually not in. A refusal is a valid answer and a
// claimed write that nothing can read back is not, so the case below asserts
// the agreement rather than the outcome.

#include "Harness.h"

#include "PAL/PAL.h"

#include <cstring>
#include <string>

#if defined(__linux__)
#include <cstdlib>
#include <unistd.h>
#include <utmpx.h>
#endif

namespace {

namespace P = Ubiquitty::PAL;

// A line no real pseudo-terminal occupies, so a record found under it was
// written by this test and not inherited from the session running it.
constexpr char probePTY[] = "/dev/pts/4094";

std::string palString(int (*query)(char*, int))
{
    char buffer[1024];
    const int n = query(buffer, static_cast<int>(sizeof(buffer)));
    return n > 0 ? std::string(buffer, static_cast<std::size_t>(n)) : std::string();
}

#if defined(__linux__)
bool sessionTableHasProbe()
{
    bool found = false;
    ::setutxent();
    while (const utmpx* entry = ::getutxent()) {
        if (entry->ut_type == USER_PROCESS && std::strcmp(entry->ut_line, "pts/4094") == 0)
            found = true;
    }
    ::endutxent();
    return found;
}
#endif

} // namespace

// The kind decides which call does anything, so a program that reads it can
// skip the other one entirely.
TEST(testTheKindAgreesWithWhichCallIsImplemented)
{
    const P::LoginSessionKind kind = P::loginSessionKind();
    const bool hasProgram = !palString(&P::loginSessionProgram).empty();

    CHECK(hasProgram == (kind == P::LoginSessionKind::ThroughLoginProgram));

    // Nothing is registered where nothing registers, whichever call is made.
    if (kind == P::LoginSessionKind::Unregistered)
        CHECK(!P::registerLoginSession(probePTY));
}

// The program is named by absolute path: a launch execs it directly, so a bare
// name would be resolved against a PATH the child has not been given yet.
TEST(testTheLoginProgramIsAnAbsolutePath)
{
    const std::string program = palString(&P::loginSessionProgram);
    if (program.empty())
        return;
    CHECK(program.front() == '/');
}

// A name is required to register a session, so a platform that registers must
// be able to answer one.
TEST(testARegisteringPlatformKnowsTheUsersName)
{
    if (P::loginSessionKind() == P::LoginSessionKind::Unregistered)
        return;
    CHECK(!palString(&P::systemLoginUserName).empty());
}

// Every string seam refuses a buffer it cannot fit rather than truncating: a
// truncated path execs the wrong program and a truncated name registers the
// wrong user.
TEST(testTheStringSeamsRefuseAShortBuffer)
{
    char oneByte[1];
    CHECK(P::loginSessionProgram(oneByte, 1) == -1);
    CHECK(P::systemLoginUserName(oneByte, 1) == -1);
    CHECK(P::loginSessionProgram(nullptr, 64) == -1);
    CHECK(P::systemLoginUserName(nullptr, 64) == -1);
}

#if defined(__linux__)
// A true answer means the record is readable, and a false one means the table
// is untouched. Either is correct; disagreeing between them is not, and it is
// the disagreement that would leave `who` listing a window that never opened.
TEST(testAClaimedRegistrationIsReadableAndIsRemoved)
{
    CHECK(P::loginSessionKind() == P::LoginSessionKind::ByRecordingThePTY);

    const bool didWrite = P::registerLoginSession(probePTY);
    CHECK(didWrite == sessionTableHasProbe());

    P::unregisterLoginSession(probePTY);
    CHECK(!sessionTableHasProbe());
}

// The case above is satisfied by a refusal, and a refusal is what an
// unprivileged process gets against the real table — so on its own it never
// reaches the code that packs a record. Redirecting the table to a file this
// process owns makes the write happen, which is the half that can be wrong:
// the line and id are derived from the device path here, and the removal has
// to match on exactly what the write stored.
TEST(testARecordRoundTripsThroughAWritableTable)
{
    char path[] = "/tmp/ubq-session-table-XXXXXX";
    const int fileDescriptor = ::mkstemp(path);
    CHECK(fileDescriptor >= 0);
    if (fileDescriptor < 0)
        return;
    ::close(fileDescriptor);
    CHECK(::utmpxname(path) == 0);

    CHECK(P::registerLoginSession(probePTY));
    CHECK(sessionTableHasProbe());

    // The user is named, or `who` prints a session belonging to nobody.
    std::string recorded;
    ::setutxent();
    while (const utmpx* entry = ::getutxent()) {
        if (entry->ut_type == USER_PROCESS && std::strcmp(entry->ut_line, "pts/4094") == 0)
            recorded.assign(entry->ut_user, ::strnlen(entry->ut_user, sizeof(entry->ut_user)));
    }
    ::endutxent();
    CHECK(recorded == palString(&P::systemLoginUserName));

    P::unregisterLoginSession(probePTY);
    CHECK(!sessionTableHasProbe());

    ::unlink(path);
}
#endif

UBIQUITTY_TEST_MAIN()
