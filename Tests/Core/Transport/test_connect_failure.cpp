// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_connect_failure.cpp — a failed connect, read as the sentence a person
// sees.
//
// The cases pin both halves: the system error code each failure arrives as is
// read as the fact it reports, and each fact's sentence names the host and the
// port the person typed. A refusal that read as "the connection failed" sends
// the person to the network when the fault is a port with nothing listening.

#include "Core/Transport/ConnectFailure.h"

#include <cerrno>
#include <string>

using namespace Ubiquitty::Transport;

// ---- The codes a connect fails with ----
TEST(test_connect_failure_classifies_errno)
{
    CHECK_MSG(classifyConnectError(ECONNREFUSED) == ConnectFailure::Refused, "ECONNREFUSED is a refusal");
    CHECK_MSG(classifyConnectError(ETIMEDOUT) == ConnectFailure::TimedOut, "ETIMEDOUT is a timeout");
    CHECK_MSG(classifyConnectError(ECONNRESET) == ConnectFailure::Reset, "ECONNRESET is a reset");
    CHECK_MSG(classifyConnectError(EHOSTUNREACH) == ConnectFailure::HostUnreachable, "EHOSTUNREACH is no route");
    CHECK_MSG(classifyConnectError(ENETUNREACH) == ConnectFailure::NetworkUnreachable, "ENETUNREACH is no network");
    CHECK_MSG(classifyConnectError(0) == ConnectFailure::Other, "no error code has no specific reading");
    CHECK_MSG(classifyConnectError(EACCES) == ConnectFailure::Other, "an unrelated code has no specific reading");
}

// ---- The sentence names what the person typed ----
TEST(test_connect_failure_sentences)
{
    const std::string refused = describeConnectFailure(classifyConnectError(ECONNREFUSED), "127.0.0.1", 1);
    CHECK_MSG(refused == "127.0.0.1 refused the connection on port 1.", refused.c_str());

    const std::string timedOut = describeConnectFailure(ConnectFailure::TimedOut, "build.example", 2222);
    CHECK_MSG(timedOut == "build.example did not answer on port 2222.", timedOut.c_str());

    const std::string unresolved = describeConnectFailure(ConnectFailure::NameNotResolved, "nowhere.invalid", 22);
    CHECK_MSG(unresolved == "No host named nowhere.invalid could be found.", unresolved.c_str());

    const std::string noRoute = describeConnectFailure(ConnectFailure::HostUnreachable, "10.0.0.9", 22);
    CHECK_MSG(noRoute == "10.0.0.9 cannot be reached from this network.", noRoute.c_str());

    const std::string other = describeConnectFailure(ConnectFailure::Other, "[::1]", 22);
    CHECK_MSG(other == "The connection to [::1] on port 22 failed.", other.c_str());
}

// ---- No sentence leaks the error's own spelling ----
TEST(test_connect_failure_sentences_carry_no_raw_error)
{
    for (const auto failure : { ConnectFailure::Refused, ConnectFailure::TimedOut, ConnectFailure::Reset, ConnectFailure::HostUnreachable,
             ConnectFailure::NetworkUnreachable, ConnectFailure::NameNotResolved, ConnectFailure::Other }) {
        const std::string sentence = describeConnectFailure(failure, "host.example", 22);
        CHECK_MSG(sentence.find("host.example") != std::string::npos, sentence.c_str());
        CHECK_MSG(sentence.find("errno") == std::string::npos, sentence.c_str());
        CHECK_MSG(sentence.back() == '.', sentence.c_str());
    }
}

// ---- A refusal is an answer; everything else may be a path still coming up ----
TEST(test_connect_failure_only_a_refusal_is_final)
{
    CHECK_MSG(!isTransientConnectFailure(ConnectFailure::Refused), "a refusal is the host's answer, not a path coming up");
    for (const auto failure : { ConnectFailure::TimedOut, ConnectFailure::Reset, ConnectFailure::HostUnreachable,
             ConnectFailure::NetworkUnreachable, ConnectFailure::NameNotResolved, ConnectFailure::Other }) {
        CHECK_MSG(isTransientConnectFailure(failure), describeConnectFailure(failure, "host.example", 22).c_str());
    }
}

// ---- A transient failure is dialled again a bounded number of times ----
TEST(test_connect_failure_retries_are_bounded)
{
    const auto first = connectRetryDelaySeconds(1);
    const auto second = connectRetryDelaySeconds(2);
    CHECK_MSG(first.has_value() && *first == 1.0, "the first failure waits a second before dialling again");
    CHECK_MSG(second.has_value() && *second == 2.0, "the second failure waits longer");
    CHECK_MSG(!connectRetryDelaySeconds(3).has_value(), "the third failure is reported to the person");
    CHECK_MSG(!connectRetryDelaySeconds(0).has_value(), "nothing failed, so nothing is dialled again");
}

UBIQUITTY_TEST_MAIN()
