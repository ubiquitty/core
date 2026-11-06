// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_session_directory — a directory errand's channel is a Sync
// connection that binds no pane.
//
// The errand's first Greet requests an unbound control connection: the host
// welcomes it, grants it a role, and serves the calls that role admits. The
// errand says Bye{Normal} before it goes so the host keeps no attachment
// waiting for a resume.
//
// A listing errand also subscribes to the host's listing on that connection
// and assembles the host's tree from what it is sent: each workspace's
// structure and each pane's two records. A refresh delivers the tree once,
// whole, before its rows; a watch delivers it again whenever it changes.
//
// The host here is a real Sync::HostConnection behind a channel that carries
// canonical envelopes in memory, with the daemon's listing Calls answered
// from the fixture's actual workspace and pane sources.

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Session/HostListing.h"
#include "Core/Session/ProofOfLife.h"
#include "Core/Session/ScriptedListingHost.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/SessionTree.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty;
namespace Sync = Ubiquitty::Protocol::Sync;
using UbiquittyTest::HeldWorkspace;
using UbiquittyTest::HostFactory;
using UbiquittyTest::QueueingDispatcher;
using UbiquittyTest::ScriptedHost;

namespace {

/// An observer that keeps what the errand reported.
struct RecordingObserver final : Session::SessionDirectoryObserver {
    int lists = 0;
    std::vector<Ubiquitty::Session::SessionRow> rows;
    int failures = 0;
    std::vector<std::string> answers;
    Session::DirectoryFailureReport lastFailure;
    /// Every tree reported, and how many rows answers had arrived before
    /// each.
    std::vector<Session::SessionTree> trees;
    std::vector<int> listsBeforeTree;
    /// Every time a watch said it had lost its host and was dialling again.
    std::vector<Session::DirectoryFailureReport> interruptions;

    void onWatchInterrupted(const Session::DirectoryFailureReport& report) override { interruptions.push_back(report); }
    /// How each terminate settled.
    std::vector<Session::TerminateOutcome> terminates;
    void onTerminateSettled(std::uint32_t, Session::TerminateOutcome outcome) override { terminates.push_back(outcome); }
    std::vector<Sync::ManagementReply> management;
    void onManagementReply(const Sync::ManagementReply& reply) override { management.push_back(reply); }

    void onSessionList(const std::vector<Ubiquitty::Session::SessionRow>& sessions) override
    {
        rows = sessions;
        ++lists;
    }
    void onSessionTree(const Session::SessionTree& tree) override
    {
        trees.push_back(tree);
        listsBeforeTree.push_back(lists);
    }
    void onDirectoryFailed(const Session::DirectoryFailureReport& report) override
    {
        ++failures;
        lastFailure = report;
    }
    void onCallAnswered(const std::string& response) override { answers.push_back(response); }
};

/// One directory against one scripted host.
struct Errands {
    ScriptedHost host;
    QueueingDispatcher dispatcher;
    HostFactory factory { host };
    RecordingObserver observer;
    Session::SessionDirectoryCore directory { &dispatcher, &factory, &observer };
    /// The reader's wall clock, which a case moves by hand.
    std::uint64_t readerNow = 900'000;

    Errands()
    {
        directory.setWallClock([this] { return readerNow; });
        // One clock for both ends of the connection: the host's.
        directory.setScreenClock([this] { return host.now; });
    }

    /// Lets time pass on both ends with nothing happening on the host: the
    /// host's clock and the dispatcher's move together, a tenth of a second
    /// a step, and each end is carried what the other sends.
    ///
    /// - Parameter seconds: How long.
    void passQuietly(int seconds)
    {
        for (int step = 0; step < seconds * 10; ++step) {
            host.pass(100'000);
            dispatcher.drain();
            dispatcher.advance(0.1);
        }
    }

    /// Lets the host's listing cadence pass and carries what it sends.
    void pump()
    {
        host.pump();
        dispatcher.drain();
    }
};

/// One directory against a host that lists what it holds.
struct ListingErrands : Errands {
    HeldWorkspace held;

    ListingErrands() { host.held = &held; }
};

/// A pane's facts without what its source stamps on them as it sends them:
/// their revision and the geometry it settled.
Sync::PaneFacts unstamped(Sync::PaneFacts facts)
{
    facts.revision = 0;
    facts.geometry = {};
    return facts;
}

/// The pane of a one-workspace tree at an index.
const Session::TreePane& paneAt(const Session::SessionTree& tree, std::size_t index)
{
    return tree.workspaces.at(0).windows.at(0).tabs.at(0).panes.at(index);
}

/// How many frames of one type the errand sent.
///
/// - Parameters:
///   - host: The host that read them.
///   - type: The frame type.
/// - Returns: The count.
std::size_t countOf(const ScriptedHost& host, Sync::FrameKind type)
{
    return static_cast<std::size_t>(std::ranges::count(host.received, type));
}

} // namespace

TEST(theDirectoryGreetsAndIsWelcomed)
{
    ListingErrands errands;
    CHECK(!errands.directory.grantedRole());
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
    CHECK_EQ(errands.observer.failures, 0);
    // A single direct Greet acquires no pane and makes exactly one listing Call.
    REQUIRE(errands.host.connection);
    REQUIRE(!errands.host.received.empty());
    CHECK(errands.host.received.front() == Sync::FrameKind::Greet);
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Greet), 1u);
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Call), 1u);
    REQUIRE(errands.directory.grantedRole());
    CHECK(*errands.directory.grantedRole() == Sync::Role::Owner);
}

TEST(theDirectoryIsToldTheRoleItsHostGranted)
{
    Errands errands;
    errands.host.maxRole = Sync::Role::Viewer;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.directory.grantedRole());
    CHECK(*errands.directory.grantedRole() == Sync::Role::Viewer);
}

TEST(aDirectoryConnectionIsBoundNoPane)
{
    ListingErrands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.host.stateAtExit);
    CHECK_EQ(errands.host.panesAtExit, 0u);
    CHECK_EQ(errands.host.connection->handleSlots(), 0u);
}

TEST(aDirectoryErrandSaysGoodbyeBeforeItExits)
{
    // Without the Bye the host holds the attachment for a resume: one
    // lingering attachment per refresh, each for the whole grace.
    ListingErrands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.host.stateAtExit);
    CHECK(*errands.host.stateAtExit == Sync::ConnectionState::Closed);
    REQUIRE(errands.host.byeAtExit);
    CHECK(*errands.host.byeAtExit == Sync::ByeReason::Normal);
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Bye), 1u);
}

TEST(aDirectoryCallReachesTheHostAndItsAnswerComesBack)
{
    Errands errands;
    const std::string request = R"({"jsonrpc":"2.0","id":1,"method":"host.methods","params":{}})";
    REQUIRE(errands.directory.call(request));
    CHECK(errands.directory.isBusy());
    CHECK(!errands.directory.call(R"({"jsonrpc":"2.0","id":2,"method":"host.describe","params":{}})"));
    errands.dispatcher.drain();
    CHECK(!errands.directory.isBusy());
    // One call, as the host read it, and the host's answer, whole.
    REQUIRE(errands.host.calls.size() == 1);
    CHECK_STR_EQ(errands.host.calls[0], request);
    REQUIRE(errands.observer.answers.size() == 1);
    CHECK_STR_EQ(errands.observer.answers[0], R"({"jsonrpc":"2.0","id":1,"result":{"served":true}})");
    CHECK_EQ(errands.observer.failures, 0);
    // A call errand lists nothing, subscribes to nothing, and leaves as a
    // listing does.
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Greet), 1u);
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Call), 1u);
    CHECK(!errands.host.didSubscribe);
    REQUIRE(errands.host.byeAtExit);
    CHECK(*errands.host.byeAtExit == Sync::ByeReason::Normal);
}

TEST(aRefusedCallIsAnAnswerNotAFailure)
{
    Errands errands;
    errands.host.answer = R"({"jsonrpc":"2.0","id":1,"error":{"code":-32003,"message":"host.methods is not for a viewer"}})";
    REQUIRE(errands.directory.call(R"({"jsonrpc":"2.0","id":1,"method":"host.methods","params":{}})"));
    errands.dispatcher.drain();
    REQUIRE(errands.observer.answers.size() == 1);
    CHECK_STR_EQ(errands.observer.answers[0], errands.host.answer);
    CHECK_EQ(errands.observer.failures, 0);
}

TEST(aDirectoryCallDoesNotSubscribeForAMethodNameInsideItsParameters)
{
    Errands errands;
    const std::string request = R"({"jsonrpc":"2.0","id":"echo","method":"host.methods","params":{"note":"host.sessions.subscribe"}})";
    REQUIRE(errands.directory.call(request));
    errands.dispatcher.drain();
    CHECK(!errands.host.didSubscribe);
    REQUIRE(errands.host.calls.size() == 1);
    CHECK_STR_EQ(errands.host.calls.front(), request);
    REQUIRE(errands.observer.answers.size() == 1);
    CHECK_STR_EQ(errands.observer.answers.front(), R"({"jsonrpc":"2.0","id":"echo","result":{"served":true}})");
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Call), 1u);
    CHECK_EQ(errands.observer.failures, 0);
}

TEST(anInvalidSubscriptionCallIsAnsweredWithoutDispatch)
{
    Errands errands;
    REQUIRE(errands.directory.call(R"({"jsonrpc":"2.0","id":"invalid","method":"host.sessions.subscribe","params":[]})"));
    errands.dispatcher.drain();
    CHECK(!errands.host.didSubscribe);
    CHECK(errands.host.calls.empty());
    REQUIRE(errands.observer.answers.size() == 1);
    bool isJSON = false;
    const auto response = Codec::JSON::parse(errands.observer.answers.front(), isJSON);
    REQUIRE(isJSON);
    CHECK_STR_EQ(response.stringOr("id"), "invalid");
    const auto* error = response.find("error");
    REQUIRE(error);
    const auto* code = error->find("code");
    REQUIRE(code && code->isNumber());
    CHECK_EQ(code->asNumber(), std::to_underlying(Protocol::Control::ErrorCode::InvalidParameters));
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Call), 1u);
    CHECK_EQ(errands.observer.failures, 0);
}

TEST(aCallThatIsNotTextFailsTheErrandWithoutReachingTheHost)
{
    Errands errands;
    REQUIRE(errands.directory.call(std::string("\xff\xfe not text")));
    errands.dispatcher.drain();
    CHECK(errands.host.calls.empty());
    CHECK(errands.observer.answers.empty());
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Malformed);
}

TEST(anEmptyCallStartsNoErrand)
{
    Errands errands;
    CHECK(!errands.directory.call({}));
    CHECK(!errands.directory.isBusy());
    CHECK(errands.host.received.empty());
}

TEST(anOversizedCallStartsNoErrand)
{
    Errands errands;
    CHECK(!errands.directory.call(std::string(Sync::maxFrameBodyBytes + 1, 'x')));
    CHECK(!errands.directory.isBusy());
    errands.dispatcher.drain();
    CHECK(errands.host.received.empty());
    CHECK(errands.observer.answers.empty());
    CHECK_EQ(errands.observer.failures, 0);

    std::string request = R"({"jsonrpc":"2.0","id":1,"method":"host.methods","params":{}})";
    request.resize(Sync::maxFrameBodyBytes, ' ');
    REQUIRE(errands.directory.call(request));
    errands.dispatcher.drain();
    REQUIRE(errands.host.calls.size() == 1);
    CHECK_EQ(errands.host.calls[0].size(), Sync::maxFrameBodyBytes);
    CHECK_EQ(errands.observer.answers.size(), 1u);
}

TEST(aHostThatRefusesTheListingDoesNotReturnFallbackRows)
{
    Errands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    // A refusal is neither an empty successful row snapshot nor fallback rows.
    CHECK(errands.host.didSubscribe);
    CHECK_EQ(errands.observer.lists, 0);
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Malformed);
    CHECK(errands.observer.trees.empty());
    CHECK(errands.host.calls.empty());
}

TEST(aListingFailureDoesNotInventATokenRequirement)
{
    struct Case {
        const char* response;
        Session::ListingFailure failure;
        Session::HostListing::Failure listingFailure;
        const char* detail;
    };
    const std::array cases {
        Case { R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"workspaces":[],"panes":0,"hostNowUnixMilliseconds":5000}})",
            Session::ListingFailure::Malformed, Session::HostListing::Failure::Malformed, "the host's session listing lacks required revision metadata; update ubiquitty-host" },
        Case { R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"code":-32601,"message":"no such method"}})",
            Session::ListingFailure::Malformed, Session::HostListing::Failure::Unsupported, "the host does not support session listing; update ubiquitty-host" },
        Case { R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"code":-32002,"message":"session listing needs the Observe scope","data":{"token":"must-not-be-displayed"}}})",
            Session::ListingFailure::Refused, Session::HostListing::Failure::Forbidden, "session listing needs the Observe scope" },
        Case { R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"code":"-32002","message":"not a valid RPC error"}})",
            Session::ListingFailure::Malformed, Session::HostListing::Failure::Malformed, "the host returned an invalid session listing response" },
    };
    for (const Case& each : cases) {
        Session::HostListing listing;
        CHECK(listing.noteAnswer(each.response, 1) == Session::HostListing::Answer::Refused);
        CHECK(listing.failure() == each.listingFailure);
        CHECK_STR_EQ(listing.failureDetail(), each.detail);
        Errands errands;
        errands.host.holdsRows = true;
        REQUIRE(errands.directory.refresh());
        errands.dispatcher.drain();
        REQUIRE(errands.host.heldRowsRequest);
        errands.host.heldRowsAnswer = each.response;
        errands.host.releaseRows();
        errands.dispatcher.drain();
        REQUIRE(errands.observer.failures == 1);
        CHECK(errands.observer.lastFailure.failure == each.failure);
        CHECK(errands.observer.lastFailure.deniedReason == Sync::ByeReason::Normal);
        CHECK_STR_EQ(errands.observer.lastFailure.detail, each.detail);
        CHECK(errands.observer.lastFailure.detail.find("must-not-be-displayed") == std::string::npos);
        CHECK_EQ(errands.observer.lists, 0);
        CHECK(errands.observer.trees.empty());
        CHECK(!errands.directory.isBusy());
    }
}

TEST(anExplicitAuthenticationVerdictStillRequiresTheToken)
{
    Errands errands;
    errands.host.holdsRows = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    std::vector<std::uint8_t> denied;
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::TokenMissing });
    REQUIRE(Sync::appendFrame(denied, Sync::FrameKind::Bye, 0, body));
    REQUIRE(errands.host.peer);
    errands.host.peer->onData(denied);
    errands.dispatcher.drain();
    REQUIRE(errands.observer.failures == 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Refused);
    CHECK(errands.observer.lastFailure.deniedReason == Sync::ByeReason::TokenMissing);
}

TEST(aRefreshDeliversTheHostsTreeBeforeItsRows)
{
    ListingErrands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
    CHECK_EQ(errands.observer.failures, 0);
    REQUIRE(errands.observer.trees.size() == 1);
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Call), 1u);
    CHECK(errands.host.didSubscribe);
    REQUIRE(errands.observer.rows.size() == 2);
    CHECK_EQ(errands.observer.rows[0].id, 1u);
    CHECK_STR_EQ(errands.observer.rows[0].title, "make all");
    CHECK_STR_EQ(errands.observer.rows[1].foregroundProcess, "vim");
    CHECK_EQ(errands.observer.rows[0].workspaceID, HeldWorkspace::workspaceID);
    CHECK_EQ(errands.observer.listsBeforeTree[0], 0);
    // What the client assembled is what the host itself builds of what it
    // holds: the same structure, records and roll-up.
    const Session::SessionTree& tree = errands.observer.trees[0];
    const Session::SessionTree hosts = errands.held.tree(errands.host.wallNowUnixMilliseconds);
    REQUIRE(tree.workspaces.size() == 1);
    CHECK_EQ(tree.workspaces[0].id, HeldWorkspace::workspaceID);
    CHECK_STR_EQ(tree.workspaces[0].name, "work");
    CHECK_EQ(tree.hostNowUnixMilliseconds, errands.host.wallNowUnixMilliseconds);
    REQUIRE(tree.workspaces[0].windows.at(0).tabs.at(0).panes.size() == 2);
    for (std::size_t index = 0; index < 2; ++index) {
        CHECK(unstamped(paneAt(tree, index).facts) == unstamped(Sync::boundedForWire(paneAt(hosts, index).facts)));
        CHECK(paneAt(tree, index).records.store == Sync::boundedForWire(paneAt(hosts, index).records).store);
        CHECK_STR_EQ(paneAt(tree, index).program, paneAt(hosts, index).program);
        CHECK(paneAt(tree, index).id == paneAt(hosts, index).id);
    }
    CHECK_STR_EQ(paneAt(tree, 0).title.programTitle, "make all");
    CHECK_STR_EQ(paneAt(tree, 1).program, "vim");
    CHECK(tree.status.kind == hosts.status.kind);
    CHECK(tree.status.kind == App::StatusKind::Blocked);
    CHECK(tree.workspaces[0].windows[0].tabs[0].split == hosts.workspaces[0].windows[0].tabs[0].split);
    // The errand left as every errand does.
    CHECK(!errands.directory.isBusy());
    REQUIRE(errands.host.byeAtExit);
    CHECK(*errands.host.byeAtExit == Sync::ByeReason::Normal);
    CHECK(errands.host.calls.empty());
}

TEST(aRefreshWaitsForTheRecordsOfEveryPane)
{
    ListingErrands errands;
    errands.host.withheld = { errands.held.right };
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    // The rows are in hand and the tree is not whole: nothing is reported.
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(errands.observer.trees.empty());
    CHECK(errands.directory.isBusy());
    // A one-shot listing cannot claim that a partial tree is a complete row
    // snapshot. Watches may display partial trees, but this errand fails.
    errands.dispatcher.passDeadlines();
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(errands.observer.trees.empty());
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(!errands.directory.isBusy());
}

TEST(aHostHoldingNothingIsListedAtOnce)
{
    // Nothing follows the answer of a host with no workspace: the answer
    // alone makes the tree whole, and empty.
    ListingErrands errands;
    errands.host.holdsNothing = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
    REQUIRE(errands.observer.trees.size() == 1);
    CHECK(errands.observer.trees[0].workspaces.empty());
    CHECK_EQ(errands.observer.trees[0].hostNowUnixMilliseconds, errands.host.wallNowUnixMilliseconds);
}

TEST(aRefreshWaitsForItsRowsHoweverSoonTheTreeIsWhole)
{
    ListingErrands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.lists == 1);
    // The second refresh's tree is whole before its rows are answered.
    errands.host.holdsRows = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
    CHECK_EQ(errands.observer.trees.size(), 1u);
    CHECK(errands.directory.isBusy());
    errands.host.releaseRows();
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 2);
    CHECK_EQ(errands.observer.trees.size(), 2u);
    CHECK(!errands.directory.isBusy());
}

TEST(aPaneNoScreenStandsBehindIsNotWaitedFor)
{
    // A leaf that names no pane on the screen plane has no records coming:
    // the tree is whole without them.
    ListingErrands errands;
    errands.held.at(errands.held.right).stated.paneID = {};
    errands.host.withheld = { errands.held.right };
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
    REQUIRE(errands.observer.trees.size() == 1);
    CHECK_STR_EQ(paneAt(errands.observer.trees[0], 0).title.programTitle, "make all");
    CHECK(paneAt(errands.observer.trees[0], 1).title.programTitle.empty());
}

TEST(aRefreshWaitsForEveryWorkspaceTheHostNamed)
{
    ListingErrands errands;
    errands.host.alsoNamed = 12;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(errands.directory.isBusy());
    errands.dispatcher.passDeadlines();
    CHECK_EQ(errands.observer.lists, 0);
    CHECK_EQ(errands.observer.trees.size(), 0u);
    CHECK_EQ(errands.observer.failures, 1);
}

TEST(aDeadlineOfAFinishedRefreshDoesNothingToTheNext)
{
    ListingErrands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.lists == 1);
    // The second refresh is held short of whole; the first one's deadline
    // then passes and must not deliver it.
    std::vector<QueueingDispatcher::Delayed> first;
    first.swap(errands.dispatcher.delayed);
    errands.host.withheld = { errands.held.right };
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    for (auto& work : first)
        std::move(work)();
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
    CHECK(errands.directory.isBusy());
}

TEST(aStructureThatDoesNotDecodeFailsTheListing)
{
    ListingErrands errands;
    errands.host.recordOverride = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t> { 0xff, 0xff, 0xff });
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(errands.observer.trees.empty());
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Malformed);
    CHECK(!errands.directory.isBusy());
}

TEST(aWatchReportsTheTreeAndReportsItAgainWhenAPaneChanges)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    CHECK(errands.directory.isWatching());
    CHECK(!errands.directory.refresh());
    errands.dispatcher.drain();
    // A watch asks for no rows and reports the tree once it is whole.
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Call), 1u);
    CHECK(errands.host.didSubscribe);
    CHECK_EQ(errands.observer.lists, 0);
    REQUIRE(errands.observer.trees.size() == 1);
    CHECK_STR_EQ(paneAt(errands.observer.trees[0], 1).title.programTitle, "vim notes");
    CHECK(errands.directory.isWatching());
    // Nothing moved: the cadence passing reports nothing, and neither do
    // bytes that change nothing the tree is built from.
    errands.pump();
    errands.host.sendUnsolicitedRows();
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.trees.size(), 1u);
    // A pane's facts move and the tree is reported again, by the host's
    // clock as the reader has kept it running.
    errands.held.at(errands.held.right).stated.title.programTitle = "vim plan";
    errands.held.at(errands.held.right).restate();
    errands.readerNow += 7'000;
    errands.pump();
    REQUIRE(errands.observer.trees.size() == 2);
    CHECK_STR_EQ(paneAt(errands.observer.trees[1], 1).title.programTitle, "vim plan");
    CHECK_EQ(errands.observer.trees[1].hostNowUnixMilliseconds, errands.host.wallNowUnixMilliseconds + 7'000);
    CHECK(errands.observer.trees[1].revision > errands.observer.trees[0].revision);
    // A pane the host stops listing leaves the tree short of whole. A watch
    // that has reported once reports every change from then on, so a pane
    // whose records never come cannot hold back everything else.
    errands.host.connection->unlistPane(errands.host.listedHandles.at(std::to_underlying(errands.held.left)));
    errands.pump();
    REQUIRE(errands.observer.trees.size() == 3);
    CHECK(paneAt(errands.observer.trees[2], 0).title.programTitle.empty());
    CHECK_STR_EQ(paneAt(errands.observer.trees[2], 1).title.programTitle, "vim plan");
    // A directory's connection is unbound control: it acquires no pane
    // attachment, so the host cannot bind the pane to it. The host lists the
    // pane again, and the tree reads its records there and follows them.
    CHECK_EQ(errands.host.connection->bindPane(*errands.held.at(errands.held.left).source), 0u);
    REQUIRE(errands.host.connection->listPane(*errands.held.at(errands.held.left).source, Sync::ListedDetail::Whole) != 0);
    errands.pump();
    errands.pump();
    const std::size_t afterRelist = errands.observer.trees.size();
    REQUIRE(afterRelist > 3);
    CHECK_STR_EQ(paneAt(errands.observer.trees.back(), 0).title.programTitle, "make all");
    errands.held.at(errands.held.left).stated.title.programTitle = "make install";
    errands.held.at(errands.held.left).restate();
    errands.pump();
    errands.pump();
    REQUIRE(errands.observer.trees.size() > afterRelist);
    CHECK_STR_EQ(paneAt(errands.observer.trees.back(), 0).title.programTitle, "make install");
    const std::size_t beforeRename = errands.observer.trees.size();
    // A workspace renamed is a structure of a newer revision.
    errands.held.name = "release";
    errands.host.offerStructure();
    errands.pump();
    REQUIRE(errands.observer.trees.size() == beforeRename + 1);
    CHECK_STR_EQ(errands.observer.trees.back().workspaces.at(0).name, "release");
    CHECK_EQ(errands.observer.trees.back().workspaces.at(0).revision, errands.host.structureRevision);
    CHECK_EQ(errands.host.structureRevision, 2u);
    // A workspace withdrawn is gone from the tree.
    errands.host.connection->withdrawWorkspaceStructure(HeldWorkspace::workspaceID);
    errands.pump();
    REQUIRE(errands.observer.trees.size() == beforeRename + 2);
    CHECK(errands.observer.trees.back().workspaces.empty());
    CHECK_EQ(errands.observer.failures, 0);
}

TEST(aProgramThatEndsWhileWatchedIsReportedEndedAndThenGone)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    // The right-hand pane's program exits with a status: its facts say so
    // while the host still holds the pane.
    UbiquittyTest::StatedPane& ended = errands.held.at(errands.held.right).stated;
    ended.title.end = { .didEnd = true, .hasStatus = true, .wasSignalled = false, .code = 3 };
    ended.status.isAtPrompt = true;
    ended.status.exitStatus = 3;
    errands.held.at(errands.held.right).restate();
    errands.pump();
    REQUIRE(errands.observer.trees.size() == 2);
    const Session::TreePane& reported = paneAt(errands.observer.trees[1], 1);
    CHECK(reported.facts.end.didEnd && reported.facts.end.hasStatus && reported.facts.end.code == 3);
    CHECK(reported.title.end.didEnd && reported.title.end.code == 3);
    CHECK(reported.status.kind == App::StatusKind::Failed);
    // The host reaps it: the pane is unlisted and leaves the structure.
    errands.host.connection->unlistPane(errands.host.listedHandles.at(std::to_underlying(errands.held.right)));
    REQUIRE(errands.held.layout->closePane(errands.held.layout->surfaceIDAt(0), errands.held.right));
    errands.host.offerStructure();
    errands.pump();
    REQUIRE(!errands.observer.trees.empty());
    const Session::SessionTree& after = errands.observer.trees.back();
    REQUIRE(after.workspaces.size() == 1);
    REQUIRE(after.workspaces[0].windows.at(0).tabs.at(0).panes.size() == 1);
    CHECK_STR_EQ(paneAt(after, 0).title.programTitle, "make all");
    // The watch goes on, and the pane that is left is whole.
    CHECK(errands.directory.isWatchLive());
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.observer.interruptions.empty());
}

TEST(aWatchReportsAPartOfTheTreeOnlyOnceItsWaitIsOver)
{
    ListingErrands errands;
    errands.host.withheld = { errands.held.right };
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    CHECK(errands.observer.trees.empty());
    errands.dispatcher.passDeadlines();
    REQUIRE(errands.observer.trees.size() == 1);
    CHECK(paneAt(errands.observer.trees[0], 1).title.programTitle.empty());
    // From here every change is reported, whole or not.
    errands.held.at(errands.held.left).stated.title.programTitle = "make test";
    errands.held.at(errands.held.left).restate();
    errands.pump();
    REQUIRE(errands.observer.trees.size() == 2);
    CHECK_STR_EQ(paneAt(errands.observer.trees[1], 0).title.programTitle, "make test");
    // The wait that is over is that watch's: the next errand waits for its
    // own tree.
    errands.directory.stopWatching();
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(errands.directory.isBusy());
}

TEST(aWatchWhoseHostIsGoingReportsThatAndNoTreeAfterIt)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    // A change and the host's Bye arrive in the same bytes: the host is
    // going, and what it said last is not reported as a tree still followed.
    errands.held.at(errands.held.left).stated.title.programTitle = "make test";
    errands.held.at(errands.held.left).restate();
    errands.host.pumpThenExit();
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.trees.size(), 1u);
    REQUIRE(errands.observer.interruptions.size() == 1);
    CHECK(errands.observer.interruptions[0].failure == Session::ListingFailure::Unreachable);
    // A host that went is dialled again, not given up on.
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isWatching());
    CHECK(!errands.directory.isWatchLive());
}

TEST(aWatchWhoseCarrierDropsSaysSoAndDialsAgain)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    CHECK(errands.directory.isWatchLive());
    REQUIRE(errands.host.peer != nullptr);
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    // Told once, with why; the watch is still a watch and follows nothing.
    REQUIRE(errands.observer.interruptions.size() == 1);
    CHECK(errands.observer.interruptions[0].failure == Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(errands.observer.interruptions[0].detail, "connection reset");
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isWatching());
    CHECK(errands.directory.isBusy());
    CHECK(!errands.directory.isWatchLive());
    CHECK_EQ(errands.host.hellos, 1);
    // It dials a second later, not before.
    errands.dispatcher.advance(0.9);
    CHECK_EQ(errands.host.hellos, 1);
    errands.dispatcher.advance(0.2);
    CHECK_EQ(errands.host.hellos, 2);
    // The new connection subscribes afresh and reports the host as it is.
    REQUIRE(errands.observer.trees.size() == 2);
    CHECK_STR_EQ(paneAt(errands.observer.trees[1], 0).title.programTitle, "make all");
    CHECK(errands.directory.isWatchLive());
    CHECK_EQ(errands.observer.interruptions.size(), 1u);
    // A second drop is a second thing to be told of, and starts its dials
    // from the first again.
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.interruptions.size(), 2u);
    errands.dispatcher.advance(1.1);
    CHECK_EQ(errands.host.hellos, 3);
    CHECK_EQ(errands.observer.trees.size(), 3u);
    CHECK_EQ(errands.observer.failures, 0);
}

TEST(aWatchBacksOffAHostThatStaysGone)
{
    // A factory that hands back no channel until it is told to.
    struct GoneFactory final : Session::ChannelFactory {
        ScriptedHost& host;
        int dials = 0;
        bool isGone = true; // bool-ok: names the one state it reports
        explicit GoneFactory(ScriptedHost& scripted)
            : host(scripted)
        {
        }
        void createChannel(Session::TransportMode, Done done) override
        {
            ++dials;
            if (isGone)
                done(nullptr, false);
            else
                done(std::make_unique<UbiquittyTest::HostChannel>(host), false);
        }
    };
    HeldWorkspace held;
    ScriptedHost host;
    host.held = &held;
    QueueingDispatcher dispatcher;
    GoneFactory factory { host };
    RecordingObserver observer;
    Session::SessionDirectoryCore directory { &dispatcher, &factory, &observer };
    REQUIRE(directory.watch());
    dispatcher.drain();
    // The first dial failed at once: told once, however long it stays gone.
    CHECK_EQ(factory.dials, 1);
    REQUIRE(observer.interruptions.size() == 1);
    CHECK(directory.isWatching());
    // A second apart three times, then doubling to half a minute and
    // staying there: dial n happens at these seconds after the first.
    const std::array<double, 9> dialledAt { 1, 2, 3, 5, 9, 17, 33, 63, 93 };
    double now = 0;
    for (std::size_t index = 0; index < dialledAt.size(); ++index) {
        dispatcher.advance(dialledAt[index] - now - 0.05);
        CHECK_MSG(factory.dials == static_cast<int>(index) + 1, ("a dial came early, before second " + std::to_string(dialledAt[index])).c_str());
        dispatcher.advance(0.1);
        CHECK_MSG(factory.dials == static_cast<int>(index) + 2, ("no dial at second " + std::to_string(dialledAt[index])).c_str());
        now = dialledAt[index] + 0.05;
    }
    CHECK_EQ(observer.interruptions.size(), 1u);
    CHECK_EQ(observer.failures, 0);
    CHECK(observer.trees.empty());
    // The host comes back: the next dial reaches it and the watch follows.
    factory.isGone = false;
    dispatcher.advance(30.0);
    REQUIRE(observer.trees.size() == 1);
    CHECK(directory.isWatchLive());
    // Having followed the host, its next drop starts from a second again.
    host.peer->onClose("connection reset");
    dispatcher.drain();
    CHECK_EQ(observer.interruptions.size(), 2u);
    const int dialsBeforeReturn = factory.dials;
    dispatcher.advance(1.1);
    CHECK_EQ(factory.dials, dialsBeforeReturn + 1);
    REQUIRE(observer.trees.size() == 2);
    // Stopped while it waits to dial, it dials no more.
    host.peer->onClose("connection reset");
    dispatcher.drain();
    CHECK_EQ(observer.interruptions.size(), 3u);
    const int dialsBeforeStop = factory.dials;
    directory.stopWatching();
    CHECK(!directory.isWatching());
    CHECK(!directory.isBusy());
    dispatcher.advance(120.0);
    CHECK_EQ(factory.dials, dialsBeforeStop);
    CHECK_EQ(observer.failures, 0);
    // And the directory is free for an errand.
    REQUIRE(directory.refresh());
    dispatcher.drain();
    CHECK_EQ(observer.lists, 1);
}

TEST(aWatchTheHostTurnsAwayEndsAndIsNotDialledAgain)
{
    // An ssh that cannot sign in will not sign in a second later.
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    errands.host.peer->onClose("user@host: Permission denied (publickey).");
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::SignInRequired);
    CHECK(errands.observer.interruptions.empty());
    CHECK(!errands.directory.isWatching());
    errands.dispatcher.advance(120.0);
    CHECK_EQ(errands.host.hellos, 1);
    // A host that stops listing itself on the way back ends the watch too.
    ListingErrands refused;
    REQUIRE(refused.directory.watch());
    refused.dispatcher.drain();
    refused.host.held = nullptr;
    refused.host.peer->onClose("connection reset");
    refused.dispatcher.drain();
    refused.dispatcher.advance(1.1);
    CHECK_EQ(refused.host.hellos, 2);
    CHECK_EQ(refused.observer.failures, 1);
    CHECK(refused.observer.lastFailure.failure == Session::ListingFailure::Malformed);
    CHECK(!refused.directory.isWatching());
    refused.dispatcher.advance(120.0);
    CHECK_EQ(refused.host.hellos, 2);
}

// A watch pinned to a daemon that stopped is told so by the bootstrap. Asking
// that daemon again gets the same answer, so the watch ends with the typed
// failure for its frontend to unpin, and never dials again on its own.
TEST(aWatchPinnedToAStoppedDaemonEndsAndIsNotDialledAgain)
{
    ListingErrands errands;
    errands.host.startupText = "\nUBIQUITTY-INSTANCE-GONE host-c756f263bc841e61.sock\n";
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::InstanceGone);
    CHECK(errands.observer.interruptions.empty());
    CHECK(!errands.directory.isWatching());
    CHECK_EQ(errands.host.connections, 1);
    errands.dispatcher.advance(120.0);
    CHECK_EQ(errands.host.connections, 1);
}

TEST(aRefreshWhoseCarrierDropsFailsAndIsNotDialledAgain)
{
    ListingErrands errands;
    errands.host.holdsRows = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.host.peer != nullptr);
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Unreachable);
    CHECK(errands.observer.interruptions.empty());
    CHECK(!errands.directory.isBusy());
    errands.dispatcher.advance(120.0);
    CHECK_EQ(errands.host.hellos, 1);
}

TEST(aWatchHoldsItsConnectionThroughAHostsSilence)
{
    // A host with nothing to say probes a connection and drops one that
    // does not answer within its silence bound. A watch answers on a timer
    // of its own, as an attached session does, so a quiet host is not a
    // lost one.
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    const std::uint64_t bound = UbiquittyTest::syncHostPolicy(0).pacing.lostSilenceMicroseconds;
    REQUIRE(bound == 30'000'000);
    // Five times the bound, and nothing on the host changes.
    errands.passQuietly(150);
    CHECK_EQ(errands.host.timesLost, 0);
    REQUIRE(errands.host.connection.has_value());
    CHECK(errands.host.connection->state() == Sync::ConnectionState::Attached);
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.observer.interruptions.empty());
    CHECK(errands.directory.isWatching());
    CHECK(errands.directory.isWatchLive());
    CHECK_EQ(errands.host.hellos, 1);
    CHECK_EQ(errands.observer.trees.size(), 1u);
    // And it still follows the host.
    errands.held.at(errands.held.left).stated.title.programTitle = "make test";
    errands.held.at(errands.held.left).restate();
    errands.passQuietly(3);
    REQUIRE(errands.observer.trees.size() == 2);
    CHECK_STR_EQ(paneAt(errands.observer.trees[1], 0).title.programTitle, "make test");
    // What the host sent then is acknowledged on the timer too, armed again
    // after it last fired: three more bounds of silence and the connection
    // stands.
    errands.passQuietly(90);
    CHECK_EQ(errands.host.timesLost, 0);
    CHECK(errands.host.connection->state() == Sync::ConnectionState::Attached);
    CHECK(errands.observer.interruptions.empty());
    CHECK(errands.directory.isWatchLive());
}

TEST(aWatchFindsAPathThatDiedWithoutSayingSo)
{
    // The carrier stays open and carries nothing: no close, no error, no
    // bytes. A watch asks a connection gone quiet whether it is there, and
    // one that does not answer is dropped and dialled again.
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    // A quiet host that answers is not a dead one: three probe intervals
    // and the watch has dropped nothing.
    errands.passQuietly(static_cast<int>(3 * Session::watchProbeIntervalSeconds));
    REQUIRE(errands.observer.interruptions.empty());
    REQUIRE(errands.directory.isWatchLive());
    CHECK_EQ(errands.host.hellos, 1);
    // The path dies. Nothing is heard and nothing says so.
    errands.host.isPathDead = true;
    // Within one interval of quiet nothing has been asked yet, or the ask
    // has not had its window.
    errands.passQuietly(5);
    CHECK(errands.observer.interruptions.empty());
    CHECK(errands.directory.isWatchLive());
    // Two intervals and the window later, at the most, the watch has asked,
    // heard nothing, and said so.
    errands.passQuietly(static_cast<int>(2 * Session::watchProbeIntervalSeconds + Session::proofOfLifeDeadlineSeconds) - 5 + 1);
    REQUIRE(errands.observer.interruptions.size() == 1);
    CHECK(errands.observer.interruptions[0].failure == Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(errands.observer.interruptions[0].detail, "the host stopped answering");
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isWatching());
    CHECK(!errands.directory.isWatchLive());
    // It dials again on the schedule every drop is dialled on. A dial into
    // the dead path opens and is never welcomed: it is given its time and
    // counted as a dial that failed, and the watch is still told only once.
    errands.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) + 3);
    CHECK_EQ(errands.host.hellos, 1);
    CHECK_EQ(errands.observer.interruptions.size(), 1u);
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isWatching());
    // The path comes back: the dial then out has its time, the next one
    // reaches the host, and the watch follows again.
    errands.host.isPathDead = false;
    errands.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) + 6);
    CHECK_EQ(errands.host.hellos, 2);
    REQUIRE(errands.observer.trees.size() == 2);
    CHECK(errands.directory.isWatchLive());
    CHECK_EQ(errands.observer.interruptions.size(), 1u);
    // A welcomed connection is not judged by the handshake's deadline, and
    // is checked as any established connection is.
    errands.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) + 3);
    CHECK(errands.directory.isWatchLive());
    CHECK_EQ(errands.observer.interruptions.size(), 1u);
    errands.host.isPathDead = true;
    errands.passQuietly(static_cast<int>(2 * Session::watchProbeIntervalSeconds + Session::proofOfLifeDeadlineSeconds) + 1);
    CHECK_EQ(errands.observer.interruptions.size(), 2u);
}

TEST(aWatchDialledIntoADeadPathIsADialThatFailed)
{
    // From the first dial: the carrier opens and nothing answers.
    ListingErrands errands;
    errands.host.isPathDead = true;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    errands.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) - 1);
    CHECK(errands.observer.interruptions.empty());
    errands.passQuietly(2);
    REQUIRE(errands.observer.interruptions.size() == 1);
    CHECK(errands.observer.interruptions[0].failure == Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(errands.observer.interruptions[0].detail, "the host did not answer");
    CHECK(errands.directory.isWatching());
    CHECK_EQ(errands.observer.failures, 0);
    // Stopped while a dial is out: nothing more is said or dialled.
    errands.directory.stopWatching();
    errands.passQuietly(40);
    CHECK_EQ(errands.observer.interruptions.size(), 1u);
    CHECK(!errands.directory.isBusy());
}

TEST(aWatchProbesOnlyAConnectionGoneQuiet)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    const auto echoes = [&] { return errands.host.connection->counters().echoesSent; };
    // A host that says something every second is heard every second: half a
    // minute of it and the watch has asked nothing.
    const std::uint64_t whileBusy = echoes();
    for (int second = 0; second < 30; ++second) {
        errands.held.at(errands.held.left).stated.title.programTitle = "make " + std::to_string(second);
        errands.held.at(errands.held.left).restate();
        errands.passQuietly(1);
    }
    CHECK(errands.observer.trees.size() > 20);
    CHECK_EQ(echoes(), whileBusy);
    // Quiet for a minute, it asks once an interval or less, and no more.
    const std::uint64_t whileQuiet = echoes();
    errands.passQuietly(60);
    const std::uint64_t asked = echoes() - whileQuiet;
    CHECK_MSG(asked >= 2 && asked <= 6, ("probes in a quiet minute: " + std::to_string(asked)).c_str());
    // A connection dropped and dialled again is asked at the same rate: the
    // timers of the one before it ask nothing of the new one.
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    errands.passQuietly(3);
    REQUIRE(errands.directory.isWatchLive());
    const std::uint64_t afterRedial = echoes();
    errands.passQuietly(60);
    const std::uint64_t askedAgain = echoes() - afterRedial;
    CHECK_MSG(askedAgain >= 2 && askedAgain <= 6, ("probes in a quiet minute after a redial: " + std::to_string(askedAgain)).c_str());
    CHECK_EQ(errands.observer.interruptions.size(), 1u);
}

TEST(aRefreshIntoADeadPathFailsInItsTime)
{
    // The carrier opens and carries nothing: no welcome, no rows, no close.
    // A refresh is given the handshake's time and then fails in Core's own
    // words: it does not wait on a carrier that will never say.
    ListingErrands errands;
    errands.host.isPathDead = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    errands.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) - 1);
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isBusy());
    errands.passQuietly(2);
    REQUIRE(errands.observer.failures == 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(errands.observer.lastFailure.detail, "the host did not answer");
    CHECK_STR_EQ(Session::listingFailureNotice(errands.observer.lastFailure, Session::NoticePlacement::Embedded), "the host could not be reached: the host did not answer");
    CHECK_STR_EQ(Session::listingFailureNotice(errands.observer.lastFailure, Session::NoticePlacement::Standalone), "The host could not be reached: the host did not answer.");
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(errands.observer.interruptions.empty());
    CHECK(!errands.directory.isBusy());
    // A refresh is not dialled again.
    errands.passQuietly(60);
    CHECK_EQ(errands.observer.failures, 1);
    CHECK_EQ(errands.host.hellos, 0);
}

TEST(aRefreshWelcomedAndThenLeftUnansweredFailsInItsTime)
{
    // The path dies after the welcome: the rows never come.
    ListingErrands errands;
    errands.host.holdsRows = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.directory.grantedRole().has_value());
    errands.host.isPathDead = true;
    // Past the handshake's time it is still asking: it was welcomed.
    errands.passQuietly(static_cast<int>(Session::directoryAnswerSeconds) - 1);
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isBusy());
    errands.passQuietly(2);
    REQUIRE(errands.observer.failures == 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(errands.observer.lastFailure.detail, "the host did not answer the listing");
    CHECK_EQ(errands.observer.lists, 0);
    CHECK(!errands.directory.isBusy());
}

TEST(aRefreshAnsweredInTimeIsNotFailedByItsDeadlines)
{
    ListingErrands errands;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.lists == 1);
    // Its deadlines pass after it is over, and the next errand's own time
    // starts from its own dial.
    errands.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) - 5);
    errands.host.holdsRows = true;
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    errands.passQuietly(8);
    CHECK_EQ(errands.observer.failures, 0);
    CHECK(errands.directory.isBusy());
    errands.host.releaseRows();
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 2);
    errands.passQuietly(40);
    CHECK_EQ(errands.observer.failures, 0);
}

TEST(aCallAndATerminateIntoADeadPathSettleInTheirTime)
{
    ListingErrands call;
    call.host.isPathDead = true;
    REQUIRE(call.directory.call(R"({"jsonrpc":"2.0","id":1,"method":"host.methods","params":{}})"));
    call.dispatcher.drain();
    call.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) + 1);
    CHECK_EQ(call.observer.failures, 1);
    CHECK(call.observer.lastFailure.failure == Session::ListingFailure::Unreachable);
    CHECK(call.observer.answers.empty());
    CHECK(!call.directory.isBusy());

    ListingErrands terminate;
    terminate.host.isPathDead = true;
    REQUIRE(terminate.directory.terminate(4));
    terminate.dispatcher.drain();
    terminate.passQuietly(static_cast<int>(Session::directoryHandshakeSeconds) - 1);
    CHECK(terminate.directory.isBusy());
    terminate.passQuietly(2);
    CHECK(!terminate.directory.isBusy());
    REQUIRE(terminate.observer.terminates.size() == 1);
    CHECK(terminate.observer.terminates[0] == Session::TerminateOutcome::Failed);
    CHECK_EQ(terminate.observer.failures, 0);
}

TEST(terminationWaitsForItsExactResultRatherThanSilenceOrExit)
{
    for (const auto& [word, outcome] : std::array {
             std::pair { "terminated", Session::TerminateOutcome::Terminated },
             std::pair { "forgotten", Session::TerminateOutcome::Terminated },
             std::pair { "denied", Session::TerminateOutcome::Denied },
             std::pair { "timed-out", Session::TerminateOutcome::TimedOut },
             std::pair { "refused", Session::TerminateOutcome::Refused } }) {
        Errands fixture;
        fixture.host.heldSessionIDs = { 7 };
        fixture.host.holdsCalls = true;
        fixture.host.answer = R"({"jsonrpc":"2.0","id":$ID,"result":{"session":7,"outcome":")" + std::string(word) + R"("}})";
        REQUIRE(fixture.directory.terminate(7));
        fixture.dispatcher.drain();
        REQUIRE(fixture.host.calls.size() == 1);
        REQUIRE(fixture.host.connection);
        CHECK(fixture.host.connection->boundPanes().empty());
        CHECK(!fixture.host.connection->facts().viewportRows && !fixture.host.connection->facts().viewportColumns);
        CHECK(fixture.host.connection->takeViewportReports().empty());
        CHECK(fixture.host.calls.front().find(R"("method":"session.terminate")") != std::string::npos);
        fixture.passQuietly(2);
        CHECK(fixture.directory.isBusy() && fixture.observer.terminates.empty());
        fixture.host.releaseCalls();
        fixture.dispatcher.drain();
        REQUIRE(fixture.observer.terminates.size() == 1);
        CHECK(fixture.observer.terminates.front() == outcome);
        fixture.passQuietly(40);
        CHECK(fixture.observer.terminates.size() == 1);
    }
}

TEST(terminationRefusesWrongTargetsAndTimesOutUncorrelatedReplies)
{
    for (const auto* answer : {
             R"({"jsonrpc":"2.0","id":$ID,"result":{"session":8,"outcome":"terminated"}})",
             R"({"jsonrpc":"2.0","id":"other","result":{"session":7,"outcome":"terminated"}})" }) {
        Errands fixture;
        fixture.host.heldSessionIDs = { 7 };
        fixture.host.answer = answer;
        REQUIRE(fixture.directory.terminate(7));
        fixture.dispatcher.drain();
        REQUIRE(fixture.host.calls.size() == 1);
        fixture.passQuietly(40);
        REQUIRE(fixture.observer.terminates.size() == 1);
        CHECK(fixture.observer.terminates.front() != Session::TerminateOutcome::Terminated);
    }
}

TEST(managementTerminationPinsTheDaemonAndUsesItsSeparateTypedAuthority)
{
    for (const auto pin : { 99u, 98u }) {
        Errands fixture;
        fixture.host.holdsCalls = true;
        fixture.host.answer = R"({"jsonrpc":"2.0","id":$ID,"result":{"session":7,"outcome":"terminated"}})";
        Sync::ManagementRequest request;
        request.daemonInstanceID = pin;
        request.operation = Sync::ManagementOperation::TerminateSession;
        request.sessionID = 7;
        REQUIRE(fixture.directory.manage(request));
        fixture.dispatcher.drain();
        CHECK_EQ(countOf(fixture.host, Sync::FrameKind::Greet), 1u);
        CHECK_EQ(countOf(fixture.host, Sync::FrameKind::Call), pin == 99 ? 1u : 0u);
        CHECK(!fixture.host.didSubscribe);
        if (pin != 99) {
            CHECK(fixture.host.calls.empty());
            CHECK_EQ(fixture.observer.failures, 1);
            CHECK(fixture.observer.management.empty());
            continue;
        }
        REQUIRE(fixture.host.calls.size() == 1);
        CHECK(fixture.host.calls.front().find(R"("method":"host.session.terminate")") != std::string::npos);
        CHECK(fixture.observer.management.empty());
        fixture.host.releaseCalls();
        fixture.dispatcher.drain();
        REQUIRE(fixture.observer.management.size() == 1);
        CHECK(fixture.observer.management.front().status == Sync::ManagementStatus::Success);
        CHECK_EQ(fixture.observer.management.front().daemonInstanceID, 99u);
        fixture.passQuietly(40);
        CHECK_EQ(fixture.observer.management.size(), 1u);
    }
}

TEST(anExitWithoutTheTerminationResultCannotConfirmSuccess)
{
    Errands fixture;
    fixture.host.heldSessionIDs = { 7 };
    fixture.host.holdsCalls = true;
    REQUIRE(fixture.directory.terminate(7));
    fixture.dispatcher.drain();
    REQUIRE(fixture.host.calls.size() == 1);
    std::vector<std::uint8_t> exit;
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye {});
    REQUIRE(Sync::appendFrame(exit, Sync::FrameKind::Bye, 0, body));
    fixture.host.peer->onData(exit);
    fixture.dispatcher.drain();
    fixture.passQuietly(40);
    REQUIRE(fixture.observer.terminates.size() == 1);
    CHECK(fixture.observer.terminates.front() != Session::TerminateOutcome::Terminated);
}

TEST(aTerminationCannotSelectAMissingHeldSession)
{
    Errands fixture;
    fixture.host.heldSessionIDs = { 7 };
    REQUIRE(fixture.directory.terminate(8));
    fixture.dispatcher.drain();
    REQUIRE(fixture.observer.terminates.size() == 1);
    CHECK(fixture.observer.terminates.front() == Session::TerminateOutcome::Refused);
    CHECK(fixture.host.calls.empty());
    CHECK_EQ(countOf(fixture.host, Sync::FrameKind::Greet), 1u);
    CHECK_EQ(countOf(fixture.host, Sync::FrameKind::Call), 0u);
    REQUIRE(fixture.host.connection);
    CHECK(fixture.host.connection->byeReason() == Sync::ByeReason::SessionGone);
    CHECK(fixture.host.connection->boundPanes().empty());
    CHECK(fixture.host.heldSessionIDs == std::vector<std::uint32_t> { 7 });
    CHECK(!fixture.directory.isBusy());
}

TEST(aRefreshIsNotProbed)
{
    // A refresh is over in a round trip or fails by its own deadlines: it
    // posts no quiet timer.
    ListingErrands errands;
    errands.host.withheld = { errands.held.right };
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    // Its own deadlines are out; nothing is due an interval of quiet on.
    CHECK(std::ranges::none_of(errands.dispatcher.delayed, [&](const QueueingDispatcher::Delayed& timer) {
        const double in = timer.dueSeconds - errands.dispatcher.nowSeconds;
        return in > Session::watchProbeIntervalSeconds - 1.0 && in < Session::watchProbeIntervalSeconds + 1.0;
    }));
    errands.host.isPathDead = true;
    errands.dispatcher.advance(1.0);
    CHECK(errands.observer.interruptions.empty());
    CHECK_EQ(errands.observer.failures, 0);
    // Its own wait for the complete tree ends in a failure.
    errands.dispatcher.advance(2.0);
    CHECK_EQ(errands.observer.lists, 0);
    errands.dispatcher.advance(60.0);
    CHECK(errands.observer.interruptions.empty());
    CHECK_EQ(errands.observer.failures, 1);
}

TEST(aWatchCutOffMidFrameReadsItsNextConnectionFromTheStart)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    // The carrier goes with half a frame delivered: a header promising a
    // body that never comes.
    std::vector<std::uint8_t> frame;
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Result { .text = R"({"jsonrpc":"2.0","id":"unasked","result":{}})" });
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Result, 0, body));
    REQUIRE(frame.size() > 4);
    errands.host.peer->onData(std::span<const std::uint8_t>(frame).first(4));
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    REQUIRE(errands.observer.interruptions.size() == 1);
    // The next connection's bytes are read as a stream of their own.
    errands.dispatcher.advance(1.1);
    CHECK_EQ(errands.host.hellos, 2);
    CHECK_EQ(errands.observer.failures, 0);
    REQUIRE(errands.observer.trees.size() == 2);
    CHECK(errands.directory.isWatchLive());
}

TEST(aWatchBegunAgainStartsFromNothingTheLastOneLeft)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.directory.isWatchLive());
    // Stopped while it follows, and begun again: the new watch follows
    // nothing until it has reported.
    errands.directory.stopWatching();
    REQUIRE(errands.directory.watch());
    CHECK(errands.directory.isWatching());
    CHECK(!errands.directory.isWatchLive());
    errands.dispatcher.drain();
    REQUIRE(errands.directory.isWatchLive());
    errands.host.hellos = 1;
    // Dropped, and stopped while it waits to dial.
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    REQUIRE(errands.observer.interruptions.size() == 1);
    errands.directory.stopWatching();
    // A new watch at once: it is not live before it has reported, and the
    // dial the last one armed does not open a second connection beside it.
    REQUIRE(errands.directory.watch());
    CHECK(!errands.directory.isWatchLive());
    errands.dispatcher.drain();
    CHECK_EQ(errands.host.hellos, 2);
    CHECK(errands.directory.isWatchLive());
    errands.dispatcher.advance(1.5);
    CHECK_EQ(errands.host.hellos, 2);
    // Its own first drop is told, and dialled a second later.
    errands.host.peer->onClose("connection reset");
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.interruptions.size(), 2u);
    errands.dispatcher.advance(1.1);
    CHECK_EQ(errands.host.hellos, 3);
}

TEST(aStoppedWatchSaysGoodbyeAndReportsNothingMore)
{
    ListingErrands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    REQUIRE(errands.observer.trees.size() == 1);
    errands.directory.stopWatching();
    CHECK(!errands.directory.isWatching());
    CHECK(!errands.directory.isBusy());
    REQUIRE(errands.host.byeAtExit);
    CHECK(*errands.host.byeAtExit == Sync::ByeReason::Normal);
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Bye), 1u);
    CHECK_EQ(errands.observer.failures, 0);
    CHECK_EQ(errands.observer.lists, 0);
    // Stopping twice, or with no watch, is nothing.
    errands.directory.stopWatching();
    CHECK_EQ(countOf(errands.host, Sync::FrameKind::Bye), 1u);
    // The directory is free for its next errand.
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    CHECK_EQ(errands.observer.lists, 1);
}

TEST(aRefreshIsNotStoppedAsAWatch)
{
    ListingErrands errands;
    errands.host.withheld = { errands.held.right };
    REQUIRE(errands.directory.refresh());
    errands.dispatcher.drain();
    errands.directory.stopWatching();
    CHECK(errands.directory.isBusy());
    CHECK(!errands.directory.isWatching());
}

TEST(aWatchOfAHostThatDoesNotListItselfFails)
{
    Errands errands;
    REQUIRE(errands.directory.watch());
    errands.dispatcher.drain();
    CHECK(errands.observer.trees.empty());
    CHECK_EQ(errands.observer.failures, 1);
    CHECK(errands.observer.lastFailure.failure == Session::ListingFailure::Malformed);
    CHECK(!errands.directory.isBusy());
}

TEST(aPaneTheClientHasBoundIsDescribedFromItsBinding)
{
    // A client attached to a workspace and listing its host holds one pane's
    // records on the binding and the other's on the listing. The tree reads
    // both.
    HeldWorkspace held;
    Sync::HostConnection host(UbiquittyTest::syncHostPolicy(0),
        Sync::HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .maxRole = Sync::Role::Owner, .principal = "test", .mayDrive = true, .verifiedKind = Sync::ClientKind::Human, .selection = { .target = Sync::AdmissionTarget::Session, .sessionID = held.at(held.left).stated.sessionID, .workspaceID = HeldWorkspace::workspaceID, .daemonInstanceID = 99, .socketName = "host-test.sock" } });
    Sync::ClientConnection client(UbiquittyTest::syncClientPolicy,
        Sync::ClientIdentity { .clientInstance = { 7, 8 }, .kind = Sync::ClientKind::Human, .facts = UbiquittyTest::syncDefaultFacts(), .resume = std::nullopt });
    std::uint64_t now = 1;
    const auto step = [&] {
        now += 20'000;
        host.receiveStream(client.takeStreamOutput(), now);
        host.service(now);
        std::vector<std::uint8_t> down;
        while (host.popStreamFrame(down, now)) {
        }
        client.receiveStream(down, now);
        client.tick(now);
    };
    client.start();
    for (int steps = 0; steps < 10 && !client.welcome(); ++steps)
        step();
    REQUIRE(client.welcome().has_value());
    REQUIRE(host.bindPane(*held.at(held.left).source) != 0);
    REQUIRE(host.listPane(*held.at(held.right).source, Sync::ListedDetail::Whole) != 0);
    host.offerWorkspaceStructure(HeldWorkspace::workspaceID, 1, held.record());
    for (int steps = 0; steps < 10; ++steps)
        step();
    const std::uint32_t bound = client.handleForPane(held.at(held.left).stated.paneID);
    REQUIRE(bound != 0);
    REQUIRE(client.paneFacts(bound) != nullptr);
    CHECK(client.listedPaneFacts(held.at(held.left).stated.paneID) == nullptr);
    // A pane held dormant is sent its status set as records, as a listed
    // pane is; one held live reads it off its screen.
    client.setView(bound, Sync::ViewState::Dormant);
    for (int steps = 0; steps < 10; ++steps)
        step();

    Session::HostListing listing;
    // Before the host answers there is nothing to assemble.
    CHECK(listing.refresh(client, 1, 0) == Session::HostListing::Change::None);
    REQUIRE(listing.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"workspaces":[9],"structureRevisions":["1"],"paneRevisions":[],"panes":1,"hostNowUnixMilliseconds":5000}})", 100) == Session::HostListing::Answer::Listing);
    CHECK(listing.refresh(client, 1, 100) == Session::HostListing::Change::Assembled);
    CHECK(listing.isWhole());
    CHECK_STR_EQ(paneAt(listing.tree(), 0).title.programTitle, "make all");
    CHECK_STR_EQ(paneAt(listing.tree(), 1).title.programTitle, "vim notes");
    // The bound pane's status set is the one its binding holds.
    const std::optional<Sync::StatusRecords> status = client.paneStatus(bound);
    REQUIRE(status.has_value());
    CHECK(!status->store.records.empty());
    CHECK(paneAt(listing.tree(), 0).records.store == status->store);
    CHECK(paneAt(listing.tree(), 0).status.kind == App::StatusKind::Blocked);
    // The same revision again assembles nothing; a newer one does.
    CHECK(listing.refresh(client, 1, 100) == Session::HostListing::Change::None);
    const std::uint64_t first = listing.tree().revision;
    CHECK(listing.refresh(client, 2, 100) == Session::HostListing::Change::Assembled);
    CHECK(listing.tree().revision > first);
    // The answer may arrive before the revised records. Cached records from
    // the same connection cannot satisfy a new subscription's revision floor.
    held.at(held.right).stated.title.programTitle = "new listing title";
    held.at(held.right).restate();
    host.offerWorkspaceStructure(HeldWorkspace::workspaceID, 2, held.record());
    const auto factsRevision = held.at(held.right).source->facts().revision;
    Session::HostListing fresh("directory.2");
    const std::string freshAnswer = R"({"jsonrpc":"2.0","id":"directory.2","result":{"workspaces":[9],"structureRevisions":["2"],"paneRevisions":[{"session":2,"revision":)"
        + std::to_string(factsRevision) + R"(}],"panes":1,"hostNowUnixMilliseconds":5000}})";
    CHECK(fresh.noteAnswer(R"({"jsonrpc":"2.0","id":"directory.1","result":{}})", 100) == Session::HostListing::Answer::NotThisCall);
    REQUIRE(fresh.noteAnswer(freshAnswer, 100) == Session::HostListing::Answer::Listing);
    CHECK(fresh.refresh(client, 3, 100) == Session::HostListing::Change::Assembled);
    CHECK(!fresh.isWhole());
    for (int steps = 0; steps < 60; ++steps)
        step();
    CHECK(fresh.refresh(client, 4, 100) == Session::HostListing::Change::Assembled);
    CHECK(fresh.isWhole());
    CHECK_STR_EQ(paneAt(fresh.tree(), 1).title.programTitle, "new listing title");
    // A structure numbered zero names no workspace: zero is never minted.
    host.offerWorkspaceStructure(0, 1, held.record());
    for (int steps = 0; steps < 10; ++steps)
        step();
    REQUIRE(client.workspaceStructures().contains(0));
    CHECK(listing.refresh(client, 3, 100) == Session::HostListing::Change::Malformed);
}

/// One function cases a notice for where it is put, and every failure's
/// words read as a sentence standing alone and as a clause after a colon.
TEST(aFailuresWordsAreCasedForWhereTheyArePut)
{
    using Session::ListingFailure;
    using Session::NoticePlacement;
    CHECK_STR_EQ(Session::placedNotice("the host did not answer", NoticePlacement::Embedded), "the host did not answer");
    CHECK_STR_EQ(Session::placedNotice("the host did not answer", NoticePlacement::Standalone), "The host did not answer.");
    CHECK_STR_EQ(Session::placedNotice("Already a sentence.", NoticePlacement::Standalone), "Already a sentence.");
    CHECK_STR_EQ(Session::placedNotice("is it there?", NoticePlacement::Standalone), "Is it there?");
    CHECK_STR_EQ(Session::placedNotice("64 sessions run", NoticePlacement::Standalone), "64 sessions run.");
    CHECK(Session::placedNotice("", NoticePlacement::Standalone).empty() && Session::placedNotice("", NoticePlacement::Embedded).empty());

    const auto report = [](ListingFailure failure, std::string detail, std::string platform = {}) {
        Session::DirectoryFailureReport made;
        made.failure = failure;
        made.detail = std::move(detail);
        made.remotePlatform = std::move(platform);
        return made;
    };
    struct Case {
        Session::DirectoryFailureReport report;
        const char* embedded;
        const char* standalone;
    };
    const std::array cases {
        Case { report(ListingFailure::Unreachable, ""), "the host could not be reached", "The host could not be reached." },
        Case { report(ListingFailure::Unreachable, "connection refused"), "the host could not be reached: connection refused", "The host could not be reached: connection refused." },
        Case { report(ListingFailure::SignInRequired, ""), "signing in over SSH needs a password or an unlocked key", "Signing in over SSH needs a password or an unlocked key." },
        Case { report(ListingFailure::AuthenticationAttemptsExhausted, ""), "the server closed the connection after too many authentication attempts",
            "The server closed the connection after too many authentication attempts." },
        Case { report(ListingFailure::EndedBeforeAnswer, ""), "the host closed the SSH connection before Ubiquitty’s host answered",
            "The host closed the SSH connection before Ubiquitty’s host answered." },
        Case { report(ListingFailure::NotInstalled, ""), "this host has no ubiquitty-host installed", "This host has no ubiquitty-host installed." },
        Case { report(ListingFailure::NotInstalled, "", "Linux x86_64"), "this Linux x86_64 host has no ubiquitty-host installed", "This Linux x86_64 host has no ubiquitty-host installed." },
        Case { report(ListingFailure::Malformed, ""), "the host sent a reply Ubiquitty doesn’t understand", "The host sent a reply Ubiquitty doesn’t understand." },
        Case { report(ListingFailure::InstanceGone, ""), "the Ubiquitty host this list was asked of is no longer running", "The Ubiquitty host this list was asked of is no longer running." },
        Case { report(ListingFailure::Refused, "not for this connection"), "not for this connection", "Not for this connection." },
        Case { report(ListingFailure::Refused, ""), "the host refused the request", "The host refused the request." },
    };
    for (const Case& each : cases) {
        CHECK_STR_EQ(Session::listingFailureNotice(each.report, NoticePlacement::Embedded), each.embedded);
        CHECK_STR_EQ(Session::listingFailureNotice(each.report, NoticePlacement::Standalone), each.standalone);
    }
    CHECK(Session::listingFailureNotice(report(ListingFailure::None, "x"), NoticePlacement::Standalone).empty());
}

TEST(anAnswerToAnotherCallIsNotTheListing)
{
    Session::HostListing listing;
    CHECK(listing.noteAnswer(R"({"jsonrpc":"2.0","id":1,"result":{"hostNowUnixMilliseconds":5}})", 1) == Session::HostListing::Answer::NotThisCall);
    CHECK(!listing.isListing());
    CHECK(!listing.isRefused());
    // The listing's own answer without the host's clock is a refusal, as an
    // error is; one that states it is the listing.
    CHECK(listing.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"workspaces":[]}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(listing.isRefused());
    CHECK(listing.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"message":"hostNowUnixMilliseconds"}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(listing.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"code":1,"data":{"hostNowUnixMilliseconds":7}}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(listing.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":"soon"}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(listing.isRefused());
    CHECK(listing.noteAnswer(R"({"jsonrpc": "2.0", "id": "ubiquitty.host-listing", "result": {"workspaces": [3, 4], "structureRevisions":["1","18446744073709551615"],"paneRevisions":[],"hostNowUnixMilliseconds": 5000}})", 100) == Session::HostListing::Answer::Listing);
    CHECK(listing.isListing());
    // A member's name inside a value is not the member.
    CHECK(listing.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"note":"hostNowUnixMilliseconds","workspaces":[],"structureRevisions":[],"paneRevisions":[],"hostNowUnixMilliseconds":5000}})", 100) == Session::HostListing::Answer::Listing);
    CHECK_EQ(listing.hostNowUnixMilliseconds(100), 5000u);
    CHECK_EQ(listing.hostNowUnixMilliseconds(350), 5250u);
    // A reader's clock that went backwards does not move the host's back.
    CHECK_EQ(listing.hostNowUnixMilliseconds(40), 5000u);
    // The answer is read as the JSON it is: a member is one at its own
    // depth, never a name found somewhere inside.
    Session::HostListing strict;
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"echo":{"hostNowUnixMilliseconds":7,"workspaces":[1]}}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"other","result":{"id":"ubiquitty.host-listing","hostNowUnixMilliseconds":7}})", 1) == Session::HostListing::Answer::NotThisCall);
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"code":1},"hostNowUnixMilliseconds":7})", 1) == Session::HostListing::Answer::Refused);
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing-2","result":{"hostNowUnixMilliseconds":7}})", 1) == Session::HostListing::Answer::NotThisCall);
    // A response that carries an error is a refusal whatever rides beside it.
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","error":{"code":1},"result":{"hostNowUnixMilliseconds":7}})", 1) == Session::HostListing::Answer::Refused);
    // Text that is not JSON answers nothing, whatever it contains.
    CHECK(strict.noteAnswer(R"("id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":7})", 1) == Session::HostListing::Answer::NotThisCall);
    CHECK(strict.noteAnswer(R"(["ubiquitty.host-listing"])", 1) == Session::HostListing::Answer::NotThisCall);
    CHECK(strict.noteAnswer({}, 1) == Session::HostListing::Answer::NotThisCall);
    // A clock that is no instant, or a workspace that is no identity, is an
    // answer this method does not write.
    for (const char* clock : { "-5", "5.5", "1e40", "true", "null", "[5]" }) {
        const std::string answer = std::string(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"workspaces":[],"hostNowUnixMilliseconds":)") + clock + "}}";
        CHECK_MSG(strict.noteAnswer(answer, 1) == Session::HostListing::Answer::Refused, clock);
    }
    for (const char* workspaces : { "[-1]", "[1.5]", "[\"1\"]", "[4294967296]", "{}", "7" }) {
        const std::string answer = std::string(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":5,"workspaces":)") + workspaces + "}}";
        CHECK_MSG(strict.noteAnswer(answer, 1) == Session::HostListing::Answer::Refused, workspaces);
    }
    // Missing revision promises must not turn a cached tree into a fresh list.
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":5}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":5,"workspaces":[1,4294967295]}})", 1) == Session::HostListing::Answer::Refused);
    // Zero is never minted, so an answer that names it names no workspace.
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":5,"workspaces":[0,4294967295]}})", 1) == Session::HostListing::Answer::Refused);
    CHECK(strict.noteAnswer(R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","result":{"hostNowUnixMilliseconds":5,"workspaces":[0]}})", 1) == Session::HostListing::Answer::Refused);
    CHECK_STR_EQ(Session::HostListing::subscribeRequest(), R"({"jsonrpc":"2.0","id":"ubiquitty.host-listing","method":"host.sessions.subscribe","params":{}})");
}

UBIQUITTY_TEST_MAIN()
