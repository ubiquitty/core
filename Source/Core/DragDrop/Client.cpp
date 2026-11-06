// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/DragDrop/Client.h"

#include "Core/EffectGate.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"
#include "Core/VT/Constants.h"

#include <new>

namespace Ubiquitty::DragDrop {
namespace {

class TerminalChannel final : public Channel {
public:
    explicit TerminalChannel(Terminal& terminal)
        : m_terminal(terminal)
    {
    }

    bool writeReply(std::span<const char> bytes) override
    {
        // The session checks a native grant before disclosing user input.
        return respondBulk(m_terminal, Capability::InputModeReport, bytes);
    }

    bool sendCommand(const Command& command) override
    {
        const bool isRevocation = command.kind == CommandKind::FinishDrop || command.kind == CommandKind::CancelDrag
            || (command.kind == CommandKind::Registration && (command.metadata.type == Type::StopAccepting || (command.metadata.type == Type::Offer && command.metadata.x == 2)));
        auto* client = isRevocation ? clientForEffectThatEscapesNothing(m_terminal.dragDropClient)
                                    : effectClient(m_terminal, m_terminal.dragDropClient, Capability::InputModeReport);
        return client && client->sendCommand(command);
    }

private:
    Terminal& m_terminal;
};

} // namespace

void receive(Terminal& terminal, std::string_view body)
{
    if (!effectAllowed(terminal, Capability::InputModeReport))
        return;
    if (!terminal.dragDropSession)
        terminal.dragDropSession.reset(new (std::nothrow) Session);
    if (!terminal.dragDropSession)
        return;
    TerminalChannel channel(terminal);
    terminal.dragDropSession->receive(body, channel);
}

void receive(Terminal& terminal, const Event& event)
{
    if (!terminal.dragDropSession)
        return;
    TerminalChannel channel(terminal);
    if (!OSC::oscAllowed(terminal, 72) || !effectAllowed(terminal, Capability::InputModeReport)) {
        terminal.dragDropSession->reset(channel);
        return;
    }
    // Keep room for an error and revoke native reads when the program stops consuming replies.
    constexpr auto errorReserve = 2 * (maxPacketPayloadBytes + maxMetadataBytes + 16);
    if (event.kind == EventKind::ReadData && static_cast<std::size_t>(terminal.bulkOutput.length) > VT::maxBulkOutputBytes - errorReserve) {
        auto refused = event;
        refused.kind = EventKind::ReadFailed;
        refused.error = Error::NoMemory;
        refused.payload = {};
        terminal.dragDropSession->receive(refused, channel);
        return;
    }
    terminal.dragDropSession->receive(event, channel);
}

void reset(Terminal& terminal)
{
    if (!terminal.dragDropSession)
        return;
    TerminalChannel channel(terminal);
    terminal.dragDropSession->reset(channel);
}

void rebindNativeOwner(Terminal& terminal)
{
    if (!terminal.dragDropSession)
        return;
    TerminalChannel channel(terminal);
    terminal.dragDropSession->rebindNativeOwner(channel);
}

void publishRegistration(Terminal& terminal)
{
    if (!terminal.dragDropSession)
        return;
    TerminalChannel channel(terminal);
    terminal.dragDropSession->publishRegistration(channel);
}

} // namespace Ubiquitty::DragDrop
