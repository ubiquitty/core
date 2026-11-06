// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

// test_extended_clipboard.cpp — OSC 5522 (Core/OSC/ExtendedClipboard.cpp) and
// the bulk reply channel it queues through (respondBulk / drainOutput). Every
// sequence is fed through the real parser, so the dispatch row, the metadata
// parse, the policy gates, the deferred completion routing and the framing are
// all exercised on the path a host actually reaches them by.

#include "Core/Codec/Base64.h"
#include "Core/OSC/Dispatch.h"
#include "Core/TerminalOperations.h"
#include "Core/TerminalShared.h"
#include "Core/VT/Constants.h"
#include "PAL/TerminalClients.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

// A clipboard whose text is the test's to choose, recording which buffer each
// call addressed so the loc=primary routing is observable.
struct ScriptedClipboard final : Ubiquitty::ClipboardClient {
    std::string text = "Hi";
    std::string written;
    Ubiquitty::ClipboardBuffer lastReadBuffer = Ubiquitty::ClipboardBuffer::Count;
    Ubiquitty::ClipboardBuffer lastWriteBuffer = Ubiquitty::ClipboardBuffer::Count;
    int readCalls = 0;
    int readRequests = 0;
    Ubiquitty::ClipboardReadConfirmation lastConfirmation = Ubiquitty::ClipboardReadConfirmation::NotRequired;
    bool acceptsReadRequests = true;

    void writeClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<const char> bytes) override
    {
        lastWriteBuffer = buffer;
        written.assign(bytes.data(), bytes.size());
    }

    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer buffer, std::span<char> output) override
    {
        lastReadBuffer = buffer;
        ++readCalls;
        int n = static_cast<int>(text.size());
        if (n > static_cast<int>(output.size()))
            n = static_cast<int>(output.size());
        std::memcpy(output.data(), text.data(), static_cast<std::size_t>(n));
        return n;
    }

    [[nodiscard]] bool requestClipboardRead(Ubiquitty::ClipboardBuffer, std::string_view,
        Ubiquitty::ClipboardReadConfirmation confirmation) override
    {
        ++readRequests;
        lastConfirmation = confirmation;
        return acceptsReadRequests;
    }
};

// A clipboard that stores and serves typed items, so the per-MIME seam is
// observable end to end: what writeClipboardItems received, what each typed
// read was asked for, and whether the untyped adapter was bypassed.
struct TypedScriptedClipboard final : Ubiquitty::ClipboardClient {
    struct Item {
        std::string mime;
        std::string bytes;
    };
    std::vector<Item> stored;
    std::vector<Item> serving;
    Ubiquitty::ClipboardBuffer lastWriteBuffer = Ubiquitty::ClipboardBuffer::Count;
    int untypedWriteCalls = 0;
    int untypedReadCalls = 0;
    int readRequests = 0;
    Ubiquitty::ClipboardReadConfirmation lastConfirmation = Ubiquitty::ClipboardReadConfirmation::NotRequired;
    bool isAsynchronous = false;

    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char>) override { ++untypedWriteCalls; }

    [[nodiscard]] int readClipboard(Ubiquitty::ClipboardBuffer, std::span<char>) override
    {
        ++untypedReadCalls;
        return -1;
    }

    void writeClipboardItems(Ubiquitty::ClipboardBuffer buffer, std::span<const Ubiquitty::ClipboardItem> items) override
    {
        lastWriteBuffer = buffer;
        stored.clear();
        for (const Ubiquitty::ClipboardItem& item : items)
            stored.push_back({ std::string { item.mime }, std::string { item.bytes.data(), item.bytes.size() } });
    }

    [[nodiscard]] int readClipboardTypes(Ubiquitty::ClipboardBuffer, std::span<char> out) override
    {
        std::string list;
        for (const Item& item : serving) {
            if (!list.empty())
                list += ' ';
            list += item.mime;
        }
        const int n = std::min(static_cast<int>(list.size()), static_cast<int>(out.size()));
        std::memcpy(out.data(), list.data(), static_cast<std::size_t>(n));
        return n;
    }

    [[nodiscard]] int readClipboardItem(Ubiquitty::ClipboardBuffer, std::string_view mime, std::span<char> out) override
    {
        for (const Item& item : serving) {
            if (item.mime != mime)
                continue;
            const int n = std::min(static_cast<int>(item.bytes.size()), static_cast<int>(out.size()));
            std::memcpy(out.data(), item.bytes.data(), static_cast<std::size_t>(n));
            return n;
        }
        return -1;
    }

    [[nodiscard]] bool requestClipboardRead(Ubiquitty::ClipboardBuffer, std::string_view,
        Ubiquitty::ClipboardReadConfirmation confirmation) override
    {
        ++readRequests;
        lastConfirmation = confirmation;
        return true;
    }

    [[nodiscard]] bool requiresAsynchronousClipboardRead() override { return isAsynchronous; }

    [[nodiscard]] const Item* storedItem(std::string_view mime) const
    {
        for (const Item& item : stored) {
            if (item.mime == mime)
                return &item;
        }
        return nullptr;
    }
};

// Drains everything the terminal has queued, across both channels, however
// large. The fixture's drain() takes one small bite; a chunked reply is many.
std::string drainAll(Ubiquitty::Terminal& t)
{
    std::string all;
    char buffer[4096];
    int n;
    while ((n = Ubiquitty::drainOutput(&t, buffer)) > 0)
        all.append(buffer, static_cast<std::size_t>(n));
    return all;
}

// Counts non-overlapping occurrences, because a stream's packet count is the
// assertion several cases turn on.
int countOccurrences(std::string_view haystack, std::string_view needle)
{
    int count = 0;
    std::size_t at = 0;
    while ((at = haystack.find(needle, at)) != std::string_view::npos) {
        ++count;
        at += needle.size();
    }
    return count;
}

// b64("text/plain") — the mime every DATA packet of this slice names.
constexpr std::string_view textPlainB64 = "dGV4dC9wbGFpbg==";

} // namespace

TEST(testReadAllowStreamsOKDataDone)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    const std::string reply = drainAll(t);

    CHECK(reply.find("\033]5522;type=read:status=OK\033\\") != std::string::npos);
    // "Hi" → "SGk=", under the text/plain mime.
    CHECK(reply.find("\033]5522;type=read:status=DATA:mime=" + std::string(textPlainB64) + ";SGk=\033\\") != std::string::npos);
    CHECK(reply.find("\033]5522;type=read:status=DONE\033\\") != std::string::npos);
    // The stream is ordered: `OK` before `DATA` before `DONE`.
    CHECK(reply.find("status=OK") < reply.find("status=DATA"));
    CHECK(reply.find("status=DATA") < reply.find("status=DONE"));
    t.clipboardClient = nullptr;
}

TEST(testReadDenyAnswersEPERM)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    const std::string reply = drainAll(t);

    CHECK(reply == "\033]5522;type=read:status=EPERM\033\\");
    CHECK(clip.readRequests == 0); // denied before the frontend hears of it
    t.clipboardClient = nullptr;
}

TEST(testListUnderDenyAnswersEPERMNotTheList)
{
    // Deny denies everything, list included: the type list answers which MIME
    // aliases are served and that a clipboard is live, which is the oracle a
    // deny policy exists to withhold. The spec's consent exemption for listing
    // survives for Allow/Prompt (testListIsAnsweredWithoutAPrompt); an
    // explicit Deny is refusal, not absent consent.
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;

    feed(t, "\033]5522;type=read;Lg==\033\\"); // payload b64(".")
    const std::string reply = drainAll(t);

    CHECK(reply == "\033]5522;type=read:status=EPERM\033\\");
    CHECK(clip.readRequests == 0); // denied before the frontend hears of it
    t.clipboardClient = nullptr;
}

TEST(testListIsAnsweredWithoutAPrompt)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    // Prompt policy, and still no prompt: listing discloses what the terminal
    // is, not what the clipboard holds.
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    feed(t, "\033]5522;type=read;Lg==\033\\"); // payload b64(".")
    const std::string reply = drainAll(t);

    CHECK(clip.readRequests == 0);
    CHECK(reply.find("status=OK") != std::string::npos);
    // The list rides a DATA packet under the "." pseudo-mime, naming the one
    // type this slice serves.
    CHECK(reply.find(":mime=Lg==;" + std::string(textPlainB64) + "\033\\") != std::string::npos);
    CHECK(reply.find("status=DONE") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testReadPromptDefersThenStreamsOnApproval)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    feed(t, "\033]5522;type=read:id=q1;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t).empty()); // nothing until the user answers
    CHECK(clip.readRequests == 1);
    CHECK(clip.lastConfirmation == Ubiquitty::ClipboardReadConfirmation::Required);
    CHECK(t.isOSC52ClipboardReadInFlight);

    // A second read while one is outstanding has a word, and it is not a
    // second prompt.
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t) == "\033]5522;type=read:status=EBUSY\033\\");
    CHECK(clip.readRequests == 1);

    // The frontend approves through the one completion call it already makes
    // for OSC 52; the routing frames the answer as 5522, id echoed.
    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    const std::string reply = drainAll(t);
    CHECK(!t.isOSC52ClipboardReadInFlight);
    CHECK(reply.find("\033]5522;type=read:status=OK:id=q1\033\\") != std::string::npos);
    CHECK(reply.find(":id=q1;SGk=\033\\") != std::string::npos);
    CHECK(reply.find("\033]5522;type=read:status=DONE:id=q1\033\\") != std::string::npos);
    // Never the other protocol's framing.
    CHECK(reply.find("\033]52;") == std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testReadPromptRefusalAnswersEPERM)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t).empty());
    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Refused);
    CHECK(drainAll(t) == "\033]5522;type=read:status=EPERM\033\\");
    CHECK(!t.isOSC52ClipboardReadInFlight);
    t.clipboardClient = nullptr;
}

TEST(testDeclinedRequestClearsTheInFlightFlag)
{
    ScriptedClipboard clip;
    clip.acceptsReadRequests = false;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t) == "\033]5522;type=read:status=EPERM\033\\");
    // A frontend with no window to ask in must not leave the invariant held,
    // or every later read on this terminal answers EBUSY forever.
    CHECK(!t.isOSC52ClipboardReadInFlight);
    t.clipboardClient = nullptr;
}

TEST(testUnservableMimeAnswersAnEmptyCompleteStream)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=read;aW1hZ2UvcG5n\033\\"); // b64("image/png")
    const std::string reply = drainAll(t);
    CHECK(reply.find("status=OK") != std::string::npos);
    CHECK(reply.find("status=DONE") != std::string::npos);
    CHECK(reply.find("status=DATA") == std::string::npos);
    CHECK(clip.readRequests == 0); // no data crosses, so no consent is asked
    t.clipboardClient = nullptr;
}

TEST(testMalformedRequestPayloadIsDroppedSilently)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // The read section's error set is closed at ENOSYS/EPERM/EBUSY, none of
    // which undecodable base64 is; OSC 52's rule for an invalid request —
    // drop, no reply — applies.
    feed(t, "\033]5522;type=read;!!notbase64!!\033\\");
    CHECK(drainAll(t).empty());
    CHECK(clip.readRequests == 0);
    t.clipboardClient = nullptr;
}

TEST(testClientlessReadAnswersENOSYS)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // No clipboard client: the requested type is unavailable, and ENOSYS is
    // the read set's word for that — never EIO, which is the write's.
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t) == "\033]5522;type=read:status=ENOSYS\033\\");
}

TEST(testHardResetMidPromptKeepsTheReadExtended)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    // Ask under the plain name, noting the matched-mimes carry: the reply
    // after the reset must echo the same requested names it deferred with.
    feed(t, "\033]5522;type=read:id=q9;dGV4dC9wbGFpbg==\033\\");
    CHECK(t.isOSC52ClipboardReadInFlight);

    // RIS arrives while the consent dialog is up. The in-flight flag survives
    // a reset by design (the stream that floods the query can also emit RIS),
    // so the rest of the outstanding read must survive WITH it — a reset that
    // kept the flag and dropped the protocol would route the user's answer
    // into OSC 52 framing the requester cannot parse.
    feed(t, "\033c");
    CHECK(t.isOSC52ClipboardReadInFlight);
    t.clipboardClient = &clip; // the reset cleared the client binding
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    const std::string reply = drainAll(t);
    CHECK(reply.find("\033]5522;type=read:status=OK:id=q9\033\\") != std::string::npos);
    CHECK(reply.find("\033]52;") == std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testOverflowedSessionSwallowsLaterChunks)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    // The OSC accumulator caps a non-image payload at 64 KiB, so the cap is
    // reached the way a sender reaches it: many packets, each legal alone.
    // 60000 base64 characters decode to 45000 bytes; 187 packets pass 8 MiB.
    std::string packet = "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;";
    packet += std::string(60000, 'A');
    packet += "\033\\";
    for (int i = 0; i < 187 && !t.didExtendedClipboardWriteOverflow; ++i)
        feed(t, packet.c_str());
    CHECK(t.didExtendedClipboardWriteOverflow);

    // Later chunks are swallowed, not re-accumulated: the poison holds the
    // items at zero, or a sender withholding the terminator drives repeated
    // fill-and-reset cycles through a commit that is already lost.
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    CHECK(t.extendedClipboardWriteItemCount == 0);
    CHECK(t.extendedClipboardWriteItems[0].length == 0);
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t) == "\033]5522;type=write:status=EIO\033\\");
    CHECK(clip.written.empty());
    t.clipboardClient = nullptr;
}

TEST(testLongLegalIDEchoesWhole)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;

    // The spec's recommended generated id is a UUID4 — 36 characters — and a
    // multiplexer routes responses by matching the echo, so a truncated echo
    // routes nowhere.
    feed(t, "\033]5522;type=read:id=123e4567-e89b-12d3-a456-426614174000;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t) == "\033]5522;type=read:status=EPERM:id=123e4567-e89b-12d3-a456-426614174000\033\\");
    t.clipboardClient = nullptr;
}

TEST(testIDIsSanitizedBeforeItIsEchoed)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;

    // ':' is the response metadata's own separator, and '!' is outside the
    // id alphabet; both are stripped or bounded before the echo, so an id
    // cannot forge packet structure in the response stream. (';' cannot even
    // reach the id: it ends the metadata section at the parse.)
    feed(t, "\033]5522;type=read:id=q!!7:x=y;dGV4dC9wbGFpbg==\033\\");
    const std::string reply = drainAll(t);
    CHECK(reply.find(":id=q7\033\\") != std::string::npos);
    CHECK(reply.find("q!!7") == std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testPrimaryLocationRoutesToThePrimaryBuffer)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=read:loc=primary;dGV4dC9wbGFpbg==\033\\");
    (void)drainAll(t);
    CHECK(clip.lastReadBuffer == Ubiquitty::ClipboardBuffer::Primary);
    t.clipboardClient = nullptr;
}

TEST(testMultiChunkReadDecodesChunkByChunk)
{
    ScriptedClipboard clip;
    clip.text.assign(10000, 'x');
    clip.text[0] = 'A';
    clip.text[9999] = 'Z';
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    const std::string reply = drainAll(t);

    // 10000 bytes at 4096 per chunk is three packets, each padded on its own
    // so a consumer decodes packet by packet.
    CHECK(countOccurrences(reply, "status=DATA") == 3);
    // First chunk: 4096 'x's led by 'A' → base64 starts "eH" after 'A''s pair;
    // cheaper and stronger: decode by hand — the packets carry the whole text.
    // The boundary bytes prove ordering: 'A' appears only in the first
    // packet's payload, 'Z' only in the last.
    const std::size_t firstData = reply.find("status=DATA");
    const std::size_t lastData = reply.rfind("status=DATA");
    CHECK(firstData != lastData);
    CHECK(reply.find("QX") > firstData); // b64("Ax...") begins "QX"
    CHECK(reply.find("status=DONE") > lastData);
    t.clipboardClient = nullptr;
}

TEST(testWriteSessionCommitsTextAndAcksDone)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write:id=w7\033\\");
    // Two wdata chunks, independently padded ("Hello " + "world"), plus an
    // unsupported-mime chunk that must be dropped without ending the session.
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGVsbG8g\033\\");
    feed(t, "\033]5522;type=wdata:mime=aW1hZ2UvcG5n;aWdub3JlZA==\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;d29ybGQ=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");

    CHECK(clip.written == "Hello world");
    CHECK(clip.lastWriteBuffer == Ubiquitty::ClipboardBuffer::Clipboard);
    CHECK(drainAll(t) == "\033]5522;type=write:status=DONE:id=w7\033\\");
    t.clipboardClient = nullptr;
}

TEST(testWriteDenyAnswersEPERMAndWritesNothing)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;

    feed(t, "\033]5522;type=write\033\\");
    CHECK(drainAll(t) == "\033]5522;type=write:status=EPERM\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGVsbG8g\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.written.empty());
    const std::string reply = drainAll(t);
    CHECK(reply.empty());
    t.clipboardClient = nullptr;
}

TEST(testWriteDataWithoutASessionIsIgnored)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGVsbG8g\033\\");
    CHECK(drainAll(t).empty());
    t.clipboardClient = nullptr;
}

TEST(testMalformedWriteChunkPoisonsTheSession)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;!!!\033\\");
    CHECK(drainAll(t) == "\033]5522;type=write:status=EINVAL\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t).empty());
    CHECK(clip.written.empty());
    t.clipboardClient = nullptr;
}

TEST(testUnchunkedLargeWriteCommitsWhole)
{
    // The 4096-byte chunking is the REPLY side's ceiling; a writer's single
    // large payload is legal and bounded only by the session cap. 12 KiB of
    // base64 decodes through the transient heap path and commits intact.
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    std::string packet = "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;";
    packet += std::string(12288, 'A'); // decodes to 9216 bytes
    packet += "\033\\";
    feed(t, packet.c_str());
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t) == "\033]5522;type=write:status=DONE\033\\");
    CHECK(clip.written.size() == 9216);
    t.clipboardClient = nullptr;
}

TEST(testWritePrimaryLocationRoutesToThePrimaryBuffer)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write:loc=primary\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.written == "Hi");
    CHECK(clip.lastWriteBuffer == Ubiquitty::ClipboardBuffer::Primary);
    (void)drainAll(t);
    t.clipboardClient = nullptr;
}

TEST(testUnknownTypeIsConsumedSilently)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=future-subcommand;cGF5bG9hZA==\033\\");
    CHECK(drainAll(t).empty());
    // A walias outside a session is a session-flow packet with no session,
    // the same shape as an orphaned wdata.
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;YWxpYXM=\033\\");
    CHECK(drainAll(t).empty());
    t.clipboardClient = nullptr;
}

TEST(testAliasRoundTripServesTheTextUnderTheAliasedName)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // Write "Hi" as text, aliasing text/html to it.
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    // walias: target b64("text/plain"), names b64("text/html")
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    clip.text = clip.written; // the store now holds what was committed
    (void)drainAll(t);

    // A read of text/html — a name only the alias serves — answers the text
    // under the requested name, which is what an alias is.
    feed(t, "\033]5522;type=read;dGV4dC9odG1s\033\\");
    std::string reply = drainAll(t);
    // b64("text/html") = "dGV4dC9odG1s"
    CHECK(reply.find(":mime=dGV4dC9odG1s;SGk=\033\\") != std::string::npos);
    CHECK(reply.find("status=DONE") != std::string::npos);

    // Asking for both names yields one payload under each, the echo naming
    // what was asked.
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbiB0ZXh0L2h0bWw=\033\\"); // b64("text/plain text/html")
    reply = drainAll(t);
    CHECK(reply.find(":mime=" + std::string(textPlainB64) + ";SGk=\033\\") != std::string::npos);
    CHECK(reply.find(":mime=dGV4dC9odG1s;SGk=\033\\") != std::string::npos);
    CHECK(countOccurrences(reply, "status=DONE") == 1);

    // The list names the alias beside the type it stands for.
    feed(t, "\033]5522;type=read;Lg==\033\\");
    reply = drainAll(t);
    // b64("text/plain text/html") carried as the list payload
    CHECK(reply.find(";dGV4dC9wbGFpbiB0ZXh0L2h0bWw=\033\\") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testWriteUnderAnAliasedNameKeepsTheData)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    // The alias is declared first, then the payload arrives only under the
    // aliased name: a terminal that ignores the declaration stores nothing
    // for this write, which is the data loss the alias exists to prevent.
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9odG1s;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.written == "Hi");
    CHECK(drainAll(t).find("status=DONE") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testNextCommitWithoutAliasesClearsTheServedOnes)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);

    // A second commit that declares no aliases replaces the payload the old
    // names described, so they stop being served with it.
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;Qnk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);

    feed(t, "\033]5522;type=read;dGV4dC9odG1s\033\\");
    const std::string reply = drainAll(t);
    CHECK(reply.find("status=OK") != std::string::npos);
    CHECK(reply.find("status=DATA") == std::string::npos);
    CHECK(reply.find("status=DONE") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testNonTextAliasTargetRecordsNothing)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=write\033\\");
    // Target b64("image/png"): a payload this slice does not store cannot be
    // aliased, or the list would promise names the read cannot honour.
    feed(t, "\033]5522;type=walias:mime=aW1hZ2UvcG5n;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);

    feed(t, "\033]5522;type=read;dGV4dC9odG1s\033\\");
    const std::string reply = drainAll(t);
    CHECK(reply.find("status=DATA") == std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testMalformedAliasPacketEndsTheSession)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;!!!\033\\");
    CHECK(drainAll(t) == "\033]5522;type=write:status=EINVAL\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t).empty());
    t.clipboardClient = nullptr;
}

TEST(testMasterToggleSilencesTheProtocol)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.isOSC52Allowed = false;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    CHECK(drainAll(t).empty());
    CHECK(clip.readRequests == 0);
    t.clipboardClient = nullptr;
}

TEST(testDetectionQueryReportsTheProtocol)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?5522$p");
    CHECK(drainAll(t) == "\033[?5522;2$y");
}

TEST(testBulkReplySurvivesSmallDrainBites)
{
    ScriptedClipboard clip;
    clip.text.assign(5000, 'y');
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    // Drain in 100-byte bites: the bulk channel's partial-drain memmove path.
    std::string all;
    char bite[100];
    int n;
    while ((n = Ubiquitty::drainOutput(&t, bite)) > 0)
        all.append(bite, static_cast<std::size_t>(n));
    CHECK(countOccurrences(all, "status=DATA") == 2); // 5000 = 4096 + 904
    CHECK(all.find("status=DONE") != std::string::npos);
    // Emptied bulk releases its capacity: a megabyte-scale allocation must
    // not sit on every idle terminal that once answered a read.
    CHECK(t.bulkOutput.data == nullptr);
    t.clipboardClient = nullptr;
}

TEST(testS8C1TCollapsesBulkFramingToo)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    t.transmit8bitC1 = true;

    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    const std::string reply = drainAll(t);
    // OSC = 0x9D, ST = 0x9C: one framing whichever channel carried the packet.
    CHECK(reply.find("\x9d"
                     "5522;type=read:status=OK\x9c")
        != std::string::npos);
    CHECK(reply.find('\033') == std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testS8C1TBulkRefusalDoesNotLeavePartialPackets)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.transmit8bitC1 = true;
    const std::string queued(Ubiquitty::VT::maxBulkOutputBytes - 5000, 'x');
    REQUIRE(t.bulkOutput.append(queued, Ubiquitty::VT::maxBulkOutputBytes));
    const std::string packets = "\x1b]7404;" + std::string(9000, 'y') + "\x1b\\";
    CHECK(!respondBulk(t, Ubiquitty::Capability::TextMeasureRead, std::span(packets.data(), packets.size())));
    CHECK_EQ(static_cast<std::size_t>(t.bulkOutput.length), queued.size());
    CHECK(std::string_view(t.bulkOutput.data, t.bulkOutput.length) == queued);
    CHECK_EQ(t.bulkOutput.data[t.bulkOutput.length], 0);
}

TEST(testBulkOverflowTakesTheCompositionBackAndAnswersEBUSY)
{
    ScriptedClipboard clip;
    clip.text.assign(Ubiquitty::VT::maxExtendedClipboardReadBytes, 'z');
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // Three undrained megabyte reads: the first two compose (~1.4 MiB framed
    // each), the third would pass maxBulkOutputBytes and must come back whole
    // — a truncated stream with no `DONE` would hang its requester forever.
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbg==\033\\");
    const std::string reply = drainAll(t);
    CHECK(countOccurrences(reply, "status=DONE") == 2);
    CHECK(countOccurrences(reply, "status=EBUSY") == 1);
    // The refused composition left no fragment behind: exactly two streams
    // opened, so the third reply is the EBUSY alone — never an OK with no
    // end. (The error rides the fixed channel, which drains ahead of the
    // bulk one, so its position in the drained bytes is not the claim.)
    CHECK(countOccurrences(reply, "status=OK") == 2);
    t.clipboardClient = nullptr;
}

TEST(testTypedWriteDeliversEveryItemWithItsOwnBytes)
{
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    // "Hi" under text/plain, "PNG" under image/png (b64 "UE5H"): two items,
    // two payloads, one session.
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata:mime=aW1hZ2UvcG5n;UE5H\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t).find("status=DONE") != std::string::npos);

    CHECK(clip.stored.size() == 2);
    const auto* text = clip.storedItem("text/plain");
    const auto* image = clip.storedItem("image/png");
    CHECK(text && text->bytes == "Hi");
    CHECK(image && image->bytes == "PNG");
    // The typed store received the set directly — the untyped adapter, which
    // would have kept only the text, never ran.
    CHECK(clip.untypedWriteCalls == 0);
    t.clipboardClient = nullptr;
}

TEST(testTypedReadStreamsOneGroupPerServedName)
{
    TypedScriptedClipboard clip;
    clip.serving = { { "text/plain", "Hi" }, { "image/png", "PNG" } };
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // b64("text/plain image/png")
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbiBpbWFnZS9wbmc=\033\\");
    const std::string reply = drainAll(t);
    // Each name's group carries that name's bytes, not a shared payload.
    CHECK(reply.find(":mime=" + std::string(textPlainB64) + ";SGk=\033\\") != std::string::npos);
    CHECK(reply.find(":mime=aW1hZ2UvcG5n;UE5H\033\\") != std::string::npos);
    CHECK(countOccurrences(reply, "status=OK") == 1);
    CHECK(countOccurrences(reply, "status=DONE") == 1);
    CHECK(clip.untypedReadCalls == 0);
    t.clipboardClient = nullptr;
}

TEST(testTypedReadOfAnUnservedNameAnswersACompleteEmptyStream)
{
    TypedScriptedClipboard clip;
    clip.serving = { { "text/plain", "Hi" } };
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // b64("image/png"): a name the client does not serve.
    feed(t, "\033]5522;type=read;aW1hZ2UvcG5n\033\\");
    const std::string reply = drainAll(t);
    CHECK(reply.find("status=OK") != std::string::npos);
    CHECK(reply.find("status=DATA") == std::string::npos);
    CHECK(reply.find("status=DONE") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testTypedListNamesTheClientsOwnTypes)
{
    TypedScriptedClipboard clip;
    clip.serving = { { "text/plain", "Hi" }, { "image/png", "PNG" } };
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=read;Lg==\033\\");
    const std::string reply = drainAll(t);
    // b64("text/plain image/png") under the "." pseudo-MIME (b64 "Lg==").
    CHECK(reply.find(":mime=Lg==;dGV4dC9wbGFpbiBpbWFnZS9wbmc=\033\\") != std::string::npos);
    // The list is consent-exempt: no prompt, no policy consulted.
    CHECK(clip.readRequests == 0);
    t.clipboardClient = nullptr;
}

TEST(testAsynchronousTypedListDefersAndSurvivesRISAndARefusal)
{
    TypedScriptedClipboard clip;
    clip.serving = { { "text/plain", "Hi" }, { "text/html", "<p>" } };
    clip.isAsynchronous = true;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    // An asynchronous client lists from its capture, so even the promptless
    // list defers — with NotRequired, because no consent is being asked.
    feed(t, "\033]5522;type=read:id=q4;Lg==\033\\");
    CHECK(drainAll(t).empty());
    CHECK(t.isOSC52ClipboardReadInFlight);
    CHECK(clip.readRequests == 1);
    CHECK(clip.lastConfirmation == Ubiquitty::ClipboardReadConfirmation::NotRequired);

    // RIS mid-capture: the outstanding read — the list flag included — must
    // survive whole, and the list is served even on a refusing completion,
    // because its deferral existed for the capture, not for a prompt.
    feed(t, "\033c");
    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Refused);
    const std::string reply = drainAll(t);
    // b64("text/plain text/html"), the id riding between the mime and the
    // payload as it does in every packet.
    CHECK(reply.find(":mime=Lg==:id=q4;dGV4dC9wbGFpbiB0ZXh0L2h0bWw=\033\\") != std::string::npos);
    CHECK(reply.find("EPERM") == std::string::npos);
    CHECK(!t.isOSC52ClipboardReadInFlight);
    t.clipboardClient = nullptr;
}

TEST(testDeferredTypedReadCarriesTheRequestedNamesToTheCompletion)
{
    TypedScriptedClipboard clip;
    clip.serving = { { "text/plain", "Hi" }, { "image/png", "PNG" } };
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Prompt;

    // b64("image/png text/plain") — both names must reach the completion,
    // which resolves them against the client only after approval.
    feed(t, "\033]5522;type=read;aW1hZ2UvcG5nIHRleHQvcGxhaW4=\033\\");
    CHECK(drainAll(t).empty());
    CHECK(clip.lastConfirmation == Ubiquitty::ClipboardReadConfirmation::Required);

    Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
    const std::string reply = drainAll(t);
    CHECK(reply.find(":mime=aW1hZ2UvcG5n;UE5H\033\\") != std::string::npos);
    CHECK(reply.find(":mime=" + std::string(textPlainB64) + ";SGk=\033\\") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testWriteItemCapacityDropsTheExtraMimeAndCommitsTheRest)
{
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    // Nine names, one byte each: application/x-0 through x-8, the ninth past
    // the item cap. Base64 of "application/x-N" varies only in its tail.
    for (int i = 0; i < 9; ++i) {
        const std::string mime = "application/x-" + std::to_string(i);
        std::string mimeB64;
        {
            char encoded[64];
            const auto characters = Ubiquitty::Codec::base64Encode(
                std::span<const char> { mime.data(), mime.size() }, std::span<char> { encoded, sizeof encoded });
            REQUIRE(characters.has_value());
            mimeB64.assign(characters->data(), characters->size());
        }
        feed(t, ("\033]5522;type=wdata:mime=" + mimeB64 + ";QQ==\033\\").c_str());
    }
    feed(t, "\033]5522;type=wdata\033\\");
    // The cap drops the extra name, not the session: the eight held items
    // commit and the ack still answers.
    CHECK(drainAll(t).find("status=DONE") != std::string::npos);
    CHECK(clip.stored.size() == static_cast<std::size_t>(Ubiquitty::VT::maxExtendedClipboardWriteItems));
    CHECK(clip.storedItem("application/x-8") == nullptr);
    CHECK(clip.storedItem("application/x-0") != nullptr);
    t.clipboardClient = nullptr;
}

TEST(testWriteByteCapacitySpansItemsTogether)
{
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    // Two items of ~4.2 MB each: neither alone passes the 8 MiB session cap,
    // together they do — the cap bounds the session, not each name a sender
    // can invent. 60000 base64 characters decode to 45000 bytes; 94 packets
    // put one item at 4.23 MB, and the pair at 8.46 MB.
    std::string textPacket = "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;";
    textPacket += std::string(60000, 'A');
    textPacket += "\033\\";
    for (int i = 0; i < 94; ++i)
        feed(t, textPacket.c_str());
    CHECK(!t.didExtendedClipboardWriteOverflow);
    std::string imagePacket = "\033]5522;type=wdata:mime=aW1hZ2UvcG5n;";
    imagePacket += std::string(60000, 'A');
    imagePacket += "\033\\";
    for (int i = 0; i < 94 && !t.didExtendedClipboardWriteOverflow; ++i)
        feed(t, imagePacket.c_str());
    CHECK(t.didExtendedClipboardWriteOverflow);

    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t) == "\033]5522;type=write:status=EIO\033\\");
    CHECK(clip.stored.empty());
    t.clipboardClient = nullptr;
}

TEST(testTypedCommitMaterialisesAliasEntries)
{
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    // walias: target b64("text/plain"), names b64("text/html")
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t).find("status=DONE") != std::string::npos);

    // A typed store holds the alias as an entry of its own, carrying the
    // text's bytes — the same data under the declared name.
    const auto* aliasEntry = clip.storedItem("text/html");
    CHECK(aliasEntry && aliasEntry->bytes == "Hi");
    const auto* text = clip.storedItem("text/plain");
    CHECK(text && text->bytes == "Hi");
    t.clipboardClient = nullptr;
}

TEST(testAliasNameWithItsOwnItemKeepsItsOwnBytes)
{
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    feed(t, "\033]5522;type=write\033\\");
    // text/html arrives as its own payload first (b64 "text/html" / "<p>"),
    // and only then is declared an alias of the text: the item's own bytes
    // win — a declaration cannot overwrite data that already arrived.
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9odG1s;PHA+\033\\");
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(drainAll(t).find("status=DONE") != std::string::npos);

    const auto* html = clip.storedItem("text/html");
    CHECK(html && html->bytes == "<p>");
    const auto* text = clip.storedItem("text/plain");
    CHECK(text && text->bytes == "Hi");
    t.clipboardClient = nullptr;
}

TEST(testUntypedDefaultPicksTheTextItemWhereverItSits)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;

    // The image item arrives first; the untyped adapter must scan past it to
    // the text rather than keep only the set's first entry.
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=aW1hZ2UvcG5n;UE5H\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);
    CHECK(clip.written == "Hi");

    // A set with no text at all clears the untyped store: what it held
    // described a clipboard this write replaced.
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=aW1hZ2UvcG5n;UE5H\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);
    CHECK(clip.written.empty());
    t.clipboardClient = nullptr;
}

TEST(testSeamDefaultReadClipboardItemAdaptsToTheUntypedRead)
{
    // The default a text-only frontend inherits: text/plain — parameters and
    // case ignored, since MIME types match caselessly — routes to the untyped
    // read, and every other name is refused. Called directly because it is
    // the seam's contract with clients that override nothing.
    ScriptedClipboard clip;
    char out[64];
    CHECK(clip.readClipboardItem(Ubiquitty::ClipboardBuffer::Clipboard, "text/plain", out) == 2);
    CHECK(std::string_view(out, 2) == "Hi");
    CHECK(clip.readClipboardItem(Ubiquitty::ClipboardBuffer::Clipboard, "TEXT/Plain;charset=utf-8", out) == 2);
    CHECK(clip.readClipboardItem(Ubiquitty::ClipboardBuffer::Clipboard, "image/png", out) == -1);
    CHECK(clip.readCalls == 2);
}

TEST(testUntypedBothNamesReadCapturesOnce)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;dGV4dC9odG1s\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);

    // One capture serves both names: the untyped client holds one payload,
    // and two captures could disagree mid-stream.
    clip.readCalls = 0;
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbiB0ZXh0L2h0bWw=\033\\");
    const std::string reply = drainAll(t);
    CHECK(clip.readCalls == 1);
    CHECK(countOccurrences(reply, ";SGk=\033\\") == 2);
    t.clipboardClient = nullptr;
}

TEST(testBareReadDefaultsToTextPlain)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // No payload at all, and separately a payload of only blanks (b64(" ")):
    // both read as text/plain — the type a caller that names nothing means.
    feed(t, "\033]5522;type=read\033\\");
    std::string reply = drainAll(t);
    CHECK(reply.find(":mime=" + std::string(textPlainB64) + ";SGk=\033\\") != std::string::npos);
    feed(t, "\033]5522;type=read;IA==\033\\");
    reply = drainAll(t);
    CHECK(reply.find(":mime=" + std::string(textPlainB64) + ";SGk=\033\\") != std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testRepeatedRequestedNameEarnsOneGroup)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // b64("text/plain text/plain"): the request names text twice, and the
    // parse dedups — a doubled name must not double the stream.
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbiB0ZXh0L3BsYWlu\033\\");
    const std::string reply = drainAll(t);
    CHECK(countOccurrences(reply, ";SGk=\033\\") == 1);
    CHECK(countOccurrences(reply, "status=DONE") == 1);
    t.clipboardClient = nullptr;
}

TEST(testOversizedServedNameRefusesTheStream)
{
    TypedScriptedClipboard clip;
    // A 200-character name the client claims to serve: its base64 passes the
    // packet's mime-field bound, so the composition refuses and the requester
    // gets EBUSY rather than a packet with a truncated name.
    const std::string longName = "application/x-" + std::string(186, 'q');
    clip.serving = { { longName, "Hi" } };
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    std::string nameB64;
    {
        char encoded[512];
        const auto characters = Ubiquitty::Codec::base64Encode(
            std::span<const char> { longName.data(), longName.size() }, std::span<char> { encoded, sizeof encoded });
        REQUIRE(characters.has_value());
        nameB64.assign(characters->data(), characters->size());
    }
    feed(t, ("\033]5522;type=read;" + nameB64 + "\033\\").c_str());
    const std::string reply = drainAll(t);
    CHECK(reply == "\033]5522;type=read:status=EBUSY\033\\");
    t.clipboardClient = nullptr;
}

TEST(testOverflowMidStreamSwallowsTheRemainingGroups)
{
    ScriptedClipboard clip;
    clip.text.assign(Ubiquitty::VT::maxExtendedClipboardReadBytes, 'z');
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;

    // Four names serving the same megabyte payload compose ~5.6 MiB framed
    // against the 4 MiB channel: the composition poisons inside the third
    // group, the fourth is swallowed by the poison, and the whole stream
    // comes back as one EBUSY — never a truncated stream with no end.
    feed(t, "\033]5522;type=write\033\\");
    // walias target b64("text/plain"), names b64("x/a x/b x/c")
    feed(t, "\033]5522;type=walias:mime=dGV4dC9wbGFpbg==;eC9hIHgvYiB4L2M=\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    (void)drainAll(t);

    // b64("text/plain x/a x/b x/c")
    feed(t, "\033]5522;type=read;dGV4dC9wbGFpbiB4L2EgeC9iIHgvYw==\033\\");
    const std::string reply = drainAll(t);
    CHECK(countOccurrences(reply, "status=EBUSY") == 1);
    CHECK(reply.find("status=DONE") == std::string::npos);
    CHECK(reply.find("status=DATA") == std::string::npos);
    t.clipboardClient = nullptr;
}

TEST(testWriteCommitRechecksWritePolicy)
{
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    feed(t, "\033]5522;type=write:id=revoked\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.stored.empty());
    CHECK(!t.isExtendedClipboardWriteActive);
    CHECK(drainAll(t) == "\033]5522;type=write:status=EPERM:id=revoked\033\\");
    t.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.stored.empty());
    t.clipboardClient = nullptr;
}

TEST(testWriteCommitRechecksCapability)
{
    struct Policy final : Ubiquitty::CapabilityPolicy {
        Ubiquitty::CapabilityDecision decision = Ubiquitty::CapabilityDecision::Allow;
        Ubiquitty::CapabilityDecision check(Ubiquitty::Capability, Ubiquitty::CapabilityOrigin, std::string_view) override { return decision; }
    } policy;
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.capabilityPolicy = &policy;
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    policy.decision = Ubiquitty::CapabilityDecision::Deny;
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.stored.empty());
    CHECK(!t.isExtendedClipboardWriteActive);
    t.capabilityPolicy = nullptr;
    t.clipboardClient = nullptr;
}

TEST(testWriteConsumesCapabilityOnlyAtCommit)
{
    struct Policy final : Ubiquitty::CapabilityPolicy {
        int checks = 0;
        Ubiquitty::CapabilityDecision check(Ubiquitty::Capability, Ubiquitty::CapabilityOrigin, std::string_view) override
        {
            return ++checks == 1 ? Ubiquitty::CapabilityDecision::Allow : Ubiquitty::CapabilityDecision::Deny;
        }
    } policy;
    TypedScriptedClipboard clip;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.clipboardClient = &clip;
    t.capabilityPolicy = &policy;
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    CHECK(policy.checks == 0);
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.stored.size() == 1);
    CHECK(clip.storedItem("text/plain") && clip.storedItem("text/plain")->bytes == "Hi");
    clip.stored.clear();
    feed(t, "\033]5522;type=write\033\\");
    feed(t, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;SGk=\033\\");
    feed(t, "\033]5522;type=wdata\033\\");
    CHECK(clip.stored.empty());
    t.capabilityPolicy = nullptr;
    t.clipboardClient = nullptr;
}

TEST(testDeferredReadsRecheckReadPolicy)
{
    for (const char* request : { "\033]52;c;?\033\\", "\033]5522;type=read\033\\", "\033]5522;type=read;Lg==\033\\" }) {
        TypedScriptedClipboard clip;
        clip.isAsynchronous = true;
        clip.serving = { { "text/plain", "secret" } };
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        t.clipboardClient = &clip;
        t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
        feed(t, request);
        CHECK(t.isOSC52ClipboardReadInFlight);
        t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
        Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
        const auto reply = drainAll(t);
        CHECK(reply == "\033]52;c;\033\\" || reply == "\033]5522;type=read:status=EPERM\033\\");
        CHECK(clip.untypedReadCalls == 0);
        CHECK(!t.isOSC52ClipboardReadInFlight);
        CHECK(!t.isPendingClipboardReadExtended);
        t.clipboardClient = nullptr;
    }
}

TEST(testDeferredReadsRecheckMasterToggle)
{
    for (const char* request : { "\033]52;c;?\033\\", "\033]5522;type=read\033\\", "\033]5522;type=read;Lg==\033\\" }) {
        TypedScriptedClipboard clip;
        clip.isAsynchronous = true;
        clip.serving = { { "text/plain", "secret" } };
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        t.clipboardClient = &clip;
        t.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
        feed(t, request);
        CHECK(t.isOSC52ClipboardReadInFlight);
        t.isOSC52Allowed = false;
        Ubiquitty::OSC::completeClipboardReadQuery(t, "c", Ubiquitty::OSC::ClipboardReadDecision::Approved);
        CHECK(drainAll(t).empty());
        CHECK(clip.untypedReadCalls == 0);
        CHECK(!t.isOSC52ClipboardReadInFlight);
        CHECK(!t.isPendingClipboardReadExtended);
        t.clipboardClient = nullptr;
    }
}

UBIQUITTY_TEST_MAIN()

TEST(testWriteBase64QuartetsCanCrossPacketBoundaries)
{
    for (std::size_t width : { 1, 2, 3, 4, 5, 7 }) {
        ScriptedClipboard clip;
        Ubiquitty::Terminal terminal;
        terminal.clipboardClient = &clip;
        feed(terminal, "\033]5522;type=write:id=split\033\\");
        const std::string_view encoded = "SGVsbG8gd29ybGQ=";
        for (std::size_t offset = 0; offset < encoded.size(); offset += width) {
            const std::string packet = "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;" + std::string(encoded.substr(offset, width)) + "\033\\";
            feed(terminal, packet);
        }
        CHECK(clip.written.empty());
        feed(terminal, "\033]5522;type=wdata\033\\");
        CHECK(clip.written == "Hello world");
        CHECK(drainAll(terminal) == "\033]5522;type=write:status=DONE:id=split\033\\");
        terminal.clipboardClient = nullptr;
    }
}

TEST(testInvalidWriteBase64AbortsOnceAndDiscardsAllData)
{
    for (const std::string_view encoded : { "YQ", "YQ=", "YQ===", "Y=Q=", "YQ==Yg==", "Y!Q=", "Y Q=", "Y", "____" }) {
        ScriptedClipboard clip;
        Ubiquitty::Terminal terminal;
        terminal.clipboardClient = &clip;
        feed(terminal, "\033]5522;type=write:id=bad\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YWJj\033\\");
        feed(terminal, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;" + std::string(encoded) + "\033\\");
        feed(terminal, "\033]5522;type=wdata\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;Yg==\033\\\033]5522;type=wdata\033\\");
        CHECK(clip.written.empty());
        CHECK(drainAll(terminal) == "\033]5522;type=write:status=EINVAL:id=bad\033\\");
        feed(terminal, "\033]5522;type=write:id=good\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;Yw==\033\\\033]5522;type=wdata\033\\");
        CHECK(clip.written == "c");
        CHECK(drainAll(terminal) == "\033]5522;type=write:status=DONE:id=good\033\\");
        terminal.clipboardClient = nullptr;
    }
}

TEST(testWriteRequiresAMIMEAndCompletedBase64BeforeChangingMIME)
{
    for (const std::string_view packets : {
             "\033]5522;type=wdata;YQ==\033\\",
             "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg;YQ==\033\\",
             "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YQ\033\\\033]5522;type=wdata:mime=dGV4dC9odG1s;Yg==\033\\",
             "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YQ==\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;Yg==\033\\" }) {
        ScriptedClipboard clip;
        Ubiquitty::Terminal terminal;
        terminal.clipboardClient = &clip;
        feed(terminal, "\033]5522;type=write\033\\");
        feed(terminal, packets);
        feed(terminal, "\033]5522;type=wdata\033\\");
        CHECK(clip.written.empty());
        CHECK(drainAll(terminal) == "\033]5522;type=write:status=EINVAL\033\\");
        terminal.clipboardClient = nullptr;
    }
}

TEST(testUnpaddedReadRequestIsIgnored)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal terminal;
    terminal.clipboardClient = &clip;
    terminal.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
    feed(terminal, "\033]5522;type=read;Lg\033\\");
    CHECK(drainAll(terminal).empty());
    terminal.clipboardClient = nullptr;
}

TEST(testResetDiscardsAnUnfinishedClipboardQuartet)
{
    ScriptedClipboard clip;
    Ubiquitty::Terminal terminal;
    terminal.clipboardClient = &clip;
    feed(terminal, "\033]5522;type=write\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YQ\033\\");
    CHECK_EQ(terminal.extendedClipboardBase64TailLength, 2);
    feed(terminal, "\033c");
    terminal.clipboardClient = &clip;
    CHECK_EQ(terminal.extendedClipboardBase64TailLength, 0);
    CHECK(!terminal.isExtendedClipboardBase64Complete);
    feed(terminal, "\033]5522;type=write\033\\\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;Yg==\033\\\033]5522;type=wdata\033\\");
    CHECK(clip.written == "b");
    CHECK(drainAll(terminal) == "\033]5522;type=write:status=DONE\033\\");
    terminal.clipboardClient = nullptr;
}

TEST(testInvalidPermissionMetadataRejectsClipboardAccess)
{
    for (const std::string_view key : { "name", "pw" }) {
        for (const std::string_view encoded : { "YQ", "YQ=", "YQ===", "Y!Q=", "Y Q=", "____", "Y=Q=", "YQ==Yg==" }) {
            ScriptedClipboard clip;
            Ubiquitty::Terminal terminal;
            terminal.clipboardClient = &clip;
            terminal.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Allow;
            const std::string metadata = ":id=test:" + std::string(key) + "=" + std::string(encoded);
            feed(terminal, "\033]5522;type=read" + metadata + ";Lg==\033\\");
            CHECK(drainAll(terminal).empty());
            CHECK_EQ(clip.readCalls, 0);
            CHECK_EQ(clip.readRequests, 0);
            feed(terminal, "\033]5522;type=write" + metadata + "\033\\");
            feed(terminal, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YQ==\033\\\033]5522;type=wdata\033\\");
            CHECK(clip.written.empty());
            CHECK(drainAll(terminal) == "\033]5522;type=write:status=EINVAL:id=test\033\\");
            terminal.clipboardClient = nullptr;
        }
    }
}

TEST(testValidPermissionMetadataRetainsClipboardConsent)
{
    for (const std::string_view key : { "name", "pw" }) {
        for (const std::string& encoded : { std::string {}, std::string("YQ=="), std::string("YWI="), std::string("YWJj"), std::string(4096, 'A') + "YQ==" }) {
            ScriptedClipboard clip;
            Ubiquitty::Terminal terminal;
            terminal.clipboardClient = &clip;
            terminal.osc52ClipboardReadPolicy = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
            const std::string metadata = ":id=test:" + std::string(key) + "=" + encoded;
            feed(terminal, "\033]5522;type=read" + metadata + ";dGV4dC9wbGFpbg==\033\\");
            CHECK(drainAll(terminal) == "\033]5522;type=read:status=EPERM:id=test\033\\");
            CHECK_EQ(clip.readCalls, 0);
            CHECK_EQ(clip.readRequests, 0);
            feed(terminal, "\033]5522;type=write" + metadata + "\033\\");
            feed(terminal, "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;YQ==\033\\\033]5522;type=wdata\033\\");
            CHECK(clip.written == "a");
            CHECK(drainAll(terminal) == "\033]5522;type=write:status=DONE:id=test\033\\");
            terminal.clipboardClient = nullptr;
        }
    }
}

TEST(pasteEventsSetResetAndSurviveModeSaveRestore)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 8, 20);
    feed(terminal, "\033[?5522h\033[?5522$p\033[?5522s\033[?5522l\033[?5522$p\033[?5522r\033[?5522$p");
    CHECK(drainAll(terminal) == "\033[?5522;1$y\033[?5522;2$y\033[?5522;1$y");
    feed(terminal, "\033c");
    CHECK(!terminal.modes.shouldReportPasteEvents);
    terminal.allowPasteControls = false;
    feed(terminal, "\033[?5522h");
    CHECK(!terminal.modes.shouldReportPasteEvents);
}

TEST(pasteNotificationsListFormatsAndKeepPrimarySeparate)
{
    using namespace Ubiquitty;
    const auto reply = OSC::clipboardPasteNotification("text/plain image/png", ClipboardBuffer::Clipboard);
    CHECK(reply == "\033]5522;type=read:status=OK\033\\"
                   "\033]5522;type=read:status=DATA:mime=Lg==;dGV4dC9wbGFpbiBpbWFnZS9wbmc=\033\\"
                   "\033]5522;type=read:status=DONE\033\\");
    CHECK(OSC::clipboardPasteNotification("text/plain", ClipboardBuffer::Primary).starts_with("\033]5522;type=read:status=OK:loc=primary\033\\"));
    CHECK(OSC::clipboardPasteNotification("", ClipboardBuffer::Clipboard).find("mime=Lg==;\033\\") != std::string::npos);
    CHECK(OSC::clipboardPasteNotification(std::string(4097, 'x'), ClipboardBuffer::Clipboard) == "\033]5522;type=read:status=EBUSY\033\\");
}
