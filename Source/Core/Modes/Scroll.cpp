// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/Scroll.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Terminal.h"

#include <cstring>
#include <span>

namespace Ubiquitty::Modes {

// DEC private 1010/1011 policy. The frontend has already established that the
// view is scrolled back; this decides whether the given trigger snaps it to
// the live tail. 1011 (keypress) is the conventional default-on.
bool shouldSnapToTail(const Ubiquitty::Terminal* t, Modes::SnapTrigger trigger)
{
    if (!t)
        return false;
    return trigger == Modes::SnapTrigger::Output ? t->modes.scrollOnOutput : t->modes.scrollOnKeypress;
}

bool alternateScrollActive(const Ubiquitty::Terminal* t)
{
    if (!t)
        return false;
    return t->modes.alternateScrollMode.isEffective(
        t->screens.isUsingAlternateScreen ? Modes::ScreenBuffer::Alternate : Modes::ScreenBuffer::Primary);
}

int alternateScrollKeys(Ubiquitty::Modes::CursorKeyMode cursorKeys, Modes::ScrollDirection direction, ScrollKeyBytes output UBIQUITTY_NOESCAPE)
{
    char* const outputBuffer = output.data();
    const int maxLength = static_cast<int>(output.size());
    if (maxLength < 3)
        return 0;
    outputBuffer[0] = 0x1b;
    outputBuffer[1] = (cursorKeys == Ubiquitty::Modes::CursorKeyMode::Application) ? 'O' : '[';
    outputBuffer[2] = (direction == Modes::ScrollDirection::Up) ? 'A' : 'B';
    return 3;
}

int alternateScrollBurst(Ubiquitty::Modes::CursorKeyMode cursorKeys, Modes::ScrollDirection direction, int count, ScrollKeyBytes output UBIQUITTY_NOESCAPE)
{
    if (count <= 0)
        return 0;
    const int sequenceLength = alternateScrollKeys(cursorKeys, direction, output);
    if (sequenceLength <= 0)
        return 0;
    const int capacity = static_cast<int>(output.size()) / sequenceLength;
    const int repetitions = count < capacity ? count : capacity;
    // The first repetition is already in place; the rest copy it, which is
    // one fill however the sequence is spelled.
    for (int repetition = 1; repetition < repetitions; ++repetition)
        std::memcpy(output.data() + static_cast<std::size_t>(repetition) * sequenceLength, output.data(), static_cast<std::size_t>(sequenceLength));
    return repetitions * sequenceLength;
}

} // namespace Ubiquitty::Modes
