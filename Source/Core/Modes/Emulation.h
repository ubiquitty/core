// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Unicode/Encodings.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Forward.h"
#include <optional>

// Which dialect the terminal speaks, on both axes.
//
// Emulation selects the sequence family (VT, and the adm/heath/hp personalities);
// the encoding pair decides how incoming bytes become codepoints and what an
// invalid sequence produces. They live together because a caller configuring one
// almost always configures the other, and both are read on the hot decode path.

namespace Ubiquitty {

// Defined in Core/VT/Constants.h, not here: the grammar selector is a
// terminal-wide type these setters happen to carry, and moving it under Modes
// would put the enum and the accessor that returns it in one scope where only
// the accessor belongs. The namespace is opened rather than spelled as a
// qualifier because C++ rejects a nested name specifier on a forward
// declaration.
namespace VT {
enum class Emulation : uint8_t;
}

namespace Modes {

/// Select the sequence grammar ([terminal] emulation).
///
/// `Emulation::VT` — the default — keeps the native DEC/ANSI path unchanged,
/// codegen-identical to a build that never knew the others existed. Linux and Sco
/// layer console extensions on top of it; Wyse, Televideo, Adm, Heath and Hp
/// switch the byte driver to a non-ANSI grammar entirely.
///
/// Apply once at session start, before the host produces output: it resets the
/// parser substate, so a mid-stream change loses whatever sequence was in flight.
///
/// - Parameters:
///   - t: The terminal whose grammar changes.
///   - e: The grammar to speak.
void setEmulation(Ubiquitty::Terminal* t, Ubiquitty::VT::Emulation e);

/// Returns the sequence grammar the terminal currently speaks.
///
/// - Parameter t: The terminal to read.
/// - Returns: The active grammar.
[[nodiscard]] Ubiquitty::VT::Emulation emulation(const Ubiquitty::Terminal* t);

/// Resolves a configuration token to a grammar.
///
/// The tokens are "vt", "linux", "sco", "wyse", "televideo", "adm", "heath" and
/// "hp". Returns `std::nullopt` for an unknown or null name.
///
/// - Parameter name: The token to resolve.
/// - Returns: The grammar if `name` is one of the tokens above, otherwise `std::nullopt`.
[[nodiscard]] std::optional<Ubiquitty::VT::Emulation> emulationFromName(const char* name);

/// Select the input byte-stream character set.
///
/// `Encoding::UTF8` — the default — feeds `processBytes` the native path
/// untouched. Any other encoding routes incoming bytes through the streaming
/// transcoder at the front of `processBytes`, ahead of the parser. Switching
/// resets the decoder's partial-sequence state. A frontend resolves a
/// configuration string with `Ubiquitty::Unicode::encodingFromName`.
void setInputEncoding(Ubiquitty::Terminal* t, Ubiquitty::Unicode::Encoding enc);
[[nodiscard]] Ubiquitty::Unicode::Encoding inputEncoding(const Ubiquitty::Terminal* t);

/// Select what the decoder substitutes for invalid or truncated UTF-8.
///
/// `InvalidUTF8Policy::FFFD` — the default — emits U+FFFD, the substitution
/// Unicode recommends for an ill-formed sequence; `Drop` emits nothing; `Latin1`
/// reinterprets each undecodable byte as ISO-8859-1. A frontend resolves a
/// configuration token with `Ubiquitty::Unicode::invalidUTF8PolicyFromName`.
void setInvalidUTF8Policy(Ubiquitty::Terminal* t, Ubiquitty::Unicode::InvalidUTF8Policy policy);
[[nodiscard]] Ubiquitty::Unicode::InvalidUTF8Policy invalidUTF8Policy(const Ubiquitty::Terminal* t);

} // namespace Modes
} // namespace Ubiquitty
