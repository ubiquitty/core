// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <cassert>
#include <climits>
#include <cstdint>
#include <stddef.h>

static void checkTermSanity(const Ubiquitty::Terminal& t)
{
    assert(t.rows > 0);
    assert(t.rows <= Ubiquitty::maxRows);
    assert(t.columns > 0);
    assert(t.columns <= Ubiquitty::maxColumns);
    assert(t.screens.active == &t.screens.primary || t.screens.active == &t.screens.alternate);
    assert(t.screens.active->cursorX >= 0);
    assert(t.screens.active->cursorX < t.columns);
    assert(t.screens.active->cursorY >= 0);
    assert(t.screens.active->cursorY < t.rows);
    assert(t.screens.active->scrollTop >= 0);
    assert(t.screens.active->scrollTop < t.rows);
    assert(t.screens.active->scrollBottom >= 0);
    assert(t.screens.active->scrollBottom < t.rows);
    assert(t.screens.active->scrollTop <= t.screens.active->scrollBottom);
    assert(t.outputLength >= 0);
    assert(t.outputLength <= static_cast<int>(t.outputBuffer.size()));
    assert(t.parser.parameterCount >= 0);
    assert(t.parser.parameterCount <= Ubiquitty::VT::maxParameters);
    assert(t.parser.osc.length >= 0);
    // osc.data starts at oscBufferInitialCapacity and grows on demand up to maxOSCBufferSize
    // (see Cell.h); the real invariant is length within the live capacity.
    assert(t.parser.osc.length <= t.parser.osc.capacity);
    assert(t.parser.osc.capacity <= Ubiquitty::VT::maxOSCBufferSize);
    assert(t.scrollbackCount >= 0);
    assert(t.scrollbackCount <= Ubiquitty::VT::fallbackScrollbackLines);
    if (t.lineStore) {
        assert(t.scrollbackCapacity == Ubiquitty::VT::fallbackScrollbackLines);
        assert(t.scrollbackCount <= t.scrollbackCapacity);
    } else {
        assert(t.scrollbackCapacity == 0);
        assert(t.scrollbackCount == 0);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Ubiquitty::Terminal t;

    Ubiquitty::hardReset(t);
    while (size > 0) {
        int chunk = size > (size_t)INT_MAX ? INT_MAX : (int)size;
        Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(data), static_cast<std::size_t>(chunk)));
        checkTermSanity(t);
        data += chunk;
        size -= (size_t)chunk;
    }
    return 0;
}
