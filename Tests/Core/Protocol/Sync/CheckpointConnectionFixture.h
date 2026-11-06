// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// In-memory canonical checkpoint ingress and scoped fallible allocation probes.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace UbiquittyTest::CheckpointConnection {

using namespace Ubiquitty::Protocol::Sync;

struct AllocationObservation {
    std::size_t refusedOrdinal = 0;
    std::atomic<std::size_t> attempts { 0 };
    std::atomic<std::size_t> refusals { 0 };
    std::atomic<std::size_t> requestedBytes { 0 };
};

enum class AllocationForm {
    Object,
    Array
};

// Each observation belongs to a synchronous receive scope. The replacement
// allocation functions keep the ordinary allocation/deallocation pairing.
class ScopedAllocationObservation {
public:
    explicit ScopedAllocationObservation(AllocationObservation& observation)
        : m_previous(active().exchange(&observation))
    {
    }

    ~ScopedAllocationObservation() { active().store(m_previous); }
    ScopedAllocationObservation(const ScopedAllocationObservation&) = delete;
    ScopedAllocationObservation& operator=(const ScopedAllocationObservation&) = delete;

    static void* allocate(std::size_t bytes, AllocationForm form)
    {
        if (AllocationObservation* observation = active().load()) {
            if (++observation->attempts == observation->refusedOrdinal) {
                ++observation->refusals;
                return nullptr;
            }
            observation->requestedBytes += bytes;
        }
        return form == AllocationForm::Array ? ::operator new[](bytes) : ::operator new(bytes);
    }

private:
    static std::atomic<AllocationObservation*>& active()
    {
        static std::atomic<AllocationObservation*> observation { nullptr };
        return observation;
    }

    AllocationObservation* m_previous;
};

constexpr ID128 firstPaneID { 9, 10 };
constexpr ID128 secondPaneID { 11, 12 };

inline ClientPolicy policyWithCellAllowance(std::size_t allowance)
{
    ClientPolicy policy { .maxCheckpointBytes = 1 << 20, .maxBufferedUpdateBytes = 1 << 20, .minAckIntervalMicroseconds = 4'000 };
    policy.maxDecodedScreenCellBytes = allowance;
    return policy;
}

inline ClientIdentity identityFixture()
{
    ClientIdentity identity;
    identity.clientInstance = { 1, 2 };
    identity.facts.viewportRows = 3;
    identity.facts.viewportColumns = 9;
    return identity;
}

inline std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(terminal.get(), 64);
    Ubiquitty::resize(*terminal, rows, columns);
    return terminal;
}

inline std::vector<std::uint8_t> checkpointOf(const Ubiquitty::Terminal& terminal, std::uint64_t generation = 1)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(terminal) && publisher.checkpoint(terminal, generation, body, baseline));
    return body;
}

inline std::vector<std::uint8_t> frameOf(FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> frame;
    CHECK(appendFrame(frame, kind, channel, body));
    return frame;
}

template<typename Message>
std::vector<std::uint8_t> messageFrame(FrameKind kind, std::uint32_t channel, const Message& message)
{
    std::vector<std::uint8_t> body;
    encode(body, message);
    return frameOf(kind, channel, body);
}

class RecordingPurgeSink final : public HistoryPurgeSink {
public:
    HistoryPurgeOutcome purge(const HistoryKey&, std::uint64_t) override
    {
        ++calls;
        return HistoryPurgeOutcome::Purged;
    }

    std::size_t calls = 0;
};

struct ConnectionFixture {
    explicit ConnectionFixture(const ClientPolicy& policy)
        : client(policy, identityFixture(), nullptr, &purges, &replicas)
    {
    }

    bool start()
    {
        client.start();
        (void)client.takeStreamOutput();
        Welcome welcome;
        welcome.wireVersion = wireVersion;
        welcome.hostInstanceID = { 41, 42 };
        welcome.attachmentID = { 3, 4 };
        welcome.resumeSecret = { 5, 6 };
        welcome.selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "checkpoint-budget.sock", .buildFlags = 0, .hostPlatform = "fixture" };
        client.receiveStream(messageFrame(FrameKind::Welcome, 0, welcome), 1);
        return client.state() == ConnectionState::Attached;
    }

    void bind(std::uint32_t handle, ID128 paneID)
    {
        client.receiveStream(messageFrame(FrameKind::PaneBind, 0, PaneBind { .handle = handle, .paneID = paneID }), 2);
    }

    void checkpoint(std::uint32_t handle, std::span<const std::uint8_t> body, std::uint32_t transfer, std::uint64_t now, AllocationObservation* observation = nullptr)
    {
        REQUIRE(body.size() >= 16);
        WireReader generationReader(body);
        const std::uint64_t generation = generationReader.u64();
        WireReader digestReader(body.last(16));
        const Digest128 digest { .low = digestReader.u64(), .high = digestReader.u64() };
        const auto head = messageFrame(FrameKind::CheckpointHead, handle, CheckpointHead { .generation = generation, .transfer = transfer, .frameClass = FrameClass::Structural, .stateDigest = digest });
        client.receiveStream(head, now);
        BulkChunk chunk { .transfer = transfer, .index = 0, .isLast = true, .bytes = body };
        Ubiquitty::Crypto::sha256(body, chunk.sha256);
        const auto frame = messageFrame(FrameKind::BulkChunk, 0, chunk);
        REQUIRE(frame.size() > 1);
        // Buffer the complete envelope and all but the final payload byte first.
        // Only the last byte delivers the checkpoint, so the observation does
        // not count the frame-ingress buffer that necessarily precedes admission.
        client.receiveStream(std::span(frame).first(frame.size() - 1), now);
        if (observation) {
            ScopedAllocationObservation scope(*observation);
            client.receiveStream(std::span(frame).last(1), now);
        } else
            client.receiveStream(std::span(frame).last(1), now);
    }

    bool seedHistory(std::uint32_t handle, const Ubiquitty::Terminal& source)
    {
        const HistoryCache* history = client.historyCache(handle);
        if (!history || !history->head())
            return false;
        const std::uint64_t firstLine = history->head() - 1;
        HistoryPublisher publisher;
        std::vector<std::uint8_t> historyBody;
        if (!publisher.capture(source) || publisher.writeBody(source, { firstLine, firstLine + 1 }, HistoryOrder::Ascending, 4'096, historyBody) != 1)
            return false;
        std::vector<std::uint8_t> body;
        encode(body, HistoryInlineHeader { .historyEpoch = history->key().epoch, .firstLine = firstLine });
        body.insert(body.end(), historyBody.begin(), historyBody.end());
        client.receiveStream(frameOf(FrameKind::HistoryInline, handle, body), 4);
        return client.state() == ConnectionState::Attached && history->lineCount() == 1;
    }

    ReplicaSet replicas;
    RecordingPurgeSink purges;
    ClientConnection client;
};

inline void checkReplicaUnchanged(const ScreenReplica& actual, const ScreenReplica& before)
{
    CHECK(actual.generation == before.generation);
    CHECK(actual.dictionaryEpoch == before.dictionaryEpoch);
    REQUIRE(actual.pens().size() == before.pens().size());
    if (!actual.pens().empty())
        CHECK(std::memcmp(actual.pens().data(), before.pens().data(), actual.pens().size() * sizeof(Ubiquitty::Pen)) == 0);
    CHECK(actual.maxPens == before.maxPens);
    CHECK(actual.storage == before.storage);
    CHECK(actual.blockHashes == before.blockHashes);
    CHECK(actual.historyEpoch == before.historyEpoch);
    CHECK(actual.historyHead == before.historyHead);
    CHECK(actual.stateDigest == before.stateDigest);
    CHECK(actual.scratch == before.scratch);
    CHECK(actual.echoContext == before.echoContext);
}

} // namespace UbiquittyTest::CheckpointConnection
