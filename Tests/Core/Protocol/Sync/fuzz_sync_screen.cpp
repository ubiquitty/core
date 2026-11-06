// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_sync_screen.cpp — libFuzzer harness for the Sync screen-plane and
// history-plane decoders (Source/Core/Protocol/Sync: applyCheckpoint,
// applyRowUpdate, decodeRowRecord, decodeHistoryBody).
//
// A client applies these bodies as they arrive from a host, so every byte is
// the peer's choice. The input either is a body, or flips bytes of a valid
// checkpoint or row update built here, so a fixed corpus reaches the decoders'
// success paths and not only their first length check. The first byte picks:
//
//   0  the input as a checkpoint body
//   1  a valid checkpoint with the input's (offset, mask) pairs applied
//   2  the input as a row update, after a valid checkpoint
//   3  a valid row update with the input's (offset, mask) pairs applied
//   4  the input as a row record
//   5  the input as a history body
//   6  a valid history body with the input's (offset, mask) pairs applied
//   7  the input as a run of operations on a client's history wants
//   8  the input as a program's output, cut at 0xFF into frames: each frame
//      the host takes, some it sends no client, and the rest reach the
//      client as chain updates with every optional op
//
// Two contracts are asserted beyond memory safety:
//
//   - a refused body leaves the client exactly as it was: the checkpoint the
//     client would itself produce, and the link ring, cluster pool and run
//     texts behind it, are the same before and after;
//   - an applied body leaves a client that can be checkpointed, and whose
//     checkpoint a second client applies;
//   - a refused history body leaves the lines decoded so far as they were,
//     and an accepted one holds only lines within the cell cap whose links
//     and clusters name entries of the line's own tables;
//   - a want refused over the cap, or of another epoch, leaves every set as
//     it was; an accepted one leaves the sets within the cap; and every set
//     stays sorted, disjoint and within what the host can send.
//
// Build via Scripts/run-tests.sh --with-fuzz; ctest drives it over a fixed
// corpus through Tests/FuzzDriver.cpp.

#include "LoopbackPaneLink.h"

#include "Core/Attributes/Hyperlinks.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/HistoryWants.h"
#include "Core/Protocol/Sync/RowForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

/// Every op a client may accept past the base set.
constexpr ChainOps allOptionalOps { ChainOp::Spans, ChainOp::ScrollRect };

std::unique_ptr<Ubiquitty::Terminal> makeTerminal()
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 6, 30);
    return terminal;
}

// A host with something in most of what a checkpoint carries, and the
// publisher that encoded its checkpoint.
struct Seed {
    std::unique_ptr<Ubiquitty::Terminal> host = makeTerminal();
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    std::vector<std::uint8_t> update;

    Seed()
    {
        Ubiquitty::processBytes(host.get(),
            "\x1b]133;A\x07$ \x1b[1;31mred\x1b[0m \xe4\xb8\xad e\xcc\x81 "
            "\x1b]8;;https://example.com/\x1b\\link\x1b]8;;\x1b\\\r\n\x1b#6wide\r\n\x1b[?69h\x1b[3;9s\x1b(0q\x0e");
        (void)(publisher.capture(*host) && publisher.checkpoint(*host, 1, checkpoint, baseline));
        Ubiquitty::processBytes(host.get(),
            "\r\n\x1b[35mnew \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 \x1b]8;;https://example.org/\x1b\\more\x1b]8;;\x1b\\");
        (void)(publisher.capture(*host) && publisher.rowUpdate(*host, baseline, update, allOptionalOps) == UpdateOutcome::Encoded);
    }
};

std::vector<std::uint8_t> clientCheckpoint(const Ubiquitty::Terminal& client)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    const bool isEncoded = publisher.capture(client) && publisher.checkpoint(client, 0, body, baseline);
    assert(isEncoded);
    (void)isEncoded;
    return body;
}

// Everything a body can change on a client: what its own checkpoint would
// say, and the dictionaries behind it, which hold entries no cell names yet.
std::vector<std::uint8_t> clientFingerprint(const Ubiquitty::Terminal& client)
{
    std::vector<std::uint8_t> fingerprint = clientCheckpoint(client);
    WireWriter writer(fingerprint);
    writer.varint(static_cast<std::uint64_t>(client.linkCount));
    for (int index = 0; index < client.linkCount; ++index) {
        const auto slot = static_cast<std::uint16_t>((client.linkHead + index) % Ubiquitty::VT::linkTableCapacity);
        if (const std::optional<LinkDefinition> link = linkDefinition(client, Ubiquitty::Attributes::Hyperlinks::table(&client)->ids[slot])) {
            writer.u16(link->id);
            writer.utf8(link->uri);
        }
    }
    writer.varint(client.clusterPool.entries().size());
    writer.varint(client.runTextStore.liveCodePoints());
    return fingerprint;
}

void flip(std::vector<std::uint8_t>& body, std::span<const std::uint8_t> pairs)
{
    if (body.empty())
        return;
    for (std::size_t index = 0; index + 1 < pairs.size(); index += 2)
        body[pairs[index] * 131u % body.size()] ^= pairs[index + 1];
}

void checkOutcome(ApplyOutcome outcome, const Ubiquitty::Terminal& client, const std::vector<std::uint8_t>& before)
{
    if (outcome != ApplyOutcome::Applied) {
        assert(clientFingerprint(client) == before);
        return;
    }
    auto second = makeTerminal();
    ScreenReplica replica;
    const ApplyOutcome again = applyCheckpoint(*second, clientCheckpoint(client), replica);
    assert(again == ApplyOutcome::Applied);
    (void)again;
}

} // namespace

/// A history body over lines that name pens, a link, a long cluster and a
/// wide character, so flipped bytes reach every table and the hash check.
const std::vector<std::uint8_t>& historySeedBody()
{
    static const std::vector<std::uint8_t> body = [] {
        auto host = makeTerminal();
        const std::string_view lines[] = {
            "\x1b[38;2;1;2;3mtinted\x1b[0m plain\r\n",
            "\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\ \xe4\xb8\x96\r\n",
            "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb family\r\n",
            "a line long enough to wrap across the thirty columns of this terminal\r\n",
        };
        for (const std::string_view line : lines)
            Ubiquitty::processBytes(host.get(), line);
        for (int index = 0; index < 8; ++index)
            Ubiquitty::processBytes(host.get(), "\r\n");
        HistoryPublisher publisher;
        std::vector<std::uint8_t> encoded;
        if (publisher.capture(*host))
            (void)publisher.writeBody(*host, { 0, 4 }, HistoryOrder::Ascending, maxHistoryTransferBytes, encoded);
        assert(!encoded.empty());
        return encoded;
    }();
    return body;
}

// Applies the input's operations to one attachment's wants against a fixed
// history, checking the sets after each.
void fuzzWants(std::span<const std::uint8_t> input)
{
    constexpr std::size_t cap = 8;
    const Ubiquitty::Screen::HistoryAddress address { .epoch = 2, .oldest = 100, .head = 900, .clearedThrough = 150 };
    HistoryWants wants(cap);
    const auto snapshot = [&] {
        std::vector<std::vector<LineRange>> sets;
        for (const HistoryPriority priority : { HistoryPriority::Viewport, HistoryPriority::Search, HistoryPriority::Backfill })
            sets.emplace_back(wants.ranges(priority).begin(), wants.ranges(priority).end());
        return sets;
    };
    std::size_t at = 0;
    const auto take = [&] { return at < input.size() ? input[at++] : std::uint8_t { 0 }; };
    while (at < input.size()) {
        const std::uint8_t operation = take();
        const auto priority = static_cast<HistoryPriority>((operation >> 4) % 3);
        switch (operation % 4) {
        case 0: {
            HistoryWant want { .historyEpoch = (operation & 8) ? 1u : 2u, .ranges = {}, .priority = priority };
            std::uint64_t next = take() * 4;
            for (std::size_t count = take() % 12; count > 0; --count) {
                const std::uint64_t from = next + take() % 64;
                const std::uint64_t to = from + 1 + take() % 128;
                want.ranges.push_back({ from, to });
                next = to;
            }
            const auto before = snapshot();
            const WantOutcome outcome = wants.replace(want, address);
            if (outcome != WantOutcome::Accepted)
                assert(snapshot() == before);
            else
                assert(wants.pendingRanges() <= cap);
            break;
        }
        case 1:
            if (const std::optional<LineRange> slice = wants.nextSlice(priority, 1 + take() % 300)) {
                assert(slice->from < slice->to);
                wants.exclude(*slice);
            }
            break;
        case 2: {
            const std::uint64_t from = take() * 4;
            wants.exclude({ from, from + take() });
            break;
        }
        default:
            wants.clear();
            break;
        }
        for (const auto& set : snapshot()) {
            for (std::size_t index = 0; index < set.size(); ++index) {
                assert(set[index].from < set[index].to);
                assert(set[index].from >= address.clearedThrough && set[index].to <= address.head);
                assert(index == 0 || set[index - 1].to < set[index].from);
            }
        }
    }
}

// A program's output, cut into frames: every chain update the host builds,
// with every optional op, applies onto the client and leaves it showing what
// the host shows.
void fuzzChainUpdates(std::span<const std::uint8_t> input)
{
    auto host = makeTerminal();
    auto client = makeTerminal();
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    ScreenReplica replica;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(*host) || !publisher.checkpoint(*host, 1, body, baseline))
        return;
    const ApplyOutcome seeded = applyCheckpoint(*client, body, replica);
    assert(seeded == ApplyOutcome::Applied);
    (void)seeded;
    std::size_t begin = 0;
    while (begin < input.size()) {
        std::size_t end = begin;
        while (end < input.size() && input[end] != 0xFF)
            ++end;
        const std::span<const std::uint8_t> frame = input.subspan(begin, end - begin);
        begin = end + 1;
        Ubiquitty::processBytes(host.get(), std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()));
        // A frame the host takes and sends nobody: the next update reaches
        // across it, as one to a client that skipped generations does.
        if (!frame.empty() && (frame[0] & 1)) {
            (void)publisher.capture(*host);
            continue;
        }
        body.clear();
        if (!publisher.capture(*host) || publisher.rowUpdate(*host, baseline, body, allOptionalOps) != UpdateOutcome::Encoded)
            return;
        const ApplyOutcome outcome = applyRowUpdate(*client, body, replica);
        assert(outcome == ApplyOutcome::Applied);
        (void)outcome;
        const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
        assert(difference.empty());
    }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    switch (data[0] % 9) {
    case 0: {
        auto client = makeTerminal();
        const std::vector<std::uint8_t> before = clientFingerprint(*client);
        ScreenReplica replica;
        checkOutcome(applyCheckpoint(*client, input, replica), *client, before);
        break;
    }
    case 1: {
        Seed seed;
        std::vector<std::uint8_t> body = seed.checkpoint;
        flip(body, input);
        auto client = makeTerminal();
        const std::vector<std::uint8_t> before = clientFingerprint(*client);
        ScreenReplica replica;
        checkOutcome(applyCheckpoint(*client, body, replica), *client, before);
        break;
    }
    case 2:
    case 3: {
        Seed seed;
        auto client = makeTerminal();
        ScreenReplica replica;
        const ApplyOutcome seeded = applyCheckpoint(*client, seed.checkpoint, replica);
        assert(seeded == ApplyOutcome::Applied);
        (void)seeded;
        std::vector<std::uint8_t> body(input.begin(), input.end());
        if (data[0] % 9 == 3) {
            body = seed.update;
            flip(body, input);
        }
        const std::vector<std::uint8_t> before = clientFingerprint(*client);
        checkOutcome(applyRowUpdate(*client, body, replica), *client, before);
        break;
    }
    case 5:
    case 6: {
        std::vector<std::uint8_t> body(input.begin(), input.end());
        if (data[0] % 9 == 6) {
            body = historySeedBody();
            flip(body, input);
        }
        std::vector<DecodedLine> lines(1);
        lines[0].number = 0xDEAD;
        if (!decodeHistoryBody(body, 1'000, HistoryOrder::Ascending, historyBodyLimits, lines)) {
            assert(lines.size() == 1 && lines[0].number == 0xDEAD);
            break;
        }
        for (std::size_t index = 1; index < lines.size(); ++index) {
            const DecodedLine& line = lines[index];
            assert(line.number == 1'000 + index - 1);
            assert(line.cells.size() <= maxHistoryLineCells);
            for (const Ubiquitty::Cell& cell : line.cells) {
                assert(cell.linkID() <= line.links.size());
                assert(cell.clusterID() <= line.clusters.size());
            }
        }
        break;
    }
    case 7:
        fuzzWants(input);
        break;
    case 8:
        fuzzChainUpdates(input);
        break;
    default: {
        Ubiquitty::Pen pen {};
        Ubiquitty::penReset(&pen);
        const Ubiquitty::Pen pens[] = { pen, pen };
        const RowReferences references { .clusters = std::vector<bool>(1u << 15, false) };
        std::vector<Ubiquitty::Cell> cells(30);
        RowAttributes attributes;
        WireReader reader(input);
        (void)decodeRowRecord(reader, pens, references, cells, attributes);
        break;
    }
    }
    return 0;
}
