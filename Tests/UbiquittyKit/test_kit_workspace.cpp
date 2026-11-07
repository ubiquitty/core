// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "UbiquittyKit/UbiquittyKit.h"
#include "WorkspaceStructureFixture.h"
#include <deque>
#include <memory>
#include <vector>

using namespace Ubiquitty;
namespace Sync = Protocol::Sync;
namespace {
struct Queue final : Session::Dispatcher {
    std::deque<Session::PostedWork> pending;
    std::deque<Session::PostedWork> delayed;
    void post(Session::PostedWork work) override { pending.push_back(std::move(work)); }
    void postDelayed(double, Session::PostedWork work) override { delayed.push_back(std::move(work)); }
    void drain()
    {
        unsigned budget = 10000;
        while (!pending.empty()) {
            REQUIRE(budget--);
            auto work = std::move(pending.front());
            pending.pop_front();
            std::move(work)();
        }
    }
};
struct Wire {
    Transport::ChannelObserver* observer = nullptr;
    std::vector<std::vector<std::uint8_t>> sent;
    bool stopped = false;
};
class Carrier final : public Transport::Channel {
public:
    explicit Carrier(std::shared_ptr<Wire> wire)
        : m_wire(std::move(wire))
    {
    }
    bool start(Transport::ChannelObserver* observer) override
    {
        m_wire->observer = observer;
        observer->onConnect();
        return true;
    }
    void send(std::span<const std::uint8_t> bytes) override { m_wire->sent.emplace_back(bytes.begin(), bytes.end()); }
    void stop() override
    {
        m_wire->stopped = true;
        m_wire->observer = nullptr;
    }
    Transport::Origin origin() const override { return {}; }
    Transport::Health health() const override { return m_wire->stopped ? Transport::Health::Closed : Transport::Health::Connected; }
    std::string description() const override { return "workspace embedder test"; }

private:
    std::shared_ptr<Wire> m_wire;
};
struct Factory final : Session::ChannelFactory {
    std::vector<std::shared_ptr<Wire>> wires;
    void createChannel(Session::TransportMode, Done done) override
    {
        auto wire = std::make_shared<Wire>();
        wires.push_back(wire);
        done(std::make_unique<Carrier>(wire), false);
    }
};
struct Observer final : UbiquittyKit::Workspace::Observer {
    unsigned changes = 0;
    void onChanged() override { ++changes; }
};
struct Fixture {
    std::unique_ptr<Queue> dispatcher = std::make_unique<Queue>();
    std::unique_ptr<Factory> transport = std::make_unique<Factory>();
    Queue* queue = dispatcher.get();
    Factory* factory = transport.get();
    Observer observer;
    UbiquittyKit::Workspace workspace { std::move(dispatcher), std::move(transport), observer };
    std::unique_ptr<Terminal> first = std::make_unique<Terminal>();
    std::unique_ptr<Terminal> second = std::make_unique<Terminal>();
    UbiquittyTest::TypedInput firstInput;
    UbiquittyTest::TypedInput secondInput;
    std::unique_ptr<Sync::PaneSource> firstSource;
    std::unique_ptr<Sync::PaneSource> secondSource;
    Sync::HostConnection host { Sync::measuredHostPolicy(0), identity() };
    std::size_t consumed = 0;
    std::uint64_t now = 1000000;

    static Sync::HostIdentity identity()
    {
        Sync::HostIdentity result;
        result.hostInstanceID = { 17, 0 };
        result.attachmentID = { 1, 17 };
        result.mayDrive = true;
        result.verifiedKind = Sync::ClientKind::Human;
        result.selection = { .target = Sync::AdmissionTarget::Workspace, .sessionID = 11, .workspaceID = 91, .daemonInstanceID = 17, .socketName = "embedding.sock", .hostPlatform = "fixture" };
        return result;
    }
    void exchange()
    {
        const auto wire = factory->wires.front();
        for (; consumed < wire->sent.size(); ++consumed)
            host.receiveStream(wire->sent[consumed], now += 20000);
        host.service(now += 20000);
        std::vector<std::uint8_t> frame;
        while (host.popStreamFrame(frame, now)) {
            wire->observer->onData(frame);
            frame.clear();
        }
        queue->drain();
        workspace.synchronize();
    }
    Fixture()
    {
        resize(*first, 24, 80);
        resize(*second, 24, 80);
        processBytes(first.get(), "first");
        processBytes(second.get(), "second");
        workspace.start({}, 80, 24);
        queue->drain();
        REQUIRE(factory->wires.size() == 1u);
        exchange();
        const std::array panes { UbiquittyTest::StructurePane { 11, 11, 39, 24 }, UbiquittyTest::StructurePane { 22, 22, 40, 24 } };
        REQUIRE(UbiquittyTest::offerStructure(host, UbiquittyTest::rowWorkspace(91, "Embedder", panes, 17)));
        exchange();
        firstSource = std::make_unique<Sync::PaneSource>(*first, Sync::ID128 { 11, 17 });
        secondSource = std::make_unique<Sync::PaneSource>(*second, Sync::ID128 { 22, 17 });
        firstSource->setInputWriter(firstInput);
        secondSource->setInputWriter(secondInput);
        REQUIRE(host.bindPane(*firstSource));
        REQUIRE(host.bindPane(*secondSource));
        firstSource->publish(Sync::FrameClass::Settled, now);
        secondSource->publish(Sync::FrameClass::Settled, now);
        for (int turn = 0; turn < 4; ++turn)
            exchange();
    }
};
}

TEST(workspaceEmbeddingOwnsReplicasAndRoutesBinaryInput)
{
    Fixture fixture;
    REQUIRE(fixture.workspace.tree());
    CHECK_EQ(fixture.workspace.tree()->id, 91u);
    CHECK_EQ(fixture.workspace.tree()->windows.front().tabs.front().panes.size(), 2u);
    REQUIRE(fixture.workspace.terminal(11));
    REQUIRE(fixture.workspace.terminal(22));
    CHECK(fixture.workspace.setView(22, 40, 23, Modes::FocusState::Focused));
    CHECK(!fixture.workspace.setView(99, 40, 23, Modes::FocusState::Focused));
    CHECK(fixture.workspace.sendInput(22, std::string_view("a\0b", 3)));
    CHECK(!fixture.workspace.sendInput(99, "foreign"));
    for (int turn = 0; turn < 4; ++turn)
        fixture.exchange();
    CHECK(fixture.secondInput.bytes() == std::string("a\0b", 3));
    CHECK(fixture.firstInput.bytes().empty());
    CHECK(fixture.workspace.socketName() == "embedding.sock");
    CHECK(fixture.observer.changes > 0u);
    fixture.workspace.stop();
    fixture.workspace.stop();
    CHECK(!fixture.workspace.terminal(22));
    CHECK(!fixture.workspace.sendInput(22, "after stop"));
    for (const auto& wire : fixture.factory->wires)
        CHECK(wire->stopped);
}

TEST(workspaceEmbeddingDropsOnlyPanesTheHostRemoved)
{
    Fixture fixture;
    const std::array panes { UbiquittyTest::StructurePane { 22, 22, 80, 24 } };
    REQUIRE(UbiquittyTest::offerStructure(fixture.host, UbiquittyTest::rowWorkspace(91, "Embedder", panes, 17, 2)));
    fixture.exchange();
    CHECK(!fixture.workspace.terminal(11));
    CHECK(fixture.workspace.terminal(22));
    CHECK(!fixture.workspace.hasEnded());
}

TEST(hostFactoryValidatesBeforeSpawningAndPinsTheAcknowledgedDaemon)
{
    CHECK(!UbiquittyKit::makeHostChannelFactory({ .hostBinary = "" }));
    CHECK(!UbiquittyKit::makeHostChannelFactory({ .sshDestination = "-oProxyCommand=anything" }));
    CHECK(!UbiquittyKit::makeHostChannelFactory({ .socketName = "../other.sock" }));
    auto factory = UbiquittyKit::makeHostChannelFactory({ .hostBinary = "/host with 'quotes'" });
    REQUIRE(factory);
    CHECK((*factory)->pinDaemonSocket("first.sock"));
    CHECK((*factory)->pinDaemonSocket("first.sock"));
    CHECK(!(*factory)->pinDaemonSocket("second.sock"));
    CHECK(!(*factory)->pinDaemonSocket("../first.sock"));
}

UBIQUITTY_TEST_MAIN()
