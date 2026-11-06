// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_sync_connection.cpp — libFuzzer harness for both ends of a Sync
// connection (Source/Core/Protocol/Sync: Envelope, Messages, Lanes,
// Connection).
//
// Every byte a connection reads is its peer's choice. The input is fed to an
// end already past its handshake, with a pane bound and a checkpoint applied,
// so frames reach the screen plane and not only the envelope; or it flips
// bytes of a valid host stream recorded here, so a fixed corpus reaches the
// update and transfer paths past their first check. The first byte picks:
//
//   0  the input as stream bytes to an attached client
//   1  the input as stream bytes to an attached host
//   2  the input cut into datagrams (a length byte each) to both ends
//   3  a recorded host stream with the input's (offset, mask) pairs applied,
//      read by a fresh client
//   4  the input as the body of every connection-plane message
//   5  the input as a script for a host with four panes and a client: writes,
//      resizes, binds, unbinds, views, correlated Calls and Results,
//      listings begun and ended, workspace structures offered and withdrawn,
//      the clock, and either direction of the link held or cut into pieces
//
// Contracts asserted beyond memory safety:
//
//   - a closed connection stays closed whatever it reads next;
//   - the generation a client's pane holds never moves backward while the
//     pane stays bound;
//   - a frame decoder never buffers more than one capped frame past a failure;
//   - under a script, where every byte is the host's own: no client history
//     cache holds an epoch its host has not opened; neither end closes,
//     no update fails its digest, two bound panes never share a handle, the
//     bytes outstanding stay within the window and one frame, the bulk queued
//     stays within the caller cap and one checkpoint, no client pane is ahead
//     of its host, and once the link is released and every pane shown, every
//     bound pane converges on its host's newest generation and holds its
//     host's facts and status records, every listed pane holds its host's
//     records, and the client holds the newest structure of each workspace
//     still standing and none that was withdrawn;
//   - a `PaneFacts` or `StatusRecords` body a decoder accepts is within every
//     cap, and encodes back to a body that decodes to the same record.
//
// Build via Scripts/run-tests.sh --with-fuzz; ctest drives it over a fixed
// corpus through Tests/FuzzDriver.cpp.

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Records.h"
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
    .datagramMaxBytes = 1100,
    .maxCheckpointBytes = 1 << 20,
};

constexpr ClientPolicy clientPolicy { .maxCheckpointBytes = 1 << 20, .maxBufferedUpdateBytes = 1 << 16, .minAckIntervalMicroseconds = 4'000 };

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

/// A host pane and both ends of one connection, attached, with the pane
/// bound and its checkpoint applied, and the host's stream as it was sent.
struct Session {
    std::unique_ptr<Ubiquitty::Terminal> terminal = std::make_unique<Ubiquitty::Terminal>();
    std::unique_ptr<PaneSource> source;
    HostConnection host { hostPolicy, hostIdentity() };
    ClientConnection client { clientPolicy, clientIdentity() };
    std::vector<std::uint8_t> hostStream;
    std::uint64_t now = 1'000'000;

    Session()
    {
        Ubiquitty::resize(*terminal, 6, 30);
        source = std::make_unique<PaneSource>(*terminal, ID128 { 1, 2 });
        Ubiquitty::processBytes(terminal.get(), "$ \x1b[1;31mred\x1b[0m \xe4\xb8\xad e\xcc\x81 \x1b]8;;https://example.com/\x1b\\link\x1b]8;;\x1b\\");
        source->publish(FrameClass::Structural, now);
        client.start();
        pump();
        pump();
        assert(host.state() == ConnectionState::Attached && client.state() == ConnectionState::Attached);
        const std::uint32_t handle = host.bindPane(*source);
        assert(handle != 0);
        // A bound pane is delivered once the client asks to see it, as every
        // client does; the checkpoint then takes a few passes to cross.
        for (int pass = 0; pass < 64 && !client.paneTerminal(handle); ++pass) {
            host.service(now);
            pump();
        }
        client.setView(handle, ViewState::Live);
        for (int pass = 0; pass < 64 && client.counters().checkpointsApplied == 0; ++pass) {
            host.service(now);
            pump();
        }
        assert(host.state() == ConnectionState::Attached && client.state() == ConnectionState::Attached);
        assert(client.paneGeneration(handle) != 0 && client.counters().checkpointsApplied != 0);
        pump();
        Ubiquitty::processBytes(terminal.get(), "\r\n\x1b[35mnew \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9");
        source->publish(FrameClass::Settled, now);
        now += 20'000;
        host.service(now);
        pump();
    }

    void pump()
    {
        std::vector<std::uint8_t> stream;
        while (host.popStreamFrame(stream, now)) {
        }
        hostStream.insert(hostStream.end(), stream.begin(), stream.end());
        client.receiveStream(stream, now);
        for (const std::vector<std::uint8_t>& datagram : host.takeDatagrams())
            client.receiveDatagram(datagram, now);
        client.tick(now);
        host.receiveStream(client.takeStreamOutput(), now);
    }
};

void flip(std::vector<std::uint8_t>& bytes, std::span<const std::uint8_t> pairs)
{
    if (bytes.empty())
        return;
    for (std::size_t index = 0; index + 1 < pairs.size(); index += 2)
        bytes[pairs[index] * 131u % bytes.size()] ^= pairs[index + 1];
}

/// Feeds a client in pieces, holding it to the contracts after each.
void feedClient(ClientConnection& client, std::span<const std::uint8_t> bytes, std::uint64_t now)
{
    for (std::size_t offset = 0; offset < bytes.size(); offset += 97) {
        const bool wasClosed = client.state() == ConnectionState::Closed;
        const std::uint64_t held = client.paneGeneration(1);
        const std::uint64_t bindings = client.counters().paneBindings;
        client.receiveStream(bytes.subspan(offset, std::min<std::size_t>(97, bytes.size() - offset)), now);
        client.tick(now + 10'000);
        assert(!wasClosed || client.state() == ConnectionState::Closed);
        // An unbind ends the pane, and a bind of the handle starts a new one.
        assert(client.counters().paneBindings != bindings || client.paneGeneration(1) >= held);
    }
}

/// Reads a script's bytes in order, 0 past the end.
struct ScriptReader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;

    [[nodiscard]] bool isDone() const { return offset >= bytes.size(); }
    std::uint8_t next() { return offset < bytes.size() ? bytes[offset++] : 0; }
};

/// A host of four panes and one client, run by a script.
struct Scenario {
    struct Pane {
        std::unique_ptr<Ubiquitty::Terminal> terminal = std::make_unique<Ubiquitty::Terminal>();
        std::unique_ptr<PaneSource> source;
        std::uint32_t handle = 0;
        /// The handle the pane is listed under, or 0.
        std::uint32_t listed = 0;
    };

    /// A workspace's structure as the script last offered it.
    struct Offered {
        std::uint64_t revision = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> record;
    };
    std::array<Offered, 3> structures;

    struct Exchange {
        std::string request;
        std::string response;
        std::string refusal;
    };

    // Sources outlive connections that retain their addresses until teardown.
    std::array<Pane, 4> panes;
    HostConnection host { hostPolicy, hostIdentity() };
    ClientConnection client { clientPolicy, clientIdentity() };
    std::vector<Exchange> calls;
    std::map<std::string, bool> owedResults;
    std::size_t takenCalls = 0;
    std::uint64_t now = 1'000'000;
    std::uint64_t nextPaneID = 1;
    bool isUpstreamHeld = false;
    bool isDownstreamHeld = false;
    std::size_t downstreamPiece = 0;
    std::vector<std::uint8_t> heldDownstream;
    std::vector<std::uint8_t> heldUpstream;

    Scenario()
    {
        for (Pane& pane : panes) {
            Ubiquitty::resize(*pane.terminal, 6, 30);
            pane.source = std::make_unique<PaneSource>(*pane.terminal, ID128 { nextPaneID++, 7 });
            pane.source->publish(FrameClass::Structural, now);
        }
        client.start();
        pump();
        pump();
        assert(host.state() == ConnectionState::Attached && client.state() == ConnectionState::Attached);
    }

    /// Moves what each end has to send, respecting the holds.
    void pump()
    {
        std::vector<std::uint8_t> stream;
        while (host.popStreamFrame(stream, now)) {
        }
        heldDownstream.insert(heldDownstream.end(), stream.begin(), stream.end());
        const std::vector<std::vector<std::uint8_t>> datagrams = host.takeDatagrams();
        if (!isDownstreamHeld) {
            // A stream may arrive in any pieces; a datagram may not arrive.
            const std::size_t piece = downstreamPiece == 0 ? heldDownstream.size() : downstreamPiece;
            for (std::size_t offset = 0; offset < heldDownstream.size(); offset += piece)
                client.receiveStream(std::span(heldDownstream).subspan(offset, std::min(piece, heldDownstream.size() - offset)), now);
            heldDownstream.clear();
            for (const std::vector<std::uint8_t>& datagram : datagrams)
                client.receiveDatagram(datagram, now);
        }
        client.tick(now);
        const std::vector<std::uint8_t> upstream = client.takeStreamOutput();
        heldUpstream.insert(heldUpstream.end(), upstream.begin(), upstream.end());
        for (const std::vector<std::uint8_t>& datagram : client.takeDatagrams()) {
            if (!isUpstreamHeld)
                host.receiveDatagram(datagram, now);
        }
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
        for (const std::string& result : client.takeResults()) {
            const auto held = owedResults.find(result);
            assert(held != owedResults.end() && !held->second);
            held->second = true;
        }
    }

    /// One service and one pump, a frame later.
    void step(std::uint64_t microseconds = 16'667)
    {
        now += microseconds;
        host.service(now);
        pump();
    }

    /// Holds the ends to what the host's own bytes promise.
    void check() const
    {
        assert(host.state() != ConnectionState::Closed);
        assert(client.state() != ConnectionState::Closed);
        assert(client.counters().digestMismatches == 0);
        constexpr std::uint64_t cappedFrame = maxEnvelopeHeaderBytes + maxFrameBodyBytes;
        assert(host.pacer().outstandingBytes() <= std::max(host.pacer().windowBytes(now), cappedFrame) + 2 * cappedFrame);
        assert(host.queuedBulkBytes() <= std::size_t { host.limits().maxQueuedBulkBytes } + hostPolicy.maxCheckpointBytes);
        for (std::size_t first = 0; first < panes.size(); ++first) {
            const Pane& pane = panes[first];
            if (pane.handle == 0)
                continue;
            assert(client.paneGeneration(pane.handle) <= pane.source->generation());
            // A record the client holds is one its host set, never a later one.
            if (const PaneFacts* facts = client.paneFacts(pane.handle))
                assert(facts->revision <= pane.source->facts().revision);
            if (const std::optional<StatusRecords> status = client.paneStatus(pane.handle))
                assert(status->revision <= pane.source->status().revision);
            // The cache never holds an epoch the host has not opened: a line of
            // a cleared epoch cannot outlive the clear, nor arrive after it.
            if (const HistoryCache* cache = client.historyCache(pane.handle))
                assert(cache->key().epoch <= pane.source->historyAddress().epoch);
            for (std::size_t second = first + 1; second < panes.size(); ++second)
                assert(panes[second].handle != pane.handle);
        }
        // A listed pane's records are ones its host set, and a listing is
        // nobody's binding.
        for (const Pane& pane : panes) {
            if (pane.listed == 0)
                continue;
            if (const PaneFacts* facts = client.listedPaneFacts(pane.source->paneID()))
                assert(facts->revision <= pane.source->facts().revision);
            assert(pane.listed >= firstListedHandle && client.paneTerminal(pane.listed) == nullptr);
        }
        for (std::size_t index = 0; index < structures.size(); ++index) {
            if (const auto held = client.workspaceStructures().find(static_cast<std::uint32_t>(index + 1)); held != client.workspaceStructures().end())
                assert(held->second.revision <= structures[index].revision);
        }
    }

    /// Publishes a pane's frame with its records set from its terminal, as a
    /// host does: what a program's bytes changed beside the picture travels
    /// with it.
    void publish(Pane& pane, FrameClass frameClass)
    {
        pane.source->setFacts(paneFactsOf(*pane.terminal, {}));
        pane.source->setStatus(statusRecordsOf(*pane.terminal));
        pane.source->publish(frameClass, now);
    }

    /// The text a script writes: its own bytes, escapes and all.
    static std::string text(ScriptReader& script)
    {
        std::string bytes;
        const std::size_t length = script.next() % 48;
        for (std::size_t index = 0; index < length; ++index)
            bytes.push_back(static_cast<char>(script.next()));
        return bytes;
    }

    /// Runs one operation of the script.
    void run(ScriptReader& script)
    {
        Pane& pane = panes[script.next() % panes.size()];
        switch (script.next() % 15) {
        case 0:
            Ubiquitty::processBytes(pane.terminal.get(), text(script));
            publish(pane, FrameClass::Settled);
            break;
        case 1:
            Ubiquitty::resize(*pane.terminal, 1 + script.next() % 40, 2 + script.next() % 120);
            publish(pane, FrameClass::Structural);
            break;
        case 2:
            if (pane.handle == 0) {
                // A listing holds the source it lists: it lets go first.
                if (pane.listed != 0) {
                    host.unlistPane(pane.listed);
                    pane.listed = 0;
                }
                // A pane bound again is a new pane to the client.
                pane.source = std::make_unique<PaneSource>(*pane.terminal, ID128 { nextPaneID++, 7 });
                publish(pane, FrameClass::Structural);
                pane.handle = host.bindPane(*pane.source);
            }
            break;
        case 3:
            if (pane.handle != 0) {
                host.unbindPane(pane.handle, 0);
                pane.handle = 0;
            }
            break;
        case 4:
            if (pane.handle != 0)
                client.setView(pane.handle, static_cast<ViewState>(script.next() % 3));
            break;
        case 5:
            step(std::uint64_t { script.next() } * 1'000);
            break;
        case 6:
            isUpstreamHeld = !isUpstreamHeld;
            break;
        case 7:
            isDownstreamHeld = !isDownstreamHeld;
            break;
        case 8:
            downstreamPiece = script.next();
            break;
        case 9:
            if (pane.handle != 0) {
                // JSON-RPC control stays on channel zero. Its params name the
                // exact pane; no retired alternate pane encoding is emitted.
                std::string payload;
                for (const unsigned char byte : text(script)) {
                    constexpr char hex[] = "0123456789abcdef";
                    payload.push_back(hex[byte >> 4]);
                    payload.push_back(hex[byte & 15]);
                }
                const std::string id = std::to_string(calls.size() + 1);
                const std::string paneLow = std::to_string(pane.source->paneID().low);
                const std::string paneHigh = std::to_string(pane.source->paneID().high);
                Exchange exchange {
                    .request = R"({"jsonrpc":"2.0","id":")" + id + R"(","method":"fuzz.echo","params":{"paneLow":)" + paneLow + R"(,"paneHigh":)" + paneHigh + R"(,"text":")" + payload + R"("}})",
                    .response = R"({"jsonrpc":"2.0","id":")" + id + R"(","result":")" + payload + R"("})",
                    .refusal = R"({"jsonrpc":"2.0","id":")" + id + R"(","error":{"code":-32000,"message":"Busy"}})",
                };
                if (client.call(exchange.request))
                    calls.push_back(std::move(exchange));
            }
            break;
        case 10: {
            // A burst big enough that the pane's checkpoint takes chunks.
            std::string burst;
            const int lines = 20 + script.next() * 4;
            for (int line = 0; line < lines; ++line)
                burst += "line " + std::to_string(line) + " of a burst long enough to wrap at thirty\r\n";
            Ubiquitty::processBytes(pane.terminal.get(), burst);
            publish(pane, script.next() % 2 ? FrameClass::Flood : FrameClass::Settled);
            break;
        }
        case 11:
            // A listing begins or ends: the pane's records and nothing else.
            if (pane.listed == 0) {
                pane.listed = host.listPane(*pane.source, ListedDetail::Whole);
            } else {
                host.unlistPane(pane.listed);
                pane.listed = 0;
            }
            break;
        case 12: {
            // A workspace's structure, small enough for a frame or not.
            Offered& offered = structures[script.next() % structures.size()];
            const std::size_t length = script.next() % 4 == 0 ? 70'000 + std::size_t { script.next() } * 64 : 1 + script.next();
            auto record = std::make_shared<std::vector<std::uint8_t>>(length);
            for (std::size_t index = 0; index < length; ++index)
                (*record)[index] = static_cast<std::uint8_t>(index * 7 + offered.revision);
            offered.record = std::move(record);
            ++offered.revision;
            host.offerWorkspaceStructure(static_cast<std::uint32_t>(&offered - structures.data()) + 1, offered.revision, offered.record);
            break;
        }
        case 13: {
            Offered& offered = structures[script.next() % structures.size()];
            if (offered.record) {
                offered.record.reset();
                ++offered.revision;
                host.withdrawWorkspaceStructure(static_cast<std::uint32_t>(&offered - structures.data()) + 1);
            }
            break;
        }
        default:
            step();
            break;
        }
    }

    /// Whether the client holds what its host listed and offered: each listed
    /// pane's records, and the newest structure of each workspace standing.
    [[nodiscard]] bool holdsListing() const
    {
        for (const Pane& pane : panes) {
            if (pane.listed == 0)
                continue;
            const PaneFacts* facts = client.listedPaneFacts(pane.source->paneID());
            const StatusRecords* status = client.listedPaneStatus(pane.source->paneID());
            if ((pane.source->facts().revision != 0 && (!facts || !(*facts == pane.source->facts()))) || (pane.source->status().revision != 0 && (!status || !(*status == pane.source->status()))))
                return false;
        }
        for (std::size_t index = 0; index < structures.size(); ++index) {
            const auto held = client.workspaceStructures().find(static_cast<std::uint32_t>(index + 1));
            const bool isHeld = held != client.workspaceStructures().end();
            if (structures[index].record ? (!isHeld || held->second.record != *structures[index].record) : isHeld)
                return false;
        }
        return true;
    }

    /// Releases the link, shows every bound pane and runs until the client
    /// holds each one's newest generation.
    void settle()
    {
        isUpstreamHeld = false;
        isDownstreamHeld = false;
        downstreamPiece = 0;
        for (const Pane& pane : panes) {
            if (pane.handle != 0)
                client.setView(pane.handle, ViewState::Live);
        }
        for (int round = 0; round < 2'000 && host.state() == ConnectionState::Attached; ++round) {
            step();
            const bool isConverged = holdsListing() && holdsResults() && std::ranges::all_of(panes, [&](const Pane& pane) {
                return pane.handle == 0 || (client.paneGeneration(pane.handle) == pane.source->generation() && holdsRecords(pane));
            });
            if (isConverged)
                return;
        }
        // A link held past the silence bound loses the connection, which is
        // the rule and not a divergence.
        if (host.state() == ConnectionState::Lost)
            return;
        for (const Pane& pane : panes)
            assert(pane.handle == 0 || (client.paneGeneration(pane.handle) == pane.source->generation() && holdsRecords(pane)));
        assert(holdsListing());
        assert(holdsResults());
    }

    /// Every accepted call was taken once and answered under its own ID.
    [[nodiscard]] bool holdsResults() const
    {
        return takenCalls == calls.size() && std::ranges::all_of(owedResults, [](const auto& entry) { return entry.second; });
    }

    /// Whether the client holds a bound pane's records as its host set them.
    [[nodiscard]] bool holdsRecords(const Pane& pane) const
    {
        const PaneFacts& facts = pane.source->facts();
        const StatusRecords& status = pane.source->status();
        const PaneFacts* heldFacts = client.paneFacts(pane.handle);
        // One carrier a reader: a pane held Live has its status in its
        // replica, with no revision, and any other as the set the host sent.
        const std::optional<StatusRecords> heldStatus = client.paneStatus(pane.handle);
        return (facts.revision == 0 || (heldFacts && *heldFacts == facts)) && (status.revision == 0 || (heldStatus && heldStatus->store == status.store));
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    switch (data[0] % 6) {
    case 0: {
        Session session;
        feedClient(session.client, input, session.now);
        break;
    }
    case 1: {
        Session session;
        const bool wasClosed = session.host.state() == ConnectionState::Closed;
        session.host.receiveStream(input, session.now);
        session.host.service(session.now + 20'000);
        std::vector<std::uint8_t> stream;
        while (session.host.popStreamFrame(stream, session.now + 20'000)) {
        }
        assert(!wasClosed || session.host.state() == ConnectionState::Closed);
        break;
    }
    case 2: {
        Session session;
        std::size_t offset = 0;
        while (offset < input.size()) {
            const std::size_t length = std::min<std::size_t>(input[offset], input.size() - offset - 1);
            const std::span<const std::uint8_t> datagram = input.subspan(offset + 1, length);
            session.client.receiveDatagram(datagram, session.now);
            session.host.receiveDatagram(datagram, session.now);
            offset += 1 + length;
        }
        session.host.service(session.now + 20'000);
        break;
    }
    case 3: {
        Session session;
        std::vector<std::uint8_t> stream = session.hostStream;
        flip(stream, input);
        ClientConnection fresh(clientPolicy, clientIdentity());
        fresh.start();
        feedClient(fresh, stream, session.now);
        break;
    }
    case 5: {
        Scenario scenario;
        ScriptReader script { input };
        for (int operation = 0; operation < 512 && !script.isDone(); ++operation) {
            scenario.run(script);
            if (scenario.host.state() == ConnectionState::Lost)
                return 0;
            scenario.check();
        }
        scenario.settle();
        break;
    }
    default: {
        (void)decodeGreet(input);
        (void)decodeWelcome(input);
        (void)decodeFacts(input);
        (void)decodeAck(input);
        (void)decodeProbe(input);
        (void)decodeEcho(input);
        (void)decodePaneBind(input);
        (void)decodePaneUnbind(input);
        (void)decodeBulkChunk(input);
        if (const std::optional<Bye> bye = decodeBye(input)) {
            assert(bye->reason <= lastByeReasonOnWire);
            assert(bye->reason != ByeReason::HandingOver || (!isRefusal(bye->reason) && !isPermanentRefusal(bye->reason)));
            std::vector<std::uint8_t> encoded;
            encode(encoded, *bye);
            assert(std::ranges::equal(encoded, input));
        }
        (void)decodeCheckpointHead(input);
        (void)decodeUpdateHeader(input);
        (void)decodeResync(input);
        (void)decodeView(input);
        (void)decodeBulkAbort(input);
        (void)decodeCall(input);
        if (const std::optional<NotifyHead> notify = decodeNotifyHead(input)) {
            // The record is the body after the kind, whatever the kind.
            assert(notify->record.data() + notify->record.size() == input.data() + input.size());
        }
        if (const std::optional<Result> result = decodeResult(input)) {
            // A result names a transfer or carries its text, never both.
            assert(result->transfer == 0 || result->text.empty());
            std::vector<std::uint8_t> encoded;
            encode(encoded, *result);
            assert(std::ranges::equal(encoded, input));
        }
        (void)decodeReceipt(input);
        if (const std::optional<PaneFacts> facts = decodePaneFacts(input)) {
            // Facts a decoder accepts are within every cap, and encode back
            // to a body that reads as the same facts.
            assert(boundedForWire(*facts) == *facts);
            std::vector<std::uint8_t> encoded;
            encodePaneFacts(encoded, *facts);
            assert(encoded.size() <= maxFrameBodyBytes);
            const std::optional<PaneFacts> again = decodePaneFacts(encoded);
            assert(again && *again == *facts);
        }
        if (std::optional<StatusFragment> fragment = decodeStatusFragment(input)) {
            assert(fragment->total <= Ubiquitty::OSC::programStatusLimit);
            assert(fragment->first + fragment->records.size() <= fragment->total);
            for (const Ubiquitty::OSC::ProgramStatus& record : fragment->records)
                assert(Ubiquitty::OSC::validProgramStatus(record));
            // Whatever the frame continues, a set an assembly yields is whole
            // and within the cap.
            StatusAssembly assembly;
            const std::uint32_t total = fragment->total;
            if (assembly.accept(std::move(*fragment)) == StatusAssembly::Outcome::Whole)
                assert(assembly.take().store.records.size() == total);
        }
        (void)decodeHistoryInlineHeader(input);
        (void)decodeHistoryHead(input);
        (void)decodeHistoryReset(input);
        if (const std::optional<HistoryWant> want = decodeHistoryWant(input)) {
            // A want the decoder accepts is one the host can use as it is.
            assert(want->ranges.size() <= maxHistoryWantRanges);
            for (std::size_t index = 0; index < want->ranges.size(); ++index) {
                assert(want->ranges[index].from < want->ranges[index].to);
                assert(index == 0 || want->ranges[index - 1].to <= want->ranges[index].from);
            }
        }
        FrameDecoder decoder;
        decoder.append(input);
        while (decoder.next()) {
        }
        if (decoder.error() != EnvelopeError::None) {
            decoder.append(input);
            assert(decoder.bufferedBytes() <= input.size());
        }
        break;
    }
    }
    return 0;
}
