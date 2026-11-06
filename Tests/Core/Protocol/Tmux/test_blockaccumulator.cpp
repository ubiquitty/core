// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_blockaccumulator — the marker-then-content machine shared by the
// screen-capture and clipboard-pull recoveries.

#include "Harness.h"

#include "Core/Protocol/Tmux/BlockAccumulator.h"
#include "Core/Protocol/Tmux/StateRecovery.h"

#include <string>

namespace {

using Ubiquitty::Protocol::Tmux::BlockAccumulator;
using Ubiquitty::Protocol::Tmux::ClipboardPullOutcome;
using Ubiquitty::Protocol::Tmux::ClipboardPullRecovery;
using LineKind = BlockAccumulator::LineKind;
using LineJoin = BlockAccumulator::LineJoin;

LineKind feed(BlockAccumulator& block, const char* line)
{
    std::string_view payload;
    return block.consumeLine(line, payload);
}

LineKind feed(BlockAccumulator& block, const char* line, std::string_view& payload)
{
    return block.consumeLine(line, payload);
}

BlockAccumulator makeBlock(LineJoin join = LineJoin::TerminateEachLine, size_t maxBytes = 1024)
{
    return { "MARK ", maxBytes, join };
}

// A line that does not start with the marker prefix belongs to normal
// command-response parsing and must be left alone.
TEST(testIdleIgnoresEverythingButTheMarker)
{
    BlockAccumulator block = makeBlock();
    CHECK_MSG(feed(block, "%window-add @1") == LineKind::NotOurs, "an unrelated line is not ours");
    CHECK_MSG(feed(block, "MAR") == LineKind::NotOurs, "a truncated prefix is not ours");
    CHECK_MSG(feed(block, "xMARK ") == LineKind::NotOurs, "the prefix must be at the start");
    CHECK_MSG(!block.active(), "and none of them armed the machine");
}

TEST(testMarkerPayloadIsTheTextAfterThePrefix)
{
    BlockAccumulator block = makeBlock();
    std::string_view payload;
    CHECK_MSG(feed(block, "MARK %7 1 2", payload) == LineKind::Marker, "the marker is recognised");
    CHECK_MSG(payload == "%7 1 2", "the payload is everything after the prefix");
    CHECK_MSG(block.active(), "the machine is armed");
    CHECK_MSG(!block.capturing(), "but not yet accumulating");
}

TEST(testMarkerWithEmptyPayload)
{
    BlockAccumulator block = makeBlock();
    std::string_view payload = "unset";
    CHECK_MSG(feed(block, "MARK ", payload) == LineKind::Marker, "a bare prefix is still a marker");
    CHECK_MSG(payload.empty(), "with an empty payload");
}

// The marker's own block must close before body lines count. A line arriving
// between the marker and its block end is not content.
TEST(testMarkerBlockMustCloseBeforeBodyIsAccumulated)
{
    BlockAccumulator block = makeBlock();
    feed(block, "MARK %1");
    CHECK_MSG(feed(block, "still the marker block") == LineKind::NotOurs,
        "a line before the marker's block end is not body");

    std::string content = "unset";
    CHECK_MSG(!block.blockEnded(content), "the marker's block end yields no content");
    CHECK_MSG(content == "unset", "and leaves the output untouched");
    CHECK_MSG(block.capturing(), "it arms accumulation instead");
}

TEST(testContentBlockYieldsAccumulatedLines)
{
    BlockAccumulator block = makeBlock();
    feed(block, "MARK %1");
    std::string ignored;
    (void)block.blockEnded(ignored);

    CHECK_MSG(feed(block, "row one") == LineKind::Body, "a body line is claimed");
    CHECK_MSG(feed(block, "row two") == LineKind::Body, "and so is the next");

    std::string content;
    CHECK_MSG(block.blockEnded(content), "the content block completes");
    CHECK_MSG(content == "row one\nrow two\n", "rows are newline-terminated");
    CHECK_MSG(!block.active(), "and the machine returns to idle");
}

// Screen rows are newline-terminated; a clipboard buffer is newline-separated,
// because a trailing newline is content the user never copied.
TEST(testLineJoinModes)
{
    BlockAccumulator terminated = makeBlock(LineJoin::TerminateEachLine);
    BlockAccumulator separated = makeBlock(LineJoin::SeparateLines);
    for (BlockAccumulator* block : { &terminated, &separated }) {
        feed(*block, "MARK %1");
        std::string ignored;
        (void)block->blockEnded(ignored);
        feed(*block, "a");
        feed(*block, "b");
    }

    std::string terminatedContent;
    std::string separatedContent;
    (void)terminated.blockEnded(terminatedContent);
    (void)separated.blockEnded(separatedContent);
    CHECK_MSG(terminatedContent == "a\nb\n", "TerminateEachLine ends every line");
    CHECK_MSG(separatedContent == "a\nb", "SeparateLines carries no trailing newline");
}

TEST(testSingleLineUnderEachJoinMode)
{
    for (LineJoin join : { LineJoin::TerminateEachLine, LineJoin::SeparateLines }) {
        BlockAccumulator block = makeBlock(join);
        feed(block, "MARK %1");
        std::string ignored;
        (void)block.blockEnded(ignored);
        feed(block, "only");

        std::string content;
        (void)block.blockEnded(content);
        const bool isTerminated = join == LineJoin::TerminateEachLine;
        CHECK_MSG(content == (isTerminated ? "only\n" : "only"), "a lone line follows the join mode");
    }
}

TEST(testEmptyContentBlockCompletes)
{
    BlockAccumulator block = makeBlock();
    feed(block, "MARK %1");
    std::string ignored;
    (void)block.blockEnded(ignored);

    std::string content = "unset";
    CHECK_MSG(block.blockEnded(content), "a content block with no lines still completes");
    CHECK_MSG(content.empty(), "and yields nothing");
}

// Past the limit, lines are still claimed so the block end routes cleanly:
// dropping state mid-block would misparse the rest of the response as pane
// records.
TEST(testCapacityDropsBytesButKeepsFraming)
{
    BlockAccumulator block = makeBlock(LineJoin::TerminateEachLine, 16);
    feed(block, "MARK %1");
    std::string ignored;
    (void)block.blockEnded(ignored);

    CHECK_MSG(feed(block, "0123456789") == LineKind::Body, "the first line fits");
    CHECK_MSG(feed(block, "way past the limit") == LineKind::Body, "a line past the limit is still claimed");
    CHECK_MSG(feed(block, "also dropped") == LineKind::Body, "and so are later ones");

    std::string content;
    CHECK_MSG(block.blockEnded(content), "the block still completes");
    CHECK_MSG(content == "0123456789\n", "only what fit was kept");
    CHECK_MSG(content.size() <= 16, "the limit is never exceeded");
}

// The limit charges exactly what the append writes. Under SeparateLines the first
// line takes no separator, so a line of exactly maxBytes fits; charging it a
// byte it never writes would drop content that had room.
TEST(testCapacityChargesOnlyTheBytesWritten)
{
    BlockAccumulator block = makeBlock(LineJoin::SeparateLines, 4);
    feed(block, "MARK %1");
    std::string ignored;
    (void)block.blockEnded(ignored);
    feed(block, "abcd"); // exactly the limit, no separator owed

    std::string content;
    (void)block.blockEnded(content);
    CHECK_MSG(content == "abcd", "a first line of exactly maxBytes fits");

    // A second line owes a separator, so it needs length + 1 of room.
    BlockAccumulator tight = makeBlock(LineJoin::SeparateLines, 4);
    feed(tight, "MARK %1");
    (void)tight.blockEnded(ignored);
    feed(tight, "ab");
    feed(tight, "cd"); // "ab" + "\n" + "cd" is 5, past the limit

    std::string tightContent;
    (void)tight.blockEnded(tightContent);
    CHECK_MSG(tightContent == "ab", "a line whose separator would overflow is dropped");

    // TerminateEachLine owes a terminator on every line, including the first.
    BlockAccumulator terminated = makeBlock(LineJoin::TerminateEachLine, 4);
    feed(terminated, "MARK %1");
    (void)terminated.blockEnded(ignored);
    feed(terminated, "abcd"); // 4 + terminator is 5, past the limit

    std::string terminatedContent;
    (void)terminated.blockEnded(terminatedContent);
    CHECK_MSG(terminatedContent.empty(), "a terminated line must fit its newline too");
}

// A block end outside a content block must not fabricate an outcome.
TEST(testBlockEndWhileIdleIsInert)
{
    BlockAccumulator block = makeBlock();
    std::string content = "unset";
    CHECK_MSG(!block.blockEnded(content), "an idle block end completes nothing");
    CHECK_MSG(content == "unset", "and touches nothing");
    CHECK_MSG(!block.active(), "the machine stays idle");
}

// A fresh attach must not let half-accumulated state swallow the next session's
// rows.
TEST(testResetAbandonsAnInFlightBlock)
{
    BlockAccumulator block = makeBlock();
    feed(block, "MARK %1");
    std::string ignored;
    (void)block.blockEnded(ignored);
    feed(block, "orphaned row");

    block.reset();
    CHECK_MSG(!block.active(), "reset returns to idle");
    CHECK_MSG(feed(block, "next session line") == LineKind::NotOurs,
        "and a following line is no longer swallowed");

    std::string content = "unset";
    CHECK_MSG(!block.blockEnded(content), "the abandoned block yields nothing");
}

// Back-to-back pulls must not leak content across blocks.
TEST(testConsecutiveBlocksDoNotBleed)
{
    BlockAccumulator block = makeBlock();
    for (const char* row : { "first", "second" }) {
        feed(block, "MARK %1");
        std::string ignored;
        (void)block.blockEnded(ignored);
        feed(block, row);
        std::string content;
        CHECK_MSG(block.blockEnded(content), "each block completes");
        CHECK_MSG(content == std::string(row) + "\n", "each carries only its own rows");
    }
}

// ---- ClipboardPullRecovery ----------------------------------------------

bool clipFeed(ClipboardPullRecovery& clip, const char* line)
{
    return clip.consumeLine(line);
}

TEST(testClipboardPullCarriesThePaneFromTheMarker)
{
    ClipboardPullRecovery clip;
    CHECK_MSG(clipFeed(clip, "UBCLIPBEGIN %9"), "the marker is claimed");
    CHECK_MSG(!clip.blockEnded().didComplete, "the marker's block completes nothing");
    CHECK_MSG(clipFeed(clip, "copied text"), "the body is claimed");

    const ClipboardPullOutcome outcome = clip.blockEnded();
    CHECK_MSG(outcome.didComplete, "the buffer block completes");
    CHECK_MSG(outcome.paneID == 9, "the pane comes from the marker");
    CHECK_MSG(outcome.text == "copied text", "the buffer is the body");
}

// A multi-line copy must round-trip without gaining a newline the user did not
// copy.
TEST(testClipboardPullHasNoTrailingNewline)
{
    ClipboardPullRecovery clip;
    clipFeed(clip, "UBCLIPBEGIN %1");
    (void)clip.blockEnded();
    clipFeed(clip, "line one");
    clipFeed(clip, "line two");

    const ClipboardPullOutcome outcome = clip.blockEnded();
    CHECK_MSG(outcome.text == "line one\nline two", "lines are separated, not terminated");
}

// The marker is server-controlled: a malformed pane id must surface as -1 so the
// caller's attribution can fail closed rather than defaulting to a real pane.
TEST(testClipboardPullMalformedPaneIsRefused)
{
    for (const char* marker : { "UBCLIPBEGIN", "UBCLIPBEGIN ", "UBCLIPBEGIN bogus", "UBCLIPBEGIN %" }) {
        ClipboardPullRecovery clip;
        if (!clipFeed(clip, marker))
            continue; // "UBCLIPBEGIN" without the trailing space is not the marker
        (void)clip.blockEnded();
        clipFeed(clip, "text");
        CHECK_MSG(clip.blockEnded().paneID == -1, "a malformed marker yields no pane");
    }
}

TEST(testClipboardPullIgnoresUnrelatedLines)
{
    ClipboardPullRecovery clip;
    CHECK_MSG(!clipFeed(clip, "%window-add @3"), "an unrelated line is not claimed");
    CHECK_MSG(!clip.active(), "and does not arm the machine");
}

TEST(testClipboardPullResetAbandonsInFlight)
{
    ClipboardPullRecovery clip;
    clipFeed(clip, "UBCLIPBEGIN %2");
    (void)clip.blockEnded();
    clipFeed(clip, "half a buffer");
    clip.reset();

    CHECK_MSG(!clip.active(), "reset returns to idle");
    CHECK_MSG(!clip.blockEnded().didComplete, "the abandoned buffer never completes");
}

// Content that looks like the marker must not restart the machine mid-block:
// the protocol block boundary delimits, body text never does.
TEST(testMarkerLikeBodyDoesNotRestartTheBlock)
{
    ClipboardPullRecovery clip;
    clipFeed(clip, "UBCLIPBEGIN %4");
    (void)clip.blockEnded();
    clipFeed(clip, "UBCLIPBEGIN %99");
    clipFeed(clip, "real text");

    const ClipboardPullOutcome outcome = clip.blockEnded();
    CHECK_MSG(outcome.paneID == 4, "the pane stays the one the marker named");
    CHECK_MSG(outcome.text == "UBCLIPBEGIN %99\nreal text",
        "marker-looking content is body, not a new marker");
}

} // namespace

UBIQUITTY_TEST_MAIN()
