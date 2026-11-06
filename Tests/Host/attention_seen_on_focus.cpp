// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/attention_seen_on_focus.cpp — a bell and a notification a pane
// latched stay until a person looks at it.
//
// The terminal keeps both so a listing or a viewer that arrives later can
// still mark the pane (unseenAttention). On the host a person looks at a pane
// when a driver the host verified as a person gains the focus on it, the
// Sync Focus of a focused window. An agent's driver gaining the same focus
// is automation, not somebody seeing the pane, and leaves the latch for the
// person who comes later. While a person's driver holds the focus they are
// still looking: what the program raises then is seen in the pass that read
// it, not when they leave and come back.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

int main()
{
    Daemon d;
    const std::array<std::string, 3> command { "/bin/sh", "-c", "printf '\\a\\033]9;the build finished\\007'; read line; printf '\\a\\033]9;the tests finished\\007'; sleep 60" };
    auto* session = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(session != nullptr, "premise: the session was not spawned");
    if (!session)
        return UBIQUITTY_TEST_SUMMARY();
    const auto unseen = [&] { return Ubiquitty::unseenAttention(session->term.get()); };
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end && !(unseen().hasBell && unseen().hasNotification)) {
        Screen::servicePTY(d, *session);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_MSG(unseen().hasBell && unseen().hasNotification, "premise: the program's bell and notification were latched");

    const Ubiquitty::Protocol::Sync::ScopeSet driving { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive };

    // 1. An agent's driver gains the focus: the pane reports focus to its
    // program, and nobody has seen it.
    const std::size_t agent = Screen::attach(d, session->id);
    Screen::grantScope(d, agent, driving);
    Screen::setTokenID(d, agent, "automation");
    // No person is attached yet to approve it, so the agent holds the standing
    // grant a person's allow-for-this-session answer gives.
    Screen::grantAgentSession(d, *session, agent);
    UbiquittyTest::ScreenClient agentClient(24, 80);
    Screen::greet(d, agent, agentClient);
    CHECK(agentClient.sendFocus(Ubiquitty::Modes::FocusState::Focused));
    Screen::deliver(d, agent, agentClient);
    CHECK_MSG(session->screenSource && session->screenSource->isAnyDriverFocused(), "premise: the agent's focus was taken as a driver's");
    Screen::clearAttentionAPersonSaw(d);
    CHECK_MSG(unseen().hasBell, "an agent's focus left the bell unseen");
    CHECK_MSG(unseen().hasNotification && unseen().notificationBody == "the build finished", "an agent's focus left the notification unseen");

    // 2. A person's driver gains it: both are seen.
    const std::size_t person = Screen::attach(d, session->id);
    Screen::grantScope(d, person, driving);
    Screen::setTokenID(d, person, "person");
    Screen::markInteractiveUser(d, person);
    UbiquittyTest::ScreenClient personClient(24, 80);
    Screen::greet(d, person, personClient);
    CHECK_MSG(unseen().hasBell && unseen().hasNotification, "attaching is not looking");
    CHECK(personClient.sendFocus(Ubiquitty::Modes::FocusState::Focused));
    Screen::deliver(d, person, personClient);
    Screen::clearAttentionAPersonSaw(d);
    CHECK_MSG(!unseen().hasBell, "a person's focus saw the bell");
    CHECK_MSG(!unseen().hasNotification && unseen().notificationBody.empty(), "a person's focus saw the notification");

    // 3. The program rings again while that person still holds the focus:
    // they are looking at it, so the pass that read it clears it. The bell is
    // what is waited for; a notification from a pane with drivers attached is
    // the capability policy's to admit.
    const std::uint32_t latchesBefore = session->term->attentionGeneration;
    const std::array<std::uint8_t, 1> enter { '\r' };
    CHECK(personClient.sendRaw(enter).has_value());
    Screen::deliver(d, person, personClient);
    const auto secondEnd = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < secondEnd && session->term->attentionGeneration == latchesBefore) {
        Screen::servicePTY(d, *session);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_MSG(session->term->attentionGeneration != latchesBefore && unseen().hasBell, "premise: the second bell was latched");
    Screen::clearAttentionAPersonSaw(d);
    CHECK_MSG(!unseen().hasBell, "a bell under a person's focus was seen in the pass that read it");
    CHECK_MSG(!unseen().hasNotification, "and nothing else stays latched under it");
    return UBIQUITTY_TEST_SUMMARY();
}
