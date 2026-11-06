// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;
using UbiquittyTest::feed;

// test_osc52_per_term_policy — negative test: two simultaneously live Terms with
// DIFFERENT clipboard-read/write policies and master-toggle must behave
// independently. Policy lives in Terminal::osc52ClipboardReadPolicy /
// osc52ClipboardWritePolicy / isOSC52Allowed; handleClipboard reads exclusively from
// those fields — no process-global is consulted on the ingest thread.
//
// Fails-without-it: any fallback to a shared global causes the ALLOW Ubiquitty::Terminal to
// observe the DENY setting from its sibling and produces no reply (CHECK(n > 0)
// trips).

#include "Core/Codec/Base64.h"
#include "Core/OSC/Dispatch.h"
#include "Core/TerminalOperations.h"
#include "PAL/TerminalClients.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Minimal clipboard stub: every read returns "Hi" (2 bytes, base64 = "SGk=").
struct FixedClipboard final : Ubiquitty::ClipboardClient {
    int writeCalls = 0;
    std::size_t lastWriteLength = 0;

    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> data) override
    {
        ++writeCalls;
        lastWriteLength = data.size();
    }

    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> outputBuffer) override
    {
        const char val[] = "Hi";
        int n = 2;
        if (n > static_cast<int>(outputBuffer.size()))
            n = static_cast<int>(outputBuffer.size());
        std::memcpy(outputBuffer.data(), val, static_cast<size_t>(n));
        return n;
    }
};

// Two Terms: one with Allow read-policy, one with Deny read-policy. The Allow Terminal
// must produce a reply; the Deny Ubiquitty::Terminal must produce an empty reply. No global setter
// is called — policies live entirely on the Ubiquitty::Terminal fields.
TEST(testTwoTermsIndependentReadPolicy)
{
    FixedClipboard clipAllow, clipDeny;

    Ubiquitty::Terminal tAllow, tDeny;
    Ubiquitty::hardReset(tAllow);
    Ubiquitty::hardReset(tDeny);

    tAllow.clipboardClient = &clipAllow;
    tDeny.clipboardClient = &clipDeny;

    // Per-Ubiquitty::Terminal policy assignment — the key invariant under test.
    tAllow.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    tDeny.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;

    char buf[64];
    int n;

    // Allow Terminal: OSC 52 read query must produce a framed, non-empty reply.
    feed(tAllow, "\033]52;c;?\033\\");
    n = drain(tAllow, buf);
    CHECK(n > 0);
    CHECK(std::strstr(buf, "\033]52;c;SGk=\033\\") != nullptr);

    // Deny Terminal: must produce an empty-data reply (no contents), not silence.
    feed(tDeny, "\033]52;c;?\033\\");
    n = drain(tDeny, buf);
    CHECK(n > 0); // well-formed empty reply so the requester doesn't hang
    CHECK(std::strstr(buf, "\033]52;c;\033\\") != nullptr); // empty data field
    CHECK(std::strstr(buf, "SGk=") == nullptr); // never leaks clipboard bytes

    tAllow.clipboardClient = nullptr;
    tDeny.clipboardClient = nullptr;
}

// Two Terms: one with Allow write-policy, one with Deny. Writes to the Allow Terminal
// reach the clipboard client; writes to the Deny Ubiquitty::Terminal are silently dropped.
TEST(testTwoTermsIndependentWritePolicy)
{
    FixedClipboard clipAllow, clipDeny;

    Ubiquitty::Terminal tAllow, tDeny;
    Ubiquitty::hardReset(tAllow);
    Ubiquitty::hardReset(tDeny);

    tAllow.clipboardClient = &clipAllow;
    tDeny.clipboardClient = &clipDeny;

    tAllow.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;
    tDeny.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;

    // Allow Terminal: write reaches the client.
    feed(tAllow, "\033]52;c;SGk=\033\\"); // "Hi"
    CHECK(clipAllow.writeCalls == 1);

    // Deny Terminal: write is silently consumed, never reaches the client.
    feed(tDeny, "\033]52;c;SGk=\033\\");
    CHECK(clipDeny.writeCalls == 0);

    tAllow.clipboardClient = nullptr;
    tDeny.clipboardClient = nullptr;
}

// Two Terms: one with isOSC52Allowed=true, one with isOSC52Allowed=false. The disabled
// Ubiquitty::Terminal must not write or respond to reads regardless of other policy fields.
TEST(testTwoTermsIndependentMasterToggle)
{
    FixedClipboard clipOn, clipOff;

    Ubiquitty::Terminal tOn, tOff;
    Ubiquitty::hardReset(tOn);
    Ubiquitty::hardReset(tOff);

    tOn.clipboardClient = &clipOn;
    tOff.clipboardClient = &clipOff;

    tOn.isOSC52Allowed = true;
    tOff.isOSC52Allowed = false;
    // Set read policy to Allow on both so the on-Ubiquitty::Terminal replies inline.
    tOn.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    tOff.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    char buf[64];
    int n;

    // tOn: write reaches client + read query yields reply.
    feed(tOn, "\033]52;c;SGk=\033\\");
    CHECK(clipOn.writeCalls == 1);

    feed(tOn, "\033]52;c;?\033\\");
    n = drain(tOn, buf);
    CHECK(n > 0);

    // tOff: write dropped, read query produces no response.
    feed(tOff, "\033]52;c;SGk=\033\\");
    CHECK(clipOff.writeCalls == 0);

    feed(tOff, "\033]52;c;?\033\\");
    n = drain(tOff, buf);
    CHECK(n == 0);

    tOn.clipboardClient = nullptr;
    tOff.clipboardClient = nullptr;
}

// A frontend that answers a read query asynchronously: it takes the request and
// replies later, which is what every real one does — the pasteboard has to be
// captured on a UI thread, and under Prompt a person has to answer first.
struct DeferringClipboard final : Ubiquitty::ClipboardClient {
    int requests = 0;
    bool accept = true; // false models a frontend with no window to ask in

    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> outputBuffer) override
    {
        const char val[] = "Hi";
        int n = 2;
        if (n > static_cast<int>(outputBuffer.size()))
            n = static_cast<int>(outputBuffer.size());
        std::memcpy(outputBuffer.data(), val, static_cast<size_t>(n));
        return n;
    }

    [[nodiscard]] bool requestClipboardRead(Ubiquitty::ClipboardBuffer, std::string_view, Ubiquitty::ClipboardReadConfirmation) override
    {
        ++requests;
        return accept;
    }
};

// A clipboard larger than one reply buffer comes back whole.
//
// The spec's read query returns the selection base64-encoded. A reply sized to
// the fixed 256-byte output buffer carried the first 168 bytes and said nothing
// about the rest, so an editor yanking the system clipboard over a link
// received a shorter string than the one on the clipboard — silently, which is
// the part that makes it a corruption rather than a limit.
//
// The sizes below straddle the clipboard bound and the reply buffer, and the content
// is a repeating pattern so a decode that dropped or duplicated a run shows up
// as the wrong bytes rather than merely the wrong length.
struct SizedClipboard final : Ubiquitty::ClipboardClient {
    std::string contents;

    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char> outputBuffer) override
    {
        if (contents.size() > outputBuffer.size())
            return -1;
        std::memcpy(outputBuffer.data(), contents.data(), contents.size());
        return static_cast<int>(contents.size());
    }
};

TEST(testClipboardReadReplyCarriesTheWholeSelection)
{
    for (const std::size_t length : { std::size_t(1), std::size_t(167), std::size_t(168), std::size_t(169),
             std::size_t(255), std::size_t(1024), std::size_t(64 * 1024) }) {
        SizedClipboard clip;
        clip.contents.resize(length);
        for (std::size_t i = 0; i < length; ++i)
            clip.contents[i] = static_cast<char>('A' + (i % 26));

        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        t.clipboardClient = &clip;
        t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

        feed(t, "\033]52;c;?\033\\");

        // Drained the way a frontend drains: repeatedly, into a fixed span,
        // until the terminal has nothing more to say. A reply that outgrows one
        // drain is not a reply that was dropped.
        std::string reply;
        char chunk[512];
        for (int guard = 0; guard < 4096; ++guard) {
            const int n = Ubiquitty::drainOutput(&t, chunk);
            if (n <= 0)
                break;
            reply.append(chunk, static_cast<std::size_t>(n));
        }

        char what[64];
        std::snprintf(what, sizeof(what), "a %zu-byte clipboard", length);

        const std::string head = "\033]52;c;";
        CHECK_MSG(reply.size() > head.size() + 2, what);
        CHECK_MSG(reply.compare(0, head.size(), head) == 0, what);
        CHECK_MSG(reply.compare(reply.size() - 2, 2, "\033\\") == 0, what);

        // The body decodes back to exactly what the clipboard held. Comparing
        // the decode rather than the length is what makes this a round-trip:
        // a reply of the right size holding the wrong bytes passes a length
        // check and fails a user.
        const std::string body = reply.substr(head.size(), reply.size() - head.size() - 2);
        std::vector<char> decoded(body.size());
        const auto roundTrip = Ubiquitty::Codec::base64Decode(body, decoded);
        CHECK_MSG(roundTrip && roundTrip->size() == length, what);
        if (roundTrip && roundTrip->size() == length)
            CHECK_MSG(std::memcmp(roundTrip->data(), clip.contents.data(), length) == 0, what);

        t.clipboardClient = nullptr;
    }
}

// One read query may be outstanding with the frontend at a time.
//
// The query is answered asynchronously, so the stream that asked keeps running:
// the number a stream can have in flight is the number of requests the frontend
// allocates and — under Prompt — dialogs it stacks. `printf` in a loop outruns
// anyone dismissing them, and the loop needs no privilege beyond writing to the
// terminal, which is what every program on the far end of the PTY can do.
//
// Fails-without-it: every query reaches the frontend, so `requests` counts all
// five instead of one.
TEST(testOnlyOneClipboardReadQueryIsOutstandingAtATime)
{
    DeferringClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    char buf[256];

    feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requests == 1);
    CHECK(drain(t, buf) == 0); // nothing to say yet — the frontend holds it

    // Four more while the first is outstanding. None reaches the frontend, and
    // each is answered empty rather than dropped: the sender is waiting on a
    // reply, and silence hangs it just as surely as a stacked dialog blocks the
    // user.
    for (int i = 0; i < 4; ++i)
        feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requests == 1);
    int n = drain(t, buf);
    CHECK(n > 0);
    CHECK(std::strstr(buf, "SGk=") == nullptr); // no clipboard bytes leaked past the cap

    // A stream that can flood the query can emit RIS between two of them. If the
    // reset cleared the flag, the cap would be something the sender turns off.
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;
    feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requests == 1);

    // The frontend answers, and the next query goes out normally.
    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    (void)drain(t, buf);
    feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requests == 2);

    t.clipboardClient = nullptr;
}

// A frontend that cannot take the request — no window to prompt in — leaves the
// sender an empty reply rather than silence, and does not latch the cap against
// a query it never accepted.
TEST(testDeclinedClipboardReadRequestStillAnswersTheSender)
{
    DeferringClipboard clip;
    clip.accept = false;

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    char buf[256];
    feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requests == 1);
    int n = drain(t, buf);
    CHECK(n > 0); // answered, so the sender proceeds
    CHECK(std::strstr(buf, "\033]52;c;\033\\") != nullptr); // empty data field
    CHECK(std::strstr(buf, "SGk=") == nullptr);

    // Nothing is outstanding, so the next query reaches the frontend too.
    feed(t, "\033]52;c;?\033\\");
    CHECK(clip.requests == 2);

    t.clipboardClient = nullptr;
}

// The data field is the rest of the payload, and the payload accumulator stores
// a NUL like any other byte. A handler that measures the field with strlen reads
// a shorter base64 body than the one that arrived and writes what it decodes, so
// the bytes a sender put behind a NUL decide nothing and the ones in front reach
// the clipboard on their own.
TEST(testClipboardWriteDecodesTheWholeDataField)
{
    FixedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;

    // "SGk=" alone is a well-formed write.
    feed(t, "\033]52;c;SGk=\033\\");
    CHECK(clip.writeCalls == 1);
    CHECK_EQ(clip.lastWriteLength, 2u);

    // The same bytes followed by a NUL are not: the field carries a byte outside
    // the base64 alphabet, so the decode refuses it and the selection is
    // cleared rather than given the bytes in front of the NUL.
    static constexpr char hidden[] = "\033]52;c;SGk=\0junk\033\\";
    feed(t, std::string_view(hidden, sizeof(hidden) - 1));
    CHECK(clip.writeCalls == 2);
    CHECK_EQ(clip.lastWriteLength, 0u);

    t.clipboardClient = nullptr;
}

// A completion with nothing outstanding is stray: a duplicate callback, or one
// racing teardown answering into state no query owns. Answering it would emit
// an unsolicited reply the stream never asked for, so it is dropped — no
// reply bytes, no latch, and the next real query still reaches the frontend.
TEST(testStrayClipboardReadCompletionIsDropped)
{
    FixedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    char buf[256];
    CHECK(!t.isOSC52ClipboardReadInFlight);

    // No query was ever asked: the completion must not answer one.
    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    CHECK(drain(t, buf) == 0);
    CHECK(!t.isOSC52ClipboardReadInFlight);

    // The surface still works: a real query after the stray one is answered.
    feed(t, "\033]52;c;?\033\\");
    int n = drain(t, buf);
    CHECK(n > 0);
    CHECK(std::strstr(buf, "\033]52;c;SGk=\033\\") != nullptr);

    t.clipboardClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
