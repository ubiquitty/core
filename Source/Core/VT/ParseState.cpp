// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/VT/ParseState.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Parser.h"

#include <cstring>

using ParserState = Ubiquitty::VT::Parser::State;

namespace Ubiquitty::VT {

bool isParserAtGround(const Ubiquitty::Terminal* t)
{
    if (!t)
        return false;
    return !t->printerControlPrefixLength && (t->parser.parseState == ParserState::Ground || t->parser.parseState == ParserState::VT52Ground);
}

void resetParseState(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    t->parser.parseState = ParserState::Ground;
    t->parser.intermediate = 0;
    t->parser.privatePrefix = 0;
    t->parser.parameterCount = 0;
    t->singleShift = 0;
}

void resetOSCBuffer(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    t->parser.osc.length = 0;
    // osc.data is null only if hardReset's initial malloc failed (OOM); guard
    // the NUL-terminate rather than dereference null (defensive).
    if (t->parser.osc.data)
        t->parser.osc.data[0] = 0;
    // If parked in OSC waiting for ST, force the parser back to
    // Ground so the next byte starts fresh instead of accumulating.
    if (t->parser.parseState == ParserState::OSC || t->parser.parseState == ParserState::OSCEscape)
        t->parser.parseState = ParserState::Ground;
}

void resetKittyBuffers(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    // Drop in-flight APC frame buffer + chunked image accumulator (both
    // unbounded-growth on a never-terminated APC); free + NULL the heap blocks.
    t->images.kittyAPC.reset();
    t->images.kittyAPCOverflow = false;
    t->images.kittyChunk.reset();
    t->images.kittyChunkActive = false;
    if (t->parser.parseState == ParserState::APC || t->parser.parseState == ParserState::DCS)
        t->parser.parseState = ParserState::Ground;
}

void resetSixelAccumulator(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    if (t->images.sixelAccumulatorBuffer) {
        std::free(t->images.sixelAccumulatorBuffer);
        t->images.sixelAccumulatorBuffer = nullptr;
        t->images.sixelAccumulatorCapacity = 0;
    }
    t->images.sixelAccumulatorLength = 0;
    t->images.sixelAccumulatorOverflow = false;
    dcsStateReset(*t);
    if (t->parser.parseState == ParserState::DCS || t->parser.parseState == ParserState::DCSPassthrough || t->parser.parseState == ParserState::DCSEscape)
        t->parser.parseState = ParserState::Ground;
}

} // namespace Ubiquitty::VT
