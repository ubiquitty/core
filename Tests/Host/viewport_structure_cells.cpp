// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/viewport_structure_cells.cpp — the size a driver's `Viewport`
// gives a pane is the size its workspace's structure reports (Docs/sync.md
// §6.10, §7.3).
//
// A workspace holds two panes side by side, and a session stands alone in a
// workspace of its own. One person drives the right-hand pane of the split;
// a second, attached to the left-hand one, reads the workspace's structure.
//
//   1. A pane laid out beside another takes nothing from its driver's
//      window: the window's size in the driver's `Facts` leaves the pane's
//      cells, and the structure's, where they were.
//   2. The driver's `Viewport` of the pane sizes it. The program is told,
//      and the second client's structure carries the pane's new cells and
//      the split's, under a later revision.
//   3. A pane that is all its tab shows takes its driver's window: the
//      `Facts` of a client attached to the lone session size it, and its
//      workspace's structure, read by that client, says so.
//
// Friend seam, the QUIC-off host build, as geometry_follows_hands. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using Ubiquitty::Host::Daemon;
using Ubiquitty::TestDoubles::StreamDouble;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Tree = Ubiquitty::Session;

namespace {

/// The one tab of a workspace's structure as a client holds it.
///
/// - Parameters:
///   - client: The client.
///   - workspaceID: The workspace.
/// - Returns: The tab and the structure's revision, or nothing when the
///   client holds no structure of that workspace or it does not decode to one
///   window of one tab.
std::optional<std::pair<Tree::TreeTab, std::uint64_t>> tabOf(const UbiquittyTest::ScreenClient& client, std::uint32_t workspaceID)
{
    const auto& held = client.connection().workspaceStructures();
    const auto found = held.find(workspaceID);
    if (found == held.end())
        return std::nullopt;
    const std::optional<Tree::TreeWorkspace> workspace = Tree::decodeWorkspaceStructure(found->second.record);
    if (!workspace || workspace->windows.size() != 1 || workspace->windows[0].tabs.size() != 1)
        return std::nullopt;
    return std::pair { workspace->windows[0].tabs[0], workspace->revision };
}

/// Describes a node's cells.
///
/// - Parameter node: The node.
/// - Returns: Its columns and rows.
std::string cellsOf(const Sync::TopologyNode& node)
{
    return std::to_string(node.columns) + "x" + std::to_string(node.rows);
}

/// Waits out a coalesce window: a pane's size changes at most once in one.
void passTheWindow()
{
    std::this_thread::sleep_for(std::chrono::microseconds(Sync::measuredGeometryPolicy().coalesceMicroseconds + 10'000));
}

} // namespace

int main()
{
    Daemon d;
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    auto rightStream = std::make_unique<StreamDouble>();
    StreamDouble& rightPTY = *rightStream;
    auto aloneStream = std::make_unique<StreamDouble>();
    StreamDouble& alonePTY = *aloneStream;
    auto& left = Screen::makeSession(d, 24, 80, std::make_unique<StreamDouble>());
    auto& right = Screen::makeSession(d, 30, 100, std::move(rightStream));
    auto& alone = Screen::makeSession(d, 24, 80, std::move(aloneStream));
    const std::uint32_t work = Screen::nameWorkspace(d, left, "work");
    Screen::splitInto(d, left, right, Ubiquitty::Panes::Orientation::Vertical);
    for (auto* session : { &left, &right, &alone })
        Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);

    // The reader, on the left-hand pane, and the driver, on the right-hand
    // one, whose window is 26 rows of 90.
    const std::size_t readerIndex = Screen::attach(d, left.id);
    Screen::grantScope(d, readerIndex, driving);
    Screen::setTokenID(d, readerIndex, "reader");
    Screen::markInteractiveUser(d, readerIndex);
    UbiquittyTest::ScreenClient reader(24, 80, Sync::ID128 { .low = 1, .high = 1 });
    Screen::greet(d, readerIndex, reader);
    const std::size_t driverIndex = Screen::attach(d, right.id);
    Screen::grantScope(d, driverIndex, driving);
    Screen::setTokenID(d, driverIndex, "driver");
    Screen::markInteractiveUser(d, driverIndex);
    UbiquittyTest::ScreenClient driver(26, 90, Sync::ID128 { .low = 2, .high = 2 });
    Screen::greet(d, driverIndex, driver);
    Screen::endReadPass(d);
    Screen::deliver(d, readerIndex, reader);

    // 1. The driver's window is no word on a pane laid out beside another.
    passTheWindow();
    Screen::endReadPass(d);
    Screen::deliver(d, readerIndex, reader);
    CHECK_MSG(right.term->columns == 100 && right.term->rows == 30, "a driver's window sized a pane laid out beside another");
    const auto before = tabOf(reader, work);
    if (!(before.has_value() && before->first.split.children.size() == 2)) {
        CHECK_MSG(false, "the reader holds no structure of the workspace to read cells from");
        return 1;
    }
    CHECK_MSG(before->first.split.children[1].columns == 100 && before->first.split.children[1].rows == 30,
        ("premise: the structure does not carry the right-hand pane's cells; it says " + cellsOf(before->first.split.children[1])).c_str());

    // 2. Its Viewport of the pane sizes it, and the structure follows.
    CHECK(driver.sendViewport(120, 40));
    Screen::receive(d, driverIndex, driver.takeStream(Screen::now()));
    Screen::endReadPass(d);
    CHECK_MSG(right.term->columns == 120 && right.term->rows == 40, "the driver's Viewport did not size its pane");
    CHECK_MSG(rightPTY.columns == 120 && rightPTY.rows == 40, "the program was not told the size the Viewport gave its pane");
    Screen::deliver(d, driverIndex, driver);
    Screen::deliver(d, readerIndex, reader);
    const auto after = tabOf(reader, work);
    if (!(after.has_value() && after->first.split.children.size() == 2)) {
        CHECK_MSG(false, "the reader holds no structure of the workspace to read cells from");
        return 1;
    }
    const Sync::TopologyNode& resized = after->first.split.children[1];
    CHECK_MSG(resized.paneID == right.screenSource->paneID(), "premise: the second leaf is not the right-hand pane");
    CHECK_MSG(resized.columns == 120 && resized.rows == 40, ("the structure a second client holds does not carry the cells the Viewport gave the pane; it says " + cellsOf(resized)).c_str());
    CHECK_MSG(after->first.split.columns == 200 && after->first.split.rows == 40, ("the split's cells did not follow its pane's; it says " + cellsOf(after->first.split)).c_str());
    CHECK_MSG(after->first.split.children[0].columns == 80 && after->first.split.children[0].rows == 24, "the pane beside it changed size with it");
    CHECK_MSG(after->second > before->second, "the structure's revision did not move with the pane's cells");

    // 3. A pane that is all its tab shows takes its driver's window.
    const std::size_t loneIndex = Screen::attach(d, alone.id);
    Screen::grantScope(d, loneIndex, driving);
    Screen::setTokenID(d, loneIndex, "lone");
    Screen::markInteractiveUser(d, loneIndex);
    UbiquittyTest::ScreenClient lone(33, 110, Sync::ID128 { .low = 3, .high = 3 });
    Screen::greet(d, loneIndex, lone);
    passTheWindow();
    Screen::endReadPass(d);
    CHECK_MSG(alone.term->columns == 110 && alone.term->rows == 33, "a driver's window did not size the pane that is all its tab shows");
    CHECK_MSG(alonePTY.columns == 110 && alonePTY.rows == 33, "the lone pane's program was not told its driver's window");
    Screen::deliver(d, loneIndex, lone);
    const auto loneTab = tabOf(lone, Screen::workspaceID(d, alone));
    if (!(loneTab.has_value())) {
        CHECK_MSG(false, "the reader holds no structure of the workspace to read cells from");
        return 1;
    }
    CHECK_MSG(loneTab->first.split.columns == 110 && loneTab->first.split.rows == 33, ("the lone pane's structure does not carry its driver's window; it says " + cellsOf(loneTab->first.split)).c_str());

    if (failures == 0)
        std::printf("viewport_structure_cells: a pane beside another takes its driver's Viewport and nothing from its window, a pane alone takes the window, "
                    "and each workspace's structure carries the resulting cells to a second client\n");
    return failures == 0 ? 0 : 1;
}
