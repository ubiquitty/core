// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_pty_corpus.cpp — is the Sync corpus the corpus it claims to be?
//
// Tests/Corpus/sync holds PTY streams cut into reads, each kept for one shape a
// publish gate has to get right: a prompt's echo, a flood that never would-
// blocks, a cluster table, a 2026 block split across reads, a cursor-hidden
// repaint split across reads, an Ink-style spinner, an editor's scroll margins.
// A measurement over the corpus says something about those shapes only while
// the entries carry them, so each entry here is checked for the property it
// exists for, and an entry with no stated property fails rather than riding
// along unexamined.

#include "Harness.h"
#include "SyncCorpus.h"

#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/TerminalRuntime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using UbiquittyTest::CorpusEntry;
using UbiquittyTest::CorpusRead;

namespace {

constexpr std::size_t recordedEntryByteLimit = 200 * 1024;

std::string_view readBytes(const CorpusEntry& entry, const CorpusRead& read)
{
    return std::string_view(entry.bytes).substr(read.offset, read.length);
}

bool contains(const CorpusEntry& entry, std::string_view needle)
{
    return entry.bytes.find(needle) != std::string::npos;
}

// The longest run of bytes read without a would-block between them.
std::size_t longestUnbrokenRun(const CorpusEntry& entry)
{
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const CorpusRead& read : entry.reads) {
        run += read.length;
        if (read.isFollowedByWouldBlock) {
            longest = std::max(longest, run);
            run = 0;
        }
    }
    return std::max(longest, run);
}

// Whether a would-block falls between an opener and its closer: some read
// ends after the opener and before the closer.
bool hasWouldBlockInside(const CorpusEntry& entry, std::string_view opener, std::string_view closer)
{
    std::size_t searchFrom = 0;
    for (;;) {
        const std::size_t open = entry.bytes.find(opener, searchFrom);
        if (open == std::string::npos)
            return false;
        const std::size_t close = entry.bytes.find(closer, open + opener.size());
        if (close == std::string::npos)
            return false;
        for (const CorpusRead& read : entry.reads) {
            const std::size_t end = read.offset + read.length;
            if (read.isFollowedByWouldBlock && end > open && end <= close)
                return true;
        }
        searchFrom = close + closer.size();
    }
}

// Whether a DECSTBM sets margins narrower than the screen.
bool setsScrollMargins(const CorpusEntry& entry)
{
    std::size_t at = 0;
    while ((at = entry.bytes.find("\x1b[", at)) != std::string::npos) {
        at += 2;
        int top = 0;
        int bottom = 0;
        std::size_t index = at;
        while (index < entry.bytes.size() && entry.bytes[index] >= '0' && entry.bytes[index] <= '9')
            top = top * 10 + (entry.bytes[index++] - '0');
        if (index >= entry.bytes.size() || entry.bytes[index] != ';')
            continue;
        ++index;
        while (index < entry.bytes.size() && entry.bytes[index] >= '0' && entry.bytes[index] <= '9')
            bottom = bottom * 10 + (entry.bytes[index++] - '0');
        if (index < entry.bytes.size() && entry.bytes[index] == 'r' && top >= 1 && bottom > top && (top > 1 || bottom < entry.rows))
            return true;
    }
    return false;
}

// Whether every read ends at a would-block, the reads span at least a second,
// and every gap between two lies in [shortestGap, longestGap): a writer that
// never fills a pass, at a steady cadence.
bool writesSteadily(const CorpusEntry& entry, std::uint64_t shortestGap, std::uint64_t longestGap)
{
    if (entry.reads.size() < 2 || entry.reads.back().atMicroseconds - entry.reads.front().atMicroseconds < 1'000'000)
        return false;
    for (std::size_t index = 0; index < entry.reads.size(); ++index) {
        if (!entry.reads[index].isFollowedByWouldBlock)
            return false;
        if (index == 0)
            continue;
        const std::uint64_t gap = entry.reads[index].atMicroseconds - entry.reads[index - 1].atMicroseconds;
        if (gap < shortestGap || gap >= longestGap)
            return false;
    }
    return true;
}

// The property each entry is kept for.
struct Shape {
    const char* name;
    const char* property;
    std::function<bool(const CorpusEntry&)> isCarriedBy;
};

const std::vector<Shape> shapes = {
    { "shell", "OSC 133 prompts and key echo one read per key", [](const CorpusEntry& entry) {
         std::size_t singleByteReads = 0;
         for (const CorpusRead& read : entry.reads)
             singleByteReads += read.length == 1 ? 1 : 0;
         return contains(entry, "\x1b]133;A") && contains(entry, "\x1b]133;D") && singleByteReads >= 50;
     } },
    { "cat-flood", "an unbroken run of reads longer than one drain pass", [](const CorpusEntry& entry) {
         return longestUnbrokenRun(entry) > 2 * static_cast<std::size_t>(Ubiquitty::Runtime::TerminalRuntime::drainPassCapacityBytes);
     } },
    { "emoji-zwj", "ZWJ sequences, regional indicators and a presentation selector", [](const CorpusEntry& entry) {
         return contains(entry, "\u200D") && contains(entry, "\U0001F1EF") && contains(entry, "\uFE0F");
     } },
    { "osc8-links", "OSC 8 links, opened and closed", [](const CorpusEntry& entry) {
         return contains(entry, "\x1b]8;id=") && contains(entry, "\x1b]8;;\x1b\\");
     } },
    { "decdwl-decdhl", "DECDWL, both DECDHL halves and DECSWL", [](const CorpusEntry& entry) {
         return contains(entry, "\x1b#6") && contains(entry, "\x1b#3") && contains(entry, "\x1b#4") && contains(entry, "\x1b#5");
     } },
    { "synchronized-output", "a DEC 2026 block with a would-block inside it", [](const CorpusEntry& entry) {
         return hasWouldBlockInside(entry, "\x1b[?2026h", "\x1b[?2026l");
     } },
    { "cursor-hidden-repaint", "a cursor-hidden repaint with a would-block inside it, and no 2026", [](const CorpusEntry& entry) {
         return hasWouldBlockInside(entry, "\x1b[?25l", "\x1b[?25h") && !contains(entry, "\x1b[?2026h");
     } },
    { "ink-spinner", "the cursor hidden throughout and a region redrawn every 80 ms, some redraws split", [](const CorpusEntry& entry) {
         const bool isHiddenThroughout = entry.bytes.rfind("\x1b[?25l", 0) == 0 && entry.bytes.find("\x1b[?25h") + 6 == entry.bytes.size();
         std::size_t splitRedraws = 0;
         for (const UbiquittyTest::RedrawSpan& span : entry.redraws) {
             for (const CorpusRead& read : entry.reads) {
                 const std::size_t end = read.offset + read.length;
                 if (read.isFollowedByWouldBlock && end > span.begin && end < span.end) {
                     ++splitRedraws;
                     break;
                 }
             }
         }
         return isHiddenThroughout && entry.redraws.size() >= 100 && splitRedraws >= 10;
     } },
    { "echo-burst", "keys echoed inside a cursor-hidden bracket longer than any deadline, shown again after", [](const CorpusEntry& entry) {
         const std::size_t hide = entry.bytes.find("\x1b[?25l");
         const std::size_t show = entry.bytes.find("\x1b[?25h", hide);
         if (hide == std::string::npos || show == std::string::npos || entry.inputs.size() < 100)
             return false;
         const auto readAt = [&entry](std::size_t offset) {
             for (const CorpusRead& read : entry.reads) {
                 if (offset < read.offset + read.length)
                     return read.atMicroseconds;
             }
             return std::uint64_t { 0 };
         };
         return readAt(show) - readAt(hide) >= 1'000'000;
     } },
    { "line-trickle-fast", "a second of writes, each its own read, every gap under the quiet window", [](const CorpusEntry& entry) {
         return writesSteadily(entry, 0, Ubiquitty::Runtime::readQuiescenceMicroseconds);
     } },
    { "line-trickle-slow", "a second of writes, each its own read, every gap past the quiet window and under a refresh", [](const CorpusEntry& entry) {
         return writesSteadily(entry, Ubiquitty::Runtime::readQuiescenceMicroseconds, Ubiquitty::Runtime::refreshIntervalMicroseconds);
     } },
    { "vim-margins", "scroll margins narrower than the screen", [](const CorpusEntry& entry) {
         return setsScrollMargins(entry);
     } },
    { "less", "the alternate screen and a repaint per keystroke", [](const CorpusEntry& entry) {
         return contains(entry, "\x1b[?1049h") && entry.reads.size() >= 20;
     } },
    { "top", "a once-a-second refresh on the alternate screen", [](const CorpusEntry& entry) {
         return contains(entry, "\x1b[?1049h") && entry.reads.size() >= 8 && entry.reads.back().atMicroseconds >= 8'000'000;
     } },
};

const Shape* shapeFor(std::string_view name)
{
    for (const Shape& shape : shapes) {
        if (name == shape.name)
            return &shape;
    }
    return nullptr;
}

} // namespace

TEST(theCorpusLoadsWholeAndEveryReadCoversItsStream)
{
    std::string error;
    const auto corpus = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    CHECK_MSG(corpus.has_value(), error.c_str());
    if (!corpus)
        return;
    for (const CorpusEntry& entry : *corpus) {
        CHECK_MSG(!entry.reads.empty(), entry.name.c_str());
        CHECK_MSG(!entry.description.empty(), entry.name.c_str());
        std::size_t covered = 0;
        for (const CorpusRead& read : entry.reads) {
            CHECK(read.offset == covered);
            CHECK(!readBytes(entry, read).empty());
            covered += read.length;
        }
        CHECK_MSG(covered == entry.bytes.size(), entry.name.c_str());
        if (entry.origin == "recorded")
            CHECK_MSG(entry.bytes.size() < recordedEntryByteLimit, entry.name.c_str());
    }
}

TEST(everyEntryCarriesTheShapeItIsKeptFor)
{
    std::string error;
    const auto corpus = UbiquittyTest::loadSyncCorpus(UbiquittyTest::syncCorpusDirectory(), error);
    CHECK_MSG(corpus.has_value(), error.c_str());
    if (!corpus)
        return;
    for (const CorpusEntry& entry : *corpus) {
        const Shape* shape = shapeFor(entry.name);
        CHECK_MSG(shape, ("an entry with no stated shape: " + entry.name).c_str());
        if (!shape)
            continue;
        CHECK_MSG(shape->isCarriedBy(entry), (entry.name + " lacks " + shape->property).c_str());
    }
    // Every stated shape has its entry: a table row whose stream went missing
    // is a shape the measurements silently stopped covering.
    for (const Shape& shape : shapes) {
        bool isPresent = false;
        for (const CorpusEntry& entry : *corpus)
            isPresent = isPresent || entry.name == shape.name;
        CHECK_MSG(isPresent, (std::string("the corpus lost ") + shape.name).c_str());
    }
}

UBIQUITTY_TEST_MAIN()
