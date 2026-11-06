// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_apply_terminal_preferences.cpp — the seat between a config fold and a
// live model (Core/Runtime/TerminalSession.h, applyTerminalPreferences).
//
// The two halves either side of it were already covered and the seat itself was
// not: the accessors have a case each, `Modes::setInputEncoding` has cases, and
// nothing drove the call that carries one to the other. That gap is the shape
// that put SGR bold at body weight on an entire frontend for a day — the
// rasterizer could draw bold, a smoke proved it could, and the renderer passed
// a literal Regular. A capability with no case over its caller is a capability
// whose caller is untested, and the two read identically from the outside.
//
// So these drive the real function against a real session and read the model
// back, rather than asserting about the values it was given.

#include "Core/Runtime/TerminalSession.h"

#include "Core/CellGrid.h"
#include "Core/Modes/Emulation.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Terminal.h"
#include "Core/Unicode/Encodings.h"

using namespace Ubiquitty::Runtime;

namespace {

struct SessionHandle {
    TerminalSession* session;
    explicit SessionHandle()
        : session(TerminalSession::make())
    {
    }
    ~SessionHandle()
    {
        session->close();
        releaseTerminalSession(session);
    }
};

// The code point at a cell, or zero where there is no model to ask.
std::uint32_t codePointAt(TerminalSession& session, int row, int column)
{
    session.lockForRead();
    std::uint32_t codePoint = 0;
    if (Ubiquitty::Terminal* term = session.terminal()) {
        if (const Ubiquitty::Cell* cell = term->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column)))
            codePoint = cell->codePoint;
    }
    session.unlockForRead();
    return codePoint;
}

} // namespace

// Every setting the call carries reaches the model, checked on the model rather
// than on the config it came from.
//
// Three settings from three config sections travel together because they share
// a lock and a walk, so a fourth arriving later is a line in this function and
// not a second traversal. What that costs is a case per setting: a walk carrying
// three values can carry two of them and read as working.
TEST(theConfigFanOutReachesTheModel)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);

    auto named = Ubiquitty::Unicode::encodingFromName("iso-8859-1");
    CHECK_MSG(named.has_value(),
        "the fixture names an encoding this build carries");
    CHECK_MSG(*named != Ubiquitty::Unicode::Encoding::UTF8,
        "and one that differs from the default, or the case cannot fail");

    Ubiquitty::Preferences::Values config;
    config.encoding = "iso-8859-1";
    config.passwordDetect = true;
    config.bellVisual = true;
    config.bellAudible = false;
    config.cursorMotion = Ubiquitty::Preferences::CursorMotionMode::Long;
    config.cursorMotionDuration = 180;

    session.applyTerminalPreferences(config);

    session.lockForRead();
    Ubiquitty::Terminal* term = session.terminal();
    CHECK_MSG(term != nullptr, "a started session has a model to apply onto");
    const bool didTakePasswordDetect = term && term->passwordDetect;
    const Ubiquitty::BellMode bell = term ? term->bellMode : Ubiquitty::BellMode {};
    const Ubiquitty::Unicode::Encoding applied
        = term ? Ubiquitty::Modes::inputEncoding(term) : Ubiquitty::Unicode::Encoding::UTF8;
    const Ubiquitty::Preferences::CursorMotionMode glide
        = term ? term->cursorMotionModePref : Ubiquitty::Preferences::CursorMotionMode::Off;
    const std::uint32_t glideMilliseconds = term ? term->cursorMotionDurationMilliseconds : 0;
    session.unlockForRead();

    CHECK_MSG(didTakePasswordDetect, "the prompt heuristic reaches the model");
    CHECK_MSG(bell == Ubiquitty::BellMode::Visual, "the bell cue reaches it from the same walk");
    CHECK_MSG(applied == named, "and so does the encoding the file named");
    // The caret's glide rides the cursor seed inside the same walk: this is
    // the route a config reload takes on the Swift shell, and a store the walk
    // never writes is not a route at all.
    CHECK_MSG(glide == Ubiquitty::Preferences::CursorMotionMode::Long, "the glide policy reaches the model");
    CHECK_MSG(glideMilliseconds == 180u, "with the duration the file named");
}

// A repeated call leaves a half-read character alive.
//
// Applying an encoding resets the decoder, which drops whatever multibyte
// sequence is mid-flight, so the function applies one only where it differs
// from the encoding already set. That guard has no observable effect on any
// setting's value — both orders end with the same encoding on the model — and
// its whole purpose is a byte that is not there to be seen. A case that reads
// values back cannot see it at all, so this one reads a character instead.
//
// The call under test carries the bell and the prompt heuristic too, so it runs
// whenever either of those changes: a settings pane toggling the bell while a
// two-byte character sits half-consumed is the sequence this protects.
TEST(anUnchangedEncodingDoesNotDropAHalfReadCharacter)
{
    SessionHandle handle;
    TerminalSession& session = *handle.session;
    session.start(24, 80);

    // U+00E9 as UTF-8 is C3 A9. The lead byte alone leaves the decoder holding
    // an incomplete sequence with nothing yet on the grid.
    const unsigned char lead[] = { 0xC3 };
    session.feedBytes({ reinterpret_cast<const char*>(lead), sizeof(lead) });

    // The config names no encoding, so the model's stays UTF-8 and the guard
    // has something to refuse. Bell set, because the realistic caller is a
    // settings change that is not about encoding at all.
    Ubiquitty::Preferences::Values config;
    config.bellVisual = true;
    session.applyTerminalPreferences(config);

    const unsigned char trail[] = { 0xA9 };
    session.feedBytes({ reinterpret_cast<const char*>(trail), sizeof(trail) });

    CHECK_MSG(codePointAt(session, 0, 0) == 0x00E9,
        "the character split across the settings change arrives whole");
}

UBIQUITTY_TEST_MAIN()
