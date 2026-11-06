// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_clipboard_store — the multi-buffer OSC 52 clipboard behaviour.
//
// Assertions name behaviour, never a platform's class, so a Swift or GTK
// implementation of the same store satisfies this file unchanged.

#include "Harness.h"

#include "Core/OSC/ClipboardStore.h"

#include <set>
#include <string>
#include <utility>

namespace {

using Ubiquitty::ClipboardBuffer;
using Ubiquitty::OSC::clipboardBufferLabel;
using Ubiquitty::OSC::clipboardPreview;
using Ubiquitty::OSC::ClipboardStore;
using Ubiquitty::OSC::maxClipboardPreviewScalars;
using Ubiquitty::OSC::maxRecentClipboardCopies;
using Ubiquitty::OSC::PasteEntry;
using Ubiquitty::OSC::PasteEntryKind;
using Ubiquitty::Title::titleScalarLength;

constexpr int bufferCount = static_cast<int>(ClipboardBuffer::Count);

// ---- buffers ------------------------------------------------------------

// c/p/q/s/0-7 land in twelve independent stores: a script must not clobber a
// value it was never given the selector for.
TEST(testMovePreservesBuffersAndHistory)
{
    ClipboardStore source;
    const std::string bytes("a\0b", 3);
    source.setBuffer(ClipboardBuffer::Primary, bytes);
    source.recordRecentCopy("recent");
    ClipboardStore destination(std::move(source));
    CHECK(destination.bufferBytes(ClipboardBuffer::Primary) == bytes);
    CHECK(destination.textForEntry(PasteEntryKind::Recent, 0, {}) == "recent");
    source.setBuffer(ClipboardBuffer::Primary, "independent");
    CHECK(destination.bufferBytes(ClipboardBuffer::Primary) == bytes);
}

TEST(testEveryBufferIsIndependent)
{
    ClipboardStore store;
    for (int i = 0; i < bufferCount; ++i) {
        if (i == static_cast<int>(ClipboardBuffer::Clipboard))
            continue;
        store.setBuffer(static_cast<ClipboardBuffer>(i), "value-" + std::to_string(i));
    }
    for (int i = 0; i < bufferCount; ++i) {
        if (i == static_cast<int>(ClipboardBuffer::Clipboard))
            continue;
        CHECK_MSG(store.bufferBytes(static_cast<ClipboardBuffer>(i)) == "value-" + std::to_string(i),
            "each buffer holds only its own value");
    }
}

// Clipboard belongs to the system pasteboard. Accepting a write here would let
// the picker show one value while ⌘V pastes another.
TEST(testClipboardBufferIsNotStored)
{
    ClipboardStore store;
    store.setBuffer(ClipboardBuffer::Clipboard, "should not land here");
    CHECK_MSG(store.bufferBytes(ClipboardBuffer::Clipboard).empty(),
        "the Clipboard buffer belongs to the host pasteboard, not the store");
}

TEST(testEmptyBytesClearABuffer)
{
    ClipboardStore store;
    store.setBuffer(ClipboardBuffer::Cut3, "something");
    store.setBuffer(ClipboardBuffer::Cut3, "");
    CHECK_MSG(store.bufferBytes(ClipboardBuffer::Cut3).empty(), "empty bytes clear a buffer");
}

// Count is the array sizer, never a slot. Storing into it would write one past
// the end of a twelve-element array.
TEST(testOutOfRangeBufferIsRefused)
{
    ClipboardStore store;
    store.setBuffer(ClipboardBuffer::Count, "past the end");
    CHECK_MSG(store.bufferBytes(ClipboardBuffer::Count).empty(), "Count is not a storable buffer");
    CHECK_MSG(store.pasteEntries("").empty(), "an out-of-range write stored nothing");
}

// Bytes, not text: OSC 52 carries base64 that need not decode to valid UTF-8.
TEST(testBufferPreservesArbitraryBytes)
{
    ClipboardStore store;
    const std::string raw("\x00\xFF\xC0 binary\n", 10);
    store.setBuffer(ClipboardBuffer::Cut7, raw);
    CHECK_MSG(store.bufferBytes(ClipboardBuffer::Cut7) == raw, "a buffer round-trips arbitrary bytes");
}

// ---- labels -------------------------------------------------------------

// Cut0-Cut7 are contiguous, so `Cut0 + n` indexes cut buffer n.
TEST(testCutBufferLabelsFollowTheEnumOrder)
{
    for (int n = 0; n < 8; ++n) {
        const auto buffer = static_cast<ClipboardBuffer>(static_cast<int>(ClipboardBuffer::Cut0) + n);
        CHECK_MSG(clipboardBufferLabel(buffer) == "cut " + std::to_string(n),
            "cut buffer n is labelled by its OSC 52 digit");
    }
}

TEST(testNamedBufferLabels)
{
    CHECK_MSG(clipboardBufferLabel(ClipboardBuffer::Clipboard) == "clipboard", "c is clipboard");
    CHECK_MSG(clipboardBufferLabel(ClipboardBuffer::Primary) == "primary", "p is primary");
    CHECK_MSG(clipboardBufferLabel(ClipboardBuffer::Secondary) == "secondary", "q is secondary");
    CHECK_MSG(clipboardBufferLabel(ClipboardBuffer::Select) == "select", "s is select");
}

// Two buffers sharing a label makes the picker ambiguous.
TEST(testEveryBufferLabelIsDistinct)
{
    std::set<std::string> labels;
    for (int i = 0; i < bufferCount; ++i)
        labels.insert(clipboardBufferLabel(static_cast<ClipboardBuffer>(i)));
    CHECK_MSG(labels.size() == static_cast<std::size_t>(bufferCount),
        "every buffer has a distinct label");
}

// ---- previews -----------------------------------------------------------

TEST(testPreviewCollapsesWhitespaceRuns)
{
    CHECK_MSG(clipboardPreview("one\n\ttwo   three") == "one two three",
        "whitespace runs collapse to a single space");
}

TEST(testPreviewTrimsBothEnds)
{
    CHECK_MSG(clipboardPreview("  \n padded \t ") == "padded", "leading and trailing whitespace is trimmed");
    CHECK_MSG(clipboardPreview("   ").empty(), "all-whitespace previews as empty");
    CHECK_MSG(clipboardPreview("").empty(), "empty previews as empty");
}

// Unicode separators collapse alongside the ASCII ones: a row that renders on
// two lines breaks the picker's layout.
TEST(testPreviewCollapsesUnicodeSeparators)
{
    CHECK_MSG(clipboardPreview("a b") == "a b", "NBSP collapses");
    CHECK_MSG(clipboardPreview("a b") == "a b", "LINE SEPARATOR collapses");
    CHECK_MSG(clipboardPreview("a b") == "a b", "PARAGRAPH SEPARATOR collapses");
    CHECK_MSG(clipboardPreview("a　b") == "a b", "IDEOGRAPHIC SPACE collapses");
    CHECK_MSG(clipboardPreview("a b") == "a b", "EM SPACE collapses");
}

TEST(testPreviewIsCappedAtTheScalarBudget)
{
    const std::string fits(maxClipboardPreviewScalars, 'x');
    CHECK_MSG(clipboardPreview(fits) == fits, "a preview at the budget is returned untouched");

    const std::string overflows(maxClipboardPreviewScalars + 1, 'x');
    const std::string elided = clipboardPreview(overflows);
    CHECK_MSG(elided != overflows, "a preview past the budget is elided");
    CHECK_MSG(titleScalarLength(elided) == maxClipboardPreviewScalars + 1,
        "an elided preview is the budget plus one ellipsis scalar");
}

// A UTF-16 budget can cut between a surrogate pair and emit an unpaired half.
// Counting scalars and slicing on scalar boundaries cannot.
TEST(testPreviewNeverSplitsAScalar)
{
    std::string multibyte;
    for (std::size_t i = 0; i < maxClipboardPreviewScalars + 20; ++i)
        multibyte += "é"; // two bytes each

    const std::string elided = clipboardPreview(multibyte);
    CHECK_MSG(titleScalarLength(elided) == maxClipboardPreviewScalars + 1,
        "a multi-byte preview is elided by scalars, not by bytes");
    // 80 two-byte scalars plus the three-byte ellipsis. A byte-wise cut would
    // land mid-scalar and this length would be off by one.
    CHECK_MSG(elided.size() == maxClipboardPreviewScalars * 2 + 3,
        "the elision landed on a scalar boundary");

    // No lone continuation byte can begin a scalar in well-formed UTF-8.
    CHECK_MSG((static_cast<unsigned char>(elided.front()) & 0xC0) != 0x80,
        "the elided preview is well-formed UTF-8");
}

// Astral scalars are four bytes and are exactly what a UTF-16 budget miscounts:
// each is one scalar but two code units, so an NSString cut at 80 units holds
// only 40 emoji.
TEST(testPreviewCountsAstralScalarsAsOne)
{
    std::string emoji;
    for (std::size_t i = 0; i < maxClipboardPreviewScalars; ++i)
        emoji += "😀"; // four bytes, one scalar, two UTF-16 code units
    CHECK_MSG(clipboardPreview(emoji) == emoji,
        "exactly the budget in astral scalars is not elided");
}

// A preview is display text over untrusted bytes; invalid UTF-8 must not throw
// away the rest of the string or run off the end.
TEST(testPreviewSurvivesInvalidUTF8)
{
    const std::string truncated("valid \xE2\x82", 8); // a 3-byte sequence cut short
    const std::string preview = clipboardPreview(truncated);
    CHECK_MSG(!preview.empty(), "a truncated sequence does not erase the preview");

    const std::string stray("\x80\x80 tail", 7); // lone continuation bytes
    CHECK_MSG(clipboardPreview(stray).find("tail") != std::string::npos,
        "stray continuation bytes do not swallow what follows");
}

// ---- recent ring --------------------------------------------------------

TEST(testRecentRingIsMostRecentFirst)
{
    ClipboardStore store;
    store.recordRecentCopy("first");
    store.recordRecentCopy("second");
    const std::vector<std::string> recent = store.recentCopies();
    CHECK_MSG(recent.size() == 2, "both copies are recorded");
    CHECK_MSG(recent[0] == "second", "the newest copy is first");
    CHECK_MSG(recent[1] == "first", "the older copy follows");
}

TEST(testRecentRingIsBounded)
{
    ClipboardStore store;
    for (std::size_t i = 0; i < maxRecentClipboardCopies * 3; ++i)
        store.recordRecentCopy("copy-" + std::to_string(i));

    const std::vector<std::string> recent = store.recentCopies();
    CHECK_MSG(recent.size() == maxRecentClipboardCopies, "the ring is bounded");
    CHECK_MSG(recent.front() == "copy-" + std::to_string(maxRecentClipboardCopies * 3 - 1),
        "the newest copy survives");
    CHECK_MSG(recent.back() == "copy-" + std::to_string(maxRecentClipboardCopies * 2),
        "the oldest surviving copy is exactly one ring back");
}

// Re-copying the same text is the most common thing a user does. Appending it
// again would fill the ring with one value and evict everything else, which is
// the opposite of what a history is for.
TEST(testRecentRingPromotesADuplicate)
{
    ClipboardStore store;
    store.recordRecentCopy("a");
    store.recordRecentCopy("b");
    store.recordRecentCopy("a");

    const std::vector<std::string> recent = store.recentCopies();
    CHECK_MSG(recent.size() == 2, "a duplicate does not grow the ring");
    CHECK_MSG(recent[0] == "a", "a re-copied value moves to the front");
    CHECK_MSG(recent[1] == "b", "the value it passed stays behind it");
}

TEST(testRecentRingIgnoresEmpty)
{
    ClipboardStore store;
    store.recordRecentCopy("");
    CHECK_MSG(store.recentCopies().empty(), "an empty copy is not recorded");
}

// ---- picker model -------------------------------------------------------

// The store never reads a pasteboard; the host passes its live text in.
TEST(testSystemClipboardIsSuppliedByTheCaller)
{
    ClipboardStore store;
    CHECK_MSG(store.pasteEntries("").empty(), "an empty store with an empty clipboard has no entries");

    const std::vector<PasteEntry> entries = store.pasteEntries("live pasteboard");
    CHECK_MSG(entries.size() == 1, "the host's clipboard text becomes the only entry");
    CHECK_MSG(entries[0].kind == PasteEntryKind::Buffer, "it is a buffer entry");
    CHECK_MSG(entries[0].index == static_cast<int>(ClipboardBuffer::Clipboard), "it is the Clipboard buffer");
    CHECK_MSG(entries[0].preview == "live pasteboard", "it previews the supplied text");
}

// Order is the picker's UI contract: clipboard, then buffers in enum order,
// then recent copies.
TEST(testEntryOrdering)
{
    ClipboardStore store;
    store.setBuffer(ClipboardBuffer::Cut2, "cut two");
    store.setBuffer(ClipboardBuffer::Primary, "primary text");
    store.recordRecentCopy("a copy");

    const std::vector<PasteEntry> entries = store.pasteEntries("system");
    CHECK_MSG(entries.size() == 4, "clipboard, two buffers and one recent copy");
    CHECK_MSG(entries[0].label == "clipboard", "the system clipboard is first");
    CHECK_MSG(entries[1].label == "primary", "buffers follow in enum order");
    CHECK_MSG(entries[2].label == "cut 2", "cut buffers come after the named ones");
    CHECK_MSG(entries[3].kind == PasteEntryKind::Recent, "recent copies come last");
    CHECK_MSG(entries[3].label == "recent copy", "recent entries share one label");
}

// An empty buffer is not a row. Eleven blank "cut n" rows would bury the two
// entries the user actually has.
TEST(testEmptyBuffersAreOmitted)
{
    ClipboardStore store;
    store.setBuffer(ClipboardBuffer::Cut0, "only this one");
    const std::vector<PasteEntry> entries = store.pasteEntries("");
    CHECK_MSG(entries.size() == 1, "only non-empty buffers become entries");
    CHECK_MSG(entries[0].label == "cut 0", "and it is the one that was set");
}

// Every entry has to be selectable: an index the picker shows but cannot resolve
// is a row that silently pastes nothing.
TEST(testEveryEntryResolvesToItsText)
{
    ClipboardStore store;
    store.setBuffer(ClipboardBuffer::Select, "selected text");
    store.setBuffer(ClipboardBuffer::Cut5, "cut five text");
    store.recordRecentCopy("copied text");

    for (const PasteEntry& entry : store.pasteEntries("system text")) {
        const std::string text = store.textForEntry(entry.kind, entry.index, "system text");
        CHECK_MSG(!text.empty(), "every listed entry resolves to text");
    }

    CHECK_MSG(store.textForEntry(PasteEntryKind::Buffer,
                  static_cast<int>(ClipboardBuffer::Clipboard), "system text")
            == "system text",
        "the Clipboard entry resolves to the host's live text");
    CHECK_MSG(store.textForEntry(PasteEntryKind::Recent, 0, "") == "copied text",
        "a recent entry resolves to its copy");
}

// The picker's index can outlive the thing it names — the ring shifts under it,
// or the panel is stale. Out of range must be empty, never a read past the end.
TEST(testOutOfRangeEntriesResolveToNothing)
{
    ClipboardStore store;
    store.recordRecentCopy("one copy");

    CHECK_MSG(store.textForEntry(PasteEntryKind::Recent, 1, "").empty(), "a stale ring index is empty");
    CHECK_MSG(store.textForEntry(PasteEntryKind::Recent, -1, "").empty(), "a negative ring index is empty");
    CHECK_MSG(store.textForEntry(PasteEntryKind::Buffer, bufferCount, "").empty(),
        "a buffer index past the end is empty");
    CHECK_MSG(store.textForEntry(PasteEntryKind::Buffer, -1, "").empty(),
        "a negative buffer index is empty");
}

// textForEntry returns the WHOLE value; only the row preview is shortened.
// Pasting the ellipsis would be a data-loss bug the picker cannot show.
TEST(testEntryTextIsNotThePreview)
{
    ClipboardStore store;
    const std::string big(maxClipboardPreviewScalars * 4, 'z');
    store.setBuffer(ClipboardBuffer::Cut1, big);

    const std::vector<PasteEntry> entries = store.pasteEntries("");
    CHECK_MSG(entries.size() == 1, "the oversized buffer is listed");
    CHECK_MSG(entries[0].preview.size() < big.size(), "its preview is shortened");
    CHECK_MSG(store.textForEntry(PasteEntryKind::Buffer,
                  static_cast<int>(ClipboardBuffer::Cut1), "")
            == big,
        "but pasting it yields the whole value");
}

} // namespace

UBIQUITTY_TEST_MAIN()
