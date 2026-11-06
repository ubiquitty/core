// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/HostDirectoryClient.h"
#include "Harness.h"

#include "Core/App/SessionOutline.h"
#include "Core/Codec/JSON.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/HostOutline.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/ProofOfLife.h"
#include "Core/Session/ScriptedListingHost.h"
#include "Core/Session/SessionRows.h"
#include "Core/Session/Workspace.h"
#include "Core/Themes/Themes.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty;

namespace {

struct Dispatcher final : Session::Dispatcher {
    std::vector<Session::PostedWork> queue;
    std::vector<Session::PostedWork> delayed;
    void post(Session::PostedWork work) override { queue.push_back(std::move(work)); }
    void postDelayed(double, Session::PostedWork work) override { delayed.push_back(std::move(work)); }
    void drain()
    {
        while (!queue.empty()) {
            auto work = std::move(queue.front());
            queue.erase(queue.begin());
            std::move(work)();
        }
    }
};

struct Channel final : Transport::Channel {
    explicit Channel(std::shared_ptr<std::vector<std::uint8_t>> written)
        : sent(std::move(written))
    {
    }

    Transport::ChannelObserver* observer { nullptr };
    bool start(Transport::ChannelObserver* value) override
    {
        observer = value;
        observer->onConnect();
        return true;
    }
    /// What the errand wrote, for a case that answers it as a host does.
    std::shared_ptr<std::vector<std::uint8_t>> sent;
    void send(std::span<const std::uint8_t> bytes) override { sent->insert(sent->end(), bytes.begin(), bytes.end()); }
    void stop() override { }
    Transport::Health health() const override { return {}; }
    Transport::Origin origin() const override { return {}; }
    std::string description() const override { return "test directory"; }
};

struct Factory final : Session::ChannelFactory {
    Channel* channel { nullptr };
    /// Outlives a refused channel, so a refusal can prove no mutation was sent.
    std::shared_ptr<std::vector<std::uint8_t>> lastSent;
    std::vector<std::uint32_t> heldSessionIDs;
    bool shouldFail { false };
    void createChannel(Session::TransportMode, Done done) override
    {
        if (shouldFail) {
            done(nullptr, false);
            return;
        }
        lastSent = std::make_shared<std::vector<std::uint8_t>>();
        auto owned = std::make_unique<Channel>(lastSent);
        channel = owned.get();
        done(std::move(owned), false);
    }
};

void sendBytes(Factory& factory, Dispatcher& dispatcher, std::string_view text)
{
    dispatcher.drain();
    factory.channel->observer->onData({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    dispatcher.drain();
}

void sendRefusal(Factory& factory, Dispatcher& dispatcher, const Protocol::Sync::Bye& refusal)
{
    dispatcher.drain();
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, refusal);
    std::vector<std::uint8_t> frame;
    REQUIRE(Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::Bye, 0, body));
    factory.channel->observer->onData(frame);
    dispatcher.drain();
}

/// A manual answer's host stays alive until its correlated Result is delivered.
struct WelcomedHost {
    std::unique_ptr<Protocol::Sync::HostConnection> connection;
    std::size_t receivedBytes = 0;
};

/// Reads the first direct Greet and answers it with an exact serving selection.
/// A bound request here names an already held numeric session, never a create.
void sendWelcome(Factory& factory, Dispatcher& dispatcher, WelcomedHost& welcomed, std::uint64_t daemonInstanceID = 99)
{
    dispatcher.drain();
    namespace Sync = Protocol::Sync;
    Sync::FrameDecoder decoder;
    decoder.append(*factory.lastSent);
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == Sync::FrameKind::Greet && !frame->channel);
    const auto greet = Sync::decodeGreet(frame->body);
    REQUIRE(greet);
    REQUIRE(!decoder.next() && decoder.error() == Sync::EnvelopeError::None && !decoder.bufferedBytes());
    if (!Sync::isUnboundTarget(greet->request.target)) {
        REQUIRE(greet->request.target == Sync::AdmissionTarget::Session && greet->request.isAttachOnly
            && greet->request.sessionID && !greet->request.workspaceID
            && std::ranges::find(factory.heldSessionIDs, greet->request.sessionID) != factory.heldSessionIDs.end());
    }
    CHECK(!greet->facts.viewportRows && !greet->facts.viewportColumns);
    Sync::ServingSelection selection {
        .target = greet->request.target,
        .sessionID = greet->request.sessionID,
        .workspaceID = greet->request.workspaceID,
        .daemonInstanceID = daemonInstanceID,
        .socketName = "host-directory-incarnation.sock",
    };
    REQUIRE(Sync::isValidSelection(selection));
    welcomed.connection = std::make_unique<Sync::HostConnection>(UbiquittyTest::syncHostPolicy(0),
        Sync::HostIdentity { .hostInstanceID = { 1, 2 }, .attachmentID = { 3, 4 }, .resumeSecret = { 5, 6 }, .maxRole = Sync::Role::Owner, .principal = "test", .mayDrive = true, .verifiedKind = Sync::ClientKind::Human, .selection = std::move(selection) });
    welcomed.receivedBytes = factory.lastSent->size();
    welcomed.connection->receiveStream(*factory.lastSent, 1);
    welcomed.connection->service(2);
    std::vector<std::uint8_t> stream;
    while (welcomed.connection->popStreamFrame(stream, 2)) {
    }
    REQUIRE(!stream.empty());
    factory.channel->observer->onData(stream);
    dispatcher.drain();
}

/// Answers the actual one outstanding Call, preserving its transport and RPC IDs.
void answerCall(Factory& factory, Dispatcher& dispatcher, WelcomedHost& welcomed,
    std::string_view method, Codec::JSON::Value result, std::uint32_t sessionID = 0)
{
    REQUIRE(welcomed.connection);
    welcomed.connection->receiveStream(std::span<const std::uint8_t>(*factory.lastSent).subspan(welcomed.receivedBytes), 3);
    welcomed.receivedBytes = factory.lastSent->size();
    const auto calls = welcomed.connection->takeCalls();
    REQUIRE(calls.size() == 1);
    bool isJSON = false;
    const auto request = Codec::JSON::parse(calls.front().text, isJSON);
    REQUIRE(isJSON && request.stringOr("method") == method && request.find("id"));
    if (sessionID) {
        const auto* parameters = request.find("params");
        REQUIRE(parameters);
        const auto* session = parameters->find("session");
        REQUIRE(session && session->isNumber() && session->asNumber() == sessionID);
    }
    auto response = Codec::JSON::Value::object();
    response.set("jsonrpc", Codec::JSON::Value::string("2.0"));
    response.set("id", *request.find("id"));
    response.set("result", std::move(result));
    REQUIRE(welcomed.connection->answerCall(calls.front().callIndex, Codec::JSON::serialize(response)) == Protocol::Sync::CallAnswer::Sent);
    welcomed.connection->service(4);
    std::vector<std::uint8_t> stream;
    while (welcomed.connection->popStreamFrame(stream, 4)) {
    }
    REQUIRE(!stream.empty());
    factory.channel->observer->onData(stream);
    dispatcher.drain();
}

/// A complete canonical empty listing needs no structure or pane records.
void finishEmptyListing(Factory& factory, Dispatcher& dispatcher)
{
    WelcomedHost welcomed;
    sendWelcome(factory, dispatcher, welcomed);
    REQUIRE(welcomed.connection && welcomed.connection->boundPanes().empty());
    auto result = Codec::JSON::Value::object();
    result.set("workspaces", Codec::JSON::Value::array());
    result.set("structureRevisions", Codec::JSON::Value::array());
    result.set("paneRevisions", Codec::JSON::Value::array());
    result.set("panes", Codec::JSON::Value::number(0));
    result.set("hostNowUnixMilliseconds", Codec::JSON::Value::number(5'000'000));
    answerCall(factory, dispatcher, welcomed, "host.sessions.subscribe", std::move(result));
}

/// The canonical frames written even when the channel has already retired.
std::size_t sentKindCount(const Factory& factory, Protocol::Sync::FrameKind kind)
{
    Protocol::Sync::FrameDecoder decoder;
    if (!factory.lastSent)
        return 0;
    decoder.append(*factory.lastSent);
    std::size_t count = 0;
    while (const auto frame = decoder.next()) {
        if (frame->kind == kind)
            ++count;
    }
    CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None);
    CHECK(!decoder.bufferedBytes());
    return count;
}

/// A directory client against hosts that list what they hold. Each channel
/// the client dials reaches the next host in turn, the last one from then on.
struct ListedHosts {
    UbiquittyTest::HeldWorkspace held;
    UbiquittyTest::ScriptedHost listing;
    UbiquittyTest::ScriptedHost watched;
    UbiquittyTest::QueueingDispatcher dispatcher;

    struct Factory final : Session::ChannelFactory {
        std::vector<UbiquittyTest::ScriptedHost*> hosts;
        std::size_t dialled = 0;
        bool shouldFail = false;
        void createChannel(Session::TransportMode, Done done) override
        {
            if (shouldFail) {
                ++dialled;
                done(nullptr, false);
                return;
            }
            UbiquittyTest::ScriptedHost* host = hosts[std::min(dialled++, hosts.size() - 1)];
            done(std::make_unique<UbiquittyTest::HostChannel>(*host), false);
        }
    };
    Factory factory;
    std::uint64_t readerNow = 4'000'000;
    /// How many times anything read the clock this client was given.
    int clockReads = 0;
    Runtime::HostDirectoryClient client { dispatcher, factory };
    std::vector<Runtime::HostDirectoryClient::Event> events;

    ListedHosts()
    {
        listing.held = &held;
        watched.held = &held;
        for (const auto& [id, pane] : held.panes) {
            (void)id;
            listing.heldSessionIDs.push_back(pane.stated.sessionID);
            watched.heldSessionIDs.push_back(pane.stated.sessionID);
        }
        factory.hosts = { &listing };
        client.setWallClock([this] {
            ++clockReads;
            return readerNow;
        });
        client.setChangeCallback([this] { events.push_back(client.snapshot().event); });
    }

    /// Lists the host and carries the errand to its end.
    void refresh()
    {
        CHECK(client.refresh());
        dispatcher.drain();
    }
};

/// Whether two colours are the same four channels.
bool sameColor(ColorRGBA a, ColorRGBA b)
{
    return Runtime::HostOutlineDetail::packed(a) == Runtime::HostOutlineDetail::packed(b);
}

/// The row of an outline with a title, or nothing.
///
/// A copy: the outline a case asks for is a temporary, and a row pointed into
/// it is gone with it.
std::optional<App::OutlineRow> rowTitled(const App::SessionOutline& outline, std::string_view title)
{
    const auto found = std::ranges::find(outline.rows, title, &App::OutlineRow::title);
    return found != outline.rows.end() ? std::optional(*found) : std::nullopt;
}

/// The sessions a snapshot's tree lists, one row a session; none when it
/// holds no tree.
std::vector<Session::SessionRow> rowsOf(const Runtime::HostDirectoryClient::Snapshot& snapshot)
{
    return snapshot.tree ? Session::sessionRows(*snapshot.tree) : std::vector<Session::SessionRow> {};
}

} // namespace

TEST(theSnapshotCarriesTheNewestTreeAndItsOutline)
{
    ListedHosts hosts;
    CHECK(!hosts.client.snapshot().tree);
    CHECK(!hosts.client.tree());
    CHECK_EQ(hosts.client.hostNowUnixMilliseconds(), 0u);
    CHECK(hosts.client.outline({}).rows.empty());
    hosts.refresh();
    CHECK(hosts.client.tree() == hosts.client.snapshot().tree);
    CHECK_EQ(hosts.client.hostNowUnixMilliseconds(), hosts.listing.wallNowUnixMilliseconds);
    const Runtime::HostDirectoryClient::Snapshot first = hosts.client.snapshot();
    CHECK(first.event == Runtime::HostDirectoryClient::Event::SessionList);
    CHECK(first.phase == Runtime::HostDirectoryClient::ListingPhase::Listed);
    REQUIRE(first.tree != nullptr);
    CHECK_EQ(first.treeRevision, 1u);
    CHECK_EQ(first.tree->workspaces.size(), 1u);
    CHECK_EQ(first.hostClockLeadMilliseconds, 1'000'000);
    // The errand's connection read the clock this client was given, at the
    // host's answer and at the tree, and the client read it for the lead: no
    // connection reads a clock of its own.
    CHECK(hosts.clockReads >= 3);
    // One change for the listing: the tree arrives with its rows, not before.
    CHECK_EQ(hosts.events.size(), 1u);
    // The outline is the tree's rows: the workspace, and a pane a row.
    const App::SessionOutline outline = hosts.client.outline({});
    REQUIRE(outline.rows.size() == 3);
    CHECK(outline.rows[0].level == App::ListedLevel::Workspace);
    CHECK_STR_EQ(outline.rows[0].title, "work");
    CHECK(outline.rows[0].status.kind == App::StatusKind::Blocked);
    CHECK(rowTitled(outline, "make all") != std::nullopt);
    CHECK(rowTitled(outline, "vim notes") != std::nullopt);
    // A newer listing replaces the tree, and the outline follows it.
    hosts.held.at(hosts.held.right).stated.title.programTitle = "vim plan";
    hosts.held.at(hosts.held.right).restate();
    hosts.refresh();
    CHECK_EQ(hosts.client.snapshot().treeRevision, 2u);
    CHECK(rowTitled(hosts.client.outline({}), "vim plan") != std::nullopt);
    CHECK(rowTitled(hosts.client.outline({}), "vim notes") == std::nullopt);
    // The snapshot taken earlier still holds the tree it was given.
    CHECK_STR_EQ(first.tree->workspaces[0].windows[0].tabs[0].panes[1].title.programTitle, "vim notes");
}

TEST(aConnectIsDecidedOnTheTreeHeldFreshAndItsQuestionIsWordedOnce)
{
    using Session::AttachAsk;
    using Session::AttachPlan;
    ListedHosts hosts;
    // Nothing held: the host has not answered.
    Session::AttachDecision decision = hosts.client.decideAttach("a host nothing remembers", "", 0, {});
    CHECK(decision.plan == AttachPlan::AwaitListing);
    CHECK(hosts.client.decideAttach("a host nothing remembers", "work", 0, {}).plan == AttachPlan::CreateNamed);
    CHECK(hosts.client.snapshot().attachQuestion == Runtime::AttachQuestion {});

    hosts.refresh();
    const std::shared_ptr<const Session::SessionTree> tree = hosts.client.tree();
    REQUIRE(tree && tree->workspaces.size() == 1);
    const Session::TreeTab& tab = tree->workspaces[0].windows[0].tabs[0];
    const std::uint32_t landing = tab.panes[tab.hostFocusedPane].sessionID;
    REQUIRE(landing != 0);
    // The name picks the one workspace: no question.
    decision = hosts.client.decideAttach("a host nothing remembers", "work", 0, {});
    CHECK(decision.plan == AttachPlan::AttachNamed && decision.workspaceID == UbiquittyTest::HeldWorkspace::workspaceID && decision.sessionID == landing);
    CHECK(hosts.client.snapshot().attachQuestion.ask == AttachAsk::None && hosts.client.snapshot().attachQuestion.text.empty());

    // Nothing picks one: the person is asked, of the workspace under the
    // title its row in the outline has.
    decision = hosts.client.decideAttach("a host nothing remembers", "", 0, {});
    CHECK(decision.plan == AttachPlan::ChooseFromList && decision.ask == AttachAsk::WhichWorkspace);
    Runtime::AttachQuestion asked = hosts.client.snapshot().attachQuestion;
    CHECK(asked.ask == AttachAsk::WhichWorkspace && asked.isNewSessionOffered);
    CHECK_STR_EQ(asked.text, "This host holds one workspace, and nothing says you were in it. Attach to it?");
    REQUIRE(asked.candidates.size() == 1);
    CHECK(asked.candidates[0].workspaceID == UbiquittyTest::HeldWorkspace::workspaceID && asked.candidates[0].sessionID == landing && asked.candidates[0].title == "work" && asked.candidates[0].state == Session::TreeWorkspaceState::Live);
    // What picking it opens is what the workspace's own row opens.
    CHECK(asked.candidates[0].open == Session::attachTargetOf(tree->workspaces[0]));
    CHECK(asked.candidates[0].open.opening == Session::AttachOpening::BySession && asked.candidates[0].open.sessionID == landing && asked.candidates[0].open.workspaceName == "work");
    // The candidates are named whatever the reader has closed or searches for.
    Runtime::HostOutlineRequest narrowed;
    narrowed.query = "matches no row at all";
    (void)hosts.client.decideAttach("a host nothing remembers", "", 0, narrowed);
    CHECK(hosts.client.snapshot().attachQuestion == asked);
    // The person's choice answers it.
    decision = hosts.client.decideAttach("a host nothing remembers", "", landing, {});
    CHECK(decision.plan == AttachPlan::AttachByID && decision.sessionID == landing);
    CHECK(hosts.client.snapshot().attachQuestion == Runtime::AttachQuestion {});

    // A name that is only a label asks, in its own words.
    CHECK(hosts.held.labels.set({ Session::Identity::ObjectKind::Workspace, UbiquittyTest::HeldWorkspace::workspaceID }, "client") == Session::Identity::LabelVerdict::Accepted);
    hosts.refresh();
    decision = hosts.client.decideAttach("a host nothing remembers", "client", 0, {});
    CHECK(decision.plan == AttachPlan::ChooseFromList && decision.ask == AttachAsk::NameIsOnlyALabel);
    asked = hosts.client.snapshot().attachQuestion;
    CHECK_STR_EQ(asked.text, "No workspace on this host is named \"client\", but one is labelled \"client\". Attach to it, or start a new workspace named \"client\"?");
    REQUIRE(asked.candidates.size() == 1);
    CHECK(asked.candidates[0].workspaceID == UbiquittyTest::HeldWorkspace::workspaceID && asked.candidates[0].sessionID == landing && asked.candidates[0].title == "work");

    // A tree a watch left behind is no answer: nothing is asked of it, and
    // nothing is concluded from it.
    hosts.factory.hosts = { &hosts.watched };
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::Live);
    CHECK(hosts.client.decideAttach("a host nothing remembers", "", 0, {}).plan == AttachPlan::ChooseFromList);
    REQUIRE(hosts.watched.peer != nullptr);
    hosts.watched.peer->onClose("connection reset");
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::Stale);
    decision = hosts.client.decideAttach("a host nothing remembers", "", 0, {});
    CHECK(decision.plan == AttachPlan::AwaitListing && decision.vanishedSessionID == 0);
    CHECK(hosts.client.decideAttach("a host nothing remembers", "client", 0, {}).plan == AttachPlan::CreateNamed);
    CHECK(hosts.client.snapshot().attachQuestion == Runtime::AttachQuestion {});
    hosts.client.stopWatching();
}

TEST(theQuestionAConnectAsksIsOneSentenceAQuestionAndNamesItsWorkspaces)
{
    using Session::AttachAsk;
    using Session::AttachPlan;
    // Two rows of an outline, as two workspaces would have.
    App::SessionOutline outline;
    for (const auto& [id, title, state] : { std::tuple { 80u, "work", Session::TreeWorkspaceState::Live }, std::tuple { 60u, "work (restorable)", Session::TreeWorkspaceState::Restorable } }) {
        // A window's row of the same number, met first, is not the
        // workspace's.
        App::OutlineRow row;
        row.level = App::ListedLevel::Window;
        row.id = { Session::Identity::ObjectKind::Window, id };
        row.title = "not this";
        outline.rows.push_back(row);
        row.level = App::ListedLevel::Workspace;
        row.id.kind = Session::Identity::ObjectKind::Workspace;
        row.title = title;
        row.workspaceState = state;
        outline.rows.push_back(row);
    }
    Session::AttachDecision decision;
    decision.plan = AttachPlan::ChooseFromList;
    decision.isNewSessionOffered = true;
    decision.ask = AttachAsk::SeveralBearTheName;
    decision.askedName = "work";
    Session::AttachTarget restoring;
    restoring.opening = Session::AttachOpening::ByRestoring;
    restoring.workspaceID = 60;
    restoring.workspaceName = "work";
    decision.candidates = { { .workspaceID = 80, .sessionID = 4, .open = {} }, { .workspaceID = 60, .sessionID = 0, .open = restoring }, { .workspaceID = 99, .sessionID = 7, .open = {} } };
    Runtime::AttachQuestion question = Runtime::attachQuestionOf(decision, outline);
    CHECK_STR_EQ(question.text, "3 workspaces on this host are named \"work\". Which one do you mean?");
    CHECK_STR_EQ(question.shortText, "3 are named \"work\". Which one?");
    REQUIRE(question.candidates.size() == 3);
    CHECK(question.candidates[0].title == "work" && question.candidates[0].sessionID == 4 && question.candidates[0].state == Session::TreeWorkspaceState::Live);
    CHECK(question.candidates[1].title == "work (restorable)" && question.candidates[1].sessionID == 0 && question.candidates[1].state == Session::TreeWorkspaceState::Restorable);
    CHECK_MSG(question.candidates[1].open == restoring, "a workspace the host would restore is picked by the name it is restored by, carried with it");
    CHECK_MSG(question.candidates[2].workspaceID == 99 && question.candidates[2].title.empty(), "a workspace the outline has no row for is still one to pick");
    CHECK(question.isNewSessionOffered);

    decision.ask = AttachAsk::NameIsOnlyALabel;
    decision.candidates.pop_back();
    CHECK_STR_EQ(Runtime::attachQuestionOf(decision, outline).text, "No workspace on this host is named \"work\", but 2 are labelled \"work\". Attach to one of them, or start a new workspace named \"work\"?");
    CHECK_STR_EQ(Runtime::attachQuestionOf(decision, outline).shortText, "None is named \"work\"; 2 are labelled it. Which one, or new?");
    decision.ask = AttachAsk::WhichWorkspace;
    CHECK_STR_EQ(Runtime::attachQuestionOf(decision, outline).text, "This host holds 2 workspaces, and nothing says which you were in. Which one?");
    CHECK_STR_EQ(Runtime::attachQuestionOf(decision, outline).shortText, "2 workspaces here. Which one?");
    // Every question is one, and none is asked for a plan that picked.
    for (const AttachAsk ask : { AttachAsk::WhichWorkspace, AttachAsk::SeveralBearTheName, AttachAsk::NameIsOnlyALabel }) {
        decision.ask = ask;
        const std::string text = Runtime::attachQuestionOf(decision, outline).text;
        CHECK(!text.empty() && text.back() == '?');
        const std::string brief = Runtime::attachQuestionOf(decision, outline).shortText;
        CHECK_MSG(!brief.empty() && brief.back() == '?' && brief.size() < text.size(), "the short form is a question too, and shorter");
    }
    decision.plan = AttachPlan::AttachNamed;
    CHECK(Runtime::attachQuestionOf(decision, outline) == Runtime::AttachQuestion {});
    decision.plan = AttachPlan::ChooseFromList;
    decision.ask = AttachAsk::None;
    CHECK(Runtime::attachQuestionOf(decision, outline) == Runtime::AttachQuestion {});
}

// ---- The changes a reader asks of a host's structure -------------------------

namespace {

using Runtime::StructureOperation;
using Runtime::StructureOutcome;
using Runtime::StructureResult;
using ObjectID = Session::Identity::ObjectID;
using ObjectKind = Session::Identity::ObjectKind;

/// The identities of what the scripted host's one workspace holds.
struct HeldObjects {
    ObjectID workspace { ObjectKind::Workspace, UbiquittyTest::HeldWorkspace::workspaceID };
    ObjectID window;
    ObjectID tab;
    ObjectID leftPane;
    ObjectID leftSession { ObjectKind::Session, 1 };

    explicit HeldObjects(const Session::SessionTree& tree)
    {
        const Session::TreeWindow& listed = tree.workspaces[0].windows[0];
        window = { ObjectKind::Window, std::to_underlying(listed.id) };
        tab = { ObjectKind::Tab, std::to_underlying(listed.tabs[0].id) };
        leftPane = { ObjectKind::Pane, std::to_underlying(listed.tabs[0].panes[0].id) };
    }
};

/// How many of a client's events were a change settling.
std::size_t settledCount(const ListedHosts& hosts)
{
    return static_cast<std::size_t>(std::ranges::count(hosts.events, Runtime::HostDirectoryClient::Event::StructureSettled));
}

} // namespace

TEST(aChangeIsOneTypedCallAndEndsAsItsNumberedResult)
{
    ListedHosts hosts;
    hosts.refresh();
    REQUIRE(hosts.client.tree() != nullptr);
    const HeldObjects held(*hosts.client.tree());
    const std::uint64_t treeBefore = hosts.client.snapshot().treeRevision;

    const std::uint64_t request = hosts.client.setLabel(held.tab, "logs");
    CHECK(request != 0);
    // Nothing has settled, and nothing was told, inside the call that asked.
    CHECK_EQ(settledCount(hosts), 0u);
    CHECK_EQ(hosts.client.snapshot().structureResult.requestID, 0u);
    hosts.dispatcher.drain();
    REQUIRE(hosts.listing.calls.size() == 1);
    CHECK_MSG(hosts.listing.calls[0].find(R"("method":"label.set")") != std::string::npos && hosts.listing.calls[0].find(R"("object":")" + Session::Identity::format(held.tab) + R"(")") != std::string::npos
            && hosts.listing.calls[0].find(R"("label":"logs")") != std::string::npos,
        hosts.listing.calls[0].c_str());
    CHECK_EQ(settledCount(hosts), 1u);
    const Runtime::HostDirectoryClient::Snapshot settled = hosts.client.snapshot();
    CHECK(settled.structureResult == (StructureResult { .requestID = request, .operation = StructureOperation::SetLabel, .target = held.tab, .outcome = StructureOutcome::Served, .reason = {} }));
    CHECK(settled.structureResults.size() == 1 && settled.structureResults[0].requestID == request);
    // Nothing follows this host, so the client asked it again: the tree a
    // served change reaches is the next one.
    CHECK(settled.treeRevision > treeBefore);
    CHECK(hosts.client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Listed);

    // Every other change is its own method with its own typed parameters.
    Runtime::StructureTags tags;
    tags.append("production");
    tags.append("billing");
    const std::uint64_t tagged = hosts.client.setTags(held.leftSession, tags);
    const std::uint64_t renamed = hosts.client.renameWorkspace(held.workspace, "client \"work\"");
    const std::uint64_t tabNamed = hosts.client.renameTab(held.tab, "");
    const std::uint64_t moved = hosts.client.moveTab(held.tab, held.window, 3);
    CHECK_MSG(moved != tabNamed, "each request has its own number");
    const Runtime::DestructiveConfirmation closing = hosts.client.confirmationFor(Runtime::DestructiveRequest::CloseWindow, held.window, "devbox");
    const auto closingRevision = hosts.client.tree()->workspaces[0].revision;
    const std::uint64_t closed = hosts.client.closeWindow(held.window, closing.token);
    hosts.dispatcher.drain();
    // The move named the tab while its rename was still out: refused here.
    REQUIRE(hosts.listing.calls.size() == 5);
    const auto said = [&](std::size_t index, const std::string& part) { return hosts.listing.calls[index].find(part) != std::string::npos; };
    CHECK_MSG(said(1, R"("method":"tags.set")") && said(1, R"("object":"session:1")") && said(1, R"("tags":["production","billing"])"), hosts.listing.calls[1].c_str());
    CHECK_MSG(said(2, R"("method":"workspace.rename")") && said(2, R"("workspace":9)") && said(2, R"("name":"client \"work\"")"), hosts.listing.calls[2].c_str());
    CHECK_MSG(said(3, R"("method":"tab.rename")") && said(3, R"("tab":)" + std::to_string(held.tab.number)) && said(3, R"("name":"")"), hosts.listing.calls[3].c_str());
    CHECK_MSG(said(4, R"("method":"window.close")") && said(4, R"("window":)" + std::to_string(held.window.number)), hosts.listing.calls[4].c_str());
    CHECK_MSG(said(4, R"("expectedRevision":")" + std::to_string(closingRevision) + R"(")"), hosts.listing.calls[4].c_str());
    const std::vector<StructureResult> results = hosts.client.snapshot().structureResults;
    const auto ended = [&](std::uint64_t id) {
        const auto found = std::ranges::find(results, id, &StructureResult::requestID);
        return found != results.end() ? std::optional(*found) : std::nullopt;
    };
    for (const std::uint64_t served : { tagged, renamed, tabNamed, closed })
        CHECK(ended(served) && ended(served)->outcome == StructureOutcome::Served);
    REQUIRE(ended(moved).has_value());
    CHECK(ended(moved)->outcome == StructureOutcome::RefusedHere);
    CHECK_STR_EQ(ended(moved)->reason, "A change to " + Session::Identity::format(held.tab) + " is still waiting for this host’s answer.");
    // Once its rename has settled, the move is asked.
    const std::uint64_t movedAgain = hosts.client.moveTab(held.tab, held.window, 3);
    hosts.dispatcher.drain();
    CHECK_MSG(hosts.listing.calls.back().find(R"("method":"tab.move")") != std::string::npos && hosts.listing.calls.back().find(R"("index":3)") != std::string::npos
            && hosts.listing.calls.back().find(R"("window":)" + std::to_string(held.window.number)) != std::string::npos,
        hosts.listing.calls.back().c_str());
    CHECK(hosts.client.snapshot().structureResult.requestID == movedAgain && hosts.client.snapshot().structureResult.outcome == StructureOutcome::Served);
    CHECK(hosts.client.snapshot().structureResults.size() <= Runtime::HostDirectoryClient::maxKeptStructureResults);
}

TEST(aChangeTheHostRefusesEndsWithTheHostsOwnSentence)
{
    ListedHosts hosts;
    hosts.refresh();
    const HeldObjects held(*hosts.client.tree());
    const std::uint64_t treeBefore = hosts.client.snapshot().treeRevision;
    hosts.listing.answer = R"({"jsonrpc":"2.0","id":$ID,"error":{"code":-32002,"message":"workspace.rename is not open to a caller attached as agent","data":{"kind":"forbidden"}}})";
    const std::uint64_t request = hosts.client.renameWorkspace(held.workspace, "mine");
    hosts.dispatcher.drain();
    const Runtime::HostDirectoryClient::Snapshot settled = hosts.client.snapshot();
    CHECK(settled.event == Runtime::HostDirectoryClient::Event::StructureSettled);
    CHECK(settled.structureResult.requestID == request && settled.structureResult.outcome == StructureOutcome::RefusedByHost);
    CHECK_STR_EQ(settled.structureResult.reason, "workspace.rename is not open to a caller attached as agent");
    // A refusal changed nothing, so nothing is asked again, and it is not
    // the listing's failure.
    CHECK_EQ(settled.treeRevision, treeBefore);
    CHECK(settled.failure == Session::ListingFailure::None && settled.error.empty());
    CHECK(settled.phase == Runtime::HostDirectoryClient::ListingPhase::Listed);
    // An answer the host gave under another id answers nothing.
    hosts.listing.answer = R"({"jsonrpc":"2.0","id":"somebody else's","result":{}})";
    const std::uint64_t unanswered = hosts.client.setLabel(held.tab, "x");
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().structureResult.requestID == unanswered && hosts.client.snapshot().structureResult.outcome == StructureOutcome::Unanswered);
    CHECK(!hosts.client.snapshot().structureResult.reason.empty());
}

TEST(aChangeOnATreeNotHeldFreshIsRefusedHereAndNeverSent)
{
    ListedHosts hosts;
    const ObjectID tab { ObjectKind::Tab, 7 };
    // No tree at all.
    const std::uint64_t early = hosts.client.setLabel(tab, "x");
    CHECK_EQ(settledCount(hosts), 0u);
    hosts.dispatcher.drain();
    CHECK_EQ(settledCount(hosts), 1u);
    CHECK(hosts.client.snapshot().structureResult == (StructureResult { .requestID = early, .operation = StructureOperation::SetLabel, .target = tab, .outcome = StructureOutcome::RefusedHere, .reason = "This host’s list is out of date. Refresh it, then try again." }));
    CHECK_EQ(hosts.factory.dialled, 0u);

    // A tree a watch left behind.
    hosts.factory.hosts = { &hosts.watched };
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::Live);
    const HeldObjects held(*hosts.client.tree());
    REQUIRE(hosts.watched.peer != nullptr);
    hosts.watched.peer->onClose("connection reset");
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::Stale);
    const std::size_t callsBefore = hosts.watched.calls.size();
    const std::uint64_t stale = hosts.client.closeWindow(held.window, 0);
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().structureResult.requestID == stale && hosts.client.snapshot().structureResult.outcome == StructureOutcome::RefusedHere);
    CHECK_STR_EQ(hosts.client.snapshot().structureResult.reason, "This host’s list is out of date. Refresh it, then try again.");
    CHECK_EQ(hosts.watched.calls.size(), callsBefore);
    hosts.client.stopWatching();
}

TEST(aChangeFollowedByAWatchAsksNoListingAndAnUnansweredOneSaysSo)
{
    ListedHosts hosts;
    hosts.factory.hosts = { &hosts.watched, &hosts.listing };
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    const HeldObjects held(*hosts.client.tree());
    const auto greetingCount = [&] { return static_cast<std::size_t>(hosts.watched.hellos + hosts.listing.hellos); };
    const std::size_t hellos = greetingCount();
    const std::uint64_t request = hosts.client.setLabel(held.leftPane, "runner");
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().structureResult.requestID == request && hosts.client.snapshot().structureResult.outcome == StructureOutcome::Served);
    // One more connection for the call, and none for a listing after it.
    CHECK_EQ(greetingCount(), hellos + 1);
    CHECK_EQ(hosts.watched.hellos, 1);
    CHECK_EQ(hosts.listing.calls.size(), 1u);
    CHECK(!hosts.listing.didSubscribe);
    CHECK(hosts.client.snapshot().isWatching);
    CHECK(hosts.client.snapshot().treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::Live);
    CHECK(hosts.client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::NotAsked);

    // A call the host reads and never answers ends in its time, as
    // unanswered, and leaves the listing's own state alone.
    hosts.listing.holdsCalls = true;
    const std::uint64_t held2 = hosts.client.setLabel(held.leftPane, "again");
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().structureResult.requestID == request);
    hosts.dispatcher.advance(Session::directoryAnswerSeconds + 1.0);
    const Runtime::HostDirectoryClient::Snapshot late = hosts.client.snapshot();
    CHECK(late.structureResult.requestID == held2 && late.structureResult.outcome == StructureOutcome::Unanswered);
    CHECK(!late.structureResult.reason.empty());
    CHECK(late.failure == Session::ListingFailure::None && late.phase == Runtime::HostDirectoryClient::ListingPhase::NotAsked);
    CHECK_EQ(greetingCount(), hellos + 2);
    CHECK_EQ(hosts.watched.hellos, 1);
    CHECK(late.isWatching && late.treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::Live);
    // Its target is free again.
    hosts.listing.holdsCalls = false;
    const std::uint64_t third = hosts.client.setLabel(held.leftPane, "third");
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().structureResult.requestID == third && hosts.client.snapshot().structureResult.outcome == StructureOutcome::Served);
    CHECK_EQ(greetingCount(), hellos + 3);
    CHECK_EQ(hosts.watched.hellos, 1);
    CHECK_EQ(hosts.listing.calls.size(), 3u);
    CHECK(!hosts.listing.didSubscribe && hosts.watched.calls.empty());
    CHECK(hosts.client.snapshot().isWatching);
    CHECK(hosts.watched.peer != nullptr);
    REQUIRE(hosts.watched.connection);
    CHECK(hosts.watched.connection->state() == Protocol::Sync::ConnectionState::Attached);
    hosts.client.stopWatching();
}

TEST(aRequestIsCheckedAgainstTheTreeBeforeAnythingIsSent)
{
    using Runtime::StructureCalls;
    using Runtime::StructureRequest;
    // A workspace of one window, one tab and twenty panes, and one the host
    // could restore.
    Session::SessionTree tree;
    Session::TreeWorkspace live;
    live.id = 80;
    Session::TreeWindow window;
    window.id = Session::Layout::SurfaceID { 11 };
    Session::TreeTab tab;
    tab.id = Session::Layout::TabID { 21 };
    for (std::uint32_t index = 0; index < 20; ++index) {
        Session::TreePane pane;
        pane.id = Session::Layout::PaneID { 100 + index };
        pane.sessionID = 200 + index;
        tab.panes.push_back(pane);
    }
    window.tabs = { tab };
    live.windows = { window };
    Session::TreeWorkspace recorded;
    recorded.id = 60;
    recorded.state = Session::TreeWorkspaceState::Restorable;
    recorded.windows.emplace_back();
    tree.workspaces = { live, recorded };

    StructureCalls calls;
    const auto request = [](StructureOperation operation, ObjectID target) {
        StructureRequest made;
        made.operation = operation;
        made.target = target;
        made.window = { ObjectKind::Window, 11 };
        return made;
    };
    const auto refusal = [&](StructureOperation operation, ObjectID target, const Session::SessionTree* held = nullptr) {
        const StructureCalls::Admission admission = calls.admit(request(operation, target), held ? held : &tree);
        CHECK(admission.call.empty() == admission.refusal.has_value());
        return admission.refusal ? admission.refusal->reason : std::string();
    };
    // What each change may name.
    CHECK(!refusal(StructureOperation::SetLabel, { ObjectKind::Client, 1 }).empty());
    CHECK(!refusal(StructureOperation::RenameWorkspace, { ObjectKind::Tab, 21 }).empty());
    CHECK(!refusal(StructureOperation::RenameTab, { ObjectKind::Workspace, 80 }).empty());
    CHECK(!refusal(StructureOperation::MoveTab, { ObjectKind::Pane, 100 }).empty());
    CHECK(!refusal(StructureOperation::CloseWindow, { ObjectKind::Tab, 21 }).empty());
    // What the tree holds.
    CHECK_STR_EQ(refusal(StructureOperation::SetLabel, { ObjectKind::Tab, 99 }), "tab:99 is no longer on this host’s list.");
    CHECK_STR_EQ(refusal(StructureOperation::SetTags, { ObjectKind::Session, 0 }), "session:0 is no longer on this host’s list.");
    CHECK_STR_EQ(refusal(StructureOperation::CloseWindow, { ObjectKind::Window, 12 }), "window:12 is no longer on this host’s list.");
    CHECK_STR_EQ(refusal(StructureOperation::RenameWorkspace, { ObjectKind::Workspace, 60 }), "Nothing runs in workspace:60 yet. Open it before changing it.");
    StructureRequest elsewhere = request(StructureOperation::MoveTab, { ObjectKind::Tab, 21 });
    elsewhere.window = { ObjectKind::Window, 12 };
    CHECK_STR_EQ(calls.admit(elsewhere, &tree).refusal.value_or(StructureResult {}).reason, "window:12 is no longer on this host’s list.");
    elsewhere.window = { ObjectKind::Tab, 21 };
    CHECK(calls.admit(elsewhere, &tree).refusal.has_value());
    CHECK_EQ(calls.inFlightCount(), 0u);
    // Every level that carries a label is found.
    std::vector<std::uint64_t> out;
    for (const ObjectID target : { ObjectID { ObjectKind::Workspace, 80 }, ObjectID { ObjectKind::Window, 11 }, ObjectID { ObjectKind::Tab, 21 }, ObjectID { ObjectKind::Pane, 119 }, ObjectID { ObjectKind::Session, 219 } }) {
        const StructureCalls::Admission admitted = calls.admit(request(StructureOperation::SetLabel, target), &tree);
        CHECK_MSG(!admitted.refusal && admitted.call.find(StructureCalls::callID(admitted.requestID)) != std::string::npos, Session::Identity::format(target).c_str());
        out.push_back(admitted.requestID);
    }
    CHECK_EQ(calls.inFlightCount(), 5u);
    // One at a time a target, and no more out than the bound.
    CHECK_STR_EQ(refusal(StructureOperation::SetTags, { ObjectKind::Tab, 21 }), "A change to tab:21 is still waiting for this host’s answer.");
    for (std::uint32_t index = 0; calls.inFlightCount() < StructureCalls::maxInFlight; ++index)
        CHECK(!calls.admit(request(StructureOperation::SetLabel, { ObjectKind::Pane, 100 + index }), &tree).refusal);
    CHECK_STR_EQ(refusal(StructureOperation::SetLabel, { ObjectKind::Pane, 118 }), "Too many changes are waiting for this host’s answer.");
    CHECK_EQ(calls.inFlightCount(), StructureCalls::maxInFlight);
    // An answer frees its target, and only its own.
    REQUIRE(out.size() == 5);
    const std::optional<StructureResult> answered = calls.noteAnswer(R"({"jsonrpc":"2.0","id":")" + StructureCalls::callID(out[0]) + R"(","result":{}})");
    REQUIRE(answered.has_value());
    CHECK(answered->requestID == out[0] && answered->outcome == StructureOutcome::Served && answered->target == (ObjectID { ObjectKind::Workspace, 80 }));
    CHECK(!calls.noteAnswer(R"({"jsonrpc":"2.0","id":")" + StructureCalls::callID(out[0]) + R"(","result":{}})").has_value());
    CHECK(!calls.noteAnswer("not json").has_value() && !calls.noteAnswer(R"({"id":7,"result":{}})").has_value());
    CHECK_EQ(calls.inFlightCount(), StructureCalls::maxInFlight - 1);
    CHECK(!calls.fail(out[0], "").has_value());
    const std::optional<StructureResult> failed = calls.fail(out[1], "");
    CHECK(failed && failed->outcome == StructureOutcome::Unanswered && failed->reason == "The host did not answer.");
    // An error with no words still says something, and an answer that is
    // neither a result nor an error is not a success.
    const std::optional<StructureResult> wordless = calls.noteAnswer(R"({"jsonrpc":"2.0","id":")" + StructureCalls::callID(out[2]) + R"(","error":{"code":-32002}})");
    CHECK(wordless && wordless->outcome == StructureOutcome::RefusedByHost && !wordless->reason.empty());
    const std::optional<StructureResult> neither = calls.noteAnswer(R"({"jsonrpc":"2.0","id":")" + StructureCalls::callID(out[3]) + R"("})");
    CHECK(neither && neither->outcome == StructureOutcome::Unanswered && !neither->reason.empty());
}

// ---- The question asked before a request ends sessions ----------------------

namespace {

using Runtime::DestructiveConfirmation;
using Runtime::DestructiveConfirmations;
using Runtime::DestructiveRequest;

/// A pane a session draws into and a program runs in.
Session::TreePane runningPane(std::uint32_t id, std::uint32_t sessionID, std::string program)
{
    Session::TreePane pane;
    pane.id = Session::Layout::PaneID { id };
    pane.sessionID = sessionID;
    pane.program = std::move(program);
    return pane;
}

/// A host of two workspaces: "work" with a window of two tabs (vim and two
/// shells, then cargo) and a second window of one pane nothing draws into;
/// and one the host would restore.
Session::SessionTree confirmedTree()
{
    Session::SessionTree tree;
    Session::TreeWorkspace work;
    work.id = 80;
    work.name = "work";
    Session::TreeWindow front;
    front.id = Session::Layout::SurfaceID { 11 };
    front.attendance.people = 2;
    Session::TreeTab editing;
    editing.id = Session::Layout::TabID { 21 };
    editing.name = "editing";
    editing.panes = { runningPane(100, 200, "vim"), runningPane(101, 201, "zsh"), runningPane(102, 202, "zsh") };
    editing.panes[0].sessionLabel = "notes";
    Session::TreeTab building;
    building.id = Session::Layout::TabID { 22 };
    building.panes = { runningPane(103, 203, "cargo") };
    front.tabs = { editing, building };
    Session::TreeWindow empty;
    empty.id = Session::Layout::SurfaceID { 12 };
    empty.label = "scratch";
    Session::TreeTab idle;
    idle.id = Session::Layout::TabID { 23 };
    idle.panes = { runningPane(104, 0, "") };
    empty.tabs = { idle };
    work.windows = { front, empty };
    Session::TreeWorkspace recorded;
    recorded.id = 60;
    recorded.name = "old";
    recorded.state = Session::TreeWorkspaceState::Restorable;
    Session::TreeWindow kept;
    kept.id = Session::Layout::SurfaceID { 13 };
    Session::TreeTab keptTab;
    keptTab.panes = { runningPane(106, 0, "vim") };
    kept.tabs = { keptTab };
    recorded.windows = { kept };
    tree.workspaces = { work, recorded };
    return tree;
}

} // namespace

TEST(anOversizedStructureChangeIsRefusedBeforeItIsQueued)
{
    const Session::SessionTree tree = confirmedTree();
    Runtime::StructureCalls calls;
    Runtime::StructureRequest request;
    request.operation = StructureOperation::RenameTab;
    request.target = { ObjectKind::Tab, 21 };
    request.text.assign(70'000, 'n');
    Runtime::StructureCalls::Admission refused = calls.admit(request, &tree);
    REQUIRE(refused.refusal);
    CHECK(refused.call.empty());
    CHECK(refused.refusal->outcome == StructureOutcome::RefusedHere);
    CHECK_STR_EQ(refused.refusal->reason, "A tab name holds at most 4096 bytes, and this one holds 70000.");
    CHECK_EQ(calls.inFlightCount(), 0u);

    request.operation = StructureOperation::RenameWorkspace;
    request.target = { ObjectKind::Workspace, 80 };
    request.text.assign(Session::maxTreeNameBytes + 1, 'n');
    refused = calls.admit(request, &tree);
    REQUIRE(refused.refusal);
    CHECK_STR_EQ(refused.refusal->reason, "A workspace name holds at most 4096 bytes, and this one holds 4097.");
    CHECK_EQ(calls.inFlightCount(), 0u);

    request.text.resize(Session::maxTreeNameBytes);
    CHECK(!calls.admit(request, &tree).refusal);
    CHECK_EQ(calls.inFlightCount(), 1u);

    // JSON escaping counts toward the frame bound even when the input text
    // itself fits in one frame.
    request.operation = StructureOperation::SetLabel;
    request.target = { ObjectKind::Tab, 21 };
    request.text.assign(12'000, '\x01');
    refused = calls.admit(request, &tree);
    REQUIRE(refused.refusal);
    CHECK(refused.call.empty());
    CHECK(refused.refusal->reason.starts_with("A host call holds at most 65535 bytes, and this one holds "));
    CHECK_EQ(calls.inFlightCount(), 1u);
    request.text = "editing";
    CHECK(!calls.admit(request, &tree).refusal);
    CHECK_EQ(calls.inFlightCount(), 2u);
}

TEST(aStructureCallMayFillOneFrameExactly)
{
    const Session::SessionTree tree = confirmedTree();
    Runtime::StructureRequest request;
    request.operation = StructureOperation::SetLabel;
    request.target = { ObjectKind::Tab, 21 };
    Runtime::StructureCalls measuring;
    const Runtime::StructureCalls::Admission empty = measuring.admit(request, &tree);
    REQUIRE(!empty.refusal);
    request.text.assign(Protocol::Sync::maxFrameBodyBytes - empty.call.size(), 'x');
    Runtime::StructureCalls exact;
    const Runtime::StructureCalls::Admission admitted = exact.admit(request, &tree);
    CHECK(!admitted.refusal);
    CHECK_EQ(admitted.call.size(), Protocol::Sync::maxFrameBodyBytes);

    request.text.push_back('x');
    Runtime::StructureCalls over;
    const Runtime::StructureCalls::Admission refused = over.admit(request, &tree);
    REQUIRE(refused.refusal);
    CHECK(refused.call.empty());
    CHECK_STR_EQ(refused.refusal->reason, "A host call holds at most 65535 bytes, and this one holds 65536.");
    CHECK_EQ(over.inFlightCount(), 0u);
}

TEST(aRequestThatEndsSessionsIsWordedFromTheTree)
{
    const Session::SessionTree tree = confirmedTree();
    // A window is called by its label, else by the name of the tab it shows.
    const DestructiveConfirmation window = Runtime::wordDestructiveRequest(tree, DestructiveRequest::CloseWindow, { ObjectKind::Window, 11 }, "devbox");
    CHECK_STR_EQ(window.title, "Close editing on devbox?");
    CHECK_STR_EQ(window.body, "The host closes the window and ends its 4 sessions and everything running in them. Running: vim, zsh (2), cargo. 2 people are attached to it.");
    CHECK_STR_EQ(window.safeButton, "Keep Window");
    CHECK_STR_EQ(window.destructiveButton, "Close Window");
    CHECK(window.sessionIDs == (std::vector<std::uint32_t> { 200, 201, 202, 203 }));
    CHECK_MSG(window.token == 0 && window.whyNot.empty(), "wording gives no token; asking does");
    // One nothing runs in is still closed, and the question says so.
    const DestructiveConfirmation scratch = Runtime::wordDestructiveRequest(tree, DestructiveRequest::CloseWindow, { ObjectKind::Window, 12 }, "");
    CHECK_STR_EQ(scratch.title, "Close scratch?");
    CHECK_STR_EQ(scratch.body, "The host closes the window. Nothing runs in it.");
    CHECK(scratch.sessionIDs.empty() && scratch.whyNot.empty());

    // A session is named by its label, its pane's, or what runs in it, and
    // is the same question asked by its pane.
    const DestructiveConfirmation session = Runtime::wordDestructiveRequest(tree, DestructiveRequest::TerminateSession, { ObjectKind::Session, 200 }, "devbox");
    CHECK_STR_EQ(session.title, "Terminate notes on devbox?");
    CHECK_STR_EQ(session.body, "The host ends the session and everything running in it. Running: vim. Anyone attached to it is asked first and can decline.");
    CHECK_STR_EQ(session.safeButton, "Keep Session");
    CHECK_STR_EQ(session.destructiveButton, "Terminate");
    const DestructiveConfirmation byPane = Runtime::wordDestructiveRequest(tree, DestructiveRequest::TerminateSession, { ObjectKind::Pane, 100 }, "devbox");
    CHECK(byPane.title == session.title && byPane.body == session.body && byPane.sessionIDs == std::vector<std::uint32_t> { 200 });
    CHECK_STR_EQ(Runtime::wordDestructiveRequest(tree, DestructiveRequest::TerminateSession, { ObjectKind::Session, 203 }, "").title, "Terminate cargo?");

    const DestructiveConfirmation workspace = Runtime::wordDestructiveRequest(tree, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "devbox");
    CHECK_STR_EQ(workspace.title, "Terminate work on devbox?");
    CHECK_STR_EQ(workspace.body, "The host ends its 4 sessions and everything running in them, and the workspace with them. Running: vim, zsh (2), cargo. Anyone attached to one is asked first and can decline.");
    CHECK_STR_EQ(workspace.safeButton, "Keep Workspace");
    CHECK(workspace.sessionIDs == window.sessionIDs);
    // An unnamed workspace is asked about by the name its row shows: the
    // name of the tab it has in front, else the folder of its pane in front.
    Session::SessionTree unnamed = tree;
    unnamed.workspaces[0].name.clear();
    CHECK_STR_EQ(Runtime::wordDestructiveRequest(unnamed, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "devbox").title, "Terminate editing on devbox?");
    unnamed.workspaces[0].windows[0].tabs[0].name.clear();
    unnamed.workspaces[0].windows[0].tabs[0].panes[0].title.currentDirectory = "/srv/api";
    CHECK_STR_EQ(Runtime::wordDestructiveRequest(unnamed, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "devbox").title, "Terminate api on devbox?");
    unnamed.workspaces[0].windows[0].tabs[0].panes[0].title.currentDirectory.clear();
    CHECK_STR_EQ(Runtime::wordDestructiveRequest(unnamed, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "devbox").title, "Terminate this workspace on devbox?");

    // Nothing to ask: what the tree does not hold, a kind the request does
    // not take, and what nothing runs in.
    const auto whyNot = [&](DestructiveRequest request, ObjectID target) {
        const DestructiveConfirmation asked = Runtime::wordDestructiveRequest(tree, request, target, "devbox");
        CHECK(asked.title.empty() && asked.body.empty() && asked.sessionIDs.empty());
        return asked.whyNot;
    };
    CHECK_STR_EQ(whyNot(DestructiveRequest::CloseWindow, { ObjectKind::Window, 99 }), "window:99 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::CloseWindow, { ObjectKind::Tab, 21 }), "tab:21 is no longer on this host’s list.");
    // A number a window bears is a window's only as a window's, and a
    // workspace's only as a workspace's.
    CHECK_STR_EQ(whyNot(DestructiveRequest::CloseWindow, { ObjectKind::Tab, 11 }), "tab:11 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateWorkspace, { ObjectKind::Session, 80 }), "session:80 is no longer on this host’s list.");
    // Nothing of a workspace the host would restore is closed or ended.
    CHECK_STR_EQ(whyNot(DestructiveRequest::CloseWindow, { ObjectKind::Window, 13 }), "window:13 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateSession, { ObjectKind::Pane, 106 }), "pane:106 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateSession, { ObjectKind::Workspace, 80 }), "workspace:80 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateWorkspace, { ObjectKind::Session, 200 }), "session:200 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateSession, { ObjectKind::Session, 0 }), "session:0 is no longer on this host’s list.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateSession, { ObjectKind::Pane, 104 }), "Nothing runs in this session, so there is nothing to terminate.");
    CHECK_STR_EQ(whyNot(DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 60 }), "Nothing runs in this workspace, so there is nothing to terminate.");

    // More sessions than one request ends is no question at all, and the
    // most that one does end is.
    Session::SessionTree crowded = tree;
    std::vector<Session::TreePane>& panes = crowded.workspaces[0].windows[0].tabs[1].panes;
    panes.clear();
    for (std::uint32_t index = 0; index < Runtime::maxSessionsEndedAtOnce - 3; ++index)
        panes.push_back(runningPane(1000 + index, 2000 + index, "sh"));
    CHECK_EQ(Runtime::wordDestructiveRequest(crowded, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "").sessionIDs.size(), Runtime::maxSessionsEndedAtOnce);
    panes.push_back(runningPane(5000, 6000, "sh"));
    CHECK_STR_EQ(Runtime::wordDestructiveRequest(crowded, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "").whyNot, "This workspace holds more sessions than one request ends.");
    // Past six programs the rest are counted.
    Session::SessionTree varied = tree;
    std::vector<Session::TreePane>& many = varied.workspaces[0].windows[0].tabs[1].panes;
    many.clear();
    for (std::uint32_t index = 0; index < 6; ++index)
        many.push_back(runningPane(1000 + index, 2000 + index, "tool" + std::to_string(index)));
    many.push_back(runningPane(1100, 2100, ""));
    CHECK_STR_EQ(Runtime::wordDestructiveRequest(varied, DestructiveRequest::CloseWindow, { ObjectKind::Window, 11 }, "").body,
        "The host closes the window and ends its 10 sessions and everything running in them. Running: vim, zsh (2), tool0, tool1, tool2, tool3, 3 more. 2 people are attached to it.");
}

TEST(aTokenIsSpentOnceOnTheRequestItsQuestionWasAbout)
{
    const Session::SessionTree tree = confirmedTree();
    const ObjectID window { ObjectKind::Window, 11 };
    const ObjectID workspace { ObjectKind::Workspace, 80 };
    DestructiveConfirmations asked;
    const char* const unconfirmed = "This was not confirmed. Nothing is ended without asking first.";

    // No tree held fresh: nothing to ask, and no token.
    const DestructiveConfirmation blind = asked.ask(nullptr, DestructiveRequest::CloseWindow, window, "devbox");
    CHECK(blind.token == 0 && asked.heldCount() == 0);
    CHECK_STR_EQ(blind.whyNot, "This host’s list is out of date. Refresh it, then try again.");
    // Nothing to ask holds nothing.
    CHECK(asked.ask(&tree, DestructiveRequest::CloseWindow, { ObjectKind::Window, 99 }, "devbox").token == 0 && asked.heldCount() == 0);

    // Never asked, zero, and a number no question was given.
    CHECK_STR_EQ(asked.redeem(0, DestructiveRequest::CloseWindow, window, &tree).refusal, unconfirmed);
    CHECK_STR_EQ(asked.redeem(1, DestructiveRequest::CloseWindow, window, &tree).refusal, unconfirmed);

    const DestructiveConfirmation close = asked.ask(&tree, DestructiveRequest::CloseWindow, window, "devbox");
    const DestructiveConfirmation terminate = asked.ask(&tree, DestructiveRequest::TerminateWorkspace, workspace, "devbox");
    CHECK(close.token != 0 && terminate.token != 0 && close.token != terminate.token);
    CHECK_STR_EQ(close.title, "Close editing on devbox?");
    // A token is for its own request and its own target, and trying it on
    // another spends nothing.
    CHECK_STR_EQ(asked.redeem(close.token, DestructiveRequest::TerminateWorkspace, workspace, &tree).refusal, unconfirmed);
    CHECK_STR_EQ(asked.redeem(close.token, DestructiveRequest::CloseWindow, { ObjectKind::Window, 12 }, &tree).refusal, unconfirmed);
    CHECK_STR_EQ(asked.redeem(close.token, DestructiveRequest::TerminateSession, window, &tree).refusal, unconfirmed);
    CHECK_STR_EQ(asked.redeem(terminate.token, DestructiveRequest::TerminateSession, { ObjectKind::Session, 200 }, &tree).refusal, unconfirmed);
    CHECK_EQ(asked.heldCount(), 2u);
    const DestructiveConfirmations::Redemption spent = asked.redeem(close.token, DestructiveRequest::CloseWindow, window, &tree);
    CHECK(spent.refusal.empty() && spent.sessionIDs == close.sessionIDs);
    CHECK_MSG(asked.redeem(close.token, DestructiveRequest::CloseWindow, window, &tree).refusal == unconfirmed, "once");

    // What it would end changed after the question: refused, and the token
    // is gone with the answer it was given for.
    Session::SessionTree grown = tree;
    grown.workspaces[0].windows[0].tabs[1].panes.push_back(runningPane(105, 204, "psql"));
    CHECK_STR_EQ(asked.redeem(terminate.token, DestructiveRequest::TerminateWorkspace, workspace, &grown).refusal, "What this would end changed after you were asked. Look again.");
    CHECK_STR_EQ(asked.redeem(terminate.token, DestructiveRequest::TerminateWorkspace, workspace, &tree).refusal, unconfirmed);
    // The same count of other sessions is a change too.
    const DestructiveConfirmation again = asked.ask(&tree, DestructiveRequest::CloseWindow, window, "devbox");
    Session::SessionTree swapped = tree;
    swapped.workspaces[0].windows[0].tabs[1].panes[0].sessionID = 250;
    CHECK_STR_EQ(asked.redeem(again.token, DestructiveRequest::CloseWindow, window, &swapped).refusal, "What this would end changed after you were asked. Look again.");
    // Gone from the tree, and a tree gone stale.
    const DestructiveConfirmation third = asked.ask(&tree, DestructiveRequest::CloseWindow, window, "devbox");
    Session::SessionTree without = tree;
    without.workspaces[0].windows.erase(without.workspaces[0].windows.begin());
    CHECK_STR_EQ(asked.redeem(third.token, DestructiveRequest::CloseWindow, window, &without).refusal, "window:11 is no longer on this host’s list.");
    const DestructiveConfirmation fourth = asked.ask(&tree, DestructiveRequest::CloseWindow, window, "devbox");
    CHECK_STR_EQ(asked.redeem(fourth.token, DestructiveRequest::CloseWindow, window, nullptr).refusal, "This host’s list is out of date. Refresh it, then try again.");
    CHECK_EQ(asked.heldCount(), 0u);

    // The newest questions are the ones held: the ninth forgets the first.
    std::vector<std::uint64_t> tokens;
    for (std::size_t index = 0; index <= DestructiveConfirmations::maxHeld; ++index)
        tokens.push_back(asked.ask(&tree, DestructiveRequest::CloseWindow, window, "devbox").token);
    CHECK_EQ(asked.heldCount(), DestructiveConfirmations::maxHeld);
    CHECK_STR_EQ(asked.redeem(tokens.front(), DestructiveRequest::CloseWindow, window, &tree).refusal, unconfirmed);
    CHECK(asked.redeem(tokens[1], DestructiveRequest::CloseWindow, window, &tree).refusal.empty());
    CHECK(asked.redeem(tokens.back(), DestructiveRequest::CloseWindow, window, &tree).refusal.empty());
}

TEST(aWindowIsClosedOnlyWithTheTokenOfItsOwnQuestion)
{
    ListedHosts hosts;
    hosts.refresh();
    REQUIRE(hosts.client.tree() != nullptr);
    const HeldObjects held(*hosts.client.tree());
    const auto reasonOf = [&](std::uint64_t request) {
        hosts.dispatcher.drain();
        const std::vector<StructureResult> results = hosts.client.snapshot().structureResults;
        const auto result = std::ranges::find(results, request, &StructureResult::requestID);
        CHECK(result != results.end() && result->outcome == StructureOutcome::RefusedHere);
        return result != results.end() ? result->reason : std::string();
    };
    const char* const unconfirmed = "This was not confirmed. Nothing is ended without asking first.";
    // Unasked, by either form, and with the token of another question.
    CHECK_STR_EQ(reasonOf(hosts.client.closeWindow(held.window)), unconfirmed);
    CHECK_STR_EQ(reasonOf(hosts.client.closeWindow(held.window, 1)), unconfirmed);
    const DestructiveConfirmation other = hosts.client.confirmationFor(DestructiveRequest::TerminateWorkspace, held.workspace, "devbox");
    REQUIRE(other.token != 0);
    CHECK_STR_EQ(other.title, "Terminate work on devbox?");
    CHECK_STR_EQ(reasonOf(hosts.client.closeWindow(held.window, other.token)), unconfirmed);
    CHECK_MSG(hosts.listing.calls.empty(), "nothing was sent");

    const DestructiveConfirmation asked = hosts.client.confirmationFor(DestructiveRequest::CloseWindow, held.window, "devbox");
    REQUIRE(asked.token != 0);
    CHECK_STR_EQ(asked.title, "Close this window on devbox?");
    CHECK_STR_EQ(asked.body, "The host closes the window and ends its 2 sessions and everything running in them. Running: make, vim.");
    // A request refused for another reason keeps its token.
    const std::uint64_t labelled = hosts.client.setLabel(held.window, "front");
    CHECK_STR_EQ(reasonOf(hosts.client.closeWindow(held.window, asked.token)), "A change to " + Session::Identity::format(held.window) + " is still waiting for this host’s answer.");
    CHECK(hosts.client.snapshot().structureResults.size() >= 1 && labelled != 0);
    const std::size_t callsBefore = hosts.listing.calls.size();
    const std::uint64_t closed = hosts.client.closeWindow(held.window, asked.token);
    hosts.dispatcher.drain();
    REQUIRE(hosts.listing.calls.size() == callsBefore + 1);
    CHECK_MSG(hosts.listing.calls.back().find(R"("method":"window.close")") != std::string::npos, hosts.listing.calls.back().c_str());
    CHECK(hosts.client.snapshot().structureResult.requestID == closed && hosts.client.snapshot().structureResult.outcome == StructureOutcome::Served);
    // Spent.
    CHECK_STR_EQ(reasonOf(hosts.client.closeWindow(held.window, asked.token)), unconfirmed);
}

TEST(aTerminateIsSentOnlyWithTheTokenOfItsOwnQuestion)
{
    ListedHosts hosts;
    hosts.refresh();
    REQUIRE(hosts.client.tree() != nullptr);
    const HeldObjects held(*hosts.client.tree());
    const char* const unconfirmed = "This was not confirmed. Nothing is ended without asking first.";
    const std::size_t dialledBefore = hosts.factory.dialled;

    Runtime::TerminateAdmission refused = hosts.client.terminate(DestructiveRequest::TerminateSession, held.leftSession, 0);
    CHECK(refused.sessionCount == 0);
    CHECK_STR_EQ(refused.whyNot, unconfirmed);
    const DestructiveConfirmation session = hosts.client.confirmationFor(DestructiveRequest::TerminateSession, held.leftSession, "devbox");
    REQUIRE(session.token != 0);
    CHECK_STR_EQ(session.title, "Terminate make on devbox?");
    // Not for the workspace, and not for a window.
    CHECK_STR_EQ(hosts.client.terminate(DestructiveRequest::TerminateWorkspace, held.workspace, session.token).whyNot, unconfirmed);
    CHECK_STR_EQ(hosts.client.terminate(DestructiveRequest::CloseWindow, held.window, session.token).whyNot, "A window is closed, not terminated.");
    CHECK_MSG(hosts.factory.dialled == dialledBefore && !hosts.client.isBusy(), "nothing was sent");

    const Runtime::TerminateAdmission one = hosts.client.terminate(DestructiveRequest::TerminateSession, held.leftSession, session.token);
    CHECK(one.sessionCount == 1 && one.whyNot.empty());
    CHECK_EQ(hosts.factory.dialled, dialledBefore + 1);
    CHECK_STR_EQ(hosts.client.terminate(DestructiveRequest::TerminateSession, held.leftSession, session.token).whyNot, unconfirmed);

    // A workspace is one terminate a session, in the tree's order, each
    // queued behind the one before.
    const DestructiveConfirmation workspace = hosts.client.confirmationFor(DestructiveRequest::TerminateWorkspace, held.workspace, "devbox");
    REQUIRE(workspace.token != 0 && workspace.sessionIDs.size() == 2);
    const Runtime::TerminateAdmission both = hosts.client.terminate(DestructiveRequest::TerminateWorkspace, held.workspace, workspace.token);
    CHECK(both.sessionCount == 2 && both.whyNot.empty());
    CHECK(hosts.client.isBusy());
}

/// A reader states what its windows show once, the same to every host's
/// outline, and Core reads the ones that name the host being listed.
TEST(aReaderStatesItsWindowsOnceAndCoreReadsTheOnesOnThisHost)
{
    using App::OutlineHere;
    using Session::AttachOpening;
    ListedHosts hosts;
    hosts.refresh();
    REQUIRE(hosts.client.tree() != nullptr);
    const HeldObjects held(*hosts.client.tree());
    const auto paneRow = [&](const App::SessionOutline& outline) {
        const auto found = std::ranges::find(outline.rows, held.leftPane, &App::OutlineRow::id);
        CHECK(found != outline.rows.end());
        return found != outline.rows.end() ? *found : App::OutlineRow {};
    };
    Runtime::HostOutlineRequest request;
    request.host = "devbox";
    // Nothing stated: nowhere, and opened by attaching.
    CHECK(paneRow(hosts.client.outline(request)).here == OutlineHere::Elsewhere);
    CHECK(paneRow(hosts.client.outline(request)).open.opening == AttachOpening::BySession);

    // The window is on another host: not here.
    request.setHere("other", UbiquittyTest::HeldWorkspace::workspaceID, static_cast<std::uint32_t>(held.leftPane.number), 1);
    CHECK(paneRow(hosts.client.outline(request)).here == OutlineHere::Elsewhere);
    request.setHere("devbox", UbiquittyTest::HeldWorkspace::workspaceID, static_cast<std::uint32_t>(held.leftPane.number), 1);
    CHECK(paneRow(hosts.client.outline(request)).here == OutlineHere::Here);
    // A host is named exactly: no case is folded and nothing is trimmed.
    request.host = "Devbox";
    CHECK(paneRow(hosts.client.outline(request)).here == OutlineHere::Elsewhere);
    // This machine is the empty name on both sides.
    request.host.clear();
    request.setHere("", UbiquittyTest::HeldWorkspace::workspaceID, 0, 1);
    CHECK(paneRow(hosts.client.outline(request)).here == OutlineHere::Here);
    request.host = "devbox";
    request.setHere("devbox", UbiquittyTest::HeldWorkspace::workspaceID, static_cast<std::uint32_t>(held.leftPane.number), 1);

    // What the app has open elsewhere says nothing of this host's rows.
    request.addAlreadyOpen("other", UbiquittyTest::HeldWorkspace::workspaceID, 500);
    CHECK(paneRow(hosts.client.outline(request)).open.opening == AttachOpening::BySession);
    request.addAlreadyOpen("devbox", UbiquittyTest::HeldWorkspace::workspaceID, 600);
    const App::OutlineRow revealed = paneRow(hosts.client.outline(request));
    CHECK(revealed.open.opening == AttachOpening::ByRevealing && revealed.open.sessionID == 600 && revealed.open.paneID == held.leftPane.number);
    // Both facts reach a reader through the by-index rows.
    const Runtime::HostOutlineRows rows = hosts.client.outlineRows(request);
    std::size_t hereRows = 0;
    for (std::size_t index = 0; index < rows.count(); ++index) {
        hereRows += rows.here(index) == std::to_underlying(OutlineHere::Here) ? 1 : 0;
        CHECK(rows.openOpening(index) == std::to_underlying(AttachOpening::ByRevealing) && rows.openSessionID(index) == 600);
    }
    CHECK_EQ(hereRows, 1u);
    CHECK_EQ(rows.here(rows.count()), 0);

    // A compact reader: the workspace that holds here is open; closed by
    // hand it is closed and says it holds here.
    const std::uint8_t workspaceKind = std::to_underlying(ObjectKind::Workspace);
    request.density = App::OutlineDensity::Compact;
    CHECK_EQ(hosts.client.outline(request).rows.size(), 3u);
    CHECK(request.close(workspaceKind, held.workspace.number));
    App::SessionOutline closed = hosts.client.outline(request);
    REQUIRE(closed.rows.size() == 1);
    CHECK(closed.rows[0].here == OutlineHere::HoldsHere);
    CHECK(request.collapsedCount() == 1 && request.collapsedKind(0) == workspaceKind && request.collapsedNumber(0) == held.workspace.number && request.openedCount() == 0);
    // With nothing here it starts closed, and opening it by hand keeps it open.
    Runtime::HostOutlineRequest elsewhere;
    elsewhere.host = "devbox";
    elsewhere.density = App::OutlineDensity::Compact;
    CHECK_EQ(hosts.client.outline(elsewhere).rows.size(), 1u);
    CHECK(elsewhere.open(workspaceKind, held.workspace.number));
    CHECK_EQ(hosts.client.outline(elsewhere).rows.size(), 3u);
    CHECK(elsewhere.openedCount() == 1 && elsewhere.openedKind(0) == workspaceKind && elsewhere.openedNumber(0) == held.workspace.number && elsewhere.collapsedCount() == 0);
    CHECK(elsewhere.close(workspaceKind, held.workspace.number));
    CHECK(elsewhere.openedCount() == 0 && elsewhere.collapsedCount() == 1);
    // Opening a pane or a tab by hand is not remembered: only a workspace
    // starts closed.
    CHECK(elsewhere.open(std::to_underlying(ObjectKind::Tab), held.tab.number) && elsewhere.openedCount() == 0);

    // "Expand all under this row": the row and every row under it open,
    // on the tree the client holds; a row the tree does not hold changes
    // nothing.
    const Runtime::HostOutlineRequest opened = hosts.client.requestOpeningAllUnder(elsewhere, workspaceKind, held.workspace.number);
    CHECK(opened.collapsedCount() == 0 && opened.openedCount() == 1);
    CHECK_EQ(hosts.client.outline(opened).rows.size(), 3u);
    const Runtime::HostOutlineRequest same = hosts.client.requestOpeningAllUnder(elsewhere, workspaceKind, 4'000'000);
    CHECK(same.collapsedCount() == 1 && same.openedCount() == 0);
    CHECK(hosts.client.requestOpeningAllUnder(elsewhere, 99, 1).collapsedCount() == 1);
    CHECK(hosts.client.requestOpeningAllUnderRecorded(elsewhere, 4'000'000, 1, 0, 0).collapsedCount() == 1);
    // Past the last entry the read-back rests.
    CHECK(opened.collapsedKind(0) == 0 && opened.collapsedNumber(0) == 0 && opened.openedKind(7) == 0 && opened.openedNumber(7) == 0);
    CHECK(opened.collapsedRecordedCount() == 0 && opened.collapsedRecordedWorkspaceID(0) == 0 && opened.collapsedRecordedLevel(0) == 0 && opened.collapsedRecordedWindow(0) == 0 && opened.collapsedRecordedTab(0) == 0);
}

/// The two buttons of a question are never drawn alike: each states what it
/// does, the safe one first and the default.
TEST(aQuestionsButtonsStateWhichIsSafeAndWhichDestroys)
{
    using Runtime::ConfirmationButtonRole;
    const Session::SessionTree tree = confirmedTree();
    const DestructiveConfirmation asked = Runtime::wordDestructiveRequest(tree, DestructiveRequest::TerminateWorkspace, { ObjectKind::Workspace, 80 }, "devbox");
    CHECK_EQ(DestructiveConfirmation::buttonCount, 2u);
    CHECK(asked.buttonTitle(0) == asked.safeButton && asked.buttonTitle(0) == "Keep Workspace");
    CHECK(asked.buttonTitle(1) == asked.destructiveButton && asked.buttonTitle(1) == "Terminate");
    CHECK(asked.buttonTitle(2).empty());
    CHECK(DestructiveConfirmation::buttonRole(0) == ConfirmationButtonRole::SafeDefault);
    CHECK(DestructiveConfirmation::buttonRole(1) == ConfirmationButtonRole::Destructive);
    CHECK(DestructiveConfirmation::buttonRole(2) == ConfirmationButtonRole::SafeDefault);
}

/// The client reports its host as the facts a section is worded from, and
/// what a Connect window holds as the facts its default button is.
TEST(theClientStatesItsHostsSectionAndItsConnectButton)
{
    using Runtime::HostKind;
    using Runtime::HostSectionState;
    using Runtime::HostTransportRefusal;
    ListedHosts hosts;
    // Before anything is asked.
    Runtime::HostSectionFacts facts = hosts.client.sectionFacts(HostKind::Remote, HostTransportRefusal::None);
    CHECK(facts.progress == Runtime::HostListingProgress::NotAsked && facts.workspaceCount == 0 && !facts.hasAnswered && !facts.isThisMachine && !facts.isStale);
    CHECK(hosts.client.sectionWords("devbox", HostKind::Remote, HostTransportRefusal::None).state == HostSectionState::Asking);
    CHECK(hosts.client.sectionWords("devbox", HostKind::Remote, HostTransportRefusal::InvalidAddress).state == HostSectionState::InvalidAddress);

    hosts.refresh();
    REQUIRE(hosts.client.tree() != nullptr);
    hosts.readerNow += 90'000;
    facts = hosts.client.sectionFacts(HostKind::ThisMachine, HostTransportRefusal::None);
    CHECK(facts.progress == Runtime::HostListingProgress::Listed && facts.workspaceCount == 1 && facts.isThisMachine && facts.failure == Session::ListingFailure::None);
    CHECK_MSG(facts.hasAnswered && facts.secondsSinceAnswered == 90, "by the clock this client was given");
    const Runtime::HostSectionWords words = hosts.client.sectionWords("This Mac", HostKind::ThisMachine, HostTransportRefusal::None);
    CHECK(words.state == HostSectionState::Sessions);
    CHECK_STR_EQ(words.subtitle, "This Mac · 1 workspace");

    // The default button: the facts name the rows by the outline's titles,
    // whatever the reader has closed.
    const HeldObjects held(*hosts.client.tree());
    Runtime::HostOutlineRequest request;
    request.host = "devbox";
    CHECK(request.close(std::to_underlying(ObjectKind::Workspace), held.workspace.number));
    Session::AttachDecision decision;
    decision.plan = Session::AttachPlan::AttachByID;
    decision.sessionID = 1;
    decision.workspaceID = UbiquittyTest::HeldWorkspace::workspaceID;
    Runtime::ConnectActionFacts button = hosts.client.connectActionFacts("devbox", decision, request, 0, 0);
    CHECK(button.isDecided && button.plan == Session::AttachPlan::AttachByID && button.decidedSessionID == 1 && button.decidedWorkspaceID == UbiquittyTest::HeldWorkspace::workspaceID);
    CHECK_STR_EQ(button.landingRowTitle, "work");
    CHECK(!button.isRowSelected && !button.isLandingShownHere && !button.isConnectPending);
    CHECK_STR_EQ(Runtime::connectAction(button).title, "Reopen “work”");
    CHECK(Runtime::connectRowMarker(button, UbiquittyTest::HeldWorkspace::workspaceID) == Runtime::ConnectRowMarker::LastUsed);
    // A selected row brings what opening it does and who is attached.
    button = hosts.client.connectActionFacts("devbox", decision, request, std::to_underlying(ObjectKind::Pane), held.leftPane.number);
    CHECK(button.isRowSelected && button.selectedOpen.opening == Session::AttachOpening::BySession && button.selectedOpen.sessionID == 1);
    CHECK_STR_EQ(Runtime::connectAction(button).title, "Open “work”");
    CHECK(Runtime::connectRowMarker(button, UbiquittyTest::HeldWorkspace::workspaceID) == Runtime::ConnectRowMarker::None);
    // A row the tree does not hold is no selection.
    CHECK(!hosts.client.connectActionFacts("devbox", decision, request, std::to_underlying(ObjectKind::Pane), 4'000'000).isRowSelected);
    // What this app already shows in a window is shown, not opened again.
    request.addAlreadyOpen("devbox", UbiquittyTest::HeldWorkspace::workspaceID, 600);
    button = hosts.client.connectActionFacts("devbox", decision, request, 0, 0);
    CHECK(button.isLandingShownHere);
    CHECK_STR_EQ(Runtime::connectAction(button).title, "Show Window");
    CHECK(Runtime::connectAction(button).effect == Runtime::ConnectActionEffect::Reveal);
    button = hosts.client.connectActionFacts("devbox", decision, request, std::to_underlying(ObjectKind::Pane), held.leftPane.number);
    CHECK(button.isRowSelected && button.selectedOpen.opening == Session::AttachOpening::ByRevealing);
    CHECK_EQ(button.selectedOpen.sessionID, 600u);
    CHECK_EQ(button.selectedOpen.paneID, held.leftPane.number);
    CHECK(Runtime::connectAction(button).effect == Runtime::ConnectActionEffect::Reveal);

    // Recorded rows have a place rather than a live identity, but selecting
    // one must still carry its refusal into the default button.
    Session::SessionTree recorded;
    recorded.workspaces.emplace_back();
    recorded.workspaces.back().id = UbiquittyTest::HeldWorkspace::workspaceID;
    recorded.workspaces.back().name = "kept";
    recorded.workspaces.back().state = Session::TreeWorkspaceState::Restorable;
    recorded.workspaces.back().windows.resize(2);
    for (Session::TreeWindow& window : recorded.workspaces.back().windows) {
        window.tabs.emplace_back();
        window.tabs.back().panes.emplace_back();
    }
    std::vector<std::uint8_t> record;
    REQUIRE(Session::encodeWorkspaceStructure(record, recorded.workspaces.back()));
    hosts.listing.recordOverride = std::make_shared<const std::vector<std::uint8_t>>(std::move(record));
    hosts.refresh();
    Runtime::HostOutlineRequest reader;
    const auto rows = hosts.client.outline(reader).rows;
    const auto found = std::ranges::find_if(rows, [](const App::OutlineRow& row) { return row.recordedKey.has_value(); });
    REQUIRE(found != rows.end());
    const auto key = *found->recordedKey;
    button = hosts.client.connectActionFacts("devbox", decision, reader, 0, 0, key.workspaceID, std::to_underlying(key.level), key.window, key.tab);
    CHECK(button.isRowSelected && button.selectedOpen.opening == Session::AttachOpening::Cannot);
    CHECK(!Runtime::connectAction(button).isEnabled);
}

TEST(aListingAnsweredWithoutATreeDropsTheOneHeld)
{
    ListedHosts hosts;
    hosts.refresh();
    REQUIRE(hosts.client.snapshot().tree != nullptr);
    // A typed empty listing replaces the old contents with an empty tree.
    // A missing method is a refusal, not successful treeless row fallback.
    hosts.listing.holdsNothing = true;
    hosts.refresh();
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionList);
    REQUIRE(hosts.client.snapshot().tree);
    CHECK(hosts.client.snapshot().tree->workspaces.empty());
    CHECK(rowsOf(hosts.client.snapshot()).empty());
    CHECK_EQ(hosts.client.snapshot().treeRevision, 2u);
    CHECK(hosts.client.outline({}).rows.empty());
    // Each newly correlated empty listing remains a fresh successful answer.
    hosts.refresh();
    CHECK_EQ(hosts.client.snapshot().treeRevision, 3u);
    CHECK(hosts.client.snapshot().tree->workspaces.empty());
}

TEST(aFailedListingKeepsTheTreeAsItKeepsTheRows)
{
    ListedHosts hosts;
    hosts.refresh();
    hosts.listing.recordOverride = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t> { 0xff, 0xff });
    hosts.refresh();
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::Failed);
    CHECK(hosts.client.snapshot().failure == Session::ListingFailure::Malformed);
    REQUIRE(hosts.client.snapshot().tree != nullptr);
    CHECK_EQ(hosts.client.snapshot().treeRevision, 1u);
    CHECK_EQ(hosts.client.outline({}).rows.size(), 3u);
}

TEST(aClosedRowAndASearchReachTheOutline)
{
    ListedHosts hosts;
    hosts.refresh();
    Runtime::HostOutlineRequest closed;
    const std::uint8_t workspaceKind = std::to_underlying(Session::Identity::ObjectKind::Workspace);
    CHECK(closed.close(workspaceKind, UbiquittyTest::HeldWorkspace::workspaceID));
    // Closing twice is closing once, and a kind or a number that names
    // nothing closes nothing.
    CHECK(closed.close(workspaceKind, UbiquittyTest::HeldWorkspace::workspaceID));
    CHECK(!closed.close(static_cast<std::uint8_t>(Session::Identity::objectKindCount), 9));
    CHECK(!closed.close(std::to_underlying(Session::Identity::ObjectKind::Pane), 0));
    CHECK(closed.close(std::to_underlying(Session::Identity::ObjectKind::Client), 3));
    CHECK_EQ(closed.collapsed.size(), 2u);
    // Opening names a row the same way, and opens only the one named.
    CHECK(!closed.open(static_cast<std::uint8_t>(Session::Identity::objectKindCount), 9));
    CHECK(!closed.open(workspaceKind, 0));
    CHECK(closed.open(std::to_underlying(Session::Identity::ObjectKind::Client), 3));
    REQUIRE(closed.collapsed.size() == 1);
    CHECK(closed.collapsed[0] == (Session::Identity::ObjectID { Session::Identity::ObjectKind::Workspace, UbiquittyTest::HeldWorkspace::workspaceID }));
    const App::SessionOutline folded = hosts.client.outline(closed);
    REQUIRE(folded.rows.size() == 1);
    CHECK(!folded.isSearching && !Runtime::HostOutlineRows(folded).isSearching());
    // A query of nothing but blanks searches for nothing.
    closed.query = " \t ";
    CHECK(!hosts.client.outline(closed).isSearching);
    CHECK_EQ(hosts.client.outline(closed).rows.size(), 1u);
    CHECK(!folded.rows[0].isExpanded);
    REQUIRE(folded.rows[0].collapsedSummary);
    CHECK_EQ(folded.rows[0].collapsedSummary->panes, 2u);
    // A search keeps what matches and opens the way to it, closed or not.
    closed.query = "MAKE";
    const App::SessionOutline found = hosts.client.outline(closed);
    CHECK_EQ(found.matchCount, 1u);
    CHECK(found.isSearching && Runtime::HostOutlineRows(found).isSearching());
    REQUIRE(found.rows.size() == 2);
    CHECK_STR_EQ(found.rows[1].title, "make all");
    CHECK(found.rows[1].isMatch);
    // A recorded row is closed by its place, at a window's or a tab's level.
    Runtime::HostOutlineRequest recorded;
    CHECK(recorded.closeRecorded(5, std::to_underlying(App::ListedLevel::Tab), 0, 1));
    CHECK(recorded.closeRecorded(5, std::to_underlying(App::ListedLevel::Tab), 0, 1));
    CHECK(recorded.closeRecorded(5, std::to_underlying(App::ListedLevel::Window), 2, 0));
    CHECK(!recorded.closeRecorded(5, std::to_underlying(App::ListedLevel::Pane), 0, 0));
    CHECK(!recorded.closeRecorded(5, std::to_underlying(App::ListedLevel::Workspace), 0, 0));
    REQUIRE(recorded.collapsedRecordedRows.size() == 2);
    CHECK(recorded.collapsedRecordedRows[0] == (App::OutlineRecordedRowKey { .workspaceID = 5, .level = App::ListedLevel::Tab, .window = 0, .tab = 1 }));
    CHECK(recorded.collapsedRecordedRows[1] == (App::OutlineRecordedRowKey { .workspaceID = 5, .level = App::ListedLevel::Window, .window = 2, .tab = 0 }));
    CHECK(!recorded.openRecorded(5, std::to_underlying(App::ListedLevel::Pane), 0, 0));
    CHECK(!recorded.openRecorded(5, std::to_underlying(App::ListedLevel::Workspace), 0, 0));
    CHECK(recorded.openRecorded(5, std::to_underlying(App::ListedLevel::Tab), 0, 1));
    REQUIRE(recorded.collapsedRecordedRows.size() == 1);
    CHECK(recorded.collapsedRecordedRows[0].level == App::ListedLevel::Window);
    CHECK(recorded.openRecorded(5, std::to_underlying(App::ListedLevel::Window), 2, 0));
    CHECK(recorded.collapsedRecordedRows.empty());
}

TEST(theOutlineReadsTheViewersTitleSettingsAndTheme)
{
    UbiquittyTest::HeldWorkspace held;
    held.at(held.left).stated.title.processChain = "make";
    held.at(held.right).stated.title.processChain = "vim";
    held.at(held.right).stated.title.hostName = "prod-2";
    const Session::SessionTree tree = held.tree(9'000);
    // The preferences decide how a pane's title is composed and which hosts
    // wear which tag.
    const auto showing = [](bool appTitle, bool process) { // bool-ok: the two parts a case turns on
        Preferences::Values values;
        values.titleTabAppTitle = values.titlePaneAppTitle = appTitle;
        values.titleTabProcess = values.titlePaneProcess = process;
        values.titleTabDirectory = values.titlePaneDirectory = false;
        values.titleTabDimensions = values.titlePaneDimensions = false;
        values.titleTabProgramStatus = values.titlePaneProgramStatus = false;
        return values;
    };
    const Preferences::Values plain = showing(true, false);
    const App::SessionOutline byTitle = Runtime::buildHostOutline(tree, plain, {}, {});
    CHECK(rowTitled(byTitle, "make all") != std::nullopt);
    CHECK(rowTitled(byTitle, "make") == std::nullopt);
    Preferences::Values composed = showing(false, true);
    composed.tags.push_back({ .name = "production", .color = "red", .matchHost = "prod-*", .matchProfile = "", .prominence = "loud" });
    const App::SessionOutline byProcess = Runtime::buildHostOutline(tree, composed, {}, {});
    CHECK(rowTitled(byProcess, "make all") == std::nullopt);
    const std::optional<App::OutlineRow> make = rowTitled(byProcess, "make");
    REQUIRE(make != std::nullopt);
    CHECK(make->tags.empty());
    const std::optional<App::OutlineRow> vim = rowTitled(byProcess, "vim");
    REQUIRE(vim != std::nullopt);
    REQUIRE(vim->tags.size() == 1);
    CHECK_STR_EQ(vim->tags[0].name, "production");
    // The reader's half reaches a title: what names a pane with nothing to
    // say for itself.
    Runtime::HostOutlineRequest named;
    named.viewer.fallbackName = "shell";
    const Preferences::Values silent = showing(false, false);
    CHECK(rowTitled(Runtime::buildHostOutline(tree, silent, {}, named), "shell") != std::nullopt);
    // The theme decides what a status is drawn in.
    REQUIRE(Themes::count() > 1);
    const Themes::StatusColors first = Runtime::readerStatusColors(plain, 0);
    CHECK(sameColor(first.color(Themes::StatusColorSlot::Blocked), Themes::get(0)->statusColors().color(Themes::StatusColorSlot::Blocked)));
    const App::SessionOutline themed = Runtime::buildHostOutline(tree, plain, first, {});
    CHECK(themed.rows.at(0).statusSlot == Themes::StatusColorSlot::Blocked);
    CHECK(sameColor(themed.rows.at(0).statusColor, first.color(Themes::StatusColorSlot::Blocked)));
}

TEST(anOutlineNamesOnlyOtherMachinesAndSeparatesProgramStatus)
{
    UbiquittyTest::HeldWorkspace held;
    held.at(held.left).stated.title.hostName = "workstation";
    held.at(held.right).stated.title.hostName = "build-box";
    const auto describedTree = [&] {
        Session::SessionTree tree = held.tree(9'000);
        for (auto& tab : tree.workspaces.front().windows.front().tabs) {
            for (auto& pane : tab.panes) {
                OSC::ProgramStatus status;
                status.app = pane.id == held.left ? "Codex" : "Claude";
                status.state = pane.id == held.left ? OSC::ProgramState::Blocked : OSC::ProgramState::Working;
                status.message = pane.id == held.left ? "Choose an answer" : "Running tests";
                pane.records.store.records.clear();
                pane.records.store.records.push_back(std::move(status));
            }
        }
        Session::deriveSessionTree(tree);
        return tree;
    };
    const std::string localStatus = "Codex: Waiting for input — Choose an answer";
    const std::string remoteStatus = "Claude: Working — Running tests";
    Preferences::Values values;
    values.titleTabAppTitle = values.titlePaneAppTitle = true;
    values.titleTabProcess = values.titlePaneProcess = false;
    values.titleTabDirectory = values.titlePaneDirectory = false;
    values.titleTabDimensions = values.titlePaneDimensions = false;
    values.titleTabProgramStatus = values.titlePaneProgramStatus = true;
    Runtime::HostOutlineRequest reader;
    reader.viewer.windowHostName = "workstation";
    reader.showsProgramStatusSeparately = true;
    const App::SessionOutline outline = Runtime::buildHostOutline(describedTree(), values, {}, reader);
    const auto local = rowTitled(outline, "make all");
    REQUIRE(local);
    CHECK_STR_EQ(local->programStatus, localStatus);
    const auto remote = std::ranges::find_if(outline.rows, [&](const App::OutlineRow& row) {
        return row.id == Session::Identity::ObjectID { Session::Identity::ObjectKind::Pane, std::to_underlying(held.right) };
    });
    REQUIRE(remote != outline.rows.end());
    CHECK(remote->title.find("build-box") != std::string::npos);
    CHECK(remote->title.find("Claude") == std::string::npos);
    CHECK_STR_EQ(remote->programStatus, remoteStatus);
    const Runtime::HostOutlineRows rows(outline);
    CHECK_STR_EQ(rows.programStatus(static_cast<std::size_t>(remote - outline.rows.begin())), remoteStatus);
    CHECK(rows.programStatus(rows.count()).empty());
    // A tab with one pane folds it into the workspace and keeps its status.
    CHECK(held.layout->closePane(held.layout->surfaceIDAt(0), held.right));
    const App::SessionOutline folded = Runtime::buildHostOutline(describedTree(), values, {}, reader);
    REQUIRE(folded.rows.size() == 1);
    CHECK_STR_EQ(folded.rows.front().programStatus, localStatus);
    CHECK(folded.rows.front().secondaryTitle.find("workstation") == std::string::npos);
}

TEST(aReaderThatShowsNoThemeIsDrawnInTheOneTheConfigurationOpensIn)
{
    using Themes::StatusColorSlot;
    const auto packed = [](const Themes::StatusColors& colors, StatusColorSlot slot) { return Runtime::HostOutlineDetail::packed(colors.color(slot)); };
    const auto same = [&](const Themes::StatusColors& a, const Themes::StatusColors& b) {
        for (std::size_t slot = 0; slot < Themes::statusColorSlotCount; ++slot) {
            if (packed(a, static_cast<StatusColorSlot>(slot)) != packed(b, static_cast<StatusColorSlot>(slot)))
                return false;
        }
        return true;
    };
    // A configuration that names no theme opens in the house one, and a
    // reader that is no terminal is drawn in its inks: not in what the bare
    // palette a terminal starts with derives, which is pure ANSI.
    Preferences::Values values;
    const int house = Themes::curatedDefaultIndex(Preferences::ColorScheme::Dark);
    REQUIRE(Themes::get(house) != nullptr);
    const Themes::StatusColors none = Runtime::readerStatusColors(values, -1);
    CHECK(same(none, Themes::get(house)->statusColors()));
    const Themes::StatusColors bare;
    CHECK_EQ(packed(bare, StatusColorSlot::Working), 0x0000eeffu);
    CHECK_EQ(packed(bare, StatusColorSlot::Failed), 0xcd0000ffu);
    CHECK_MSG(!same(none, bare), "the house theme's inks are not the bare palette's");
    // The theme the configuration names is the one it opens in.
    int other = -1;
    for (int index = 0; index < Themes::count() && other < 0; ++index) {
        if (index != house && !same(Themes::get(index)->statusColors(), Themes::get(house)->statusColors()))
            other = index;
    }
    REQUIRE(other >= 0);
    values.themeName = Themes::get(other)->name;
    CHECK(same(Runtime::readerStatusColors(values, -1), Themes::get(other)->statusColors()));
    // A name the catalog does not hold is no theme named.
    values.themeName = "no theme is called this";
    CHECK(same(Runtime::readerStatusColors(values, -1), Themes::get(house)->statusColors()));
    // A reader that shows a theme is drawn in it, whatever is configured,
    // and an index the catalog does not hold is no theme shown.
    CHECK(same(Runtime::readerStatusColors(values, other), Themes::get(other)->statusColors()));
    CHECK(same(Runtime::readerStatusColors(values, Themes::count() + 7), Themes::get(house)->statusColors()));
    // The directory client's outline, asked with no theme, carries them.
    ListedHosts hosts;
    hosts.refresh();
    const App::SessionOutline outline = hosts.client.outline({});
    REQUIRE(!outline.rows.empty());
    CHECK(outline.rows[0].statusSlot == StatusColorSlot::Blocked);
    CHECK_EQ(Runtime::HostOutlineDetail::packed(outline.rows[0].statusColor), packed(Runtime::readerStatusColors(Preferences::values(), -1), StatusColorSlot::Blocked));
    CHECK(Runtime::HostOutlineDetail::packed(outline.rows[0].statusColor) != 0xcdcd00ffu);
}

TEST(aTabsRowIsComposedByTheTabsSettings)
{
    // A window of two tabs has a row a tab, called by the tab composition.
    UbiquittyTest::HeldWorkspace held;
    const Session::Layout::SurfaceID window = held.layout->surfaceIDAt(0);
    (void)held.layout->addTab(window);
    const Session::Layout::PaneID added = held.layout->focusedPane(window);
    held.panes.try_emplace(std::to_underlying(added), 3, "htop screen", "htop");
    held.at(added).stated.title.processChain = "htop";
    const Session::SessionTree tree = held.tree(9'000);
    Preferences::Values values;
    values.titleTabAppTitle = false;
    values.titleTabProcess = true;
    values.titleTabDirectory = false;
    values.titleTabDimensions = false;
    values.titleTabProgramStatus = false;
    // The pane composition says otherwise, so a tab's row called by it
    // would read differently.
    values.titlePaneAppTitle = true;
    values.titlePaneProcess = false;
    values.titlePaneDirectory = false;
    values.titlePaneDimensions = false;
    values.titlePaneProgramStatus = false;
    const App::SessionOutline outline = Runtime::buildHostOutline(tree, values, {}, {});
    const auto tab = std::ranges::find_if(outline.rows, [](const App::OutlineRow& row) { return row.level == App::ListedLevel::Tab && row.title == "htop"; });
    CHECK(tab != outline.rows.end());
    CHECK(rowTitled(outline, "htop screen") == std::nullopt);
    CHECK(rowTitled(outline, "make all") != std::nullopt);
}

TEST(theMarksAReaderHasColoursForReachTheOutline)
{
    ListedHosts hosts;
    hosts.refresh();
    Runtime::HostOutlineRequest request;
    const App::SessionOutline themed = hosts.client.outline(request);
    REQUIRE(!themed.rows.empty());
    CHECK(!themed.rows[0].notificationInk.isPlatformColor && !themed.rows[0].progressInk.isPlatformColor);
    request.platformStatusColors.hasNotification = true;
    request.platformStatusColors.hasProgress = true;
    const App::SessionOutline deferred = hosts.client.outline(request);
    REQUIRE(!deferred.rows.empty());
    CHECK(deferred.rows[0].notificationInk.isPlatformColor && deferred.rows[0].progressInk.isPlatformColor);
}

TEST(aRecordedRowClosedInTheRequestIsClosedInTheOutline)
{
    using Session::PaneNode;
    // A workspace nothing runs in: a window of one tab of two panes, and a
    // window of one pane. Its window rows have no identity to be closed by.
    Session::HostLayoutRecord record;
    record.arrangement.name = "kept";
    Session::WorkspaceWindow first;
    Session::WorkspaceTab split;
    split.root = PaneNode::makeSplit(false, 0.5, PaneNode::makeLeaf({}, "tail"), PaneNode::makeLeaf({}, "vim"));
    first.tabs.push_back(split);
    record.arrangement.windows.push_back(first);
    Session::WorkspaceWindow second;
    Session::WorkspaceTab single;
    single.root = PaneNode::makeLeaf({}, "shell");
    second.tabs.push_back(single);
    record.arrangement.windows.push_back(second);
    record.panes = { { 100, 30, "", "", {}, {} }, { 50, 15, "", "", {}, {} }, { 80, 24, "", "", {}, {} } };
    const Session::Identity::LabelBook labels;
    const std::array restorable { Session::TreeRestorableSource { 5, &record } };
    const Session::SessionTree tree = Session::buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 9'000, .restorable = restorable }, labels,
        [](std::uint32_t, Session::Layout::PaneID) { return Session::TreePaneFacts {}; });
    const Preferences::Values values;
    Runtime::HostOutlineRequest request;
    const App::SessionOutline open = Runtime::buildHostOutline(tree, values, {}, request);
    REQUIRE(open.rows.size() == 5);
    REQUIRE(open.rows[1].recordedKey.has_value());
    const App::OutlineRecordedRowKey key = *open.rows[1].recordedKey;
    CHECK(request.closeRecorded(key.workspaceID, std::to_underlying(key.level), key.window, key.tab));
    const App::SessionOutline closed = Runtime::buildHostOutline(tree, values, {}, request);
    CHECK_EQ(closed.rows.size(), 3u);
    CHECK(!closed.rows.at(1).isExpanded);
}

TEST(anAgeIsMeasuredAgainstTheHostsClockKeptRunning)
{
    // The host's clock a quarter of an hour ahead of this machine's, the
    // same distance behind it, and a day ahead: an age is the host's own
    // every time.
    for (const std::int64_t lead : { std::int64_t { 1'000'000 }, std::int64_t { -1'000'000 }, std::int64_t { 86'400'000 } }) {
        ListedHosts hosts;
        hosts.listing.wallNowUnixMilliseconds = static_cast<std::uint64_t>(static_cast<std::int64_t>(hosts.readerNow) + lead);
        // The pane last wrote ten seconds before the host answered, and was
        // typed into two minutes before that.
        hosts.held.at(hosts.held.left).stated.activity = { hosts.listing.wallNowUnixMilliseconds - 10'000, hosts.listing.wallNowUnixMilliseconds - 130'000 };
        hosts.held.at(hosts.held.left).restate();
        hosts.refresh();
        CHECK_EQ(hosts.client.snapshot().hostClockLeadMilliseconds, lead);
        const std::optional<App::OutlineRow> atAnswer = rowTitled(hosts.client.outline({}), "make all");
        REQUIRE(atAnswer != std::nullopt);
        CHECK(atAnswer->sinceOutput.isKnown);
        CHECK_EQ(atAnswer->sinceOutput.seconds, 10u);
        CHECK(atAnswer->sinceInput.isKnown);
        CHECK_EQ(atAnswer->sinceInput.seconds, 130u);
        // A pane nobody typed into has no age to give.
        const std::optional<App::OutlineRow> untouched = rowTitled(hosts.client.outline({}), "vim notes");
        REQUIRE(untouched != std::nullopt);
        CHECK(!untouched->sinceInput.isKnown && !untouched->sinceOutput.isKnown);
        // Five seconds pass here; the host's clock is five seconds on,
        // however far it is from this machine's.
        hosts.readerNow += 5'000;
        const std::optional<App::OutlineRow> later = rowTitled(hosts.client.outline({}), "make all");
        REQUIRE(later != std::nullopt);
        CHECK_EQ(later->sinceOutput.seconds, 15u);
        CHECK_EQ(later->sinceInput.seconds, 135u);
        CHECK_EQ(hosts.client.hostNowUnixMilliseconds(), hosts.listing.wallNowUnixMilliseconds + 5'000);
        // This machine's clock set back does not make the pane younger than
        // the host said it was.
        hosts.readerNow -= 60'000;
        CHECK_EQ(rowTitled(hosts.client.outline({}), "make all")->sinceOutput.seconds, 10u);
    }
}

TEST(aListedHostIsSearchedByEverythingItsRowsShow)
{
    // Through a real listing: the host describes itself over a connection,
    // the client assembles the tree, and the outline of that tree is searched
    // by a tag a rule gives, a tag a person put, a label, a session's label,
    // a title, a program and a directory.
    ListedHosts hosts;
    UbiquittyTest::HeldWorkspace& held = hosts.held;
    using Session::Identity::ObjectKind;
    (void)held.labels.set({ ObjectKind::Pane, std::to_underlying(held.left) }, "builder");
    (void)held.labels.set({ ObjectKind::Session, held.at(held.right).stated.sessionID }, "nightly");
    (void)held.tags.set({ ObjectKind::Pane, std::to_underlying(held.right) }, std::vector<std::string> { "mine" });
    held.at(held.left).stated.title.hostName = "prod-2";
    held.at(held.left).stated.title.currentDirectory = "/srv/app";
    held.at(held.left).stated.title.processChain = "make";
    held.at(held.left).restate();
    held.at(held.right).stated.title.currentDirectory = "/home/user/notes";
    held.at(held.right).restate();
    hosts.refresh();
    REQUIRE(hosts.client.snapshot().tree != nullptr);
    const Session::SessionTree& tree = *hosts.client.snapshot().tree;

    Preferences::Values values;
    values.titleTabAppTitle = values.titlePaneAppTitle = true;
    values.titleTabDirectory = values.titlePaneDirectory = false;
    values.titleTabProcess = values.titlePaneProcess = false;
    values.titleTabDimensions = values.titlePaneDimensions = false;
    values.titleTabProgramStatus = values.titlePaneProgramStatus = false;
    values.tags.push_back({ .name = "production", .color = "#ff0000", .matchHost = "prod-*", .matchProfile = "", .prominence = "loud" });
    values.tags.push_back({ .name = "mine", .color = "#00ff00", .matchHost = "", .matchProfile = "never", .prominence = "" });
    const auto found = [&](std::string_view query) {
        Runtime::HostOutlineRequest request;
        request.query = query;
        std::vector<std::string> titles;
        // The panes found: a parent that shows what its focused pane runs
        // is found by it too, and is not what is being asked about here.
        for (const App::OutlineRow& row : Runtime::buildHostOutline(tree, values, {}, request).rows) {
            if (row.isMatch && row.level == App::ListedLevel::Pane)
                titles.push_back(row.title);
        }
        return titles;
    };
    const std::vector<std::string> builder { "builder" };
    const std::vector<std::string> notes { "vim notes" };

    // The reader's rule resolved against the listed pane's host: the pane
    // wears the tag in the rule's colour, loud, and its parents wear it too.
    const App::SessionOutline plain = Runtime::buildHostOutline(tree, values, {}, {});
    const std::optional<App::OutlineRow> left = rowTitled(plain, "builder");
    const std::optional<App::OutlineRow> right = rowTitled(plain, "vim notes");
    REQUIRE(left != std::nullopt && right != std::nullopt);
    REQUIRE(left->tags.size() == 1);
    CHECK_STR_EQ(left->tags[0].name, "production");
    CHECK(left->tags[0].prominence == Session::TagProminence::Loud);
    CHECK(left->tags[0].color.set && left->tags[0].color.red == 255 && left->tags[0].color.green == 0);
    // A tag a person put on the pane is the host's, in the colour the
    // reader's rules give the name.
    REQUIRE(right->tags.size() == 1);
    CHECK_STR_EQ(right->tags[0].name, "mine");
    CHECK(right->tags[0].color.set && right->tags[0].color.green == 255);
    REQUIRE(!plain.rows.empty());
    CHECK_MSG(plain.rows[0].isExpanded && plain.rows[0].tags.empty(), "the workspace's row, open, leaves its panes' tags to the panes drawn under it");
    // The label is the row's title and what runs is beside it; the
    // session's label is a chip.
    CHECK_STR_EQ(left->secondaryTitle, "make all");
    CHECK(right->labelChips == std::vector<std::string> { "nightly" });

    CHECK(found("production") == builder);
    CHECK(found("MINE") == notes);
    CHECK(found("builder") == builder);
    CHECK(found("nightly") == notes);
    CHECK(found("make all") == builder);
    CHECK(found("vim") == notes);
    CHECK(found("/srv/app") == builder);
    CHECK(found("home/USER") == notes);
    // The program, where no title says it.
    CHECK(found("make") == builder);
    // Every term has to be found in the one row.
    CHECK(found("vim nightly") == notes);
    CHECK(found("vim production").empty());
    CHECK(found("nothing-has-this").empty());
}

TEST(aHostListsItselfToAViewerAsToAnyone)
{
    // The listing asks no role: a connection the host grants a viewer's is
    // sent the tree a driver's is.
    ListedHosts hosts;
    hosts.listing.maxRole = Protocol::Sync::Role::Viewer;
    hosts.refresh();
    const Runtime::HostDirectoryClient::Snapshot snapshot = hosts.client.snapshot();
    CHECK(snapshot.event == Runtime::HostDirectoryClient::Event::SessionList);
    REQUIRE(snapshot.tree != nullptr);
    CHECK_EQ(hosts.client.outline({}).rows.size(), 3u);
    REQUIRE(hosts.listing.connection.has_value());
    CHECK(hosts.listing.connection->grantedRole() == Protocol::Sync::Role::Viewer);
}

TEST(aWatchKeepsTheTreeCurrentAndNeverMakesAnErrandWait)
{
    ListedHosts hosts;
    hosts.factory.hosts = { &hosts.watched, &hosts.listing };
    CHECK(!hosts.client.snapshot().isWatching);
    REQUIRE(hosts.client.watch());
    CHECK(hosts.client.snapshot().isWatching);
    hosts.dispatcher.drain();
    REQUIRE(hosts.events.size() == 1);
    CHECK(hosts.events[0] == Runtime::HostDirectoryClient::Event::SessionTree);
    CHECK_EQ(hosts.client.snapshot().treeRevision, 1u);
    // A second ask to watch is the watch already out: nothing is dialled.
    CHECK(hosts.client.watch());
    CHECK_EQ(hosts.factory.dialled, 1u);
    // A watch is not an errand: nothing is busy, and a listing runs beside
    // it on a connection of its own.
    CHECK(!hosts.client.isBusy());
    hosts.refresh();
    CHECK_EQ(hosts.factory.dialled, 2u);
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionList);
    CHECK(hosts.client.snapshot().isWatching);
    // A newer empty listing clears the stale rows, without stopping the watch.
    hosts.listing.holdsNothing = true;
    const std::uint64_t beforeTreeless = hosts.client.snapshot().treeRevision;
    hosts.refresh();
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionList);
    CHECK(hosts.client.snapshot().tree != nullptr);
    CHECK_EQ(hosts.client.snapshot().treeRevision, beforeTreeless + 1);
    CHECK(hosts.client.snapshot().tree->workspaces.empty());
    CHECK(rowsOf(hosts.client.snapshot()).empty());
    CHECK(hosts.client.snapshot().isWatching);
    const std::uint64_t afterListing = hosts.client.snapshot().treeRevision;
    // The host changes and the watch reports it.
    hosts.held.at(hosts.held.left).stated.title.programTitle = "make check";
    hosts.held.at(hosts.held.left).restate();
    hosts.watched.pump();
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionTree);
    CHECK_EQ(hosts.client.snapshot().treeRevision, afterListing + 1);
    CHECK(rowTitled(hosts.client.outline({}), "make check") != std::nullopt);
    // The watch's own connection keeps the host's clock running by the
    // clock this client was given.
    hosts.readerNow += 3'000;
    hosts.held.at(hosts.held.left).stated.title.programTitle = "make clean";
    hosts.held.at(hosts.held.left).restate();
    hosts.watched.pump();
    hosts.dispatcher.drain();
    CHECK_EQ(hosts.client.snapshot().tree->hostNowUnixMilliseconds, hosts.watched.wallNowUnixMilliseconds + 3'000);
    CHECK_EQ(hosts.client.snapshot().hostClockLeadMilliseconds, 1'000'000);
    // Stopped, it says goodbye and reports nothing more.
    const std::size_t reported = hosts.events.size();
    hosts.client.stopWatching();
    CHECK(!hosts.client.snapshot().isWatching);
    REQUIRE(hosts.watched.byeAtExit);
    CHECK(*hosts.watched.byeAtExit == Protocol::Sync::ByeReason::Normal);
    CHECK_EQ(hosts.events.size(), reported);
    CHECK(hosts.client.snapshot().watchError.empty());
    // The tree it last reported stays.
    CHECK(hosts.client.snapshot().tree != nullptr);
}

TEST(aWatchThatLosesItsHostSaysSoMarksTheTreeStaleAndDialsAgain)
{
    using Freshness = Runtime::HostDirectoryClient::TreeFreshness;
    ListedHosts hosts;
    hosts.factory.hosts = { &hosts.watched };
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::None);
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().isWatching);
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Live);
    CHECK_EQ(hosts.client.snapshot().treeHeldAtUnixSeconds, 4'000);
    // The carrier goes.
    REQUIRE(hosts.watched.peer != nullptr);
    hosts.watched.peer->onClose("connection reset");
    hosts.dispatcher.drain();
    const Runtime::HostDirectoryClient::Snapshot dropped = hosts.client.snapshot();
    CHECK(dropped.event == Runtime::HostDirectoryClient::Event::WatchInterrupted);
    // Still a watch, and saying why it follows nothing: the tree is the one
    // it held, marked as left behind.
    CHECK(dropped.isWatching);
    CHECK(dropped.watchError.find("connection reset") != std::string::npos);
    REQUIRE(dropped.tree != nullptr);
    CHECK(dropped.treeFreshness == Freshness::Stale);
    CHECK_EQ(dropped.treeRevision, 1u);
    // A watch dropping says nothing about the listing.
    CHECK(dropped.phase == Runtime::HostDirectoryClient::ListingPhase::NotAsked);
    CHECK(dropped.failure == Session::ListingFailure::None);
    CHECK(dropped.error.empty());
    // Asking to watch again while it dials is the watch already out.
    CHECK(hosts.client.watch());
    CHECK_EQ(hosts.factory.dialled, 1u);
    // It dials a second later and follows again.
    hosts.readerNow += 9'000;
    hosts.dispatcher.advance(1.1);
    CHECK_EQ(hosts.factory.dialled, 2u);
    const Runtime::HostDirectoryClient::Snapshot back = hosts.client.snapshot();
    CHECK(back.event == Runtime::HostDirectoryClient::Event::SessionTree);
    CHECK(back.isWatching);
    CHECK(back.watchError.empty());
    CHECK(back.treeFreshness == Freshness::Live);
    CHECK_EQ(back.treeRevision, 2u);
    CHECK_EQ(back.treeHeldAtUnixSeconds, 4'009);
    // Stopped, the tree stays as of now and is followed by nothing.
    hosts.client.stopWatching();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Listed);
    CHECK(!hosts.client.snapshot().isWatching);
}

TEST(aListingSaysHowCurrentItsTreeIsBesideAWatch)
{
    using Freshness = Runtime::HostDirectoryClient::TreeFreshness;
    ListedHosts hosts;
    // A listing alone: as of the listing.
    hosts.refresh();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Listed);
    CHECK_EQ(hosts.client.snapshot().treeHeldAtUnixSeconds, 4'000);
    // Beside a watch that follows the host, its tree is as live as the
    // watch's.
    hosts.factory.hosts = { &hosts.watched, &hosts.listing };
    hosts.factory.dialled = 0;
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    hosts.refresh();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Live);
    // Beside a watch that lost its host, a listing that answers is fresher
    // than what the watch left behind, and is not followed.
    hosts.watched.peer->onClose("connection reset");
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Stale);
    hosts.refresh();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Listed);
    CHECK(hosts.client.snapshot().isWatching);
    CHECK(!hosts.client.snapshot().watchError.empty());
    // Stopped while it dials: the reason goes with the watch.
    hosts.client.stopWatching();
    CHECK(hosts.client.snapshot().watchError.empty());
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Listed);
    // An empty typed tree is still a dated successful listing, not absent data.
    hosts.listing.holdsNothing = true;
    hosts.refresh();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Listed);
    CHECK_EQ(hosts.client.snapshot().treeHeldAtUnixSeconds, 4'000);
    REQUIRE(hosts.client.snapshot().tree);
    CHECK(hosts.client.snapshot().tree->workspaces.empty());
    CHECK(rowsOf(hosts.client.snapshot()).empty());
}

TEST(aWatchTheHostRefusesEndsAndLeavesItsTreeBehind)
{
    using Freshness = Runtime::HostDirectoryClient::TreeFreshness;
    ListedHosts hosts;
    hosts.factory.hosts = { &hosts.watched };
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().treeFreshness == Freshness::Live);
    // The host comes back refusing to list itself.
    hosts.watched.held = nullptr;
    hosts.watched.peer->onClose("connection reset");
    hosts.dispatcher.drain();
    hosts.dispatcher.advance(1.1);
    const Runtime::HostDirectoryClient::Snapshot ended = hosts.client.snapshot();
    CHECK(ended.event == Runtime::HostDirectoryClient::Event::WatchEnded);
    CHECK(!ended.isWatching);
    CHECK(!ended.watchError.empty());
    CHECK(ended.tree != nullptr);
    CHECK(ended.treeFreshness == Freshness::Stale);
    // It is not dialled again.
    hosts.dispatcher.advance(120.0);
    CHECK_EQ(hosts.factory.dialled, 2u);
    // A new watch dials and clears the reason.
    hosts.watched.held = &hosts.held;
    REQUIRE(hosts.client.watch());
    CHECK(hosts.client.snapshot().watchError.empty());
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Live);
}

TEST(aWatchThatEndsWhileItFollowsLeavesItsTreeStale)
{
    using Freshness = Runtime::HostDirectoryClient::TreeFreshness;
    ListedHosts hosts;
    hosts.factory.hosts = { &hosts.watched };
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().treeFreshness == Freshness::Live);
    // The carrier ends it outright: an ssh that can no longer sign in.
    hosts.watched.peer->onClose("user@host: Permission denied (publickey).");
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::WatchEnded);
    CHECK(!hosts.client.snapshot().isWatching);
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Stale);
    // Stopping a watch that already left its tree behind does not unmark it.
    hosts.client.stopWatching();
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Stale);
    // And one stopped while it dials leaves what it left behind as it was.
    ListedHosts dialling;
    dialling.factory.hosts = { &dialling.watched };
    REQUIRE(dialling.client.watch());
    dialling.dispatcher.drain();
    dialling.watched.peer->onClose("connection reset");
    dialling.dispatcher.drain();
    REQUIRE(dialling.client.snapshot().treeFreshness == Freshness::Stale);
    dialling.client.stopWatching();
    CHECK(dialling.client.snapshot().treeFreshness == Freshness::Stale);
    CHECK(!dialling.client.snapshot().isWatching);
}

TEST(aTreeGoesStaleTheMomentItsWatchFindsThePathDead)
{
    using Freshness = Runtime::HostDirectoryClient::TreeFreshness;
    ListedHosts hosts;
    hosts.factory.hosts = { &hosts.watched };
    REQUIRE(hosts.client.watch());
    hosts.dispatcher.drain();
    REQUIRE(hosts.client.snapshot().treeFreshness == Freshness::Live);
    // The carrier stays open and carries nothing.
    hosts.watched.isPathDead = true;
    hosts.dispatcher.advance(Session::watchProbeIntervalSeconds * 2 + Session::proofOfLifeDeadlineSeconds - 0.5);
    // Asked, and inside the window for the answer: still live.
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Live);
    CHECK(hosts.client.snapshot().watchError.empty());
    hosts.dispatcher.advance(0.6);
    const Runtime::HostDirectoryClient::Snapshot dead = hosts.client.snapshot();
    CHECK(dead.event == Runtime::HostDirectoryClient::Event::WatchInterrupted);
    CHECK(dead.treeFreshness == Freshness::Stale);
    CHECK(dead.isWatching);
    CHECK(dead.watchError.find("stopped answering") != std::string::npos);
    REQUIRE(dead.tree != nullptr);
    // Back a second later, when the path is.
    hosts.watched.isPathDead = false;
    hosts.dispatcher.advance(1.1);
    CHECK(hosts.client.snapshot().treeFreshness == Freshness::Live);
    CHECK(hosts.client.snapshot().watchError.empty());
}

TEST(aListingIntoADeadPathFailsInTheSnapshotAndFreesTheQueue)
{
    // What a Connect window reads: asking, and then a failure in Core's
    // words, never asking for good.
    ListedHosts hosts;
    hosts.listing.isPathDead = true;
    CHECK(hosts.client.refresh());
    // A terminate queued behind it waits its turn.
    CHECK(hosts.client.terminate(4));
    hosts.dispatcher.drain();
    CHECK(hosts.client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Asking);
    hosts.dispatcher.advance(Session::directoryHandshakeSeconds - 1.0);
    CHECK(hosts.client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Asking);
    hosts.dispatcher.advance(1.5);
    const Runtime::HostDirectoryClient::Snapshot failed = hosts.client.snapshot();
    CHECK(failed.phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
    CHECK(failed.failure == Session::ListingFailure::Unreachable);
    CHECK_STR_EQ(failed.error, "The host could not be reached: the host did not answer.");
    CHECK_STR_EQ(failed.failureDetail, "The host did not answer.");
    // The queue moved on: the terminate was dialled, and settles in its own
    // time as one that never reached the host.
    CHECK_EQ(hosts.factory.dialled, 2u);
    CHECK(hosts.client.isBusy());
    hosts.dispatcher.advance(Session::directoryHandshakeSeconds + 0.5);
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::TerminateSettled);
    CHECK(hosts.client.snapshot().terminateOutcome == Session::TerminateOutcome::Failed);
    CHECK(!hosts.client.isBusy());
    // The listing's failure is still the listing's.
    CHECK(hosts.client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
}

TEST(aWatchOfAHostThatCannotBeDialledKeepsDialling)
{
    Dispatcher dispatcher;
    Factory factory;
    factory.shouldFail = true;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    // The carrier fails inside the start: reported, and still a watch.
    CHECK(client.watch());
    CHECK(client.snapshot().isWatching);
    CHECK(client.snapshot().event == Runtime::HostDirectoryClient::Event::WatchInterrupted);
    CHECK(!client.snapshot().watchError.empty());
    CHECK(client.snapshot().treeFreshness == Runtime::HostDirectoryClient::TreeFreshness::None);
    // A timer for the next dial is out.
    CHECK(!dispatcher.delayed.empty());
    client.stopWatching();
    CHECK(!client.snapshot().isWatching);
}

TEST(everyFieldOfARowIsReadByItsIndex)
{
    // A row with everything set, so that an accessor reading a neighbour's
    // field reads a different value.
    App::OutlineRow row;
    row.level = App::ListedLevel::Tab;
    row.depth = 2;
    row.id = { Session::Identity::ObjectKind::Tab, 41 };
    row.recordedKey = App::OutlineRecordedRowKey { .workspaceID = 5, .level = App::ListedLevel::Window, .window = 6, .tab = 7 };
    row.foldedWindow = Session::Identity::ObjectID { Session::Identity::ObjectKind::Window, 42 };
    row.foldedTab = Session::Identity::ObjectID { Session::Identity::ObjectKind::Tab, 43 };
    row.foldedPane = Session::Identity::ObjectID { Session::Identity::ObjectKind::Pane, 44 };
    row.path = { 8, 9, 10, 11 };
    row.workspaceState = Session::TreeWorkspaceState::RestoredAfterHostRestart;
    row.endsWhenDetached = true;
    row.title = "title";
    row.secondaryTitle = "secondary";
    row.labelChips = { "outer", "inner" };
    row.tags = { { .name = "plain", .color = {}, .prominence = Session::TagProminence::Quiet }, { .name = "red", .color = { 1, 2, 3, 4, true }, .prominence = Session::TagProminence::Loud, .showsName = false } };
    row.status.kind = App::StatusKind::Blocked;
    row.status.count = 12;
    row.status.paneCount = 13;
    row.status.blockKind = OSC::ProgramBlockKind::Question;
    row.status.progress = { 2, 55 };
    row.status.hasUnseenBell = true;
    row.status.hasUnseenNotification = true;
    row.status.indicatorColor = ColorRGBA { 10, 20, 30, 40 };
    row.status.tabColor = ColorRGBA { 50, 60, 70, 80 };
    row.status.badgeCount = 14;
    row.statusSlot = Themes::StatusColorSlot::Attention;
    row.statusColor = { 0x11, 0x22, 0x33, 0x44 };
    row.statusInk = { .isPlatformColor = false, .color = { 0x51, 0x52, 0x53, 0x54 } };
    row.progressInk = { .isPlatformColor = true, .color = { 0x61, 0x62, 0x63, 0x64 } };
    row.progressMark = App::StatusMark::ProgressError;
    row.bellInk = { .isPlatformColor = false, .color = { 0x71, 0x72, 0x73, 0x74 } };
    row.notificationInk = { .isPlatformColor = true, .color = { 0x81, 0x82, 0x83, 0x84 } };
    row.badge = "badge";
    row.activity = { 15, 16 };
    row.sinceOutput = { .isKnown = true, .seconds = 17 };
    row.sinceInput = { .isKnown = true, .seconds = 18 };
    row.sinceActivity = { .isKnown = true, .seconds = 29 };
    row.attendance = { .people = 19, .agents = 20 };
    row.program = "program";
    row.directory = "/home/user";
    row.columns = 21;
    row.rows = 22;
    row.isHostCurrent = true;
    row.isExpandable = true;
    row.isExpanded = true;
    row.collapsedSummary = App::OutlineSummary { .windows = 23, .tabs = 24, .panes = 25, .worstStatus = App::StatusKind::Failed, .worstStatusCount = 26, .newestActivity = { 27, 28 } };
    row.isMatch = true;
    App::SessionOutline outline;
    // What the row's actions read.
    row.open.opening = Session::AttachOpening::ByRestoring;
    row.open.sessionID = 51;
    row.open.workspaceID = 52;
    row.open.workspaceName = "restored";
    row.open.paneID = 53;
    row.open.obstacle = Session::AttachObstacle::PartOfARecord;
    row.own = { { .id = { Session::Identity::ObjectKind::Window, 54 }, .label = "window label", .tags = { "one" }, .name = "" },
        { .id = { Session::Identity::ObjectKind::Tab, 55 }, .label = "tab label", .tags = { "two", "three" }, .name = "tab name" } };
    row.tabPlace = App::OutlineTabPlace { .tab = { Session::Identity::ObjectKind::Tab, 56 }, .window = { Session::Identity::ObjectKind::Window, 57 }, .index = 58, .tabCount = 59, .canMoveEarlier = true, .canMoveLater = false, .acceptedIndices = { 64, 65 }, .otherWindows = { { .window = { Session::Identity::ObjectKind::Window, 60 }, .title = "first other", .index = 61 }, { .window = { Session::Identity::ObjectKind::Window, 62 }, .title = "second other", .index = 63 } } };
    row.attentionTitle = "attention title";
    row.attentionBody = "attention body";
    row.accessory = App::TabAccessory::ColouredBell;
    row.accessoryInk = ColorRGBA { 0x0A, 0x0B, 0x0C, 255 };
    outline.revision = 77;
    outline.matchCount = 3;
    outline.rows = { App::OutlineRow {}, row };
    const Runtime::HostOutlineRows rows(outline);
    CHECK_EQ(rows.count(), 2u);
    CHECK_EQ(rows.revision(), 77u);
    CHECK_EQ(rows.matchCount(), 3u);
    CHECK_EQ(rows.level(1), 2);
    CHECK_EQ(rows.depth(1), 2);
    CHECK_EQ(rows.idKind(1), std::to_underlying(Session::Identity::ObjectKind::Tab));
    CHECK_EQ(rows.idNumber(1), 41u);
    CHECK(rows.hasRecordedKey(1));
    CHECK_EQ(rows.recordedWorkspaceID(1), 5u);
    CHECK_EQ(rows.recordedLevel(1), 1);
    CHECK_EQ(rows.recordedWindow(1), 6u);
    CHECK_EQ(rows.recordedTab(1), 7u);
    CHECK_EQ(rows.foldedWindowNumber(1), 42u);
    CHECK_EQ(rows.foldedTabNumber(1), 43u);
    CHECK_EQ(rows.foldedPaneNumber(1), 44u);
    CHECK_EQ(rows.pathWorkspace(1), 8u);
    CHECK_EQ(rows.pathWindow(1), 9u);
    CHECK_EQ(rows.pathTab(1), 10u);
    CHECK_EQ(rows.pathPane(1), 11u);
    CHECK_EQ(rows.workspaceState(1), 1);
    CHECK(rows.doesEndWhenDetached(1));
    CHECK_STR_EQ(rows.title(1), "title");
    CHECK_STR_EQ(rows.secondaryTitle(1), "secondary");
    CHECK_EQ(rows.labelChipCount(1), 2u);
    CHECK_STR_EQ(rows.labelChip(1, 0), "outer");
    CHECK_STR_EQ(rows.labelChip(1, 1), "inner");
    CHECK(rows.labelChip(1, 2).empty());
    CHECK_EQ(rows.tagCount(1), 2u);
    CHECK_STR_EQ(rows.tagName(1, 0), "plain");
    CHECK(!rows.hasTagColor(1, 0));
    CHECK_EQ(rows.tagColor(1, 0), 0u);
    CHECK(!rows.isTagLoud(1, 0));
    CHECK_STR_EQ(rows.tagName(1, 1), "red");
    CHECK(rows.hasTagColor(1, 1));
    CHECK_EQ(rows.tagColor(1, 1), 0x01020304u);
    CHECK(rows.isTagLoud(1, 1));
    // A tag whose rule hides its name keeps the name and is drawn without it.
    CHECK(rows.isTagNameShown(1, 0));
    CHECK(!rows.isTagNameShown(1, 1));
    CHECK_STR_EQ(rows.tagSpokenName(1, 1), "red");
    CHECK(rows.tagName(1, 2).empty());
    CHECK(!rows.hasTagColor(1, 2));
    CHECK(!rows.isTagLoud(1, 2));
    CHECK(!rows.isTagNameShown(1, 2));
    CHECK_EQ(rows.statusKind(1), std::to_underlying(App::StatusKind::Blocked));
    CHECK_EQ(rows.statusCount(1), 12u);
    CHECK_EQ(rows.statusPaneCount(1), 13u);
    CHECK_EQ(rows.blockKind(1), std::to_underlying(OSC::ProgramBlockKind::Question));
    CHECK_EQ(rows.progressState(1), 2);
    CHECK_EQ(rows.progressValue(1), 55);
    CHECK(rows.hasUnseenBell(1));
    CHECK(rows.hasUnseenNotification(1));
    CHECK(rows.hasIndicatorColor(1));
    CHECK_EQ(rows.indicatorColor(1), 0x0a141e28u);
    CHECK(rows.hasTabColor(1));
    CHECK_EQ(rows.tabColor(1), 0x323c4650u);
    CHECK_EQ(rows.badgeCount(1), 14u);
    CHECK_EQ(rows.statusSlot(1), std::to_underlying(Themes::StatusColorSlot::Attention));
    CHECK_EQ(rows.statusColor(1), 0x11223344u);
    CHECK(!rows.isStatusInkPlatform(1));
    CHECK_EQ(rows.statusInkColor(1), 0x51525354u);
    CHECK(rows.isProgressInkPlatform(1));
    CHECK_EQ(rows.progressInkColor(1), 0x61626364u);
    CHECK_EQ(rows.progressMark(1), std::to_underlying(App::StatusMark::ProgressError));
    CHECK(!rows.isBellInkPlatform(1));
    CHECK_EQ(rows.bellInkColor(1), 0x71727374u);
    CHECK(rows.isNotificationInkPlatform(1));
    CHECK_EQ(rows.notificationInkColor(1), 0x81828384u);
    // The words are the ones a printed listing says for the same row.
    CHECK_STR_EQ(rows.statusWords(1), "blocked 12/13: question");
    CHECK_STR_EQ(rows.blockKindWord(1), "question");
    CHECK_STR_EQ(rows.progressWords(1), "progress error");
    CHECK_STR_EQ(rows.attendanceWords(1), "19 people, 20 agents");
    CHECK_STR_EQ(rows.collapsedWords(1), "23 windows, 24 tabs, 25 panes");
    CHECK_MSG(rows.stateWords(1).empty(), "only a workspace's own row says its state");
    CHECK_MSG(rows.lifetimeWords(1).empty(), "and only a workspace's own row says how long it lasts");
    CHECK_STR_EQ(rows.badge(1), "badge");
    CHECK_EQ(rows.lastOutputUnixMilliseconds(1), 15u);
    CHECK_EQ(rows.lastInputUnixMilliseconds(1), 16u);
    CHECK(rows.isOutputAgeKnown(1));
    CHECK_EQ(rows.secondsSinceOutput(1), 17u);
    CHECK(rows.isInputAgeKnown(1));
    CHECK_EQ(rows.secondsSinceInput(1), 18u);
    CHECK(rows.isActivityAgeKnown(1) && !rows.isActivityAgeKnown(0) && !rows.isActivityAgeKnown(2));
    CHECK_EQ(rows.secondsSinceActivity(1), 29u);
    CHECK_EQ(rows.attachedPeople(1), 19u);
    CHECK_EQ(rows.attachedAgents(1), 20u);
    CHECK_STR_EQ(rows.program(1), "program");
    CHECK_STR_EQ(rows.directory(1), "/home/user");
    CHECK_EQ(rows.columns(1), 21);
    CHECK_EQ(rows.rows(1), 22);
    CHECK(rows.isHostCurrent(1));
    CHECK(rows.isExpandable(1));
    CHECK(rows.isExpanded(1));
    CHECK(rows.hasCollapsedSummary(1));
    CHECK_EQ(rows.collapsedWindows(1), 23u);
    CHECK_EQ(rows.collapsedTabs(1), 24u);
    CHECK_EQ(rows.collapsedPanes(1), 25u);
    CHECK_EQ(rows.collapsedWorstStatus(1), std::to_underlying(App::StatusKind::Failed));
    CHECK_EQ(rows.collapsedWorstStatusCount(1), 26u);
    CHECK_EQ(rows.collapsedNewestOutputUnixMilliseconds(1), 27u);
    CHECK_EQ(rows.collapsedNewestInputUnixMilliseconds(1), 28u);
    CHECK(rows.isMatch(1));
    CHECK_STR_EQ(rows.hostCurrentWords(1), "The tab its window shows");
    CHECK_EQ(rows.openOpening(1), std::to_underlying(Session::AttachOpening::ByRestoring));
    CHECK_EQ(rows.openSessionID(1), 51u);
    CHECK_EQ(rows.openWorkspaceID(1), 52u);
    CHECK_STR_EQ(rows.openWorkspaceName(1), "restored");
    CHECK_EQ(rows.openPaneID(1), 53u);
    CHECK_STR_EQ(rows.openObstacle(1), std::string(Session::attachObstacleSentence(Session::AttachObstacle::PartOfARecord)));
    CHECK_EQ(rows.ownCount(1), 2u);
    CHECK(rows.ownKind(1, 0) == std::to_underlying(Session::Identity::ObjectKind::Window) && rows.ownNumber(1, 0) == 54);
    CHECK(rows.ownKind(1, 1) == std::to_underlying(Session::Identity::ObjectKind::Tab) && rows.ownNumber(1, 1) == 55);
    CHECK(rows.ownLabel(1, 0) == "window label" && rows.ownLabel(1, 1) == "tab label");
    CHECK(rows.ownName(1, 0).empty() && rows.ownName(1, 1) == "tab name");
    CHECK(rows.ownTagCount(1, 0) == 1 && rows.ownTagCount(1, 1) == 2);
    CHECK(rows.ownTag(1, 0, 0) == "one" && rows.ownTag(1, 1, 0) == "two" && rows.ownTag(1, 1, 1) == "three" && rows.ownTag(1, 1, 2).empty());
    CHECK(rows.hasTabPlace(1));
    CHECK(rows.tabPlaceTab(1) == 56 && rows.tabPlaceWindow(1) == 57 && rows.tabPlaceIndex(1) == 58 && rows.tabPlaceTabCount(1) == 59);
    CHECK(rows.canTabMoveEarlier(1) && !rows.canTabMoveLater(1));
    CHECK_EQ(rows.tabPlaceOtherWindowCount(1), 2u);
    CHECK(rows.tabPlaceOtherWindow(1, 0) == 60 && rows.tabPlaceOtherWindowTitle(1, 0) == "first other" && rows.tabPlaceOtherWindowIndex(1, 0) == 61);
    CHECK(rows.tabPlaceOtherWindow(1, 1) == 62 && rows.tabPlaceOtherWindowTitle(1, 1) == "second other" && rows.tabPlaceOtherWindowIndex(1, 1) == 63);
    CHECK(rows.tabPlaceOtherWindow(1, 2) == 0 && rows.tabPlaceOtherWindowTitle(1, 2).empty() && rows.tabPlaceOtherWindowIndex(1, 2) == 0);
    CHECK(rows.tabPlaceAcceptedIndexCount(1) == 2 && rows.tabPlaceAcceptedIndex(1, 0) == 64 && rows.tabPlaceAcceptedIndex(1, 1) == 65 && rows.tabPlaceAcceptedIndex(1, 2) == 0);
    REQUIRE(rows.detailCount(1) >= 2);
    CHECK(rows.detailLabel(1, 0) == "Program" && rows.detailValue(1, 0) == "program");
    CHECK(rows.detailLabel(1, 1) == "Directory" && rows.detailValue(1, 1) == "/home/user");
    CHECK(rows.detailLabel(1, rows.detailCount(1)).empty() && rows.detailValue(1, rows.detailCount(1)).empty());
    CHECK_STR_EQ(rows.attentionTitle(1), "attention title");
    CHECK_STR_EQ(rows.attentionBody(1), "attention body");
    CHECK_EQ(rows.accessory(1), std::to_underlying(App::TabAccessory::ColouredBell));
    CHECK(rows.hasAccessoryInk(1));
    CHECK_EQ(rows.accessoryInk(1), 0x0a0b0cffu);
    // The row with nothing set, and a row past the last, rest.
    for (const std::size_t index : { std::size_t { 0 }, std::size_t { 2 } }) {
        CHECK_EQ(rows.idNumber(index), 0u);
        CHECK(!rows.hasRecordedKey(index));
        CHECK_EQ(rows.recordedWorkspaceID(index), 0u);
        CHECK_EQ(rows.recordedLevel(index), 0);
        CHECK_EQ(rows.recordedWindow(index), 0u);
        CHECK_EQ(rows.recordedTab(index), 0u);
        CHECK_EQ(rows.foldedWindowNumber(index), 0u);
        CHECK_EQ(rows.foldedTabNumber(index), 0u);
        CHECK_EQ(rows.foldedPaneNumber(index), 0u);
        CHECK(rows.title(index).empty());
        CHECK_EQ(rows.labelChipCount(index), 0u);
        CHECK_EQ(rows.tagCount(index), 0u);
        CHECK(!rows.hasIndicatorColor(index));
        CHECK_EQ(rows.indicatorColor(index), 0u);
        CHECK(!rows.hasTabColor(index));
        CHECK_EQ(rows.tabColor(index), 0u);
        CHECK(!rows.isOutputAgeKnown(index));
        CHECK(!rows.hasCollapsedSummary(index));
        CHECK_EQ(rows.collapsedWindows(index), 0u);
        CHECK_EQ(rows.collapsedTabs(index), 0u);
        CHECK_EQ(rows.collapsedPanes(index), 0u);
        CHECK_EQ(rows.collapsedWorstStatus(index), 0);
        CHECK_EQ(rows.collapsedWorstStatusCount(index), 0u);
        CHECK_EQ(rows.collapsedNewestOutputUnixMilliseconds(index), 0u);
        CHECK_EQ(rows.collapsedNewestInputUnixMilliseconds(index), 0u);
        CHECK(rows.labelChip(index, 0).empty());
        CHECK(rows.tagName(index, 0).empty());
        CHECK_EQ(rows.tagColor(index, 0), 0u);
        CHECK(rows.hostCurrentWords(index).empty());
        CHECK(rows.secondLine(index).empty() && rows.detailCount(index) == 0 && rows.detailLabel(index, 0).empty() && rows.detailValue(index, 0).empty());
        CHECK(rows.tabPlaceAcceptedIndexCount(index) == 0 && rows.tabPlaceAcceptedIndex(index, 0) == 0);
        CHECK(rows.openOpening(index) == 0 && rows.openSessionID(index) == 0 && rows.openWorkspaceID(index) == 0 && rows.openPaneID(index) == 0);
        CHECK(rows.openWorkspaceName(index).empty() && rows.openObstacle(index).empty());
        CHECK(rows.ownCount(index) == 0 && rows.ownKind(index, 0) == 0 && rows.ownNumber(index, 0) == 0 && rows.ownTagCount(index, 0) == 0);
        CHECK(rows.ownLabel(index, 0).empty() && rows.ownName(index, 0).empty() && rows.ownTag(index, 0, 0).empty());
        CHECK(!rows.hasTabPlace(index) && rows.tabPlaceTab(index) == 0 && rows.tabPlaceWindow(index) == 0 && rows.tabPlaceIndex(index) == 0 && rows.tabPlaceTabCount(index) == 0);
        CHECK(!rows.canTabMoveEarlier(index) && !rows.canTabMoveLater(index) && rows.tabPlaceOtherWindowCount(index) == 0 && rows.tabPlaceOtherWindow(index, 0) == 0);
        CHECK(rows.attentionTitle(index).empty() && rows.attentionBody(index).empty());
        CHECK(rows.accessory(index) == 0 && !rows.hasAccessoryInk(index) && rows.accessoryInk(index) == 0);
        CHECK(!rows.isStatusInkPlatform(index) && !rows.isProgressInkPlatform(index) && !rows.isBellInkPlatform(index) && !rows.isNotificationInkPlatform(index));
        CHECK_EQ(rows.progressMark(index), std::to_underlying(App::StatusMark::Progress));
        CHECK(rows.statusWords(index).empty() && rows.blockKindWord(index).empty() && rows.progressWords(index).empty());
        CHECK(rows.attendanceWords(index).empty() && rows.collapsedWords(index).empty() && rows.stateWords(index).empty());
    }
    // A workspace's row that is not live says which it is.
    App::OutlineRow restorable;
    restorable.workspaceState = Session::TreeWorkspaceState::Restorable;
    outline.rows = { restorable };
    CHECK_STR_EQ(Runtime::HostOutlineRows(outline).stateWords(0), "restorable");
    CHECK(Runtime::HostOutlineRows(outline).lifetimeWords(0).empty());
    outline.rows[0].endsWhenDetached = true;
    CHECK_STR_EQ(Runtime::HostOutlineRows(outline).lifetimeWords(0), "ends when detached");
    // Past the last row every accessor rests, the ones a default row sets
    // too.
    CHECK_EQ(rows.level(2), 0);
    CHECK_EQ(rows.statusSlot(2), 0);
    CHECK_EQ(rows.statusColor(2), 0u);
    CHECK(!rows.isMatch(2));
}

TEST(directoryRetainsSuccessfulListAcrossFailureAndAcceptsEmptySuccess)
{
    ListedHosts hosts;
    auto& client = hosts.client;
    auto& factory = hosts.factory;
    const auto window = hosts.held.layout->surfaceIDAt(0);
    REQUIRE(hosts.held.layout->closePane(window, hosts.held.right));
    hosts.held.panes.erase(std::to_underlying(hosts.held.right));
    auto& pane = hosts.held.at(hosts.held.left);
    pane.stated.sessionID = 42;
    pane.stated.title.programTitle = "remote shell";
    pane.restate();
    int changes = 0;
    client.setChangeCallback([&changes] { ++changes; });
    hosts.refresh();
    CHECK(!client.isBusy());
    REQUIRE(rowsOf(client.snapshot()).size() == 1);
    CHECK_EQ(rowsOf(client.snapshot()).front().id, 42u);
    CHECK_STR_EQ(rowsOf(client.snapshot()).front().title, "remote shell");
    CHECK(client.snapshot().daemonSocketName == "host-test.sock");
    CHECK(client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionList);
    factory.shouldFail = true;
    CHECK(client.refresh());
    CHECK(client.snapshot().event == Runtime::HostDirectoryClient::Event::Failed);
    CHECK(!client.snapshot().error.empty());
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
    CHECK(client.snapshot().failure == Session::ListingFailure::Unreachable);
    CHECK_EQ(rowsOf(client.snapshot()).front().id, 42u);
    CHECK(client.snapshot().daemonSocketName == "host-test.sock");
    factory.shouldFail = false;
    hosts.listing.holdsNothing = true;
    hosts.refresh();
    CHECK(client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionList);
    CHECK(rowsOf(client.snapshot()).empty());
    CHECK(client.snapshot().error.empty());
    // An answer of nothing is an answer: Listed, with no failure, and dated.
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Listed);
    CHECK(client.snapshot().failure == Session::ListingFailure::None);
    CHECK(client.snapshot().answeredAtUnixSeconds > 0);
    CHECK_EQ(client.snapshot().hostBuild.wireVersion, Ubiquitty::Protocol::Sync::wireVersion);
    CHECK_EQ(changes, 3);
}

// The phase is a fact a dialog reads before any answer: nothing asked, then
// asking, and the asking state is set before the errand can settle, so a
// carrier that fails at once leaves Failed rather than being overwritten.
TEST(directoryPhaseMovesFromNotAskedThroughAsking)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::NotAsked);
    CHECK(client.refresh());
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Asking);
    finishEmptyListing(factory, dispatcher);
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Listed);

    factory.shouldFail = true;
    CHECK(client.refresh());
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
    CHECK(client.snapshot().failure == Session::ListingFailure::Unreachable);
}

// A host that answers over ssh with the deploy sentinel runs no host: the
// failure is typed NotInstalled and carries the platform it named, which is
// what an install offer needs. No English instruction rides along.
TEST(directoryReportsAHostWithNothingInstalledWithItsPlatform)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    CHECK(client.refresh());
    sendBytes(factory, dispatcher, "Last login: today\nUBIQUITTY-NEED-DEPLOY Linux aarch64\n");
    const auto snapshot = client.snapshot();
    CHECK(snapshot.phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
    CHECK(snapshot.failure == Session::ListingFailure::NotInstalled);
    CHECK_STR_EQ(snapshot.remotePlatform, "Linux aarch64");
    CHECK(snapshot.error.find("Choose Connect") == std::string::npos);
}

// A host that refuses the listing says why, and the reason is a fact the
// dialog keys its token state on rather than a sentence it parses.
TEST(directoryReportsARefusalWithTheHostsReason)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    CHECK(client.refresh());
    sendRefusal(factory, dispatcher, { .reason = Protocol::Sync::ByeReason::TokenExpired });
    const auto snapshot = client.snapshot();
    CHECK(snapshot.phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
    CHECK(snapshot.failure == Session::ListingFailure::Refused);
    CHECK(snapshot.deniedReason == Protocol::Sync::ByeReason::TokenExpired);
}

// A listing refused for a version carries which side is older and what the
// host runs, so the Connect window can offer an update before any window
// exists.
TEST(directoryReportsWhichSideOfAVersionRefusalIsOlder)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    CHECK(client.refresh());
    // The wire has one version, so a host that refuses it speaks another.
    constexpr std::uint16_t newer = Protocol::Sync::wireVersion + 1;
    sendRefusal(factory, dispatcher, { .reason = Protocol::Sync::ByeReason::VersionMismatch, .peer = Protocol::Sync::PeerMetadata { .build = { .isKnown = true, .wireVersion = newer, .appVersion = "0.2.0.7" }, .platform = "Linux x86_64" } });
    const auto snapshot = client.snapshot();
    CHECK(snapshot.failure == Session::ListingFailure::Refused);
    CHECK(snapshot.olderSide == Protocol::Sync::OlderSide::Client);
    CHECK_STR_EQ(snapshot.remotePlatform, "Linux x86_64");
    CHECK(snapshot.hostBuild.isKnown);
    CHECK_EQ(snapshot.hostBuild.wireVersion, newer);
    CHECK_STR_EQ(snapshot.hostBuild.appVersion, "0.2.0.7");
}

// Asks while one is out coalesce into exactly one follow-up: Connect and
// Manage sharing a client both get an answer, and the host is asked twice,
// not once per caller.
TEST(directoryCoalescesAsksIntoOneFollowUp)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    int listings = 0;
    client.setChangeCallback([&client, &listings] {
        if (client.snapshot().event == Runtime::HostDirectoryClient::Event::SessionList)
            ++listings;
    });
    CHECK(client.refresh());
    CHECK(client.refresh());
    CHECK(client.refresh());
    CHECK(client.isBusy());
    finishEmptyListing(factory, dispatcher);
    CHECK_EQ(listings, 1);
    // The follow-up is out, and the phase still says a newer answer is coming.
    REQUIRE(client.isBusy());
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Asking);
    finishEmptyListing(factory, dispatcher);
    CHECK_EQ(listings, 2);
    CHECK(!client.isBusy());
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Listed);
}

// A terminate asked while a listing is out is queued behind it and runs, rather
// than being refused for arriving second.
TEST(directoryRunsATerminateQueuedBehindARefresh)
{
    Dispatcher dispatcher;
    Factory factory;
    factory.heldSessionIDs = { 71 };
    Runtime::HostDirectoryClient client(dispatcher, factory);
    CHECK(client.refresh());
    CHECK(client.terminate(71));
    finishEmptyListing(factory, dispatcher);
    REQUIRE(client.isBusy());
    // The queued errand must reach its exact Call and correlated result;
    // a closed carrier alone is never evidence of termination.
    WelcomedHost welcomed;
    sendWelcome(factory, dispatcher, welcomed);
    auto result = Codec::JSON::Value::object();
    result.set("session", Codec::JSON::Value::number(71));
    result.set("outcome", Codec::JSON::Value::string("terminated"));
    answerCall(factory, dispatcher, welcomed, "session.terminate", std::move(result), 71);
    const auto snapshot = client.snapshot();
    CHECK(snapshot.event == Runtime::HostDirectoryClient::Event::TerminateSettled);
    CHECK_EQ(snapshot.lastTerminateSessionID, 71u);
    CHECK(snapshot.terminateOutcome == Session::TerminateOutcome::Terminated);
    CHECK(!client.isBusy());
}

TEST(directoryReportsTerminateFailureSeparatelyFromListing)
{
    Dispatcher dispatcher;
    Factory factory;
    factory.shouldFail = true;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    int changes = 0;
    client.setChangeCallback([&changes] { ++changes; });
    CHECK(client.terminate(71));
    auto snapshot = client.snapshot();
    CHECK(snapshot.event == Runtime::HostDirectoryClient::Event::TerminateSettled);
    CHECK(snapshot.terminateOutcome == Session::TerminateOutcome::Failed);
    CHECK_EQ(snapshot.lastTerminateSessionID, 71u);
    CHECK_EQ(changes, 1);
    CHECK(!client.isBusy());
}

TEST(directoryMoveAndDestructionKeepPostedWorkSafe)
{
    Dispatcher dispatcher;
    Factory factory;
    int changes = 0;
    {
        Runtime::HostDirectoryClient original(dispatcher, factory);
        original.setChangeCallback([&changes] { ++changes; });
        CHECK(original.refresh());
        Runtime::HostDirectoryClient moved(std::move(original));
        CHECK(moved.isBusy());
    }
    dispatcher.drain();
    CHECK_EQ(changes, 0);
}

TEST(directoryOwnsCallbackCapturesThroughReentrantReplacement)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    auto captured = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = captured;
    bool wasCalled = false;
    client.setChangeCallback([captured, &client, &lifetime, &wasCalled] {
        client.setChangeCallback({});
        CHECK(!lifetime.expired());
        CHECK_EQ(*captured, 42);
        wasCalled = true;
    });
    captured.reset();
    CHECK(!lifetime.expired());
    CHECK(client.refresh());
    finishEmptyListing(factory, dispatcher);
    CHECK(wasCalled);
    CHECK(lifetime.expired());
}

TEST(managementRejectsAnUnpinnedMutationAndAReplacedDaemon)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    Protocol::Sync::ManagementRequest request;
    request.operation = Protocol::Sync::ManagementOperation::DisconnectClient;
    request.clientID = 42;
    CHECK(!client.manage(request));
    request.daemonInstanceID = 17;
    REQUIRE(client.manage(request));
    WelcomedHost welcomed;
    sendWelcome(factory, dispatcher, welcomed, 18);
    REQUIRE(welcomed.connection);
    CHECK_EQ(sentKindCount(factory, Protocol::Sync::FrameKind::Greet), 1u);
    CHECK_EQ(sentKindCount(factory, Protocol::Sync::FrameKind::Call), 0u);
    CHECK(!client.isBusy());
    CHECK(client.snapshot().event == Runtime::HostDirectoryClient::Event::Failed);
    CHECK(client.snapshot().error.find("restarted") != std::string::npos);
}

TEST(managementCorrelatesRepliesAndRetainsSnapshotAfterTimeout)
{
    ListedHosts hosts;
    Protocol::Sync::ManagementRequest request;
    hosts.listing.answer = R"({"jsonrpc":"2.0","id":$ID,"result":{"method":"host.management.list","daemonInstance":"99","status":"success","detail":"","clients":[{"client":42,"session":0,"attachedUnix":"0","principal":"","label":"","transport":"","identity":""}],"grants":[],"tokens":[]}})";
    REQUIRE(hosts.client.manage(request));
    hosts.dispatcher.drain();
    CHECK(!hosts.client.isBusy());
    REQUIRE(hosts.listing.calls.size() == 1);
    CHECK(hosts.listing.calls.front().find(R"("method":"host.management.list")") != std::string::npos);
    CHECK(hosts.listing.calls.front().find(R"("daemonInstance":"99")") != std::string::npos);
    CHECK_EQ(std::ranges::count(hosts.listing.received, Protocol::Sync::FrameKind::Greet), 1);
    CHECK_EQ(std::ranges::count(hosts.listing.received, Protocol::Sync::FrameKind::Call), 1);
    CHECK(!hosts.listing.didSubscribe);
    REQUIRE(hosts.client.snapshot().management.clients.size() == 1);
    CHECK_EQ(hosts.client.snapshot().management.clients.front().clientID, 42u);
    // The next errand is welcomed, but its only Result names another call.
    hosts.listing.answer = R"({"jsonrpc":"2.0","id":"other","result":{}})";
    REQUIRE(hosts.client.manage(request));
    hosts.dispatcher.drain();
    CHECK(hosts.client.isBusy());
    hosts.dispatcher.advance(Session::directoryAnswerSeconds + 1);
    CHECK(!hosts.client.isBusy());
    CHECK(hosts.client.snapshot().event == Runtime::HostDirectoryClient::Event::Failed);
    CHECK(hosts.client.snapshot().error.find("didn’t answer the request in time") != std::string::npos);
    CHECK_EQ(hosts.client.snapshot().management.clients.front().clientID, 42u);
}

TEST(aPickedDaemonVersionRefusalStaysAListingFailure)
{
    for (const bool hasVersion : { false, true }) {
        Dispatcher dispatcher;
        Factory factory;
        Runtime::HostDirectoryClient client(dispatcher, factory);
        Session::HostInstanceInfo picked { "host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 1 };
        if (hasVersion)
            picked.appVersion = "0.2.0 (example)";
        client.setExpectedDaemon(picked);
        REQUIRE(client.refresh());
        sendRefusal(factory, dispatcher, { .reason = Protocol::Sync::ByeReason::VersionMismatch });
        const auto result = client.snapshot();
        CHECK(result.phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
        CHECK(result.failure == Session::ListingFailure::InstanceUnreadable);
        CHECK(result.error.find("Its sessions keep running") != std::string::npos);
        CHECK((result.error.find("0.2.0 (example)") != std::string::npos) == hasVersion);
        CHECK(!Session::isPickedDaemonGone(picked.socketName, "", result.daemonSocketName));
        CHECK(result.tree == nullptr);
    }
}

TEST(aPickedDaemonCannotBeReplacedByAnotherDaemonsAnswer)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    client.setExpectedDaemon({ "host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 1 });
    REQUIRE(client.refresh());
    WelcomedHost host;
    sendWelcome(factory, dispatcher, host);
    CHECK(client.snapshot().phase == Runtime::HostDirectoryClient::ListingPhase::Failed);
    CHECK(client.snapshot().failure == Session::ListingFailure::InstanceUnreadable);
    CHECK(client.snapshot().tree == nullptr);
    CHECK(client.snapshot().daemonSocketName.empty());
    CHECK(client.snapshot().error.find("A different host answered") != std::string::npos);
}

TEST(aPickedDaemonsWireVersionTakesPrecedenceOverDiscovery)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    client.setExpectedDaemon({ "host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 1, "0.2.0 (discovered)" });
    REQUIRE(client.refresh());
    sendRefusal(factory, dispatcher, { .reason = Protocol::Sync::ByeReason::VersionMismatch, .peer = Protocol::Sync::PeerMetadata { .build = { .isKnown = true, .wireVersion = Protocol::Sync::wireVersion, .appVersion = "0.2.1" }, .platform = "Linux x86_64" } });
    const auto result = client.snapshot();
    CHECK(result.failure == Session::ListingFailure::InstanceUnreadable);
    CHECK_STR_EQ(result.hostBuild.appVersion, "0.2.1");
    CHECK(result.error.find("(0.2.1)") != std::string::npos);
    CHECK(result.error.find("discovered") == std::string::npos);
}

TEST(aPickedDaemonsUnreadableRefusalUsesItsDiscoveredVersion)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    client.setExpectedDaemon({ "host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 1, "0.2.0 (discovered)" });
    REQUIRE(client.refresh());
    sendRefusal(factory, dispatcher, { .reason = Protocol::Sync::ByeReason::VersionMismatch, .peer = Protocol::Sync::PeerMetadata { .build = { .isKnown = true, .wireVersion = 0, .appVersion = "unreadable" }, .platform = "" } });
    const auto result = client.snapshot();
    CHECK(result.failure == Session::ListingFailure::InstanceUnreadable);
    CHECK(result.error.find("0.2.0 (discovered)") != std::string::npos);
    CHECK(result.error.find("Its sessions keep running") != std::string::npos);
}

TEST(aPickedDaemonsAuthRefusalAndGoneProbeKeepTheirMeaning)
{
    Dispatcher dispatcher;
    Factory factory;
    Runtime::HostDirectoryClient client(dispatcher, factory);
    client.setExpectedDaemon({ "host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 1 });
    REQUIRE(client.refresh());
    sendRefusal(factory, dispatcher, { .reason = Protocol::Sync::ByeReason::TokenExpired });
    CHECK(client.snapshot().failure == Session::ListingFailure::Refused);
    CHECK(client.snapshot().deniedReason == Protocol::Sync::ByeReason::TokenExpired);
    REQUIRE(client.refresh());
    sendBytes(factory, dispatcher, "\nUBIQUITTY-INSTANCE-GONE host-0123456789abcdef.sock\n");
    CHECK(client.snapshot().failure == Session::ListingFailure::InstanceGone);
}

UBIQUITTY_TEST_MAIN()
