// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// ScriptedListingHost.h — a host for a directory errand to dial, in memory.
//
// The host end is a real Sync::HostConnection behind a channel that carries
// canonical frames in memory. Its greeting admits an unbound host target or
// an exact attach-only session in its explicit held inventory. A host handed
// a workspace lists itself to a connection that subscribes: it sends the
// workspace's structure and each pane's two records, as a daemon does.
//

#include "Harness.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/SyncLink.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Session/StatedPane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/Channel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace UbiquittyTest {

using namespace Ubiquitty;
namespace Sync = Ubiquitty::Protocol::Sync;

/// A dispatcher that runs nothing until the case says so.
struct QueueingDispatcher final : Session::Dispatcher {
    std::vector<Session::PostedWork> queued;

    void post(Session::PostedWork work) override { queued.push_back(std::move(work)); }
    /// Work posted for later, and when it is due on this dispatcher's clock.
    struct Delayed {
        double dueSeconds = 0;
        Session::PostedWork work;
        void operator()() && { std::move(work)(); }
    };
    /// Delayed work is the errand's deadlines and timers, kept until a case
    /// lets the time pass.
    std::vector<Delayed> delayed;
    /// The dispatcher's clock, which only `advance` moves.
    double nowSeconds = 0;
    void postDelayed(double seconds, Session::PostedWork work) override { delayed.push_back({ nowSeconds + seconds, std::move(work) }); }

    /// Lets every deadline armed so far pass, whenever it was due, then runs
    /// what that queued.
    void passDeadlines()
    {
        std::vector<Delayed> due;
        due.swap(delayed);
        for (auto& work : due)
            std::move(work)();
        drain();
    }

    /// Moves the clock on and runs what has come due, earliest first, and
    /// what that queued; work it arms for within the same span runs too.
    ///
    /// - Parameter seconds: How far the clock moves.
    void advance(double seconds)
    {
        const double until = nowSeconds + seconds;
        for (;;) {
            const auto next = std::ranges::min_element(delayed, {}, &Delayed::dueSeconds);
            if (next == delayed.end() || next->dueSeconds > until)
                break;
            // Each runs at the instant it was due, so what it arms is timed
            // from then and not from the end of the span.
            nowSeconds = std::max(nowSeconds, next->dueSeconds);
            Delayed due = std::move(*next);
            delayed.erase(next);
            std::move(due)();
            drain();
        }
        nowSeconds = until;
        drain();
    }

    /// Runs what was queued, and what that queued, until nothing is.
    void drain()
    {
        while (!queued.empty()) {
            std::vector<Session::PostedWork> running;
            running.swap(queued);
            for (auto& work : running)
                std::move(work)();
        }
    }
};

/// A pane a host runs, with the records a host reads for it.
struct HostedPane {
    Terminal terminal;
    std::unique_ptr<Sync::PaneSource> source;
    UbiquittyTest::StatedPane stated;

    /// Runs a pane and states what it shows.
    ///
    /// - Parameters:
    ///   - number: The low half of the pane's identity, and its session.
    ///   - title: The title its program set.
    ///   - program: Its foreground program.
    HostedPane(std::uint64_t number, std::string title, std::string program)
    {
        resize(terminal, 24, 80);
        stated.paneID = { .low = number, .high = 77 };
        stated.sessionID = static_cast<std::uint32_t>(number);
        stated.structuralGeneration = 1;
        stated.title.programTitle = std::move(title);
        stated.title.columns = 80;
        stated.title.rows = 24;
        stated.program = std::move(program);
        source = std::make_unique<Sync::PaneSource>(terminal, stated.paneID);
        source->publish(Sync::FrameClass::Structural, 1);
        restate();
    }

    /// Hands the pane's source the records the statement makes.
    void restate()
    {
        const Session::TreePaneFacts facts = stated.treeFacts();
        source->setFacts(Sync::boundedForWire(facts.facts));
        source->setStatus(Sync::boundedForWire(facts.records));
    }
};

/// What a scripted host holds: one workspace of one window whose tab is
/// split in two, and a pane behind each half.
struct HeldWorkspace {
    static constexpr std::uint32_t workspaceID = 9;
    std::shared_ptr<Session::Identity::Allocator> identities = std::make_shared<Session::Identity::Allocator>();
    std::unique_ptr<Session::Layout::Workspace> layout = std::make_unique<Session::Layout::Workspace>(identities);
    Session::Identity::LabelBook labels;
    /// The tags a person put on the workspace's windows, tabs and panes.
    Session::Identity::TagBook tags;
    Session::Layout::PaneID left {}, right {};
    std::map<std::uint32_t, HostedPane> panes;
    std::string name = "work";

    /// Splits the workspace's one tab in two and runs a pane in each half.
    HeldWorkspace()
    {
        const Session::Layout::SurfaceID window = layout->surfaceIDAt(0);
        left = layout->focusedPane(window);
        right = layout->splitFocusedPane(window, Panes::Orientation::Vertical, 0.5f);
        panes.try_emplace(std::to_underlying(left), 1, "make all", "make");
        panes.try_emplace(std::to_underlying(right), 2, "vim notes", "vim");
        panes.at(std::to_underlying(left)).stated.status.programState = OSC::ProgramState::Blocked;
        panes.at(std::to_underlying(left)).restate();
    }

    /// The pane behind a leaf of the layout.
    ///
    /// - Parameter pane: The leaf.
    /// - Returns: The pane.
    HostedPane& at(Session::Layout::PaneID pane) { return panes.at(std::to_underlying(pane)); }

    /// The tree the host itself builds of what it holds.
    ///
    /// - Parameter nowUnixMilliseconds: The host's clock.
    /// - Returns: The tree.
    [[nodiscard]] Session::SessionTree tree(std::uint64_t nowUnixMilliseconds) const
    {
        const std::array sources { Session::TreeWorkspaceSource { .id = workspaceID, .name = name, .layout = layout.get() } };
        return Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = nowUnixMilliseconds, .workspaces = sources, .tags = &tags }, labels,
            [this](std::uint32_t, Session::Layout::PaneID id) { return panes.at(std::to_underlying(id)).stated.treeFacts(); });
    }

    /// The workspace's structure as the host sends it.
    ///
    /// - Returns: The record's bytes.
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> record() const
    {
        std::vector<std::uint8_t> bytes;
        CHECK(Session::encodeWorkspaceStructure(bytes, tree(0).workspaces.front()));
        return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    }
};

/// The canonical host end of an errand's channel, without any pane attachment.
struct ScriptedHost {
    /// What the host holds, when it lists itself; a host with none refuses
    /// the subscription.
    HeldWorkspace* held = nullptr;
    /// Only these existing sessions may be selected by a bound errand.
    std::vector<std::uint32_t> heldSessionIDs;
    /// The host's wall clock, as its answer states it.
    std::uint64_t wallNowUnixMilliseconds = 5'000'000;
    /// The panes the host lists; every pane it holds unless a case withholds
    /// one.
    std::vector<Session::Layout::PaneID> withheld;
    /// A workspace the answer names beside the one it holds.
    std::optional<std::uint32_t> alsoNamed;
    /// Sent in place of the structure when set. The record then stands for
    /// all the host holds: the answer names no pane revision and the host
    /// lists no pane, as a host whose structure runs nothing would.
    std::shared_ptr<const std::vector<std::uint8_t>> recordOverride;
    std::uint64_t structureRevision = 0;
    bool didSubscribe = false; // the errand called the subscription
    /// The handle each pane is listed under.
    std::map<std::uint32_t, std::uint32_t> listedHandles;
    /// The exact credential bytes the errand's Greet presented.
    std::string helloToken;
    /// How many valid Greets the host has read: one per physical connection.
    int hellos = 0;
    /// How many physical connections were opened to the host.
    int connections = 0;
    /// What the bootstrap prints before any host speaks, when it ends there:
    /// a connection that carries these bytes and nothing the host says.
    std::string startupText;
    /// How many times the host found a connection silent and dropped it.
    int timesLost = 0;
    /// Whether the path has died without saying so: what the errand sends
    /// goes nowhere, the host sends nothing, and the carrier stays open.
    bool isPathDead = false; // bool-ok: names the one state it reports
    /// Whether the host holds its rows back until `releaseRows`.
    bool holdsRows = false; // bool-ok: names the one state it reports
    std::optional<std::uint32_t> heldRowsRequest;
    std::string heldRowsAnswer;
    /// Whether the host that lists itself holds nothing at all.
    bool holdsNothing = false; // bool-ok: names the one state it reports
    /// The most the host's credential check allows the peer.
    Sync::Role maxRole = Sync::Role::Owner;
    std::optional<Sync::HostConnection> connection;
    Sync::FrameIngress decoder { Sync::maxFrameBodyBytes };
    Transport::ChannelObserver* peer = nullptr;
    std::uint64_t now = 1;
    /// The canonical frame kinds the errand sent, in wire order.
    std::vector<Sync::FrameKind> received;
    /// The calls the errand made, as the host read them.
    std::vector<std::string> calls;
    /// The answer the host gives the next call; a result naming the method
    /// when empty. `$ID` in it stands for the id the call carried.
    std::string answer;
    /// Whether the host reads a call and says nothing until `releaseCalls`.
    bool holdsCalls = false; // bool-ok: names the one state it reports
    std::vector<std::uint32_t> heldCalls;

    /// The answer to one call: `answer` with the call's own id in it, or a
    /// served result under that id.
    ///
    /// - Parameter call: The call's text.
    /// - Returns: The JSON-RPC response.
    [[nodiscard]] std::string answerTo(const std::string& call) const
    {
        bool isJSON = false; // bool-ok: the parser's out flag
        const Codec::JSON::Value parsed = Codec::JSON::parse(call, isJSON);
        const Codec::JSON::Value* id = isJSON ? parsed.find("id") : nullptr;
        const std::string spelled = id ? Codec::JSON::serialize(*id) : std::string("1");
        if (answer.empty())
            return R"({"jsonrpc":"2.0","id":)" + spelled + R"(,"result":{"served":true}})";
        std::string response = answer;
        if (const std::size_t at = response.find("$ID"); at != std::string::npos)
            response.replace(at, 3, spelled);
        return response;
    }

    /// Answers the calls a host that holds them back was made.
    void releaseCalls()
    {
        if (!connection || !peer)
            return;
        for (std::size_t index = 0; index < heldCalls.size(); ++index)
            (void)connection->answerCall(heldCalls[index], answerTo(calls[calls.size() - heldCalls.size() + index]));
        heldCalls.clear();
        std::vector<std::uint8_t> out;
        collect(out, 1);
        if (!out.empty())
            peer->onData(out);
    }
    /// The actual connection state when the errand's canonical Bye arrived.
    std::optional<Sync::ConnectionState> stateAtExit;
    std::optional<Sync::ByeReason> byeAtExit;
    std::size_t panesAtExit = 0;

    /// Starts a new physical fixture connection without inheriting authority.
    void beginConnection()
    {
        ++connections;
        decoder.reset();
        listedHandles.clear();
        structureRevision = 0;
        heldRowsRequest.reset();
        heldRowsAnswer.clear();
        heldCalls.clear();
        Sync::HostIdentity identity;
        identity.hostInstanceID = { 1, 2 };
        identity.attachmentID = { 3, 4 };
        identity.resumeSecret = { 5, 6 };
        identity.principal = "test";
        identity.mayDrive = true;
        identity.verifiedKind = Sync::ClientKind::Human;
        identity.selection = { .target = Sync::AdmissionTarget::Control, .sessionID = 0, .workspaceID = 0, .daemonInstanceID = 99, .socketName = "host-test.sock", .buildFlags = 0, .hostPlatform = "test" };
        connection.emplace(UbiquittyTest::syncHostPolicy(0), identity);
        connection->setGreetingAdmission([this, identity](const Sync::Greet& greeting) mutable -> std::expected<Sync::HostIdentity, Sync::Bye> {
            const auto& request = greeting.request;
            const bool isHeldSession = request.target == Sync::AdmissionTarget::Session
                && request.isAttachOnly && request.sessionID && !request.workspaceID
                && std::ranges::find(heldSessionIDs, request.sessionID) != heldSessionIDs.end();
            if ((!Sync::isUnboundTarget(request.target) && !isHeldSession)
                || (greeting.request.expectedDaemonInstanceID && greeting.request.expectedDaemonInstanceID != identity.selection.daemonInstanceID))
                return std::unexpected(Sync::Bye { .reason = Sync::ByeReason::SessionGone, .detail = "the listing fixture has no such held target" });
            CHECK(!greeting.facts.viewportRows && !greeting.facts.viewportColumns);
            ++hellos;
            helloToken.clear();
            if (greeting.credential)
                helloToken.assign(greeting.credential->begin(), greeting.credential->end());
            identity.maxRole = maxRole;
            identity.selection.target = greeting.request.target;
            identity.selection.sessionID = isHeldSession ? request.sessionID : 0;
            return identity;
        });
    }

    /// Reads what the errand wrote and answers it.
    ///
    /// - Parameter bytes: The errand's bytes.
    void receive(std::span<const std::uint8_t> bytes)
    {
        if (isPathDead)
            return;
        if (!connection)
            beginConnection();
        std::vector<std::uint8_t> out;
        const auto result = decoder.consume(bytes, [](std::span<const std::uint8_t> prefix) {
            Sync::FrameHeader header;
            Sync::EnvelopeError error = Sync::EnvelopeError::None;
            const auto status = Sync::readFrameHeader(prefix, header, error);
            if (status == Sync::FrameHeaderState::NeedsMore)
                return Sync::FrameHeaderDecision {};
            if (status == Sync::FrameHeaderState::Failed)
                return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Refused };
            return Sync::FrameHeaderDecision { .state = Sync::FrameHeaderDecision::State::Admitted, .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
            Sync::FrameHeader header;
            Sync::EnvelopeError error = Sync::EnvelopeError::None;
            const auto status = Sync::readFrameHeader(prefix, header, error);
            CHECK(status == Sync::FrameHeaderState::Complete);
            if (status != Sync::FrameHeaderState::Complete)
                return Sync::FrameDelivery::Stop;
            received.push_back(header.kind);
            std::vector<std::uint8_t> frame(prefix.begin(), prefix.end());
            frame.insert(frame.end(), body.begin(), body.end());
            connection->receiveStream(frame, ++now);
            if (header.kind == Sync::FrameKind::Bye) {
                stateAtExit = connection->state();
                byeAtExit = connection->byeReason();
                panesAtExit = connection->boundPanes().size();
            }
            for (const Sync::ReceivedCall& call : connection->takeCalls()) {
                const auto request = Sync::decodeCallRequest(call.text);
                if (request.outcome == Protocol::Control::RequestOutcome::Respond) {
                    connection->refuseCall(call.callIndex, Codec::JSON::serialize(request.response), Sync::CallRefusal::Unreadable);
                    continue;
                }
                if (call.admission == Sync::CallAdmission::OverLimit) {
                    connection->refuseCall(call.callIndex, Sync::busyResponse(request.idOrNull()), Sync::CallRefusal::Busy);
                    continue;
                }
                if (request.method == "host.sessions.subscribe") {
                    std::string response = subscribed(call.text);
                    if (holdsRows) {
                        heldRowsRequest = call.callIndex;
                        heldRowsAnswer = std::move(response);
                    } else {
                        (void)connection->answerCall(call.callIndex, std::move(response));
                    }
                    continue;
                }
                calls.push_back(call.text);
                if (holdsCalls) {
                    heldCalls.push_back(call.callIndex);
                    continue;
                }
                (void)connection->answerCall(call.callIndex, answerTo(call.text));
            }
            collect(out, 1);
            return Sync::FrameDelivery::Continue; });
        CHECK(result.state != Sync::FrameIngress::State::Refused);
        if (!out.empty() && peer)
            peer->onData(out);
    }

    /// Answers the subscription, and owes the connection what a host that
    /// lists itself sends.
    ///
    /// - Parameter call: The exact subscription request.
    /// - Returns: The JSON-RPC response.
    std::string subscribed(const std::string& call)
    {
        using Codec::JSON::Value;
        didSubscribe = true;
        bool isJSON = false;
        const Value request = Codec::JSON::parse(call, isJSON);
        Value response = Value::object();
        response.set("jsonrpc", Value::string("2.0"));
        response.set("id", *request.find("id"));
        if (!held)
            return R"({"jsonrpc":"2.0","id":)" + Codec::JSON::serialize(*request.find("id")) + R"(,"error":{"code":-32601,"message":"no such method"}})";
        Value result = Value::object();
        Value workspaces = Value::array();
        Value revisions = Value::array();
        Value panes = Value::array();
        if (!holdsNothing) {
            offerStructure();
            workspaces.push(Value::number(HeldWorkspace::workspaceID));
            revisions.push(Value::string(std::to_string(structureRevision)));
            if (alsoNamed) {
                workspaces.push(Value::number(*alsoNamed));
                revisions.push(Value::string("1"));
            }
            for (auto& [id, pane] : held->panes) {
                if (recordOverride)
                    break;
                if (pane.stated.paneID != Sync::ID128 {}) {
                    Value expected = Value::object();
                    expected.set("session", Value::number(pane.stated.sessionID));
                    expected.set("revision", Value::number(pane.source->facts().revision));
                    panes.push(std::move(expected));
                }
                if (std::ranges::find(withheld, Session::Layout::PaneID { id }) != withheld.end())
                    continue;
                listedHandles[id] = connection->listPane(*pane.source, Sync::ListedDetail::Whole);
                CHECK(listedHandles[id] != 0);
            }
        }
        result.set("workspaces", std::move(workspaces));
        result.set("structureRevisions", std::move(revisions));
        result.set("paneRevisions", std::move(panes));
        result.set("panes", Value::number(listedHandles.size()));
        result.set("hostNowUnixMilliseconds", Value::number(wallNowUnixMilliseconds));
        response.set("result", std::move(result));
        return Codec::JSON::serialize(response);
    }

    /// Owes the connection the workspace's structure as it now stands.
    void offerStructure()
    {
        connection->offerWorkspaceStructure(HeldWorkspace::workspaceID, ++structureRevision, recordOverride ? recordOverride : held->record());
    }

    /// Services the connection and gathers its canonical frames.
    ///
    /// - Parameters:
    ///   - out: The buffer to extend.
    ///   - advance: How far the host's clock moves first, in microseconds.
    void collect(std::vector<std::uint8_t>& out, std::uint64_t advance)
    {
        now += advance;
        if (!connection)
            return;
        connection->service(now);
        while (connection->popStreamFrame(out, now)) {
        }
    }

    /// Moves the host's clock on, sends what it then owes, and drops the
    /// carrier when the host has found the connection silent, as a daemon
    /// drops a client it lost.
    ///
    /// - Parameter microseconds: How far the host's clock moves.
    void pass(std::uint64_t microseconds)
    {
        if (isPathDead) {
            now += microseconds;
            return;
        }
        std::vector<std::uint8_t> out;
        collect(out, microseconds);
        if (!out.empty() && peer)
            peer->onData(out);
        if (connection && connection->state() == Sync::ConnectionState::Lost && peer) {
            ++timesLost;
            Transport::ChannelObserver* dropped = peer;
            peer = nullptr;
            dropped->onClose("the connection closed");
        }
    }

    /// Lets the listing's cadence pass and sends what the host then owes.
    void pump()
    {
        std::vector<std::uint8_t> out;
        collect(out, 2 * Sync::listedRecordsIntervalMicroseconds);
        if (!out.empty() && peer)
            peer->onData(out);
    }

    /// Answers the rows a host that holds them back was asked for.
    void releaseRows()
    {
        if (!heldRowsRequest || !peer)
            return;
        std::vector<std::uint8_t> out;
        (void)connection->answerCall(*heldRowsRequest, std::move(heldRowsAnswer));
        heldRowsRequest.reset();
        collect(out, 1);
        peer->onData(out);
    }

    /// Sends a session list nobody asked for: bytes that reach the errand
    /// and change nothing it holds.
    void sendUnsolicitedRows()
    {
        std::vector<std::uint8_t> out;
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Result { .transfer = 0, .text = R"({"jsonrpc":"2.0","id":"unasked","result":{}})" });
        CHECK(Sync::appendFrame(out, Sync::FrameKind::Result, 0, body));
        if (peer)
            peer->onData(out);
    }

    /// Lets the listing's cadence pass and sends what the host then owes,
    /// with the host's canonical normal Bye behind it in the same bytes.
    void pumpThenExit()
    {
        std::vector<std::uint8_t> out;
        collect(out, 2 * Sync::listedRecordsIntervalMicroseconds);
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal });
        CHECK(Sync::appendFrame(out, Sync::FrameKind::Bye, 0, body));
        if (peer)
            peer->onData(out);
    }
};

/// A channel that carries an errand's bytes to a scripted host in memory.
struct HostChannel final : Transport::Channel {
    ScriptedHost& host;

    explicit HostChannel(ScriptedHost& scripted)
        : host(scripted)
    {
    }

    /// Connects at once: the host is in this process.
    ///
    /// - Parameter watcher: The errand's end.
    /// - Returns: True.
    bool start(Transport::ChannelObserver* watcher) override
    {
        host.beginConnection();
        if (!host.startupText.empty()) {
            watcher->onConnect();
            watcher->onData(std::span(reinterpret_cast<const std::uint8_t*>(host.startupText.data()), host.startupText.size()));
            return true;
        }
        host.peer = watcher;
        watcher->onConnect();
        return true;
    }
    /// Hands the errand's bytes to the host, which answers before this
    /// returns.
    ///
    /// - Parameter data: The bytes.
    void send(std::span<const std::uint8_t> data) override { host.receive(data); }
    /// Parts the host from the errand.
    void stop() override { host.peer = nullptr; }
    [[nodiscard]] Transport::Origin origin() const override { return {}; }
    [[nodiscard]] Transport::Health health() const override { return Transport::Health::Connected; }
    [[nodiscard]] std::string description() const override { return "a channel to a scripted host"; }
};

/// A factory whose every channel reaches one scripted host.
struct HostFactory final : Session::ChannelFactory {
    ScriptedHost& host;

    explicit HostFactory(ScriptedHost& scripted)
        : host(scripted)
    {
    }

    /// Hands back a channel to the host, at once.
    ///
    /// - Parameter done: Receives the channel.
    void createChannel(Session::TransportMode, Done done) override { done(std::make_unique<HostChannel>(host), false); }
};

} // namespace UbiquittyTest
