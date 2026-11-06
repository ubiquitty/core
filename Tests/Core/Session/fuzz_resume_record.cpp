// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_resume_record.cpp — libFuzzer harness for the persisted resume record
// (Source/Core/Session/ResumeRecordFormat.{h,cpp}).
//
// A record is read back from storage the client does not fully control: a
// browser's origin storage, a file under an editor's data directory, or a
// sealed file whose plaintext a bug could still shape. The decoder must refuse
// cleanly or yield a record that is bounded and canonical: an accepted record
// materializes, re-encodes to the same bytes, and refuses every truncation.
//
// The input is tried as a whole record, and as edits to a valid record with a
// picture and drafts, so a fixed corpus reaches the paths behind the header.

#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Session/ResumeRecordFormat.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace Ubiquitty::Protocol::Sync;
using namespace Ubiquitty::Session;

constexpr std::size_t budget = 1 << 20;

/// A record with one pictured pane, one bare pane and two drafts.
std::vector<std::uint8_t> seedRecord()
{
    ResumeRecord record { .hostInstanceID = { 1, 2 }, .clientInstance = { 3, 4 }, .attachmentID = { 5, 6 }, .resumeSecret = { 7, 8 }, .certificatePin = { 9 }, .panes = {}, .drafts = {} };
    ResumeRecordPane pictured { .paneID = { 11, 12 }, .lastInputSeq = 37, .view = ViewState::Dormant };
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 12);
    Ubiquitty::processBytes(&terminal, "\x1b[31mr\x1b[0m");
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    if (publisher.capture(terminal) && publisher.checkpoint(terminal, 17, pictured.checkpoint, baseline)) {
        Ubiquitty::Terminal passive;
        if (applyCheckpoint(passive, pictured.checkpoint, pictured.replica) == ApplyOutcome::Applied)
            pictured.checkpointDigest = digestOf(pictured.checkpoint);
        else
            pictured.checkpoint.clear();
    }
    pictured.effects = { .acknowledged = 2, .consumed = 3 };
    record.panes.push_back(std::move(pictured));
    record.panes.push_back({ .paneID = { 13, 14 }, .lastInputSeq = 1 });
    record.drafts.push_back({ { 11, 12 }, RawInput { { 'l', 's' } } });
    record.drafts.push_back({ { 11, 12 }, PasteInput { { 'p' }, 0, PasteEnd::Final } });
    const auto encoded = encodeResumeRecord(record, budget);
    assert(encoded && "the seed record encodes");
    return encoded ? std::vector<std::uint8_t>(encoded->view().begin(), encoded->view().end()) : std::vector<std::uint8_t> {};
}

/// An accepted record is bounded, materializes and is canonical.
///
/// - Parameter input: Candidate record bytes.
void check(std::span<const std::uint8_t> input)
{
    const auto view = decodeResumeRecord(input, budget);
    if (!view)
        return;
    assert(view->paneCount <= maxPanesPerConnection);
    const auto owned = materializeResumeRecord(input, budget, budget * 4);
    if (!owned) {
        // Only the storage budget may refuse what validation accepted.
        assert(owned.error() == ResumeRecordError::TooLarge || owned.error() == ResumeRecordError::AllocationFailed);
        return;
    }
    assert(owned->panes.size() == view->paneCount);
    assert(owned->drafts.size() == view->draftCount);
    for (const auto& pane : owned->panes)
        assert(pane.checkpoint.empty() || digestOf(pane.checkpoint) == pane.checkpointDigest);
    const auto encoded = encodeResumeRecord(*owned, budget);
    assert(encoded && "a materialized record re-encodes");
    assert(std::ranges::equal(encoded->view(), input) && "decode then encode is identity");
    for (const std::size_t length : { std::size_t { 0 }, input.size() / 2, input.size() - 1 })
        assert(!decodeResumeRecord(input.first(length), budget));
    std::vector<std::uint8_t> extended(input.begin(), input.end());
    extended.push_back(0);
    assert(!decodeResumeRecord(extended, budget));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::uint8_t> input(data, size);
    check(input);
    static const std::vector<std::uint8_t> seed = seedRecord();
    if (seed.empty())
        return 0;
    // Pairs of bytes edit the seed at an offset, so every field is reachable.
    auto edited = seed;
    for (std::size_t index = 1; index < input.size(); index += 2)
        edited[input[index - 1] * 7919u % edited.size()] = input[index];
    check(edited);
    // A prefix of the seed with the input appended exercises every boundary.
    std::vector<std::uint8_t> spliced(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(size % seed.size()));
    spliced.insert(spliced.end(), input.begin(), input.end());
    check(spliced);
    return 0;
}
