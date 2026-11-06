// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_kitty — drives the REAL Kitty graphics control-protocol parser
// (Source/Core/Kitty/Graphics.cpp, linked via CORE_SRCS — no copy).
//
// The image codecs (PNG/zlib/TIFF) already have their own fuzzers; the surface
// this one covers is the KITTY WIRE PROTOCOL parser that sits in front of them:
// the APC control-key grammar (a=…,f=…,s=…,v=…,i=…,m=…,o=z,…), chunked-transfer
// assembly (m=1 continuations), the base64 payload accumulator, and the
// placement geometry. That parser runs synchronously inside dispatch() and is
// reached only through `ESC _ G <control>;<payload> ESC \`, so we frame every
// fuzz input as an APC-G body and feed it through the real VT100 parser exactly
// as it arrives on the wire. Embedded `ESC \` / `ESC _ G` in the payload let the
// fuzzer build multi-frame chunked transfers on its own.
//
// A crash here is an ASan/UBSan finding in the Kitty parser; the sanity asserts
// pin the state-machine invariants (image / placement counts within their caps).

#include "Core/CellGrid.h"
#include "Core/Kitty/Graphics.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>

static void checkKittySanity(const Ubiquitty::Terminal& t)
{
    // The Kitty APC accumulator never exceeds its documented ceiling.
    assert(t.images.kittyAPC.length >= 0);
    assert(t.images.kittyAPC.length <= t.images.kittyAPC.capacity);
    assert(t.images.kittyAPC.capacity <= Ubiquitty::Kitty::maxAPCCapacity);
    // Core parser invariants still hold after a graphics transfer.
    assert(t.rows > 0 && t.rows <= Ubiquitty::maxRows);
    assert(t.columns > 0 && t.columns <= Ubiquitty::maxColumns);
    assert(t.screens.active == &t.screens.primary || t.screens.active == &t.screens.alternate);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // ESC _ G — enter the Kitty graphics APC. Payload follows verbatim.
    static const char apcStart[3] = { 0x1b, '_', 'G' };
    Ubiquitty::processBytes(&t, std::string_view(apcStart, sizeof(apcStart)));

    while (size > 0) {
        int chunk = size > (size_t)INT_MAX ? INT_MAX : (int)size;
        Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(data), static_cast<std::size_t>(chunk)));
        data += chunk;
        size -= (size_t)chunk;
    }

    // ESC \ — terminate the APC so a never-closed transfer still dispatches.
    static const char apcEnd[2] = { 0x1b, '\\' };
    Ubiquitty::processBytes(&t, std::string_view(apcEnd, sizeof(apcEnd)));

    // Run the async-decode drain (installs decoded pixels into TerminalImage slots,
    // emits kitty OK/error responses) so the completion path is exercised too.
    Ubiquitty::Kitty::drainAsyncDecodes(&t);

    checkKittySanity(t);
    return 0;
}
