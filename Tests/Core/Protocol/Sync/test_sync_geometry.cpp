// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_geometry — the size follows the hands (Docs/sync.md §6.10): the
// newest typing driver owns a pane's size, pointer motion and agents never
// take it, a pin freezes it, an agent's explicit resize applies once until
// the owner takes the size back, and the program is signalled at most once
// per coalesce window whoever moves the size.

#include "AttachmentLink.h"
#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Geometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

constexpr ID128 first { 1, 100 };
constexpr ID128 second { 2, 100 };
constexpr ID128 agent { 3, 100 };
constexpr std::uint64_t window = measuredGeometryPolicy().coalesceMicroseconds;

constexpr PaneGeometry small { .rows = 24, .columns = 80 };
constexpr PaneGeometry large { .rows = 30, .columns = 100 };

/// The sizes an arbiter applied, and when.
struct Applied {
    std::vector<std::uint64_t> times;
    std::vector<PaneGeometry> sizes;

    /// Takes what is due at `now`.
    void take(GeometryArbiter& arbiter, std::uint64_t now) { record(arbiter.due(now), now); }

    /// Takes what is due before an attachment's input is written.
    void takeBeforeInput(GeometryArbiter& arbiter, ID128 typist, std::uint64_t now) { record(arbiter.dueBeforeInput(typist, now), now); }

    /// Records a size applied.
    void record(std::optional<PaneGeometry> due, std::uint64_t now)
    {
        if (due) {
            times.push_back(now);
            sizes.push_back(*due);
        }
    }

    /// The shortest gap between two applications, or `nullopt` with fewer
    /// than two.
    [[nodiscard]] std::optional<std::uint64_t> shortestGap() const
    {
        std::optional<std::uint64_t> shortest;
        for (std::size_t index = 1; index < times.size(); ++index) {
            const std::uint64_t gap = times[index] - times[index - 1];
            shortest = shortest ? std::min(*shortest, gap) : gap;
        }
        return shortest;
    }
};

/// Sends one frame from a pane connection's client end to its host, past
/// the client's own refusals, as a client that does not keep them would.
///
/// - Parameters:
///   - connection: The connection.
///   - kind: The frame's kind.
///   - body: Its body.
///   - now: The time.
void sendRaw(UbiquittyTest::PaneConnection& connection, FrameKind kind, std::span<const std::uint8_t> body, std::uint64_t now)
{
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, kind, connection.handle, body));
    connection.host.receiveStream(frame, now);
}

/// The sentence of the `Bye` a host has queued.
///
/// - Parameter host: The host end, closed.
/// - Returns: The `Bye`'s detail, or empty when the host queued none.
std::string byeDetail(HostConnection& host)
{
    std::vector<std::uint8_t> stream;
    while (host.popStreamFrame(stream, 1'000'000)) {
    }
    FrameDecoder decoder;
    decoder.append(stream);
    while (const std::optional<Frame> frame = decoder.next()) {
        if (frame->kind != FrameKind::Bye)
            continue;
        if (const std::optional<Bye> bye = decodeBye(frame->body))
            return bye->detail;
    }
    return {};
}

/// A viewport's body.
///
/// - Parameter viewport: The viewport.
/// - Returns: Its encoding.
std::vector<std::uint8_t> bodyOf(const Viewport& viewport)
{
    std::vector<std::uint8_t> body;
    encode(body, viewport);
    return body;
}

constexpr Viewport largeViewport { .rows = 30, .columns = 100, .cellPixelWidth = 9, .cellPixelHeight = 18, .textAreaPixelWidth = 900, .textAreaPixelHeight = 540, .drawablePixelWidth = 920, .drawablePixelHeight = 560 };

} // namespace

// A driver's Viewport is its size of that pane: the pane's arbiter hears it
// as the hand's viewport, so it applies to a pane laid out beside others,
// where the window's size in the client's Facts is no word on the pane. The
// owner reads the pixels the client draws the pane in from the report.
TEST(aDriversViewportIsItsSizeOfThatPane)
{
    UbiquittyTest::OwnedPane pane;
    pane.source->geometry().setPlacement(PanePlacement::BesideOthers);
    UbiquittyTest::PaneConnection driver(*pane.source, { 31, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    CHECK_MSG(pane.source->geometry().due(250) != std::optional { large }, "premise: the window's size is not a laid-out pane's");
    REQUIRE(driver.client.sendViewport(driver.handle, largeViewport));
    driver.pump(300);
    CHECK(driver.host.state() == ConnectionState::Attached);
    CHECK(pane.source->geometry().due(300 + window) == std::optional { large });
    const std::vector<ViewportReport> reports = driver.host.takeViewportReports();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].handle == driver.handle);
    CHECK(reports[0].viewport == largeViewport);
    CHECK(driver.host.takeViewportReports().empty());
}

// A drag sends many: the owner is handed the newest of each pane, once.
TEST(theOwnerTakesTheNewestViewportOfEachPane)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection driver(*pane.source, { 32, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    Viewport dragged = largeViewport;
    for (std::uint16_t columns = 90; columns <= 100; ++columns) {
        dragged.columns = columns;
        REQUIRE(driver.client.sendViewport(driver.handle, dragged));
    }
    driver.pump(300);
    const std::vector<ViewportReport> reports = driver.host.takeViewportReports();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].viewport == largeViewport);
}

// An agent has no viewport of a pane. Its client sends none, and one that
// arrives all the same closes the connection with the reason, naming the
// call an agent sets a size by.
TEST(anAgentsViewportIsRefusedWithTheReason)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection agentConnection(*pane.source, { 33, 0 }, ClientKind::Agent, UbiquittyTest::DriveScope::Granted, ClientKind::Agent);
    CHECK_MSG(!agentConnection.client.sendViewport(agentConnection.handle, largeViewport), "an agent's client sent a viewport");
    sendRaw(agentConnection, FrameKind::Viewport, bodyOf(largeViewport), 300);
    CHECK(agentConnection.host.state() == ConnectionState::Closed);
    CHECK(agentConnection.host.byeReason() == std::optional { ByeReason::Malformed });
    const std::string detail = byeDetail(agentConnection.host);
    CHECK_MSG(detail.find("agent") != std::string::npos && detail.find("pane.overrideGeometry") != std::string::npos, detail.c_str());
    CHECK(!pane.source->geometry().status().isOverridden);
    CHECK(pane.source->geometry().due(300 + window) != std::optional { large });
}

// A person the host could not verify stands as the agent it verified.
TEST(aViewportFromAnUnverifiedPersonIsAnAgents)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection claimed(*pane.source, { 34, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted, ClientKind::Agent);
    CHECK_MSG(!claimed.client.sendViewport(claimed.handle, largeViewport), "the granted Agent role must override the client's Human claim");
    claimed.pump(250);
    CHECK(claimed.host.state() == ConnectionState::Attached);
    sendRaw(claimed, FrameKind::Viewport, bodyOf(largeViewport), 300);
    CHECK(claimed.host.state() == ConnectionState::Closed);
    CHECK(byeDetail(claimed.host).find("agent") != std::string::npos);
}

// A viewer, and a person whose authority may not drive, may not size a pane.
TEST(aViewportFromAClientThatMayNotDriveIsRefused)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection viewer(*pane.source, { 35, 0 }, ClientKind::Viewer, UbiquittyTest::DriveScope::Granted);
    CHECK_MSG(!viewer.client.sendViewport(viewer.handle, largeViewport), "a viewer's client sent a viewport");
    sendRaw(viewer, FrameKind::Viewport, bodyOf(largeViewport), 300);
    CHECK(viewer.host.state() == ConnectionState::Closed);
    CHECK(byeDetail(viewer.host).find("may not drive") != std::string::npos);

    UbiquittyTest::PaneConnection observer(*pane.source, { 36, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Withheld);
    sendRaw(observer, FrameKind::Viewport, bodyOf(largeViewport), 400);
    CHECK(observer.host.state() == ConnectionState::Closed);
    CHECK(byeDetail(observer.host).find("may not drive") != std::string::npos);
    CHECK(pane.source->geometry().due(400 + window) != std::optional { large });
}

// A size of no rows or no columns sizes nothing and closes nothing, and one
// past what a terminal holds is clamped to it, in the arbiter and in the
// report alike.
TEST(aViewportOfNoSizeIsDroppedAndAnAbsurdOneClamped)
{
    UbiquittyTest::OwnedPane pane;
    pane.source->geometry().setPlacement(PanePlacement::BesideOthers);
    UbiquittyTest::PaneConnection driver(*pane.source, { 37, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    (void)pane.source->geometry().due(250);
    Viewport none = largeViewport;
    none.rows = 0;
    CHECK_MSG(!driver.client.sendViewport(driver.handle, none), "the client sent a viewport of no rows");
    sendRaw(driver, FrameKind::Viewport, bodyOf(none), 300);
    none = largeViewport;
    none.columns = 0;
    sendRaw(driver, FrameKind::Viewport, bodyOf(none), 300);
    CHECK(driver.host.state() == ConnectionState::Attached);
    CHECK(driver.host.takeViewportReports().empty());
    CHECK(!pane.source->geometry().due(300 + window).has_value());

    Viewport absurd = largeViewport;
    absurd.rows = 60'000;
    absurd.columns = 60'000;
    sendRaw(driver, FrameKind::Viewport, bodyOf(absurd), 400);
    CHECK(driver.host.state() == ConnectionState::Attached);
    const PaneGeometry clamped { .rows = static_cast<std::uint16_t>(Ubiquitty::maxRows), .columns = static_cast<std::uint16_t>(Ubiquitty::maxColumns) };
    CHECK(pane.source->geometry().due(400 + 2 * window) == std::optional { clamped });
    const std::vector<ViewportReport> reports = driver.host.takeViewportReports();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].viewport.rows == clamped.rows && reports[0].viewport.columns == clamped.columns);
}

// A body that is not exactly one viewport, and a viewport for a handle the
// host never bound, each close the connection.
TEST(anUnreadableViewportAndOneForNoPaneClose)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection shortBody(*pane.source, { 38, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    std::vector<std::uint8_t> body = bodyOf(largeViewport);
    body.pop_back();
    sendRaw(shortBody, FrameKind::Viewport, body, 300);
    CHECK(shortBody.host.byeReason() == std::optional { ByeReason::Malformed });

    UbiquittyTest::PaneConnection longBody(*pane.source, { 39, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    body = bodyOf(largeViewport);
    body.push_back(0);
    sendRaw(longBody, FrameKind::Viewport, body, 300);
    CHECK(longBody.host.byeReason() == std::optional { ByeReason::Malformed });

    UbiquittyTest::PaneConnection stray(*pane.source, { 40, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Viewport, stray.handle + 7, bodyOf(largeViewport)));
    stray.host.receiveStream(frame, 300);
    CHECK(stray.host.byeReason() == std::optional { ByeReason::UnboundHandle });
}

// The codec carries every field, and the kind is a client's, on a pane's
// channel, on the interactive lane.
TEST(aViewportRoundTripsAndIsAClientsPaneFrame)
{
    const std::optional<Viewport> decoded = decodeViewport(bodyOf(largeViewport));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == largeViewport);
    // Rows, columns, then the cell, the text area and the drawable, each a
    // width and a height, little-endian: what a client in another language
    // writes from the document.
    const std::vector<std::uint8_t> expected { 30, 0, 100, 0, 9, 0, 18, 0, 0x84, 0x03, 0x1C, 0x02, 0x98, 0x03, 0x30, 0x02 };
    CHECK(bodyOf(largeViewport) == expected);
    CHECK(frameKindOf(73) == std::optional { FrameKind::Viewport });
    CHECK(directionOf(FrameKind::Viewport) == Direction::ClientToHost);
    CHECK(!isConnectionKind(FrameKind::Viewport));
    CHECK(laneOf(FrameKind::Viewport) == Lane::Interactive);
}

// Two drivers type in turn every 10 ms, each with a different window: the
// owner changes fifty times a second, and the program is signalled at most
// once per coalesce window, the newest owner's size last.
TEST(twoTypistsAlternatingEveryTenMillisecondsSignalAtMostOncePerWindow)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    std::uint64_t now = 1'000;
    arbiter.noteViewport(first, GeometryStanding::Driver, small, now);
    arbiter.noteViewport(second, GeometryStanding::Driver, large, now);
    Applied applied;
    const std::uint64_t span = 1'000'000;
    for (const std::uint64_t until = now + span; now < until; now += 1'000) {
        if (now % 10'000 == 0) {
            // Each keystroke is written, its typist's size first if due.
            const ID128 typist = (now / 10'000) % 2 == 0 ? first : second;
            arbiter.noteTyping(typist, GeometryStanding::Driver, now);
            applied.takeBeforeInput(arbiter, typist, now);
        }
        applied.take(arbiter, now);
    }
    CHECK(applied.times.size() >= 2);
    CHECK_MSG(applied.times.size() <= span / window + 1, (std::to_string(applied.times.size()) + " signals in a second").c_str());
    REQUIRE(applied.shortestGap().has_value());
    CHECK_MSG(*applied.shortestGap() >= window, (std::to_string(*applied.shortestGap()) + " us between two signals").c_str());
    // The typing stops; the last typist's size follows within a window.
    const ID128 lastTypist = *arbiter.status().owner;
    for (const std::uint64_t until = now + window; now <= until; now += 1'000)
        applied.take(arbiter, now);
    CHECK(arbiter.applied() == std::optional { lastTypist == first ? small : large });
}

// An owner's drag reports a size every 5 ms: the program is signalled at most
// once per window, and the trailing size is the one it ends at.
TEST(anOwnersDragCoalescesToOneSignalPerWindowWithTheTrailingValue)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    std::uint64_t now = 1'000;
    arbiter.noteViewport(first, GeometryStanding::Driver, small, now);
    arbiter.noteTyping(first, GeometryStanding::Driver, now);
    Applied applied;
    std::uint16_t columns = 80;
    const std::uint64_t dragEnd = now + 200'000;
    for (; now < dragEnd; now += 1'000) {
        if (now % 5'000 == 0)
            arbiter.noteViewport(first, GeometryStanding::Driver, { .rows = 24, .columns = ++columns }, now);
        applied.take(arbiter, now);
    }
    for (const std::uint64_t until = now + window; now <= until; now += 1'000)
        applied.take(arbiter, now);
    CHECK(applied.times.size() <= 200'000 / window + 1);
    REQUIRE(applied.shortestGap().has_value());
    CHECK(*applied.shortestGap() >= window);
    REQUIRE(!applied.sizes.empty());
    CHECK_EQ(applied.sizes.back().columns, columns);
    // A size held back inside a window is due at the window's end, when the
    // owner wakes for it.
    arbiter.noteViewport(first, GeometryStanding::Driver, { .rows = 24, .columns = 200 }, now);
    CHECK(!arbiter.due(now).has_value());
    CHECK(arbiter.nextDueMicroseconds() == std::optional { applied.times.back() + window });
    // The first change of the drag was not held back.
    CHECK(applied.times.front() <= 1'000 + 5'000);
}

// The owner resizes its window and types inside the window: its input was
// typed against the new size, which reaches the program first.
TEST(anOwnersInputCarriesItsOwnNewSize)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    arbiter.noteViewport(first, GeometryStanding::Driver, large, 2'000);
    CHECK(arbiter.due(2'000) == std::optional { large });
    constexpr PaneGeometry larger { .rows = 31, .columns = 101 };
    arbiter.noteViewport(first, GeometryStanding::Driver, larger, 10'000);
    CHECK(!arbiter.due(10'000).has_value());
    arbiter.noteTyping(first, GeometryStanding::Driver, 11'000);
    CHECK(arbiter.dueBeforeInput(first, 11'000) == std::optional { larger });
}

// A typist taking the size from another inside the window waits for the
// window like any change: typists in turn signal once per window.
TEST(aNewOwnersInputWaitsForTheWindow)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteViewport(second, GeometryStanding::Driver, large, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    arbiter.noteTyping(second, GeometryStanding::Driver, 2'000);
    CHECK(arbiter.dueBeforeInput(second, 2'000) == std::optional { large });
    arbiter.noteTyping(first, GeometryStanding::Driver, 12'000);
    CHECK(!arbiter.dueBeforeInput(first, 12'000).has_value());
    CHECK(arbiter.due(2'000 + window) == std::optional { small });
}

// Until anyone types, the newest viewport a driver reported applies, as a
// lone client's window resize does.
TEST(untilAnyoneTypesTheNewestViewportApplies)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, large, 1'000);
    CHECK(arbiter.due(1'000) == std::optional { large });
    CHECK(arbiter.status().owner == std::optional { first });
}

// A pane laid out beside others has its size from its place among them: the
// window of the client that attaches, and of the one that types, moves
// nothing, and the size the client shows the pane itself at still does.
TEST(aLaidOutPaneKeepsItsGridWhateverTheWindow)
{
    GeometryArbiter arbiter;
    arbiter.seed(large);
    arbiter.setPlacement(PanePlacement::BesideOthers);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, small, 1'000);
    CHECK(!arbiter.due(1'000).has_value());
    CHECK(!arbiter.nextDueMicroseconds().has_value());
    arbiter.noteTyping(first, GeometryStanding::Driver, 2'000);
    CHECK(!arbiter.dueBeforeInput(first, 2'000).has_value());
    CHECK(!arbiter.status().owner.has_value());
    constexpr PaneGeometry leaf { .rows = 12, .columns = 40 };
    arbiter.noteViewport(first, GeometryStanding::Driver, leaf, 3'000);
    CHECK(arbiter.due(3'000) == std::optional { leaf });
    CHECK(arbiter.status().owner == std::optional { first });
    // The window again, later than the pane's own size: still nothing.
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, small, 3'000 + window);
    CHECK(!arbiter.due(3'000 + 2 * window).has_value());
}

// A pane that is the whole window is sized by the window, and the typist's
// window wins.
TEST(aPaneThatIsTheWindowTakesTheTypistsWindow)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, small, 1'000);
    // A window not measured yet is no size.
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, { .rows = 0, .columns = 80 }, 1'500);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, { .rows = 24, .columns = 0 }, 1'500);
    CHECK(!arbiter.due(1'500).has_value());
    arbiter.noteWindowViewport(second, GeometryStanding::Driver, large, 2'000);
    CHECK(arbiter.due(2'000) == std::optional { large });
    arbiter.noteTyping(first, GeometryStanding::Driver, 3'000);
    CHECK(arbiter.status().owner == std::optional { first });
    CHECK(arbiter.due(3'000 + window) == std::optional { small });
    // The typist's window grows: its input carries the new size.
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 4'000 + window);
    CHECK(arbiter.dueBeforeInput(first, 4'000 + window) == std::optional { large });
}

// The newer of a hand's two sizes is the one it shows the pane at.
TEST(aHandsNewerSizeStandsBetweenItsWindowAndItsPane)
{
    constexpr PaneGeometry leaf { .rows = 12, .columns = 40 };
    constexpr PaneGeometry wider { .rows = 40, .columns = 132 };
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 1'000);
    arbiter.noteViewport(first, GeometryStanding::Driver, leaf, 2'000);
    CHECK(arbiter.due(2'000) == std::optional { leaf });
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, wider, 3'000 + window);
    CHECK(arbiter.due(3'000 + window) == std::optional { wider });
}

// A client's `Facts` carry its window's size with every other fact, so a
// focus change or a theme says the size again as it stood. Said again, it is
// not a newer report: the size the client has since given the pane itself
// stands, and a window gaining the focus resizes nothing.
TEST(aWindowsSizeSaidAgainIsNoNewerThanItWas)
{
    constexpr PaneGeometry leaf { .rows = 12, .columns = 40 };
    GeometryArbiter arbiter;
    arbiter.seed(small);
    // The window as it was when the client connected, then the pane as the
    // client came to show it.
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 1'000);
    arbiter.noteViewport(first, GeometryStanding::Driver, leaf, 2'000);
    CHECK(arbiter.due(2'000) == std::optional { leaf });
    // The focus moves, and moves back: the same window size, twice more.
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 3'000 + window);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 4'000 + window);
    CHECK(!arbiter.due(5'000 + window).has_value());
    CHECK(!arbiter.nextDueMicroseconds().has_value());
    CHECK(arbiter.applied() == std::optional { leaf });
    // With a typist too: its repeated window size takes nothing from its pane's.
    arbiter.noteTyping(first, GeometryStanding::Driver, 6'000 + window);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 7'000 + window);
    CHECK(!arbiter.dueBeforeInput(first, 8'000 + window).has_value());
    // And it does not take a pane back from an override it did not answer.
    arbiter.overrideSize(second, small, 9'000 + 2 * window);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 10'000 + 2 * window);
    CHECK(arbiter.status().isOverridden);
}

// A pane that stops sharing its window takes the window's size, kept from
// when it was reported; one that starts sharing keeps the size it has.
TEST(theWindowCountsAgainWhenThePaneIsAlone)
{
    GeometryArbiter arbiter;
    arbiter.seed(large);
    arbiter.setPlacement(PanePlacement::BesideOthers);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, small, 1'000);
    CHECK(!arbiter.due(1'000).has_value());
    arbiter.setPlacement(PanePlacement::WholeWindow);
    CHECK(arbiter.status().owner == std::optional { first });
    CHECK(arbiter.due(2'000) == std::optional { small });
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, large, 3'000 + window);
    arbiter.setPlacement(PanePlacement::BesideOthers);
    CHECK(!arbiter.due(3'000 + window).has_value());
    CHECK(arbiter.applied() == std::optional { small });
}

// An agent's explicit size on a laid-out pane holds whatever window a driver
// reports; on a pane that is the window, the driver's new window takes it back.
TEST(aWindowTakesNothingBackFromAnOverrideOnALaidOutPane)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.setPlacement(PanePlacement::BesideOthers);
    arbiter.overrideSize(agent, large, 1'000);
    CHECK(arbiter.due(1'000) == std::optional { large });
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, small, 2'000);
    CHECK(arbiter.status().isOverridden);
    arbiter.setPlacement(PanePlacement::WholeWindow);
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, small, 3'000);
    CHECK(arbiter.status().isOverridden);
    constexpr PaneGeometry other { .rows = 50, .columns = 132 };
    arbiter.noteWindowViewport(first, GeometryStanding::Driver, other, 4'000);
    CHECK(!arbiter.status().isOverridden);
}

// A provisional key's window moves with it to the attachment.
TEST(rekeyingKeepsTheWindowTheProvisionalKeyReported)
{
    constexpr ID128 provisional { 9, 0 };
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 500);
    arbiter.noteWindowViewport(provisional, GeometryStanding::Driver, large, 1'000);
    arbiter.rekey(provisional, first);
    CHECK(arbiter.status().owner == std::optional { first });
    CHECK(arbiter.due(1'000) == std::optional { large });
}

// One connection shows two panes of a workspace in one window: neither takes
// the window's size, on the bind or when the client reports its facts again,
// and a pane that is the window takes it.
TEST(twoPanesOfOneWindowNeverBothTakeItsSize)
{
    UbiquittyTest::OwnedPane left;
    Ubiquitty::Terminal rightTerminal;
    Ubiquitty::resize(rightTerminal, 40, 120);
    PaneSource right(rightTerminal, ID128 { 19, 10 });
    right.publish(FrameClass::Structural, 1);
    Ubiquitty::resize(left.terminal, 30, 100);
    left.source->geometry().seed(large);
    left.source->geometry().setPlacement(PanePlacement::BesideOthers);
    constexpr PaneGeometry tall { .rows = 40, .columns = 120 };
    right.geometry().seed(tall);
    right.geometry().setPlacement(PanePlacement::BesideOthers);
    UbiquittyTest::PaneConnection window(*left.source, { 31, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    (void)window.host.bindPane(right);
    window.pump(300);
    CHECK(!left.source->geometry().due(1'000'000).has_value());
    CHECK(!right.geometry().due(1'000'000).has_value());
    window.client.setFocus(Ubiquitty::Modes::FocusState::Unfocused);
    window.pump(1'000'100);
    window.pump(1'000'200);
    CHECK(!left.source->geometry().due(2'000'000).has_value());
    CHECK(!right.geometry().due(2'000'000).has_value());
    // Alone in the window, the pane is the window.
    left.source->geometry().setPlacement(PanePlacement::WholeWindow);
    CHECK(left.source->geometry().due(2'000'000) == std::optional { small });
    CHECK(!right.geometry().due(2'000'000).has_value());
}

// A typist owns the size: another driver's window resize does not take it.
TEST(aDriverWhoHasNotTypedDoesNotTakeTheSize)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 2'000);
    arbiter.noteViewport(second, GeometryStanding::Driver, large, 3'000);
    CHECK(!arbiter.due(3'000).has_value());
    CHECK(!arbiter.nextDueMicroseconds().has_value());
    CHECK(arbiter.status().owner == std::optional { first });
    arbiter.noteTyping(second, GeometryStanding::Driver, 4'000);
    CHECK(arbiter.status().owner == std::optional { second });
    CHECK(arbiter.due(4'000) == std::optional { large });
}

// A pin freezes the size whatever the owner does; unpinned, the owner's size
// applies again.
TEST(aPinnedPaneIgnoresTheOwnersResize)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    arbiter.pin(large);
    CHECK(arbiter.due(1'000) == std::optional { large });
    CHECK(arbiter.status().isPinned);
    // A pin the host made on nobody's behalf names nobody; one an attachment
    // asked for names it, under the key the attachment ends up with.
    CHECK(!arbiter.status().pinnedBy.has_value());
    arbiter.pin(large, second);
    CHECK(arbiter.status().isPinned && arbiter.status().pinnedBy == std::optional { second });
    constexpr ID128 rekeyed { 0x77, 0x78 };
    arbiter.rekey(second, rekeyed);
    CHECK(arbiter.status().pinnedBy == std::optional { rekeyed });
    arbiter.pin(large);
    CHECK(arbiter.status().isPinned && !arbiter.status().pinnedBy.has_value());
    arbiter.noteViewport(first, GeometryStanding::Driver, PaneGeometry { .rows = 40, .columns = 120 }, 100'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 100'000);
    CHECK(!arbiter.due(200'000).has_value());
    arbiter.unpin();
    constexpr PaneGeometry largest { .rows = 40, .columns = 120 };
    CHECK(arbiter.due(200'000) == std::optional { largest });
    CHECK(arbiter.status().owner == std::optional { first });
}

// An agent's typing and its window never take the size.
TEST(anAgentNeverOwnsGeometry)
{
    CHECK(geometryStanding(ClientKind::Agent, true) == GeometryStanding::Bystander);
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    arbiter.noteViewport(agent, geometryStanding(ClientKind::Agent, true), large, 2'000);
    arbiter.noteTyping(agent, geometryStanding(ClientKind::Agent, true), 3'000);
    CHECK(!arbiter.due(3'000).has_value());
    CHECK(arbiter.status().owner == std::optional { first });
}

// A viewer, or a human without the Drive scope, never takes the size.
TEST(aViewerCannotTakeGeometry)
{
    CHECK(geometryStanding(ClientKind::Viewer, true) == GeometryStanding::Bystander);
    CHECK(geometryStanding(ClientKind::Human, false) == GeometryStanding::Bystander);
    CHECK(geometryStanding(ClientKind::Human, true) == GeometryStanding::Driver);
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(second, geometryStanding(ClientKind::Viewer, true), large, 1'000);
    arbiter.noteTyping(second, geometryStanding(ClientKind::Viewer, true), 1'000);
    CHECK(!arbiter.due(1'000).has_value());
    CHECK(!arbiter.status().owner.has_value());
}

// An agent's explicit resize applies at once and names the agent; the owner
// typing takes the size back.
TEST(anOverrideAppliesOnceUntilTheOwnerTakesTheSizeBack)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    arbiter.overrideSize(agent, large, 100'000);
    CHECK(arbiter.due(100'000) == std::optional { large });
    CHECK(arbiter.status().owner == std::optional { agent });
    CHECK(arbiter.status().isOverridden);
    // The owner resending the size it had, as a focus change does, keeps the
    // override.
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 200'000);
    CHECK(!arbiter.due(200'000).has_value());
    CHECK(arbiter.status().isOverridden);
    arbiter.noteTyping(first, GeometryStanding::Driver, 300'000);
    CHECK(arbiter.status().owner == std::optional { first });
    CHECK(!arbiter.status().isOverridden);
    CHECK(arbiter.due(300'000) == std::optional { small });
}

// The newest typist's grid replaces an older driver's: the first driver owns
// by typing, the second takes the size by typing later, and the first's
// reported size does not take it back.
TEST(aNewerTypistsGridReplacesAnOlderDriversGrid)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    CHECK(arbiter.status().owner == std::optional { first });
    arbiter.noteViewport(second, GeometryStanding::Driver, large, 200'000);
    arbiter.noteTyping(second, GeometryStanding::Driver, 300'000);
    CHECK(arbiter.status().owner == std::optional { second });
    CHECK(arbiter.due(300'000) == std::optional { large });
    // The older driver's resize, with no typing, is no claim.
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 400'000);
    CHECK(!arbiter.due(400'000).has_value());
    CHECK(arbiter.status().owner == std::optional { second });
}

// A terminal.resize override is not a pin: the next typist, whichever driver
// it is, takes the size from the override.
TEST(anOverrideYieldsToTheNextTypistWhoeverItIs)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.noteViewport(second, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(first, GeometryStanding::Driver, 1'000);
    arbiter.overrideSize(agent, large, 100'000);
    CHECK(arbiter.due(100'000) == std::optional { large });
    CHECK(arbiter.status().isOverridden);
    CHECK(!arbiter.status().isPinned);
    arbiter.noteTyping(second, GeometryStanding::Driver, 300'000);
    CHECK(arbiter.status().owner == std::optional { second });
    CHECK(!arbiter.status().isOverridden);
    CHECK(arbiter.due(300'000) == std::optional { small });
}

// An override from an attachment already known as a driver leaves it a
// driver: once a driver takes the size back, that attachment can own it.
TEST(anOverrideKeepsTheAskersStanding)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    arbiter.noteViewport(first, GeometryStanding::Driver, small, 1'000);
    arbiter.overrideSize(first, large, 2'000);
    CHECK(arbiter.drivingOwner() == std::optional { first });
    arbiter.noteTyping(first, GeometryStanding::Driver, 3'000);
    CHECK(arbiter.status().owner == std::optional { first });
    CHECK(!arbiter.status().isOverridden);
}

// An agent resizing in a loop signals the program at most once per window.
TEST(anOverrideInALoopSignalsAtMostOncePerWindow)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    Applied applied;
    std::uint64_t now = 1'000;
    for (const std::uint64_t until = now + 500'000; now < until; now += 1'000) {
        if (now % 5'000 == 0)
            arbiter.overrideSize(agent, (now / 5'000) % 2 == 0 ? small : large, now);
        applied.take(arbiter, now);
    }
    CHECK(applied.times.size() <= 500'000 / window + 1);
    REQUIRE(applied.shortestGap().has_value());
    CHECK(*applied.shortestGap() >= window);
}

// A client's events heard under a provisional key before its attachment was
// known count as the attachment's.
TEST(rekeyingKeepsWhatTheProvisionalKeyHeard)
{
    GeometryArbiter arbiter;
    arbiter.seed(small);
    const ID128 provisional { 9, 0 };
    arbiter.noteViewport(provisional, GeometryStanding::Driver, large, 1'000);
    arbiter.noteTyping(provisional, GeometryStanding::Driver, 1'000);
    arbiter.noteViewport(second, GeometryStanding::Driver, small, 2'000);
    arbiter.rekey(provisional, first);
    CHECK(arbiter.status().owner == std::optional { first });
    CHECK(arbiter.due(2'000) == std::optional { large });
}

// The hands one pane tracks are bounded, and a new one displaces the one
// heard from longest ago, never the owner.
TEST(theHandsAPaneTracksAreBoundedAndTheOldestGoes)
{
    GeometryArbiter arbiter;
    for (std::uint64_t index = 1; index <= GeometryArbiter::maxHands; ++index)
        arbiter.noteViewport({ index, 7 }, GeometryStanding::Driver, small, index);
    const ID128 newest { GeometryArbiter::maxHands, 7 };
    arbiter.noteViewport(newest, GeometryStanding::Driver, large, 1'000);
    arbiter.noteTyping(newest, GeometryStanding::Driver, 1'000);
    for (std::uint64_t index = 1; index <= 10 * GeometryArbiter::maxHands; ++index)
        arbiter.noteViewport({ GeometryArbiter::maxHands + index, 7 }, GeometryStanding::Driver, small, 1'000 + index);
    CHECK(arbiter.status().owner == std::optional { newest });
}

// Two drivers on one pane: the second's pointer motion leaves the size with
// the first, and its typing takes it.
TEST(pointerMotionFromASecondDriverDoesNotTakeGeometryAndTypingDoes)
{
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection a(*pane.source, { 11, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    UbiquittyTest::PaneConnection b(*pane.source, { 12, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted);
    const ID128 aID { 11, 100 };
    const ID128 bID { 12, 100 };
    const std::array<std::uint8_t, 1> typed { 'a' };
    REQUIRE(a.client.sendRaw(a.handle, typed).has_value());
    a.pump(300);
    REQUIRE(pane.source->geometry().status().owner == std::optional { aID });
    const std::array<PointerEvent, 1> motion { PointerEvent { .kind = PointerEventKind::Motion, .button = 0, .modifiers = 0, .row = 3, .column = 4 } };
    REQUIRE(b.client.sendPointer(b.handle, motion).has_value());
    b.pump(400);
    CHECK(pane.source->geometry().status().owner == std::optional { aID });
    REQUIRE(b.client.sendRaw(b.handle, typed).has_value());
    b.pump(500);
    CHECK(pane.source->geometry().status().owner == std::optional { bID });
}

// A client declaring itself a person that the host could not verify as one
// stands as what the host verified: its typing never takes the size.
TEST(aSelfDeclaredHumanTheHostCannotVerifyNeverOwnsGeometry)
{
    CHECK(standingKind(ClientKind::Human, ClientKind::Agent) == ClientKind::Agent);
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection claimed(*pane.source, { 21, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted, ClientKind::Agent);
    const std::array<std::uint8_t, 1> typed { 'a' };
    REQUIRE(claimed.client.sendRaw(claimed.handle, typed).has_value());
    claimed.pump(300);
    CHECK_STR_EQ(pane.writer.bytes(), std::string("a"));
    CHECK(!pane.source->geometry().status().owner.has_value());
    CHECK(!pane.source->geometry().drivingOwner().has_value());
}

// A verified person may declare itself less: as an agent it stands as one,
// and its typing never takes the size.
TEST(aVerifiedHumanDeclaringItselfAnAgentStandsAsAnAgent)
{
    CHECK(standingKind(ClientKind::Agent, ClientKind::Human) == ClientKind::Agent);
    CHECK(standingKind(ClientKind::Viewer, ClientKind::Human) == ClientKind::Viewer);
    CHECK(standingKind(ClientKind::Human, ClientKind::Human) == ClientKind::Human);
    UbiquittyTest::OwnedPane pane;
    UbiquittyTest::PaneConnection modest(*pane.source, { 22, 0 }, ClientKind::Agent, UbiquittyTest::DriveScope::Granted, ClientKind::Human);
    const std::array<std::uint8_t, 1> typed { 'b' };
    REQUIRE(modest.client.sendRaw(modest.handle, typed).has_value());
    modest.pump(300);
    CHECK_STR_EQ(pane.writer.bytes(), std::string("b"));
    CHECK(!pane.source->geometry().drivingOwner().has_value());
    // The same verified person declaring itself one owns the size.
    UbiquittyTest::PaneConnection person(*pane.source, { 23, 0 }, ClientKind::Human, UbiquittyTest::DriveScope::Granted, ClientKind::Human);
    REQUIRE(person.client.sendRaw(person.handle, typed).has_value());
    person.pump(400);
    constexpr ID128 personID { 23, 100 };
    CHECK(pane.source->geometry().drivingOwner() == std::optional { personID });
}

// A resume on a new connection is the same attachment: the size it owned and
// the primary it was stay with it.
TEST(aSwapKeepsGeometryAndPrimaryOnTheAttachment)
{
    using namespace UbiquittyTest::AttachmentLink;
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    CHECK(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    const std::optional<ID128> owner = host.pane.source->geometry().status().owner;
    REQUIRE(owner.has_value());
    CHECK(host.table.primary(host.pane.source->geometry()) == owner);
    host.acceptKeepingTheOld();
    (void)client.connect(now);
    pumpFor(host, client, now, 100'000);
    REQUIRE(client.connection() && client.connection()->welcome());
    CHECK(client.connection()->welcome()->attachmentID == *owner);
    CHECK(host.pane.source->geometry().status().owner == owner);
    CHECK(host.table.primary(host.pane.source->geometry()) == owner);
}

// The owner's transport is lost: the primary falls to the human driver who
// typed most recently and is Live, and returns to the owner on its resume.
TEST(thePrimaryFallsToTheLiveTypingHumanWhileTheOwnerIsLost)
{
    UbiquittyTest::AttachmentLink::SequenceSecrets secrets;
    AttachmentTable table({ 41, 42 }, measuredResumePolicy(), secrets);
    const ConnectionAuthority authority { .principal = "alice", .mayDrive = true };
    const ID128 ownerID = table.create({ 1, 0 }, ClientKind::Human, authority, 1, 1).attachmentID;
    const ID128 otherID = table.create({ 2, 0 }, ClientKind::Human, authority, 2, 1).attachmentID;
    const ID128 agentID = table.create({ 3, 0 }, ClientKind::Agent, authority, 3, 1).attachmentID;
    GeometryArbiter arbiter;
    arbiter.noteViewport(otherID, GeometryStanding::Driver, small, 1'000);
    arbiter.noteTyping(otherID, GeometryStanding::Driver, 1'000);
    arbiter.noteTyping(agentID, GeometryStanding::Bystander, 2'000);
    arbiter.noteViewport(ownerID, GeometryStanding::Driver, large, 3'000);
    arbiter.noteTyping(ownerID, GeometryStanding::Driver, 3'000);
    CHECK(table.primary(arbiter) == std::optional { ownerID });
    table.noteTransportLost(ownerID, 1, 4'000);
    CHECK(table.primary(arbiter) == std::optional { otherID });
    table.noteTransportLost(otherID, 2, 5'000);
    CHECK(!table.primary(arbiter).has_value());
}

UBIQUITTY_TEST_MAIN()
