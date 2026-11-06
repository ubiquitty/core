// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/EffectDelivery.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Terminal.h"
#include "Harness.h"
#include "LoopbackPaneLink.h"
#include "ScreenPlaneFixture.h"
#include "TerminalFixture.h"

#include <array>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr std::uint64_t titleDaemonInstanceID = 17;

std::vector<std::uint8_t> acknowledgedTitle(SyncClientCore& core, std::uint32_t sessionID, std::string_view title)
{
    HandshakeRequest request;
    request.sessionID = sessionID;
    request.rows = 24;
    request.columns = 80;
    std::vector<std::uint8_t> greeting;
    core.buildHello(request, greeting);
    UbiquittyTest::HostScreen host(sessionID, titleDaemonInstanceID);
    host.plane().receive(greeting);
    Ubiquitty::Protocol::Sync::PaneFacts facts;
    facts.programTitle = title;
    host.plane().setFacts(facts);
    std::vector<std::uint8_t> wire;
    host.plane().publish(wire);
    return wire;
}

} // namespace

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;

template<typename Message>
void appendScreenMessage(std::vector<std::uint8_t>& wire, Sync::FrameKind kind, std::uint32_t channel, const Message& message)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, message);
    REQUIRE(Sync::appendFrame(wire, kind, channel, body));
}

void appendPaneTitle(std::vector<std::uint8_t>& wire, std::uint32_t handle, std::string title)
{
    Sync::PaneFacts facts;
    facts.revision = 2;
    facts.programTitle = std::move(title);
    std::vector<std::uint8_t> body;
    Sync::encodePaneFacts(body, facts);
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::PaneFacts, handle, body));
}

struct HandoverObserver final : SyncClientObserver {
    unsigned acknowledgements = 0;
    unsigned titles = 0;
    unsigned refusals = 0;
    std::function<void()> onAcknowledgement;

    void onHelloAck(std::uint32_t, const std::string& socketName) override
    {
        const std::string originalSocket = socketName;
        ++acknowledgements;
        if (onAcknowledgement)
            onAcknowledgement();
        CHECK(socketName == originalSocket);
    }
    void onPaneFacts(std::uint32_t, const Sync::PaneFacts& facts) override { titles += !facts.programTitle.empty(); }
    void onRefused(Sync::ByeReason) override { ++refusals; }
};

struct HandoverClient {
    UbiquittyTest::HostScreen host { 7, 17 };
    Ubiquitty::Terminal terminal;
    HandoverObserver observer;
    SyncClientCore core { &terminal, &observer };
    Sync::Welcome successor;

    explicit HandoverClient(std::optional<Sync::ID128> predecessor = std::nullopt, std::uint64_t nextDaemon = 22)
        : host(7, 17, 24, 80, 0, Sync::Role::Owner, predecessor)
    {
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 24, 80);
        UbiquittyTest::feed(host.terminal(), "kept picture");
        HandshakeRequest request;
        request.sessionID = 7;
        request.rows = 24;
        request.columns = 80;
        std::vector<std::uint8_t> greeting;
        core.buildHello(request, greeting);
        host.plane().receive(greeting);
        std::vector<std::uint8_t> wire;
        host.plane().publish(wire);
        REQUIRE(core.feed(std::as_bytes(std::span { wire })));
        REQUIRE(core.hasScreen());
        REQUIRE(core.screen()->welcome());
        successor = *core.screen()->welcome();
        successor.hostInstanceID = { 22, 0 };
        successor.predecessorHostInstanceID = core.screen()->welcome()->hostInstanceID;
        successor.attachmentID = { 2, 22 };
        successor.resumeSecret = { 3, 22 };
        successor.selection.daemonInstanceID = nextDaemon;
        successor.selection.socketName = "successor.sock";
        successor.resumed = { { .paneID = { 7, 17 }, .outcome = Sync::ResumeOutcome::Diffed, .inputWritten = 0 } };
        core.noteTransportLost();
        request.expectedDaemonInstanceID = 17;
        greeting.clear();
        core.buildHello(request, greeting);
        // The new request is not an admitted identity until Welcome.
        CHECK_EQ(observer.acknowledgements, 1u);
    }

    std::vector<std::uint8_t> welcomeAndBind() const
    {
        std::vector<std::uint8_t> wire;
        appendScreenMessage(wire, Sync::FrameKind::Welcome, 0, successor);
        appendScreenMessage(wire, Sync::FrameKind::PaneBind, 0, Sync::PaneBind { .handle = 11, .paneID = { 7, 17 } });
        return wire;
    }
};

} // namespace

TEST(aRealHostsFullIdentityIsIndependentOfItsCarrierDaemonID)
{
    const Sync::ID128 predecessor { 0x1975, 0x987654321 };
    HandoverClient fixture(predecessor);
    REQUIRE(fixture.core.attachment()->held());
    CHECK(fixture.core.attachment()->held()->hostInstanceID == predecessor);
    fixture.successor.hostInstanceID = { 0x2986, 0xabcdef123 };
    const auto wire = fixture.welcomeAndBind();
    REQUIRE(fixture.core.feed(std::as_bytes(std::span { wire })));
    CHECK(fixture.core.isSuccessorOf(17));
    CHECK(!fixture.core.isSuccessorOf(predecessor.low));
    CHECK_EQ(fixture.core.daemonInstanceID(), 22u);
    CHECK(fixture.core.isPaneBound(7));
    CHECK(fixture.core.attachment()->paneTerminal({ 7, 17 }) == &fixture.terminal);
}

TEST(aChangedFullHostCannotReuseThePredecessorsCarrierIdentity)
{
    HandoverClient fixture(Sync::ID128 { 0x1975, 0x987654321 }, 17);
    const auto wire = fixture.welcomeAndBind();
    CHECK(!fixture.core.feed(std::as_bytes(std::span { wire })));
    CHECK_EQ(fixture.observer.acknowledgements, 1u);
    CHECK_EQ(fixture.observer.refusals, 1u);
    CHECK(!fixture.core.isSuccessorOf(17));
}

TEST(aProvisionalCarrierCannotReplaceTheAdmittedPredecessor)
{
    HandoverClient fixture(Sync::ID128 { 0x1975, 0x987654321 });
    HandshakeRequest request;
    request.sessionID = 7;
    request.expectedDaemonInstanceID = 17;
    std::vector<std::uint8_t> wire;
    fixture.core.buildHello(request, wire);
    wire.clear();
    CHECK_EQ(fixture.observer.acknowledgements, 1u);
    fixture.successor.hostInstanceID = { 0x2986, 0xabcdef123 };
    const auto welcome = fixture.welcomeAndBind();
    wire.insert(wire.end(), welcome.begin(), welcome.end());
    REQUIRE(fixture.core.feed(std::as_bytes(std::span { wire })));
    CHECK(fixture.core.isSuccessorOf(17));
    CHECK(!fixture.core.isSuccessorOf(22));
    CHECK_EQ(fixture.observer.acknowledgements, 2u);
}

TEST(aColdRecordUsesItsExplicitCarrierPinAndIndependentFullHost)
{
    for (const bool isSuccessor : { false, true }) {
        const Sync::ID128 previous { 0x1975, 0x987654321 };
        const Sync::ID128 client { 1234, 5678 };
        auto store = std::make_shared<Sync::MemoryResumeRecordStore>();
        Sync::ResumeRecord record;
        record.hostInstanceID = previous;
        record.clientInstance = client;
        record.attachmentID = { 3, 4 };
        record.resumeSecret = { 5, 6 };
        record.panes.push_back({ .paneID = { 7, 17 } });
        REQUIRE(store->write(record) == Sync::ResumeWriteOutcome::Committed);
        HandoverObserver observer;
        SyncClientCore core(nullptr, nullptr);
        REQUIRE(core.prepareResumeStore(store, client, {}));
        REQUIRE(core.attachPreparedObserver(observer));
        HandshakeRequest request;
        request.sessionID = 7;
        request.expectedDaemonInstanceID = 17;
        std::vector<std::uint8_t> wire;
        core.buildHello(request, wire);
        CHECK_EQ(observer.acknowledgements, 0u);
        Sync::Welcome welcome;
        welcome.wireVersion = Sync::wireVersion;
        welcome.hostInstanceID = isSuccessor ? Sync::ID128 { 0x2986, 0xabcdef123 } : previous;
        welcome.attachmentID = { 7, 8 };
        welcome.resumeSecret = { 9, 10 };
        welcome.selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 7, .workspaceID = 0, .daemonInstanceID = isSuccessor ? 22u : 17u, .socketName = "cold-resume.sock", .buildFlags = 0, .hostPlatform = "fixture" };
        if (isSuccessor)
            welcome.predecessorHostInstanceID = previous;
        welcome.resumed = { { .paneID = { 7, 17 }, .outcome = Sync::ResumeOutcome::Checkpointed, .inputWritten = 0 } };
        wire.clear();
        appendScreenMessage(wire, Sync::FrameKind::Welcome, 0, welcome);
        REQUIRE(core.feed(std::as_bytes(std::span { wire })));
        CHECK_EQ(observer.acknowledgements, 1u);
        CHECK(core.isSuccessorOf(17) == isSuccessor);
        REQUIRE(store->read());
        CHECK(store->read()->hostInstanceID == welcome.hostInstanceID);
    }
}

TEST(aSuccessorIsAcknowledgedOnlyAfterItsExactPredecessorAndKeepsThePaneIssuer)
{
    HandoverClient fixture;
    const auto wire = fixture.welcomeAndBind();
    REQUIRE(fixture.core.feed(std::as_bytes(std::span { wire })));
    CHECK_EQ(fixture.observer.acknowledgements, 2u);
    CHECK(fixture.core.isSuccessorOf(17));
    CHECK(!fixture.core.isSuccessorOf(18));
    CHECK(fixture.core.isPaneBound(7));
    CHECK_EQ(fixture.core.paneHandle(7), 11u);
    CHECK(fixture.core.hasScreen());
    CHECK(fixture.core.attachment()->paneTerminal({ 7, 17 }) == &fixture.terminal);
    CHECK(UbiquittyTest::firstDisagreement(fixture.host.terminal(), fixture.terminal).empty());
    CHECK(fixture.core.sendIntent(7, Sync::RawInput { .bytes = { 'x' } }).has_value());
    CHECK_EQ(fixture.core.attachment()->pendingCount({ 7, 17 }), 1u);
    CHECK_EQ(fixture.core.attachment()->pendingCount({ 7, 22 }), 0u);
    CHECK(fixture.core.setPaneViewport(7, { .rows = 24, .columns = 80 }));
}

TEST(aSuccessorCannotPublishPaneFactsBeforeItsWelcome)
{
    HandoverClient fixture;
    std::vector<std::uint8_t> premature;
    appendPaneTitle(premature, 11, "not admitted");
    CHECK(!fixture.core.feed(std::as_bytes(std::span { premature })));
    CHECK_EQ(fixture.observer.titles, 0u);
    CHECK_EQ(fixture.observer.acknowledgements, 1u);
    CHECK(!fixture.core.isSuccessorOf(17));
}

TEST(aSuccessorCannotReplaceThePictureWithoutMatchingBothCarrierAndPredecessor)
{
    for (unsigned fault = 0; fault < 3; ++fault) {
        HandoverClient fixture;
        if (fault == 0)
            fixture.successor.predecessorHostInstanceID.reset();
        else if (fault == 1)
            fixture.successor.predecessorHostInstanceID = Sync::ID128 { 18, 0 };
        else
            fixture.successor.hostInstanceID = { 17, 0 };
        auto wire = fixture.welcomeAndBind();
        appendPaneTitle(wire, 11, "must not escape");
        CHECK(!fixture.core.feed(std::as_bytes(std::span { wire })));
        CHECK_EQ(fixture.observer.acknowledgements, 1u);
        CHECK_EQ(fixture.observer.refusals, 1u);
        CHECK_EQ(fixture.observer.titles, 0u);
        CHECK(!fixture.core.isSuccessorOf(17));
        CHECK(!fixture.core.isPaneBound(7));
        CHECK(fixture.core.attachment()->isEnded({ 7, 17 }));
        CHECK(!fixture.core.attachment()->held());
        CHECK(!fixture.core.sendIntent(7, Sync::RawInput { .bytes = { 'x' } }).has_value());
        CHECK(UbiquittyTest::firstDisagreement(fixture.host.terminal(), fixture.terminal).empty());
    }
}

TEST(aSuccessorAcknowledgementCanRetireTheConnectionBeforeItsCoalescedPaneBind)
{
    HandoverClient fixture;
    fixture.observer.onAcknowledgement = [&] {
        HandshakeRequest replacement;
        replacement.agentLabel = "replacement";
        std::vector<std::uint8_t> greeting;
        fixture.core.buildHello(replacement, greeting);
    };
    auto wire = fixture.welcomeAndBind();
    appendPaneTitle(wire, 11, "stale successor suffix");
    REQUIRE(fixture.core.feed(std::as_bytes(std::span { wire })));
    CHECK_EQ(fixture.observer.acknowledgements, 2u);
    CHECK_EQ(fixture.observer.titles, 0u);
    CHECK(!fixture.core.sawHelloAck());
    CHECK(!fixture.core.isPaneBound(7));
    CHECK(fixture.core.attachment()->kind() == Sync::ClientKind::Agent);
    CHECK(UbiquittyTest::firstDisagreement(fixture.host.terminal(), fixture.terminal).empty());
}

TEST(aNestedSameKindReconnectUsesTheSuccessorsRotatedIdentity)
{
    HandoverClient fixture;
    bool shouldReconnect = true;
    fixture.observer.onAcknowledgement = [&] {
        if (!std::exchange(shouldReconnect, false))
            return;
        HandshakeRequest request;
        request.sessionID = 7;
        request.expectedDaemonInstanceID = 22;
        std::vector<std::uint8_t> greeting;
        fixture.core.buildHello(request, greeting);
        const auto held = fixture.core.attachment()->held();
        REQUIRE(held);
        CHECK(held->hostInstanceID == (Sync::ID128 { 22, 0 }));
        CHECK(held->resumeSecret == fixture.successor.resumeSecret);
        std::vector<std::uint8_t> response;
        auto resumed = fixture.successor;
        resumed.predecessorHostInstanceID.reset();
        resumed.selection.socketName = "replacement.sock";
        appendScreenMessage(response, Sync::FrameKind::Welcome, 0, resumed);
        appendScreenMessage(response, Sync::FrameKind::PaneBind, 0, Sync::PaneBind { .handle = 22, .paneID = { 7, 17 } });
        REQUIRE(fixture.core.feed(std::as_bytes(std::span { response })));
    };
    auto wire = fixture.welcomeAndBind();
    appendPaneTitle(wire, 11, "stale successor suffix");
    REQUIRE(fixture.core.feed(std::as_bytes(std::span { wire })));
    CHECK_EQ(fixture.observer.acknowledgements, 3u);
    CHECK_EQ(fixture.observer.titles, 0u);
    CHECK_EQ(fixture.core.paneHandle(7), 22u);
    CHECK(fixture.core.hasScreen());
    CHECK(fixture.core.attachment()->kind() == Sync::ClientKind::Human);
}

TEST(nestedGreetingsKeepBorrowedTitlesAliveAndDiscardBothOldSuffixes)
{
    struct Observer final : SyncClientObserver {
        SyncClientCore* core = nullptr;
        std::vector<std::string> titles;
        unsigned depth = 0;
        void onPaneFacts(std::uint32_t, const Sync::PaneFacts& facts) override
        {
            const std::string_view title = facts.programTitle;
            if (title.empty())
                return;
            const std::string saved(title);
            titles.emplace_back(title);
            if (++depth < 3) {
                auto next = acknowledgedTitle(*core, depth + 1, depth == 1 ? "second" : "third");
                if (depth == 1)
                    appendPaneTitle(next, 1, "stale nested suffix");
                CHECK(core->feed(std::as_bytes(std::span { next })));
            }
            // The canonical record still belongs to the old apply, not the
            // attachment the nested greeting just installed.
            CHECK(title == saved);
        }
    } observer;
    Ubiquitty::Terminal terminal;
    SyncClientCore core(&terminal, &observer);
    observer.core = &core;
    auto first = acknowledgedTitle(core, 1, "first");
    appendPaneTitle(first, 1, "stale outer suffix");
    CHECK(core.feed(std::as_bytes(std::span { first })));
    const std::array expectedTitles { "first", "second", "third" };
    CHECK_EQ(observer.titles.size(), expectedTitles.size());
    for (std::size_t index = 0; index < std::min(observer.titles.size(), expectedTitles.size()); ++index)
        CHECK_STR_EQ(observer.titles[index], expectedTitles[index]);
    CHECK_EQ(core.sessionID(), 3u);
    CHECK(core.sawHelloAck());
}

TEST(aDecodedFrameObserverCanReplaceTheConnectionBeforeDispatch)
{
    struct Observer final : SyncClientObserver {
        SyncClientCore* core = nullptr;
        bool shouldReconnect = true;
        std::vector<std::string> titles;
        void onFrameDecoded(std::uint8_t, std::size_t, std::size_t) override
        {
            if (!shouldReconnect)
                return;
            shouldReconnect = false;
            core->resetForReconnect();
            auto replacement = acknowledgedTitle(*core, 2, "replacement");
            CHECK(core->feed(std::as_bytes(std::span { replacement })));
        }
        void onPaneFacts(std::uint32_t, const Sync::PaneFacts& facts) override
        {
            if (!facts.programTitle.empty())
                titles.emplace_back(facts.programTitle);
        }
    } observer;
    Ubiquitty::Terminal terminal;
    SyncClientCore core(&terminal, &observer);
    observer.core = &core;
    auto obsolete = acknowledgedTitle(core, 1, "obsolete");
    CHECK(core.feed(std::as_bytes(std::span { obsolete })));
    CHECK_EQ(core.sessionID(), 2u);
    CHECK(observer.titles == std::vector<std::string> { "replacement" });
}

TEST(refusingFromAnObserverStopsTheRestOfTheCurrentRead)
{
    struct Observer final : SyncClientObserver {
        SyncClientCore* core = nullptr;
        unsigned titles = 0;
        void onPaneFacts(std::uint32_t, const Sync::PaneFacts& facts) override
        {
            if (facts.programTitle.empty())
                return;
            ++titles;
            core->refuseHost();
        }
    } observer;
    Ubiquitty::Terminal terminal;
    SyncClientCore core(&terminal, &observer);
    observer.core = &core;
    auto wire = acknowledgedTitle(core, 1, "first");
    appendPaneTitle(wire, 1, "must not dispatch");
    CHECK(!core.feed(std::as_bytes(std::span { wire })));
    CHECK_EQ(observer.titles, 1u);
    CHECK(!core.sawHelloAck());
    CHECK(!core.feed(std::as_bytes(std::span { wire })));
}

TEST(aScreenNotificationCanReplaceAttachmentKindTwiceWhileTheOuterReadUnwinds)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    struct Observer final : SyncClientObserver {
        SyncClientCore* core = nullptr;
        std::array<std::unique_ptr<UbiquittyTest::HostScreen>, 3> hosts;
        unsigned notifications = 0;
        unsigned staleTitles = 0;

        void greet(std::uint32_t sessionID)
        {
            HandshakeRequest hello;
            hello.sessionID = sessionID;
            hello.rows = 24;
            hello.columns = 80;
            if (sessionID == 2)
                hello.agentLabel = "callback-test";
            core->setPaneView(sessionID, Sync::ViewState::Live);
            std::vector<std::uint8_t> greeting;
            core->buildHello(hello, greeting);
            auto& host = hosts[sessionID - 1]->plane();
            host.receive(greeting);
            std::vector<std::uint8_t> reply;
            host.publish(reply);
            if (sessionID != 3)
                appendPaneTitle(reply, 1, "obsolete screen's suffix");
            CHECK(core->feed(std::as_bytes(std::span { reply })));
        }

        void onScreenChanged(std::uint32_t sessionID) override
        {
            ++notifications;
            if (sessionID == 3)
                return;
            const auto* connection = core->screen();
            const auto* attachment = core->attachment();
            const auto kind = attachment->kind();
            greet(sessionID + 1);
            CHECK(connection->state() == Sync::ConnectionState::Closed);
            CHECK(attachment->kind() == kind);
            CHECK(core->screen() != connection);
            CHECK(core->attachment() != attachment);
        }

        void onPaneFacts(std::uint32_t, const Sync::PaneFacts& facts) override { staleTitles += !facts.programTitle.empty(); }
    } observer;
    for (unsigned index = 0; index < observer.hosts.size(); ++index) {
        observer.hosts[index] = std::make_unique<UbiquittyTest::HostScreen>(index + 1, index + 1);
        UbiquittyTest::feed(observer.hosts[index]->terminal(), index == 0 ? "first" : index == 1 ? "second"
                                                                                                 : "third");
    }
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 24, 80);
    SyncClientCore core(&terminal, &observer);
    observer.core = &core;
    observer.greet(1);
    CHECK_EQ(observer.notifications, 3u);
    CHECK_EQ(observer.staleTitles, 0u);
    CHECK_EQ(core.sessionID(), 3u);
    REQUIRE(core.screen());
    CHECK(core.screen()->state() == Sync::ConnectionState::Attached);
    CHECK(core.attachment()->kind() == Sync::ClientKind::Human);
    REQUIRE(UbiquittyTest::cell(terminal, 0, 0));
    CHECK_EQ(UbiquittyTest::cell(terminal, 0, 0)->codePoint, U't');
}

TEST(aCommandObserverCanReplaceTheConnectionWithoutInvalidatingItsBorrowedContext)
{
    struct Observer final : SyncClientObserver {
        SyncClientCore* core = nullptr;
        unsigned commands = 0;
        std::vector<std::string> titles;

        void onCommandDone(const Sync::EffectView& effect, const Sync::CommandFinishedEffect& command, Sync::EffectDisposition disposition) override
        {
            ++commands;
            const std::string directory(command.currentDirectory);
            const std::string line(command.commandLine);
            const auto generation = effect.generation;
            CHECK(disposition == Sync::EffectDisposition::Post);
            auto replacement = acknowledgedTitle(*core, 2, "replacement");
            CHECK(core->feed(std::as_bytes(std::span { replacement })));
            CHECK(command.currentDirectory == directory);
            CHECK(command.commandLine == line);
            CHECK_EQ(command.exitStatus, 17);
            CHECK_EQ(command.durationMicroseconds, 42u);
            CHECK_EQ(effect.generation, generation);
            CHECK_EQ(effect.sequence, 1u);
            CHECK(effect.kind == Sync::EffectKind::CommandFinished);
        }

        void onPaneFacts(std::uint32_t, const Sync::PaneFacts& facts) override
        {
            if (!facts.programTitle.empty())
                titles.push_back(facts.programTitle);
        }
    } observer;
    Ubiquitty::Terminal terminal;
    SyncClientCore core(&terminal, &observer);
    observer.core = &core;
    const auto greeting = acknowledgedTitle(core, 1, "");
    REQUIRE(core.feed(std::as_bytes(std::span { greeting })));
    REQUIRE(core.screen());
    const auto generation = core.screen()->paneGeneration(core.paneHandle(1));
    const auto body = Sync::encodeEffectBody(Sync::CommandFinishedEffect { 17, 42, "/work/borrowed-command-context", "printf 'borrowed command context'" });
    REQUIRE(body);
    const auto record = Sync::encodeEffect({ .paneID = { 1, titleDaemonInstanceID }, .generation = generation, .sequence = 1, .primary = core.clientInstance(), .kind = Sync::EffectKind::CommandFinished, .payload = body->view() });
    REQUIRE(record);
    const auto packet = Sync::encodeEffectPacket({ 1, titleDaemonInstanceID }, generation, 1, 0, record->view());
    REQUIRE(packet);
    std::vector<std::uint8_t> wire;
    REQUIRE(Sync::appendFrame(wire, Sync::FrameKind::Effect, core.paneHandle(1), packet->view()));
    appendPaneTitle(wire, core.paneHandle(1), "obsolete command suffix");
    CHECK(core.feed(std::as_bytes(std::span { wire })));
    CHECK_EQ(observer.commands, 1u);
    CHECK_EQ(core.sessionID(), 2u);
    CHECK(observer.titles == std::vector<std::string> { "replacement" });
}

UBIQUITTY_TEST_MAIN()
