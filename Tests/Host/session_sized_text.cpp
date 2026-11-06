// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/session_sized_text.cpp — a daemon session's terminal keeps a
// sized run as a run, so a mirror can be handed one.
//
// The daemon's terminal is the model of record for every attached client, and
// the screen plane carries what that model holds: a run the model printed plain is
// plain on every mirror, whatever the mirror's renderer could have drawn. So
// the daemon's terminal draws sized text, and a client without the band crop
// flattens a run to its cells on receipt rather than the daemon flattening it
// for everyone (Docs/proportional-text.md).
//
// Friend seam, same QUIC-OFF host build as create_session_refusal.
//
// Exit 0 on success.
//
#include "Assertions.h"
#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/GridBounds.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Platform/Host/Daemon.h"

#include <cstdio>
#include <string>

namespace Ubiquitty::Host {
struct SessionSizedTextAccess {
    static void useShell(Daemon& d, const std::string& path) { d.m_shellPath = path; }
    static Ubiquitty::Terminal* createSession(Daemon& d)
    {
        Daemon::Session* s = d.createSession(80, 24, {}, -1, 0);
        return s ? s->term.get() : nullptr;
    }
};
} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using A = Ubiquitty::Host::SessionSizedTextAccess;

int main()
{
    Daemon d;
    A::useShell(d, "/bin/sh");
    Ubiquitty::Terminal* term = A::createSession(d);
    CHECK_MSG(term != nullptr && term->grid != nullptr, "the session was not made");
    CHECK_MSG(term->drawsSizedText, "the daemon's terminal does not draw sized text");

    // A proportional run claims its cells and names its text from the anchor.
    Ubiquitty::processBytes(term, "\033]66;p=1:c=4;Hi\033\\");
    const Ubiquitty::Cell* anchor = term->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(0));
    CHECK_MSG(anchor != nullptr && Ubiquitty::isProportionalRunAnchor(term, *anchor),
        "a proportional run printed plain in the daemon's terminal");
    CHECK_MSG(term->screens.active->cursorX == 4, "a proportional run did not advance the cursor by its claimed cells");

    // A doubled glyph is a block whose cells carry a size specification.
    Ubiquitty::processBytes(term, "\r\n\033]66;s=2;A\033\\");
    const Ubiquitty::Cell* block = term->grid->cellAt(Ubiquitty::Row(1), Ubiquitty::Column(0));
    CHECK_MSG(block != nullptr && Ubiquitty::cellPen(*block).extendedAttributes.textSizeSpecID() != 0,
        "a sized glyph printed at the ordinary size in the daemon's terminal");

    if (failures == 0)
        std::printf("session_sized_text: the daemon's terminal keeps a proportional run and a sized block as the model a mirror is handed\n");
    return failures == 0 ? 0 : 1;
}
