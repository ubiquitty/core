// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Pen.h"
#include "Core/ReplyBuilder.h"
#include "Core/TerminalOperations.h"
#include <cstdint>

// SGR (Select Graphic Rendition) parameter folder. Reaches into
// Terminal's parameters[] / parameterCount / parameterSeparatorWasColon[] and mutates the
// active screen's pen. The VT100 parser owns CSI 'm' recognition and
// hands the fully-parsed parameter vector to apply().

namespace Ubiquitty::SGR {

/// Applies the parsed SGR parameters to the terminal's pen.
///
/// - Parameter t: The terminal whose pen the rendition changes.
void apply(Ubiquitty::Terminal& t);

/// Saves the pen for XTPOPSGR (XTPUSHSGR, `CSI Pm # {`).
///
/// - Parameters:
///   - t: The terminal whose pen is saved.
///   - mask: Bit Ps - 1 for each XTPUSHSGR parameter Ps the pop restores.
void push(Ubiquitty::Terminal& t, uint32_t mask);

/// Restores the attributes the matching XTPUSHSGR named (XTPOPSGR, `CSI # }`).
///
/// - Parameter t: The terminal whose pen is restored.
void pop(Ubiquitty::Terminal& t);

/// Writes a pen as the parameters of the SGR that recreates it from SGR 0:
/// `0`, then the attributes in the order xterm writes them (xtermFormatSGR), then
/// the colours, indexed and direct ones in the ITU-T T.416 colon form.
///
/// - Parameters:
///   - pen: The rendition to write.
///   - output: Where the parameters are appended, without the CSI or the `m`.
void writeParameters(const Ubiquitty::Pen& pen, Ubiquitty::ReplyBuilder& output);

} // namespace Ubiquitty::SGR
