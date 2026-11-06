// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"

namespace Ubiquitty::Modes {

// A DEC mode whose effect depends on state the mode itself does not carry.
//
// Two readings exist, and they are not interchangeable:
//
//   - `asRequested()` — what the host set. DECRQM reports this, and it is what
//     crosses the sync wire, since a peer derives its own effective value from
//     its own screen and conformance state.
//   - `isEffective()` — what the feature does. Takes the precondition, which the
//     caller supplies from live state.
//
// There is no `operator bool`. `if (modes.someMode)` would answer the requested
// question where the effective one was meant, and read as correct either way.
// DEC 1007 is the concrete case: the host may set it while the primary screen is
// showing, and honouring it there turns the user's scrollback wheel into cursor
// keys.
//
// Naming the precondition in the signature means a caller holding no
// `Modes::ScreenBuffer` cannot ask whether alternate scroll is effective — which is the
// right answer, because it does not know.
//
/// `Holds` is a template parameter rather than a member, so the object stays one
/// byte and the predicate inlines: a conditional mode costs what the bare `bool`
/// cost. Modes are read on the per-byte parse path, and there are 57 of them.
template<typename Precondition, bool (*Holds)(Precondition)>
class ConditionalMode {
public:
    constexpr ConditionalMode() = default;

    constexpr ConditionalMode& operator=(bool requested)
    {
        m_requested = requested;
        return *this;
    }

    /// For DECRQM and the sync wire. Named `asRequested` rather than `value` so a
    /// behavioural use reads wrong at the call site.
    [[nodiscard]] constexpr bool asRequested() const { return m_requested; }

    [[nodiscard]] constexpr bool isEffective(Precondition p) const { return m_requested && Holds(p); }

private:
    bool m_requested = false;
};

} // namespace Ubiquitty::Modes
