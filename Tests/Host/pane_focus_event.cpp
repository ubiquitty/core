// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/pane_focus_event.cpp — the daemon records a pane gaining the
// focus when a client's screen plane says so (Docs/sync.md §6.7).
//
// A pane is focused while some driver's view of it holds the keyboard: the
// client's own focus, in its Facts, and the pane's, in a Focus on its
// channel. The event log gets one pane-focused event each time the pane goes
// from no driver holding it to one, whichever frame or departure moved it,
// and none while it only changes hands or stays held.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Modes/Focus.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Session/EventLog.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <cstdint>
#include <cstdio>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
using Ubiquitty::Modes::FocusState;
using Ubiquitty::Session::EventKind;

namespace {

void run()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const auto focusedEvents = [&] { return Screen::eventCount(d, EventKind::PaneFocused); };

    UbiquittyTest::ScreenClient first(24, 80, Sync::ID128 { .low = 1, .high = 1 });
    const std::size_t firstIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, firstIndex, driving);
    Screen::markInteractiveUser(d, firstIndex);
    Screen::greet(d, firstIndex, first);
    REQUIRE(first.connection().welcome().has_value());
    CHECK_MSG(focusedEvents() == 0, "a pane no client said it holds was recorded as focused");

    // The pane's focus, on its channel. The host's terminal holds it too: a
    // bell asks to be notified of only while nobody is looking.
    session.term->windowFocused = false;
    REQUIRE(first.setPaneFocus(FocusState::Focused));
    Screen::deliver(d, firstIndex, first);
    CHECK_MSG(focusedEvents() == 1, "a client taking the pane's focus was not recorded once");
    CHECK_MSG(session.term->windowFocused, "the host's terminal was not told its pane gained the focus");
    Screen::deliver(d, firstIndex, first);
    CHECK_MSG(focusedEvents() == 1, "a pane that stayed focused was recorded again");

    // A second driver holds it too, then the first lets go: it changed hands
    // and was never without one.
    UbiquittyTest::ScreenClient second(24, 80, Sync::ID128 { .low = 2, .high = 2 });
    const std::size_t secondIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, secondIndex, driving);
    Screen::markInteractiveUser(d, secondIndex);
    Screen::greet(d, secondIndex, second);
    REQUIRE(second.connection().welcome().has_value());
    REQUIRE(second.setPaneFocus(FocusState::Focused));
    Screen::deliver(d, secondIndex, second);
    REQUIRE(first.setPaneFocus(FocusState::Unfocused));
    Screen::deliver(d, firstIndex, first);
    CHECK_MSG(focusedEvents() == 1, "a pane that changed hands was recorded as focused again");

    // The client's own focus goes, in its Facts, and comes back.
    second.setFocus(FocusState::Unfocused);
    Screen::deliver(d, secondIndex, second);
    CHECK_MSG(focusedEvents() == 1, "losing the focus was recorded as gaining it");
    CHECK_MSG(!session.term->windowFocused && Ubiquitty::Modes::bellWantsNotification(session.term.get()), "the host's terminal was not told its pane lost the focus");
    second.setFocus(FocusState::Focused);
    Screen::deliver(d, secondIndex, second);
    CHECK_MSG(focusedEvents() == 2, "a pane focused again through its client's Facts was not recorded");
    CHECK_MSG(session.term->windowFocused, "the host's terminal was not told its pane has the focus again");

    // The holder leaves, and another takes the pane after it.
    Screen::drop(d, secondIndex);
    REQUIRE(first.setPaneFocus(FocusState::Focused));
    Screen::deliver(d, firstIndex, first);
    CHECK_MSG(focusedEvents() == 3, "a pane taken after its holder left was not recorded");

    // An observer's focus is no driver's.
    UbiquittyTest::ScreenClient watcher(24, 80, Sync::ID128 { .low = 3, .high = 3 });
    REQUIRE(first.setPaneFocus(FocusState::Unfocused));
    Screen::deliver(d, firstIndex, first);
    const std::size_t watcherIndex = Screen::attach(d, session.id);
    Screen::grantScope(d, watcherIndex, Sync::ScopeSet { Sync::Scope::Observe });
    Screen::markInteractiveUser(d, watcherIndex);
    Screen::greet(d, watcherIndex, watcher);
    REQUIRE(watcher.connection().welcome().has_value());
    (void)watcher.setPaneFocus(FocusState::Focused);
    Screen::deliver(d, watcherIndex, watcher);
    CHECK_MSG(focusedEvents() == 3, "an observer's focus was recorded as the pane's");
}

} // namespace

int main()
{
    run();
    if (failures == 0)
        std::printf("pane_focus_event: a pane gaining a driver's focus is recorded once, by its Focus, its client's Facts or a holder leaving\n");
    return failures == 0 ? 0 : 1;
}
