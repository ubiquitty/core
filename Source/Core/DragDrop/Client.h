// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/CheckedPtr.h"
#include "Core/DragDrop/Session.h"

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::DragDrop {

/// Receives operations admitted by a native drag grant. Calls hold the model lock.
class Client : public CanMakeCheckedPtr {
public:
    virtual ~Client() = default;
    [[nodiscard]] virtual bool sendCommand(const Command&) = 0;
};

/// Dispatches an OSC 72 body under the terminal's input-report policy.
void receive(Terminal&, std::string_view);

/// Delivers authenticated native input under the model lock.
void receive(Terminal&, const Event&);

/// Cancels native resources and registrations before a terminal reset.
void reset(Terminal&);
/// Revokes grants held by a disconnected or replaced native owner.
void rebindNativeOwner(Terminal&);
/// Replays registration through the terminal's current native client.
void publishRegistration(Terminal&);

} // namespace Ubiquitty::DragDrop
