// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The former Catalog corpus now exercises canonical replacements. The filename
// retains the existing bounded corpus registration, not an alternate wire codec.
// Four selectors retain host/client scripts and malformed fragmented/coalesced
// envelopes: 0 host Notify/PaneFacts, 1 client Call and inline/bulk Result,
// 2 retired kind 35 to an attached client, 3 retired kind 35 to an attached host.
// Calls and notifications retain exact bytes and order; Results retain exact
// correlation regardless of inline/bulk completion order. Bound pane records
// converge on their newest revision. Retired kind 35 closes either reader,
// even for empty input, and no traffic after that failure reopens it.

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr HostPolicy hostPolicy {
    .pacing = { .windowFloorBytes = 16 * 1024, .probeIdleMicroseconds = 2'000'000, .roundTripStaleMicroseconds = 10'000'000, .rateFilterMicroseconds = 10'000'000, .datagramLossFloorMicroseconds = 20'000 },
    .lanes = { .bulkSharePermille = 250 },
    .datagramMaxBytes = 0,
    .maxCheckpointBytes = 1 << 20,
};

constexpr ClientPolicy clientPolicy { .maxCheckpointBytes = 1 << 20, .maxBufferedUpdateBytes = 1 << 16, .minAckIntervalMicroseconds = 4'000 };

/// Reads a bounded script's bytes in order, zero past the end.
struct ScriptReader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    [[nodiscard]] bool isDone() const { return offset >= bytes.size(); }
    std::uint8_t next() { return offset < bytes.size() ? bytes[offset++] : 0; }
};

HostIdentity hostIdentity()
{
    HostIdentity identity;
    identity.hostInstanceID = { 10, 0 };
    identity.attachmentID = { 1, 10 };
    identity.resumeSecret = { 2, 10 };
    identity.mayDrive = true;
    identity.verifiedKind = ClientKind::Human;
    identity.selection = { .target = AdmissionTarget::Session, .sessionID = 9, .workspaceID = 0, .daemonInstanceID = 10, .socketName = "test.sock", .buildFlags = 0, .hostPlatform = {} };
    return identity;
}

ClientIdentity clientIdentity()
{
    ClientIdentity identity;
    identity.clientInstance = { 3, 10 };
    identity.facts.viewportRows = 6;
    identity.facts.viewportColumns = 30;
    identity.facts.refreshIntervalMicroseconds = 16'667;
    return identity;
}

/// Produces valid JSON string contents around the complete wire body's cap.
std::string payload(ScriptReader& script, std::size_t wrapperBytes)
{
    constexpr std::size_t sizes[] = { 0, 1, 7, 200, maxFrameBodyBytes - 1, maxFrameBodyBytes, maxFrameBodyBytes + 1, 3 * maxFrameBodyBytes / 2 };
    const std::uint8_t fill = script.next();
    const std::size_t bodyBytes = sizes[script.next() % std::size(sizes)];
    std::string text(bodyBytes > wrapperBytes ? bodyBytes - wrapperBytes : 0, 'a');
    for (std::size_t index = 0; index < text.size(); ++index)
        text[index] = static_cast<char>('a' + (fill + index) % 26);
    return text;
}

/// Never retains callback borrows or reenters the connection.
struct Recorder final : ClientPaneDelegate {
    std::map<std::uint32_t, ID128> bindings;

    void paneBound(std::uint32_t handle, ID128 paneID) override
    {
        assert(!bindings.contains(handle));
        bindings.emplace(handle, paneID);
    }

    void paneUnbound(std::uint32_t handle, ID128 paneID) override
    {
        assert(bindings.contains(handle) && bindings.at(handle) == paneID);
        bindings.erase(handle);
    }

    void paneRecordsChanged(std::uint32_t handle, ID128 paneID) override
    {
        assert(bindings.contains(handle) && bindings.at(handle) == paneID);
    }
};

/// A host of three panes and independent expected control transcripts.
struct Link {
    struct Pane {
        std::unique_ptr<Ubiquitty::Terminal> terminal = std::make_unique<Ubiquitty::Terminal>();
        std::unique_ptr<PaneSource> source;
        std::uint32_t handle = 0;
    };
    struct Exchange {
        std::string request;
        std::string response;
        std::string refusal;
    };

    // Sources and callback storage outlive both readers and their destructors.
    std::array<Pane, 3> panes;
    Recorder recorder;
    HostConnection host { hostPolicy, hostIdentity() };
    ClientConnection client { clientPolicy, clientIdentity(), &recorder };
    std::vector<std::vector<std::uint8_t>> sentNotifies;
    std::vector<Exchange> calls;
    std::map<std::string, bool> owedResults;
    std::size_t takenNotifies = 0;
    std::size_t takenCalls = 0;
    std::uint64_t now = 1'000'000;
    std::uint64_t nextPaneID = 1;
    bool isUpstreamHeld = false;
    std::vector<std::uint8_t> heldUpstream;

    Link()
    {
        for (Pane& pane : panes)
            Ubiquitty::resize(*pane.terminal, 6, 30);
        client.start();
        pump();
        pump();
        assert(host.state() == ConnectionState::Attached && client.state() == ConnectionState::Attached);
    }

    void pump()
    {
        std::vector<std::uint8_t> stream;
        while (host.popStreamFrame(stream, now)) {
        }
        client.receiveStream(stream, now);
        for (const Notify& notification : client.takeNotifies()) {
            assert(takenNotifies < sentNotifies.size());
            std::vector<std::uint8_t> body;
            encode(body, notification);
            assert(body == sentNotifies[takenNotifies]);
            ++takenNotifies;
        }
        for (const std::string& result : client.takeResults()) {
            const auto held = owedResults.find(result);
            assert(held != owedResults.end() && !held->second);
            held->second = true;
        }
        client.tick(now);
        const std::vector<std::uint8_t> upstream = client.takeStreamOutput();
        heldUpstream.insert(heldUpstream.end(), upstream.begin(), upstream.end());
        if (!isUpstreamHeld) {
            host.receiveStream(heldUpstream, now);
            heldUpstream.clear();
            for (const ReceivedCall& call : host.takeCalls()) {
                assert(takenCalls < calls.size());
                const Exchange& expected = calls[takenCalls++];
                assert(call.text == expected.request);
                if (call.admission == CallAdmission::Admitted && host.answerCall(call.callIndex, expected.response) == CallAnswer::Sent) {
                    const auto inserted = owedResults.emplace(expected.response, false);
                    assert(inserted.second);
                } else {
                    host.refuseCall(call.callIndex, expected.refusal, CallRefusal::Busy);
                    const auto inserted = owedResults.emplace(expected.refusal, false);
                    assert(inserted.second);
                }
            }
        }
    }

    void step()
    {
        now += 16'667;
        host.service(now);
        pump();
    }

    void bind(Pane& pane)
    {
        if (pane.handle)
            return;
        pane.source = std::make_unique<PaneSource>(*pane.terminal, ID128 { nextPaneID++, 9 });
        pane.source->publish(FrameClass::Structural, now);
        pane.handle = host.bindPane(*pane.source);
    }

    void unbind(Pane& pane)
    {
        if (!pane.handle)
            return;
        host.unbindPane(pane.handle, 0);
        pane.handle = 0;
    }

    [[nodiscard]] bool isConverged() const
    {
        if (takenNotifies != sentNotifies.size() || takenCalls != calls.size()
            || !std::ranges::all_of(owedResults, [](const auto& entry) { return entry.second; }))
            return false;
        return std::ranges::all_of(panes, [&](const Pane& pane) {
            if (!pane.handle)
                return true;
            const PaneFacts* facts = client.paneFacts(pane.handle);
            return client.paneGeneration(pane.handle) == pane.source->generation()
                && (!pane.source->facts().revision || (facts && *facts == pane.source->facts()));
        });
    }

    void settle()
    {
        isUpstreamHeld = false;
        for (int round = 0; round < 4'000 && host.state() == ConnectionState::Attached; ++round) {
            step();
            if (isConverged())
                return;
        }
        if (host.state() != ConnectionState::Lost)
            assert(isConverged());
    }

    void sendCall(ScriptReader& script)
    {
        const std::string id = std::to_string(calls.size() + 1);
        const std::string prefix = R"({"jsonrpc":"2.0","id":")" + id + R"(","result":")";
        const std::string suffix = R"("})";
        Exchange exchange {
            .request = R"({"jsonrpc":"2.0","id":")" + id + R"(","method":"fuzz.echo","params":{}})",
            // An inline Result prepends the zero transfer's one-byte varint.
            .response = prefix + payload(script, 1 + prefix.size() + suffix.size()) + suffix,
            .refusal = R"({"jsonrpc":"2.0","id":")" + id + R"(","error":{"code":-32000,"message":"Busy"}})",
        };
        if (client.call(exchange.request))
            calls.push_back(std::move(exchange));
    }
};

/// Runs canonical control/records over the corpus's bounded pane churn.
void runScript(ScriptReader script, bool sendsCalls)
{
    Link link;
    for (Link::Pane& pane : link.panes)
        link.bind(pane);
    link.settle();
    for (int operation = 0; operation < 256 && !script.isDone(); ++operation) {
        Link::Pane& pane = link.panes[script.next() % link.panes.size()];
        switch (script.next() % 6) {
        case 0:
            link.bind(pane);
            break;
        case 1:
            link.unbind(pane);
            break;
        case 2:
            if (sendsCalls) {
                link.sendCall(script);
            } else {
                // A kind a client stores unread; its record is opaque here.
                const std::string text = payload(script, 1);
                std::vector<std::uint8_t> notification { std::to_underlying(NotifyKind::Events) };
                notification.insert(notification.end(), text.begin(), text.end());
                const bool sent = link.host.sendNotify(notification);
                assert(sent == (notification.size() <= maxFrameBodyBytes));
                if (sent)
                    link.sentNotifies.push_back(std::move(notification));
            }
            break;
        case 3:
            if (pane.handle) {
                PaneFacts facts;
                facts.terminalName = "pane " + std::to_string(pane.source->paneID().low) + " revision " + std::to_string(script.next());
                pane.source->setFacts(std::move(facts));
            }
            break;
        case 4:
            link.isUpstreamHeld = !link.isUpstreamHeld;
            break;
        default:
            link.step();
            break;
        }
        link.pump();
        if (link.host.state() == ConnectionState::Lost)
            return;
        assert(link.host.state() == ConnectionState::Attached && link.client.state() == ConnectionState::Attached);
    }
    link.settle();
}

/// Manufactures only a retired kind byte, never a compatibility codec.
/// Every subsequent fuzz body remains capped and length-prefixed.
std::vector<std::uint8_t> retiredFrames(std::span<const std::uint8_t> input, std::uint32_t channel)
{
    assert(channel && channel < 128 && !frameKindOf(35));
    std::vector<std::uint8_t> stream { 35, static_cast<std::uint8_t>(channel), 0 };
    for (std::size_t offset = 0; offset < input.size();) {
        const std::size_t length = std::min<std::size_t>(input[offset], input.size() - offset - 1);
        stream.push_back(35);
        stream.push_back(static_cast<std::uint8_t>(channel));
        stream.push_back(static_cast<std::uint8_t>(length < 128 ? length : (length & 127) | 128));
        if (length >= 128)
            stream.push_back(static_cast<std::uint8_t>(length >> 7));
        stream.insert(stream.end(), input.begin() + offset + 1, input.begin() + offset + 1 + length);
        offset += 1 + length;
    }
    return stream;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (!size)
        return 0;
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    switch (data[0] % 4) {
    case 0:
        runScript({ input }, false);
        break;
    case 1:
        runScript({ input }, true);
        break;
    default: {
        Link link;
        link.bind(link.panes[0]);
        link.settle();
        const std::vector<std::uint8_t> stream = retiredFrames(input, link.panes[0].handle);
        for (std::size_t offset = 0; offset < stream.size(); offset += 61) {
            const auto piece = std::span(stream).subspan(offset, std::min<std::size_t>(61, stream.size() - offset));
            if (data[0] % 4 == 2) {
                link.client.receiveStream(piece, link.now);
                assert(link.client.state() == ConnectionState::Closed);
            } else {
                link.host.receiveStream(piece, link.now);
                assert(link.host.state() == ConnectionState::Closed);
            }
        }
        // A well-formed control frame cannot revive the failed decoder.
        std::vector<std::uint8_t> body;
        encode(body, Probe { .time = link.now });
        std::vector<std::uint8_t> valid;
        const bool appended = appendFrame(valid, FrameKind::Probe, 0, body);
        assert(appended);
        if (data[0] % 4 == 2) {
            link.client.receiveStream(valid, link.now);
            assert(link.client.state() == ConnectionState::Closed);
        } else {
            link.host.receiveStream(valid, link.now);
            assert(link.host.state() == ConnectionState::Closed);
        }
        break;
    }
    }
    return 0;
}
