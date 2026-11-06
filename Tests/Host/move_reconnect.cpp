// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/move_reconnect.cpp — a client whose pane another client moved
// while its link was down resumes the same session, pane and workspace, and
// is continued with a diff into the picture the host holds now
// (Docs/sync.md §6.9).
//
//   1. A client attaches to one pane of a split workspace and holds its
//      picture; its link drops.
//   2. A person on a second connection extracts that pane into a window of
//      its own, and the pane's program prints while nobody shows it.
//   3. The first client resumes on a new connection: the attachment, the
//      session, the pane and the workspace are the ones it held, the pane
//      stands in its new window, and the picture it is brought to is the
//      host's, continued with a diff rather than drawn again.
//   4. The same, with the pane resized while it was away: the picture it is
//      brought to is the host's at the new size, whether diffed or drawn.
//   5. The same, with the pane closed while it was away: the resume says the
//      pane is gone, and the attachment carries on without it.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), the QUIC-off host build as
// resume_swap. Exit 0 on success.

#include "Assertions.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Layout = Ubiquitty::Session::Layout;

namespace {

/// What a client holds of a pane, as its resume names it.
Sync::ResumeRequest resumeOf(Sync::ReplicaSet& replicas, Sync::ID128 paneID, const Sync::Welcome& welcome)
{
    Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    if (Sync::ReplicaSet::Held* held = replicas.find(paneID))
        request.panes.push_back({ .paneID = paneID, .ackedGeneration = held->replica.generation, .ackedRowHashesDigest = Sync::replicaDigest(held->replica), .historyEpoch = held->replica.historyEpoch, .cachedRanges = {}, .lastInputSeqSent = 0, .view = Sync::ViewState::Live });
    return request;
}

/// Attaches a person under a token on a new connection.
std::size_t attachUnder(Daemon& d, std::uint32_t sessionID, const char* token)
{
    const std::size_t index = Screen::attach(d, sessionID);
    Screen::grantScope(d, index, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::setTokenID(d, index, token);
    Screen::markInteractiveUser(d, index);
    return index;
}

void write(Daemon& d, auto& session, const char* text)
{
    Ubiquitty::processBytes(session.term.get(), text);
    session.isFrameDue = true;
    Screen::publish(d, session);
}

/// The visible grid, row by row, as text.
std::string gridText(const Ubiquitty::Terminal& terminal)
{
    std::string text;
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            const std::uint32_t codePoint = cell && cell->codePoint ? cell->codePoint : ' ';
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : '?');
        }
        text.push_back('\n');
    }
    return text;
}

/// A person on a connection of its own who changes the workspace's tree.
struct Mover {
    Daemon& daemon;
    Ubiquitty::HostTest::FullCarrier carrier;
    std::size_t index;
    UbiquittyTest::ScreenClient client { 24, 80 };
    int nextID = 1;

    Mover(Daemon& d, auto& session)
        : daemon(d)
        , index(Screen::attach(d, session.id, carrier.takeDaemonEnd()))
    {
        CHECK(carrier.isReady());
        Screen::markInteractiveUser(d, index);
        Screen::setTokenID(d, index, "mover");
        Screen::grantScope(d, index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::bindWorkspace(d, index, session);
        Screen::greet(d, index, client);
    }

    /// Calls a method and returns its one answer.
    std::string call(const std::string& method, const std::string& parameters)
    {
        const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(nextID++) + ",\"method\":\"" + method + "\",\"params\":" + parameters + "}";
        CHECK(client.call(request));
        Screen::deliver(daemon, index, client);
        Screen::deliver(daemon, index, client);
        const auto results = client.takeResults();
        CHECK_MSG(results.size() == 1, (method + " drew " + std::to_string(results.size()) + " results").c_str());
        return results.empty() ? std::string() : results.front();
    }
};

/// What happens to the client's pane while its link is down.
enum class WhileAway : std::uint8_t {
    Moved,
    MovedAndResized,
    Closed,
};

void run(WhileAway whileAway)
{
    Daemon d;
    auto& first = Screen::makeSession(d, 24, 156);
    auto& second = Screen::makeSession(d, 24, 156);
    const std::uint32_t workspace = Screen::nameWorkspace(d, first, "moving");
    Screen::splitInto(d, first, second, Ubiquitty::Panes::Orientation::Vertical);
    auto& model = Screen::layout(d, first);
    const auto window = model.activeSurface();
    const std::uint32_t pane = Screen::layoutPane(d, first);
    const Sync::ID128 paneID = first.screenSource->paneID();
    Ubiquitty::processBytes(first.term.get(), "$ ");
    Screen::publish(d, first, Ubiquitty::Runtime::FrameClass::Structural);

    // 1. A client holds the pane, and its link drops.
    Sync::ReplicaSet replicas;
    const std::size_t heldIndex = attachUnder(d, first.id, "alice");
    UbiquittyTest::ScreenClient held(first.term->rows, first.term->columns, replicas, std::nullopt);
    Screen::greet(d, heldIndex, held);
    write(d, first, "before the move\r\n");
    Screen::deliver(d, heldIndex, held);
    REQUIRE(held.connection().welcome().has_value());
    const Sync::Welcome welcome = *held.connection().welcome();
    REQUIRE(held.generation() == first.screenSource->generation());
    held.storeReplicas();
    Screen::drop(d, heldIndex);
    REQUIRE(Screen::attachments(d).find(welcome.attachmentID) && Screen::attachments(d).find(welcome.attachmentID)->state == Sync::AttachmentState::Lost);

    // 2. Another person moves the pane into a window of its own, and the
    // pane's program prints while nobody shows it.
    Mover mover(d, second);
    const auto revision = Screen::windowStructureRevision(d, std::to_underlying(window));
    REQUIRE(revision);
    const std::string moved = mover.call("pane.extractWindow",
        "{\"window\":" + std::to_string(std::to_underlying(window)) + ",\"pane\":" + std::to_string(pane) + ",\"expectedRevision\":\"" + std::to_string(revision) + "\"}");
    CHECK_MSG(moved.find("\"result\"") != std::string::npos && moved.find("\"error\"") == std::string::npos, moved.c_str());
    const auto movedTo = model.surfaceHolding(Layout::PaneID { pane });
    REQUIRE(movedTo != Layout::invalidSurface && movedTo != window);
    write(d, first, "while the pane was moved and nobody showed it\r\n");
    if (whileAway == WhileAway::MovedAndResized) {
        Ubiquitty::resize(*first.term, 30, 100);
        write(d, first, "and resized\r\n");
    }
    if (whileAway == WhileAway::Closed) {
        const std::string closed = mover.call("workspace.closePane", "{\"pane\":" + std::to_string(pane) + "}");
        CHECK_MSG(closed.find("\"result\"") != std::string::npos && closed.find("\"error\"") == std::string::npos, closed.c_str());
        REQUIRE(!Screen::hasSession(d, first.id));
    }

    // 3. The client resumes on a new connection.
    // A closed pane's session is gone, so the resume arrives through the
    // workspace's other session and still names the pane it held.
    const std::size_t resumedIndex = attachUnder(d, whileAway == WhileAway::Closed ? second.id : first.id, "alice");
    UbiquittyTest::ScreenClient resumed(24, 156, replicas, resumeOf(replicas, paneID, welcome));
    Screen::greet(d, resumedIndex, resumed);
    Screen::deliver(d, resumedIndex, resumed);
    REQUIRE(resumed.connection().welcome().has_value());
    const Sync::Welcome& again = *resumed.connection().welcome();
    CHECK_MSG(again.attachmentID == welcome.attachmentID, "the resume did not continue the attachment the client held");
    REQUIRE(again.resumed.size() == 1);
    CHECK_MSG(again.resumed[0].paneID == paneID, "the resumed pane is not the pane the client held");
    if (whileAway == WhileAway::Closed) {
        CHECK_MSG(again.resumed[0].outcome == Sync::ResumeOutcome::Gone, "a pane closed while the client was away was not said to be gone");
        return;
    }
    if (whileAway == WhileAway::Moved) {
        CHECK_MSG(again.resumed[0].outcome == Sync::ResumeOutcome::Diffed, "a moved pane was drawn again rather than continued with a diff");
        CHECK_EQ(Screen::connection(d, resumedIndex)->counters().checkpointsSent, 0u);
    }
    CHECK_MSG(Screen::hasSession(d, first.id) && first.screenSource->paneID() == paneID, "the session or its pane identity did not survive the move");
    CHECK_MSG(Screen::layoutPane(d, first) == pane, "the host's pane number changed with the move");
    CHECK_MSG(&Screen::layout(d, first) == &model && Screen::nameWorkspace(d, first, "moving") == workspace, "the pane left its workspace");
    CHECK_MSG(model.surfaceHolding(Layout::PaneID { pane }) == movedTo, "the pane does not stand in the window it was moved to");
    CHECK_MSG(resumed.generation() == first.screenSource->generation(), "the resumed client does not hold the host's picture");
    const Ubiquitty::Terminal* picture = resumed.pane();
    REQUIRE(picture != nullptr);
    CHECK_MSG(picture->rows == first.term->rows && picture->columns == first.term->columns, "the replayed picture is not the host's size");
    CHECK_MSG(gridText(*picture) == gridText(*first.term), "the replayed picture is not the one the host holds");
    write(d, first, "after\r\n");
    Screen::deliver(d, resumedIndex, resumed);
    CHECK_MSG(resumed.generation() == first.screenSource->generation(), "the resumed connection is not served after the move");
}

} // namespace

int main()
{
    for (const WhileAway whileAway : { WhileAway::Moved, WhileAway::MovedAndResized, WhileAway::Closed })
        run(whileAway);
    if (failures == 0)
        std::printf("move_reconnect: a client resumes the session, pane and workspace another client moved while its link was down, "
                    "is continued with a diff into the host's picture, is brought to the new size of a pane resized meanwhile, "
                    "and is told a pane closed meanwhile is gone\n");
    return failures == 0 ? 0 : 1;
}
