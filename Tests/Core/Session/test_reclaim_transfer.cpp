// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Image/TerminalImage.h"
#include "Core/LineStore.h"
#include "Core/Session/ReclaimTransfer.h"
#include "Core/Terminal.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Session;
namespace Sync = Ubiquitty::Protocol::Sync;
using UbiquittyTest::feed;

// Independent small receiver policy; no host, default configuration, socket,
// process, timer or PTY is constructed by this fixture.
constexpr Sync::HandoverTerminalLimits terminalLimits {
    .maxRecordBytes = 4 * 1024 * 1024,
    .maxCheckpointBytes = 1024 * 1024,
    .maxScreenCellBytes = 64 * 1024,
    .maxHostStateBytes = 256 * 1024,
    .maxLinkBytes = 64 * 1024,
    .maxParserBytes = 4096,
    .images = { 8192, Image::terminalImageBytesBudgetDefault },
    .history = { 256 * 1024, 256, 256, 16384 },
    .text = { 64 * 1024, ClusterPool::maxEntries, RunTextStore::maxEntries, 16384 },
    .visual = { 64 * 1024, Tek::tekMaxPrimitives },
    .sounds = { 64 * 1024, 0 },
};
constexpr ReclaimTransferLimits limits { terminalLimits, 4096, 5 * 1024 * 1024 };
constexpr AdoptOperation operation { { 51, 61 }, 17 };
constexpr ReclaimInputBarrier barrier { { 71, 81 }, 12 };
constexpr Protocol::Control::ReclaimTarget target {
    .handle = 3,
    .sessionID = 11,
    .daemonInstanceID = 17,
    .hostInstanceID = { 21, 31 },
    .attachmentID = { 41, 51 },
    .paneID = { 11, 21 },
};

/// An actual owner-aware FIFO whose sink takes only the chosen prefix.
struct QueuedSink {
    PAL::OutputQueue queue;
    std::string delivered;
    std::size_t allowance = 0;
    unsigned calls = 0;

    static int accept(void* context, std::span<const char> bytes)
    {
        auto& sink = *static_cast<QueuedSink*>(context);
        ++sink.calls;
        const auto count = std::min(bytes.size(), sink.allowance);
        sink.delivered.append(bytes.data(), count);
        sink.allowance -= count;
        return static_cast<int>(count);
    }
};

/// Uses the same real parser and raw-pixel Kitty pattern as the full-terminal
/// handover tests. Host-only replies/history/parser state are not a thin picture.
void fillTerminal(Terminal& terminal)
{
    terminal.scrollbackLimit = 128;
    resize(terminal, 6, 40);
    for (int index = 0; index < 18; ++index)
        feed(terminal, "retained history\r\n");
    feed(terminal, "\033]133;A\007prompt\033]133;B\007"
                   "\033]7;file://host/test/reclaim\007"
                   "\033]8;id=reclaim;https://example.test/reclaim\033\\linked\033]8;;\033\\"
                   "\033_Ga=T,f=32,s=1,v=1,i=7,p=9,c=2,r=3,C=1,q=2;/wAA/w==\033\\"
                   "\033[2;5r\033[3;4H\0337"
                   "\033[?1049halternate\033[?1049l\0338"
                   "\033[6n"
                   "a\314\201");
    terminal.answerback = "retained reclaim host state";
    terminal.sequenceGate.oscDeny[0] = 2;
    terminal.sequenceGate.oscDenyCount = 1;
}

struct Fixture {
    Terminal terminal;
    QueuedSink sink;
    Sync::HandoverBytes body;
    PAL::OutputQueue::Unwritten pending;
    bool ready = false;

    Fixture()
    {
        fillTerminal(terminal);
        auto encoded = Sync::encodeHandoverTerminal(terminal, 9, limits.terminal);
        CHECK(encoded.has_value());
        if (!encoded)
            return;
        body = std::move(*encoded);
        sink.allowance = 3;
        CHECK_EQ(sink.queue.submitOwned(81, std::span("abcdef", 6), QueuedSink::accept, &sink), 0);
        CHECK_EQ(sink.queue.submit(std::span("reply", 5), QueuedSink::accept, &sink), 0);
        auto copied = sink.queue.copyUnwritten();
        CHECK(copied.has_value());
        if (!copied)
            return;
        pending = std::move(*copied);
        ready = true;
    }

    ReclaimCapsuleView capsule() const
    {
        ReclaimCapsuleView value { .target = target, .operation = operation, .barrier = barrier, .inputWritten = barrier.finalInputSeq, .terminalBody = body.view(), .acceptedPTY = pending.bytes(), .acceptedRuns = pending.runs() };
        const auto digest = reclaimCapsuleSHA256(value, limits);
        CHECK(digest.has_value());
        if (digest)
            value.capsuleSHA256 = *digest;
        return value;
    }

    std::expected<std::unique_ptr<ReclaimTransfer>, ReclaimTransferError> prepare() const
    {
        return ReclaimTransfer::prepare(target, operation, barrier, capsule(), limits);
    }
};

ReclaimTransferEvidence evidence(AdoptDisposition disposition)
{
    ReclaimTransferEvidence value { .target = target, .barrier = barrier, .outcome = { .operation = operation, .disposition = disposition } };
    if (disposition != AdoptDisposition::Unknown)
        value.outcome.target = { target.sessionID, target.paneID };
    return value;
}

ReclaimTransferEvidence evidence(const ReclaimTransfer& transfer, AdoptDisposition disposition)
{
    auto value = evidence(disposition);
    value.capsuleSHA256 = transfer.capsuleSHA256();
    return value;
}

} // namespace

TEST(reclaimCapsuleOwnsTheCompleteTerminalAndExactAcceptedSuffix)
{
    Fixture source;
    REQUIRE(source.ready);
    const auto borrowed = source.capsule();
    REQUIRE(source.terminal.lineStore && source.terminal.lineStore->count() > 0);
    REQUIRE(source.terminal.images.livePlacementCount > 0);
    CHECK(source.sink.delivered == "abc");
    CHECK(std::string_view(source.pending.bytes().data(), source.pending.bytes().size()) == "defreply");
    const auto calls = source.sink.calls;
    auto prepared = source.prepare();
    REQUIRE(prepared);
    auto& transfer = **prepared;
    CHECK(transfer.ownership() == ReclaimOwnership::Daemon);
    CHECK(transfer.hasPreparedInstallation());
    CHECK(!transfer.takeCommitted());
    REQUIRE(transfer.beginCommit());
    CHECK(transfer.ownership() == ReclaimOwnership::Unresolved);
    REQUIRE(transfer.observe(evidence(transfer, AdoptDisposition::Committed)));
    CHECK(transfer.ownership() == ReclaimOwnership::Local);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    std::unique_ptr<ReclaimInstallation> installed;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        installed = transfer.takeCommitted();
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    REQUIRE(installed && installed->terminal.terminal);
    CHECK(!transfer.hasPreparedInstallation());
    CHECK(!transfer.takeCommitted());
    CHECK(installed->target == target && installed->operation == operation && installed->barrier == barrier);
    CHECK_EQ(installed->inputWritten, barrier.finalInputSeq);
    CHECK_EQ(installed->terminal.generation, 9u);
    const auto reproduced = Sync::encodeHandoverTerminal(*installed->terminal.terminal, 9, limits.terminal);
    REQUIRE(reproduced);
    CHECK(std::ranges::equal(reproduced->view(), borrowed.terminalBody));
    REQUIRE(installed->acceptedPTY.runs().size() == 2);
    CHECK_EQ(installed->acceptedPTY.runs()[0].owner, 81u);
    CHECK_EQ(installed->acceptedPTY.runs()[0].bytes, 3u);
    CHECK_EQ(installed->acceptedPTY.runs()[1].owner, 0u);

    QueuedSink successor;
    REQUIRE(successor.queue.restoreUnwritten(installed->acceptedPTY));
    CHECK_EQ(successor.calls, 0u);
    CHECK_EQ(successor.queue.submitOwned(99, std::span("later", 5), QueuedSink::accept, &successor), 0);
    successor.allowance = PAL::OutputQueue::maxPending;
    REQUIRE(successor.queue.drain(QueuedSink::accept, &successor) == 0);
    CHECK(source.sink.delivered + successor.delivered == "abcdefreplylater");
    CHECK_EQ(source.sink.calls, calls);
    CHECK_EQ(source.sink.queue.pendingBytes(), 8u);

    // The transferred model must still parse as a host, including REP, saved
    // cursor/margins and the previously open grapheme, not just show old cells.
    constexpr std::string_view continuation = "\314\202\033[b\0338\033[2;4H\033[3b\033[6n";
    feed(source.terminal, continuation);
    feed(*installed->terminal.terminal, continuation);
    const auto expected = Sync::encodeHandoverTerminal(source.terminal, 10, limits.terminal);
    const auto actual = Sync::encodeHandoverTerminal(*installed->terminal.terminal, 10, limits.terminal);
    REQUIRE(expected && actual);
    CHECK(std::ranges::equal(expected->view(), actual->view()));
    std::array<char, 256> left {}, right {};
    const auto leftCount = drainOutput(&source.terminal, left);
    const auto rightCount = drainOutput(installed->terminal.terminal.get(), right);
    REQUIRE(leftCount >= 0 && static_cast<std::size_t>(leftCount) <= left.size());
    REQUIRE(rightCount >= 0 && static_cast<std::size_t>(rightCount) <= right.size());
    CHECK(std::ranges::equal(std::span(left).first(static_cast<std::size_t>(leftCount)),
        std::span(right).first(static_cast<std::size_t>(rightCount))));
}

TEST(reclaimCapsuleRequiresEveryExactIdentityBeforeMaterialization)
{
    Fixture source;
    REQUIRE(source.ready);
    const auto valid = source.capsule();
    const auto refuse = [&](auto change) {
        auto invalid = valid;
        change(invalid);
        const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
        CHECK(!ReclaimTransfer::prepare(target, operation, barrier, invalid, limits));
        CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
        CHECK_EQ(source.sink.queue.pendingBytes(), 8u);
    };
    refuse([](auto& value) { ++value.target.handle; });
    refuse([](auto& value) { ++value.target.sessionID; });
    refuse([](auto& value) { ++value.target.daemonInstanceID; });
    refuse([](auto& value) { ++value.target.hostInstanceID.high; });
    refuse([](auto& value) { ++value.target.attachmentID.low; });
    refuse([](auto& value) { ++value.target.paneID.high; });
    refuse([](auto& value) { ++value.operation.operationID.high; });
    refuse([](auto& value) { ++value.operation.daemonInstanceID; });
    refuse([](auto& value) { ++value.barrier.clientInstance.low; });
    refuse([](auto& value) { ++value.barrier.finalInputSeq; });
    auto invalidTarget = target;
    invalidTarget.attachmentID = {};
    CHECK(ReclaimTransfer::prepare(invalidTarget, operation, barrier, valid, limits).error() == ReclaimTransferError::InvalidIdentity);
    CHECK(ReclaimTransfer::prepare(target, { {}, operation.daemonInstanceID }, barrier, valid, limits).error() == ReclaimTransferError::InvalidIdentity);
    CHECK(ReclaimTransfer::prepare(target, operation, { {}, barrier.finalInputSeq }, valid, limits).error() == ReclaimTransferError::InvalidIdentity);
}

TEST(reclaimBarrierRequiresAKnownFinalQueueAdmissionReceiptNotKernelDelivery)
{
    Fixture source;
    REQUIRE(source.ready);
    auto capsule = source.capsule();
    capsule.inputWritten.reset();
    CHECK(reclaimCapsuleSHA256(capsule, limits).error() == ReclaimTransferError::InputBarrier);
    capsule.inputWritten = barrier.finalInputSeq - 1;
    CHECK(reclaimCapsuleSHA256(capsule, limits).error() == ReclaimTransferError::InputBarrier);
    capsule.inputWritten = barrier.finalInputSeq + 1;
    CHECK(reclaimCapsuleSHA256(capsule, limits).error() == ReclaimTransferError::InputBarrier);
    capsule.inputWritten = barrier.finalInputSeq;
    CHECK_EQ(source.sink.queue.pendingBytes(), 8u);
    CHECK(reclaimCapsuleSHA256(capsule, limits));
    // Zero is valid only when supplied as an explicit known receipt for the
    // exact zero-numbered barrier; a missing ledger entry stays absent.
    capsule.barrier.finalInputSeq = 0;
    capsule.inputWritten = 0;
    CHECK(reclaimCapsuleSHA256(capsule, limits));
    capsule.inputWritten.reset();
    CHECK(!reclaimCapsuleSHA256(capsule, limits));
}

TEST(reclaimCapsuleDigestBindsMetadataLengthsPartitionAndEverySuffixByte)
{
    Fixture source;
    REQUIRE(source.ready);
    const auto valid = source.capsule();
    const auto differs = [&](auto change) {
        auto changed = valid;
        change(changed);
        const auto digest = reclaimCapsuleSHA256(changed, limits);
        REQUIRE(digest);
        CHECK(*digest != valid.capsuleSHA256);
    };
    differs([](auto& value) { ++value.target.handle; });
    differs([](auto& value) { ++value.target.sessionID; });
    differs([](auto& value) { ++value.target.hostInstanceID.low; });
    differs([](auto& value) { ++value.target.attachmentID.high; });
    differs([](auto& value) { ++value.target.paneID.high; });
    differs([](auto& value) { ++value.operation.operationID.low; });
    differs([](auto& value) { ++value.operation.daemonInstanceID; ++value.target.daemonInstanceID; });
    differs([](auto& value) { ++value.barrier.clientInstance.high; });
    differs([](auto& value) { ++value.barrier.finalInputSeq; ++*value.inputWritten; });
    std::string suffix(valid.acceptedPTY.begin(), valid.acceptedPTY.end());
    suffix[0] = 'X';
    differs([&](auto& value) { value.acceptedPTY = suffix; });
    auto runs = std::vector<PAL::OutputQueue::Run>(valid.acceptedRuns.begin(), valid.acceptedRuns.end());
    runs[0].owner = 82;
    differs([&](auto& value) { value.acceptedRuns = runs; });
    --runs[0].bytes;
    ++runs[1].bytes;
    differs([&](auto& value) { value.acceptedRuns = runs; });
    auto corrupt = valid;
    corrupt.capsuleSHA256[0] ^= 1;
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, corrupt, limits).error() == ReclaimTransferError::DigestMismatch);
}

TEST(reclaimCapsuleEnforcesComponentAndPartitionBoundsIndependently)
{
    Fixture source;
    REQUIRE(source.ready);
    const auto valid = source.capsule();
    auto bounded = limits;
    bounded.maxAcceptedPTYBytes = valid.acceptedPTY.size() - 1;
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, valid, bounded).error() == ReclaimTransferError::OverCapacity);
    bounded = limits;
    bounded.maxCapsuleBytes = valid.terminalBody.size() + valid.acceptedPTY.size();
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, valid, bounded).error() == ReclaimTransferError::OverCapacity);
    bounded = limits;
    bounded.terminal.maxRecordBytes = valid.terminalBody.size() - 1;
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, valid, bounded).error() == ReclaimTransferError::OverCapacity);
    bounded = limits;
    bounded.terminal.maxScreenCellBytes = 1;
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, valid, bounded).error() == ReclaimTransferError::TerminalRefused);
    bounded = limits;
    bounded.maxCapsuleBytes = 0;
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, valid, bounded).error() == ReclaimTransferError::InvalidPolicy);
    auto invalid = valid;
    invalid.acceptedRuns = {};
    CHECK(reclaimCapsuleSHA256(invalid, limits).error() == ReclaimTransferError::InvalidPartition);
    std::array<PAL::OutputQueue::Run, 2> runs { PAL::OutputQueue::Run { 81, 0 }, PAL::OutputQueue::Run { 0, 8 } };
    invalid.acceptedRuns = runs;
    CHECK(reclaimCapsuleSHA256(invalid, limits).error() == ReclaimTransferError::InvalidPartition);
    runs = { PAL::OutputQueue::Run { 81, 3 }, PAL::OutputQueue::Run { 81, 5 } };
    CHECK(reclaimCapsuleSHA256(invalid, limits).error() == ReclaimTransferError::InvalidPartition);
    runs = { PAL::OutputQueue::Run { 81, 3 }, PAL::OutputQueue::Run { 0, 6 } };
    CHECK(reclaimCapsuleSHA256(invalid, limits).error() == ReclaimTransferError::InvalidPartition);
    auto body = std::vector<std::uint8_t>(valid.terminalBody.begin(), valid.terminalBody.end());
    body.back() ^= 1;
    invalid = valid;
    invalid.terminalBody = body;
    const auto digest = reclaimCapsuleSHA256(invalid, limits);
    REQUIRE(digest);
    invalid.capsuleSHA256 = *digest;
    CHECK(ReclaimTransfer::prepare(target, operation, barrier, invalid, limits).error() == ReclaimTransferError::TerminalRefused);
    CHECK_EQ(source.sink.queue.pendingBytes(), 8u);
}

TEST(reclaimLostReleaseKeepsTheCompletePreparedOwnerAndNeverThawsOnUnknown)
{
    Fixture source;
    REQUIRE(source.ready);
    auto prepared = source.prepare();
    REQUIRE(prepared);
    auto& transfer = **prepared;
    // Unrequested release cannot activate a local reader.
    CHECK(!transfer.observe(evidence(transfer, AdoptDisposition::Committed)));
    CHECK(transfer.ownership() == ReclaimOwnership::Daemon);
    REQUIRE(transfer.beginCommit());
    const auto unknown = evidence(transfer, AdoptDisposition::Unknown);
    REQUIRE(transfer.observe(unknown));
    REQUIRE(transfer.observe(evidence(transfer, AdoptDisposition::Prepared)));
    CHECK(transfer.ownership() == ReclaimOwnership::Unresolved);
    CHECK(transfer.hasPreparedInstallation());
    CHECK(!transfer.takeCommitted());
    auto wrong = evidence(transfer, AdoptDisposition::Committed);
    ++wrong.outcome.operation.daemonInstanceID;
    CHECK(!transfer.observe(wrong));
    wrong = evidence(transfer, AdoptDisposition::Committed);
    ++wrong.target.attachmentID.low;
    CHECK(!transfer.observe(wrong));
    wrong = evidence(transfer, AdoptDisposition::Committed);
    ++wrong.barrier.finalInputSeq;
    CHECK(!transfer.observe(wrong));
    wrong = evidence(transfer, AdoptDisposition::Committed);
    wrong.capsuleSHA256[0] ^= 1;
    CHECK(!transfer.observe(wrong));
    wrong = evidence(transfer, AdoptDisposition::Committed);
    ++wrong.outcome.target.sessionID;
    CHECK(!transfer.observe(wrong));
    CHECK(transfer.ownership() == ReclaimOwnership::Unresolved);
    CHECK(transfer.hasPreparedInstallation());
    REQUIRE(transfer.beginCommit());
    const auto released = evidence(transfer, AdoptDisposition::Committed);
    REQUIRE(transfer.observe(released));
    REQUIRE(transfer.observe(released));
    CHECK(!transfer.observe(evidence(transfer, AdoptDisposition::Aborted)));
    auto installation = transfer.takeCommitted();
    REQUIRE(installation && installation->terminal.terminal);
    CHECK(std::string_view(installation->acceptedPTY.bytes().data(), installation->acceptedPTY.bytes().size()) == "defreply");
    CHECK(!transfer.takeCommitted());
}

TEST(reclaimTerminalNegativeEvidenceKeepsDaemonOwnershipNotLocalPermission)
{
    Fixture source;
    REQUIRE(source.ready);
    for (const auto disposition : { AdoptDisposition::Aborted, AdoptDisposition::Expired, AdoptDisposition::Rejected }) {
        auto prepared = source.prepare();
        REQUIRE(prepared);
        auto& transfer = **prepared;
        REQUIRE(transfer.beginCommit());
        auto result = evidence(transfer, disposition);
        if (disposition == AdoptDisposition::Rejected)
            result.outcome.rejection = AdoptRejection::InvalidCandidate;
        REQUIRE(transfer.observe(result));
        CHECK(transfer.ownership() == ReclaimOwnership::Daemon);
        CHECK(transfer.hasPreparedInstallation());
        CHECK(!transfer.takeCommitted());
        CHECK(!transfer.beginCommit());
    }
}

TEST(reclaimPreparationAllocationRefusalNeverConsumesTheOriginalOwners)
{
    Fixture source;
    REQUIRE(source.ready);
    const auto capsule = source.capsule();
    const auto refused = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(sizeof(Terminal));
        const auto prepared = ReclaimTransfer::prepare(target, operation, barrier, capsule, limits);
        CHECK(!prepared && prepared.error() == ReclaimTransferError::NoMemory);
    }
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() > refused);
    CHECK_EQ(source.sink.queue.pendingBytes(), 8u);
    CHECK(source.sink.delivered == "abc");
    const auto after = Sync::encodeHandoverTerminal(source.terminal, 9, limits.terminal);
    REQUIRE(after);
    CHECK(std::ranges::equal(after->view(), capsule.terminalBody));
    REQUIRE(source.prepare());
}

UBIQUITTY_TEST_MAIN()
