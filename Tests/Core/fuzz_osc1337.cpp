// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_osc1337 — drives the iTerm2 OSC 1337 inline-image chain end-to-end
// through the REAL VT100 parser (Source/Core/OSC/ITerm2.cpp dispatch +
// Source/Core/Image/Inline/Images.cpp + the container codecs, all linked
// via CORE_SRCS — no copy).
//
// The image codecs (PNG/zlib/TIFF) already have their own fuzzers, and fuzz_sixel
// hits the sixel decoder directly. The surface this one covers is the chain that
// sits in front of the codecs and had no e2e arm:
//
//   handleOSC1337  ->  Inline::handleInlineImage  ->  base64 decode  ->
//   decodeContainerToRGBA  (container sniff -> PNG / TIFF / zlib)
//
// i.e. the OSC 1337 `File=` argument grammar (name/size/width/height/inline=1
// key=value pairs, the `:` separator), the base64 payload accumulator, and the
// container dispatch — reached only through `ESC ] 1337 ; File=<args>:<b64> ST`.
// Every fuzz input is framed as the File= argument, so the arg parser + base64 +
// dispatch all run; coverage guides the fuzzer to well-formed inline= payloads
// that reach the real decoders. Embedded ST/BEL in the payload let it build
// multi-sequence inputs. A crash here is an ASan/UBSan finding anywhere along
// the chain; the sanity asserts pin the parser invariants after the transfer.

#include "Core/CellGrid.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>

static void checkTermSanity(const Ubiquitty::Terminal& t)
{
    assert(t.rows > 0 && t.rows <= Ubiquitty::maxRows);
    assert(t.columns > 0 && t.columns <= Ubiquitty::maxColumns);
    assert(t.screens.active == &t.screens.primary || t.screens.active == &t.screens.alternate);
    assert(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < t.columns);
    assert(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < t.rows);
    // The OSC accumulator invariant the inline-image payload rides through.
    assert(t.parser.osc.length >= 0);
    assert(t.parser.osc.length <= t.parser.osc.capacity);
    assert(t.parser.osc.capacity <= Ubiquitty::VT::maxOSCBufferSize);
    assert(t.parser.parameterCount >= 0 && t.parser.parameterCount <= Ubiquitty::VT::maxParameters);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // ESC ] 1337 ; File= — enter the iTerm2 inline-image handler. The fuzz body
    // is the File= argument (key=value;… : base64) verbatim.
    static const char introducer[] = { 0x1b, ']', '1', '3', '3', '7', ';', 'F', 'i', 'l', 'e', '=' };
    Ubiquitty::processBytes(&t, std::string_view(introducer, sizeof(introducer)));

    while (size > 0) {
        int chunk = size > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(size);
        Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(data), static_cast<std::size_t>(chunk)));
        data += chunk;
        size -= static_cast<size_t>(chunk);
    }

    // ST — terminate the OSC so a never-closed payload still dispatches through
    // the inline-image decode (BEL/ST embedded in the body build multi-sequence
    // inputs on their own).
    static const char st[] = { 0x1b, '\\' };
    Ubiquitty::processBytes(&t, std::string_view(st, sizeof(st)));

    // OSC 1337 images decode synchronously inside dispatch; the drain is a no-op
    // for this path but keeps parity with the async (Kitty) completion arm.
    Ubiquitty::drainAsyncDecodes(t);

    checkTermSanity(t);
    return 0;
}
