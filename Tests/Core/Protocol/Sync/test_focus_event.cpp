// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/Protocol/Sync/test_focus_event.cpp — DEC mode 1004 on a host's
// terminal.
//
// A program enables 1004 on the HOST's terminal, but only the client can see
// whether the window showing that session has focus. The host applies what the
// client observed through `Modes::focusChanged`, and that call is what answers
// the program and what bellWantsNotification reads: a host that never applied
// it would believe an attached client is focused forever.
//
// A client that cannot observe focus reports nothing, so the host makes no
// call and the program hears nothing: one gating on a focus-in that will never
// arrive is worse off than one that was never told, because it waits instead
// of proceeding.
//

#include "Assertions.h"
#include "TerminalFixture.h"

#include "Core/Modes/Focus.h"
#include "Core/Protocol/Sync/FrameGates.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

// Whether the program under test asked for focus reports at all — the gate that
// decides between a reply and silence, so it is the thing to name.
enum class ModeState {
    Disabled,
    Enabled
};

/// What the host hands the PTY after applying a client's observed focus.
///
/// - Parameters:
///   - observed: The focus the client saw, or nothing when it could not tell.
///   - mode: Whether the program enabled DEC 1004 first.
/// - Returns: The bytes the terminal queued for the program.
std::string replyAfter(std::optional<Ubiquitty::Modes::FocusState> observed, ModeState mode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    if (mode == ModeState::Enabled)
        UbiquittyTest::feed(t, "\x1b[?1004h");
    t.outputLength = 0;

    if (observed) {
        namespace Sync = Ubiquitty::Protocol::Sync;
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Focus { .isFocused = *observed == Ubiquitty::Modes::FocusState::Focused });
        const auto focus = Sync::decodeFocus(body);
        CHECK(focus);
        if (focus)
            Ubiquitty::Modes::focusChanged(&t, focus->isFocused ? Ubiquitty::Modes::FocusState::Focused : Ubiquitty::Modes::FocusState::Unfocused);
    }
    return { t.outputBuffer.data(), static_cast<std::size_t>(t.outputLength) };
}

} // namespace

int main()
{
    using Ubiquitty::Modes::FocusState;

    // The two states a client can actually observe reach the program as the
    // sequences DEC 1004 defines.
    CHECK(replyAfter(FocusState::Focused, ModeState::Enabled) == "\x1b[I");
    CHECK(replyAfter(FocusState::Unfocused, ModeState::Enabled) == "\x1b[O");

    // Nothing is emitted when the program never asked, so a focus-reporting
    // client cannot inject bytes into a session that did not enable the mode.
    CHECK(replyAfter(FocusState::Focused, ModeState::Disabled).empty());
    CHECK(replyAfter(FocusState::Unfocused, ModeState::Disabled).empty());

    // An unobserved focus is silence, not Unfocused.
    CHECK(replyAfter(std::nullopt, ModeState::Enabled).empty());

    // The bell path is the reason the bit matters beyond CSI I/O: a mirror that
    // believes it is focused forever can never raise a notification.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CHECK(!Ubiquitty::Modes::bellWantsNotification(&t)); // a fresh terminal is focused
        Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Unfocused);
        CHECK(Ubiquitty::Modes::bellWantsNotification(&t));
    }

    // Raw tag 0x11 is canonical PaneListed, a host-to-client kind: no role
    // may use it to send focus to the host.
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto oldTag = Sync::frameKindOf(0x11);
    CHECK(oldTag == Sync::FrameKind::PaneListed);
    if (oldTag) {
        for (const auto role : { Sync::Role::Owner, Sync::Role::Driver, Sync::Role::Viewer, Sync::Role::Agent })
            CHECK(!Sync::admitsSender(*oldTag, role));
    }
    const std::array<std::uint8_t, 2> ambiguous { 1, 0 };
    const std::array<std::uint8_t, 1> invalid { 2 };
    CHECK(!Sync::decodeFocus({}));
    CHECK(!Sync::decodeFocus(ambiguous));
    CHECK(!Sync::decodeFocus(invalid));

    if (failures == 0)
        std::printf("focus_event: an observed focus answers DEC 1004 on the host, and an unobserved one stays silent\n");
    return failures == 0 ? 0 : 1;
}
