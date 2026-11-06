// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/rowdelta_baseline_corruption.cpp — the baseline the host keeps
// for a client is what the client holds, frame after frame.
//
// The host diffs each client's next update against the row hashes it
// recorded for that client. A baseline that recorded something other than
// what the update carried — a row whose encoding was refused part way, a row
// that names a link the update had to define first — makes a later update
// skip a row the client does not have, or resend one it does, and the client
// shows the wrong grid with every frame well-formed.
//
// This drives one session through the interleaving that can splice a stale
// row into a baseline: two plain rows; then a plain row beside a row under an
// OSC 8 link, which the update carries with the link's definition; then frames
// that leave the linked row alone and edit around it. After every frame the
// client holds the host's screen, and the client never asks for a resync — a
// baseline out of step is caught by the digest, and would surface as one.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h). Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <cstdio>
#include <string>
#include <string_view>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

void writePlainRow(Ubiquitty::Terminal& t, int row, std::string_view text)
{
    Ubiquitty::processBytes(&t, "\x1b[" + std::to_string(row + 1) + ";1H");
    Ubiquitty::processBytes(&t, text);
}

// Same row, but the text rides an OSC 8 hyperlink: every cell carries a link id
// the client resolves through a definition the update has to carry.
void writeLinkedRow(Ubiquitty::Terminal& t, int row, std::string_view text)
{
    Ubiquitty::processBytes(&t, "\x1b[" + std::to_string(row + 1) + ";1H\x1b]8;;http://example.test\x1b\\");
    Ubiquitty::processBytes(&t, text);
    Ubiquitty::processBytes(&t, "\x1b]8;;\x1b\\");
}

} // namespace

int main()
{
    constexpr int rows = 4, columns = 20;
    Daemon d;
    auto& s = Screen::makeSession(d, rows, columns);
    const std::size_t index = Screen::attach(d, s.id);
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(rows, columns);
    Screen::greet(d, index, client);

    int frame = 0;
    const auto publishAndCheck = [&](const char* what) {
        s.isFrameDue = true;
        Screen::publish(d, s);
        Screen::deliver(d, index, client);
        ++frame;
        CHECK_MSG(client.generation() == s.screenSource->generation(), (std::string(what) + ": the client did not reach the frame").c_str());
        const std::string difference = client.pane() ? UbiquittyTest::firstDisagreement(*s.term, *client.pane()) : std::string("no pane");
        CHECK_MSG(difference.empty(), (std::string(what) + ": " + difference).c_str());
    };

    publishAndCheck("the first frame");
    writePlainRow(*s.term, 0, "XXXX");
    writePlainRow(*s.term, 1, "BBBB");
    publishAndCheck("two plain rows");
    writePlainRow(*s.term, 0, "YYYY");
    writeLinkedRow(*s.term, 1, "CCCC");
    publishAndCheck("a plain row beside a linked one");
    writePlainRow(*s.term, 0, "ZZZZ");
    publishAndCheck("an edit beside the linked row");
    writePlainRow(*s.term, 2, "DDDD");
    writeLinkedRow(*s.term, 1, "CCCC");
    publishAndCheck("the linked row rewritten unchanged");
    writePlainRow(*s.term, 1, "EEEE");
    publishAndCheck("the linked row back to plain text");

    CHECK_MSG(client.connection().counters().resyncsSent == 0, "the client asked for a resync: an update did not apply onto the baseline the host recorded");
    CHECK(client.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == 1);

    if (failures == 0)
        std::printf("rowdelta_baseline_corruption: across plain, linked and unchanged rows the client holds the host's screen after "
                    "every one of %d frames, each an update onto the baseline the host recorded\n",
            frame);
    return failures == 0 ? 0 : 1;
}
