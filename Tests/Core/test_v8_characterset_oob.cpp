// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include <memory>

// test_v8_characterset_oob.cpp — AddressSanitizer negative for the saved-cursor
// character-set bounds check: a wire-supplied slot index reaches an array of
// four, and only the checkpoint's validation keeps the read inside it.
//
// THREAT: a hostile host's Sync checkpoint can plant an out-of-range
// savedCharacterSetActive / savedCharacterSetGR (the DECSC slot the per-screen
// state block carries). The DECSC path that normally fills these on a real
// Ubiquitty::Terminal guarantees 0..3, but the wire is untrusted.
// Ubiquitty::Screen::restoreCursor (DECRC, ESC 8; Source/Core/Screen/Cursor.cpp)
// applies the saved slot RAW:
//     t->characterSetActive = s->savedCharacterSetActive;
//     t->characterSetGR     = s->savedCharacterSetGR;
// The char-translate path then indexes the 4-element characterSetG[] with it,
// unchecked:
//     resolveGLByte(t->characterSetG[activeG], ...)   // activeG = characterSetActive
//     t->characterSetG[t->characterSetGR]                  // GR byte path
// (Source/Core/VT/VT.cpp). So a crafted checkpoint + a later ESC 8 + a single
// graphic byte is an out-of-bounds read on characterSetG[].
//
// The checkpoint decode refuses a screen state whose saved slots fall outside
// [0,3] before anything is applied, so the client keeps the slots it had.
//
// This binary is built WITH -fsanitize=address. It drives the FULL live path
// — force the bad value on the host, encode its checkpoint, apply it to a
// passive client Terminal, then feed ESC 8 (DECRC) + one graphic byte through
// Terminal::processBytes so the translate executes the indexed characterSetG[]
// read. With the refusal present ASan reports nothing; remove it and ASan
// reports a heap-buffer-overflow READ on characterSetG[] during the translate.
//
// On the saved-index magnitude: characterSetG is a std::array<int,4> embedded mid-
// struct in the heap-allocated Terminal, so a small OOB index (e.g. 5, 99) reads
// an UNRELATED Ubiquitty::Terminal field still inside the same allocation — a real type-
// confused read the check closes, but one heap-ASan cannot see (no allocation
// boundary crossed). The wire carries the saved slot as a full integer, so
// a hostile checkpoint is free to plant an index that walks characterSetG[] clear
// past the END of the Ubiquitty::Terminal allocation; that read crosses ASan's heap redzone
// and is reported. We use such an index here so the negative is ASan-visible —
// it is the identical vulnerability (any index outside [0,3]), chosen at a
// magnitude the sanitizer can witness. The same check ([0,3]) closes both the
// small intra-object read and this allocation-escaping one.

#include "Core/TerminalOperations.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Ubiquitty;

// characterSetG[idx] escapes the END of the Ubiquitty::Terminal allocation when idx walks past
// the bytes remaining from characterSetG to the struct end. Picking +64 ints past
// that guarantees the read lands in ASan's heap redzone (without the check).
static int allocationEscapingForwardIndex()
{
    // Measured against a live object rather than with offsetof: Terminal is not
    // standard-layout, and offsetof on such a type is conditionally-supported —
    // accepted by one compiler and rejected by another, which is a portability
    // trap in a test whose whole subject is an out-of-bounds index.
    const Ubiquitty::Terminal probe;
    const auto base = reinterpret_cast<const unsigned char*>(&probe);
    const auto field = reinterpret_cast<const unsigned char*>(&probe.characterSetG);
    const std::size_t bytesAfter = sizeof(Ubiquitty::Terminal) - static_cast<std::size_t>(field - base);
    return static_cast<int>(bytesAfter / sizeof(int)) + 64;
}

// Applies `host`'s checkpoint to a freshly resized passive client, which is
// the untrusted boundary the check guards. The hostile checkpoint is refused,
// so the client keeps its own state.
static std::unique_ptr<Ubiquitty::Terminal> applyCheckpointInto(const Ubiquitty::Terminal& host)
{
    auto client = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*client, 24, 80);
    CHECK(UbiquittyTest::copyThroughCheckpoint(host, *client) != Protocol::Sync::ApplyOutcome::Applied);
    return client;
}

// GL path: a hostile savedCharacterSetActive restored by ESC 8, then a GL byte
// ('A') drives resolveGLByte(characterSetG[characterSetActive]).
TEST(testGlCharacterSetActiveOob)
{
    std::printf("testGlCharacterSetActiveOob\n");

    const int hostileIndex = allocationEscapingForwardIndex(); // outside [0,3]

    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 24, 80);
    host->screens.active->hasSavedCursor = true;
    host->screens.active->savedCharacterSetActive = hostileIndex; // OOB index into characterSetG[4]

    auto client = applyCheckpointInto(*host);

    CHECK(client->screens.active->savedCharacterSetActive >= 0 && client->screens.active->savedCharacterSetActive <= 3);

    // Live apply: ESC 8 (DECRC) copies the saved slot into characterSetActive,
    // then 'A' (a GL byte) drives resolveGLByte(characterSetG[characterSetActive]).
    // Applied unchecked this reads characterSetG[hostileIndex]; refused it is in-bounds.
    const char* decrc = "\033"
                        "8"; // ESC 8 — DECRC
    Ubiquitty::processBytes(client.get(), decrc);
    CHECK(client->characterSetActive >= 0 && client->characterSetActive <= 3);
    const char* gl = "A"; // 0x41 < 0x80 -> GL translate path
    Ubiquitty::processBytes(client.get(), gl);

    client.reset();
    host.reset();
}

// GR path: a hostile savedCharacterSetGR restored by ESC 8, then a GR byte
// (>=0xA0). characterSetGR != 0 invokes the GR set, so putCharacter routes the byte to
// resolveGRByte -> characterSetG[characterSetGR].
TEST(testGrCharacterSetGrOob)
{
    std::printf("testGrCharacterSetGrOob\n");

    const int hostileIndex = allocationEscapingForwardIndex(); // outside [0,3]

    auto host = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*host, 24, 80);
    host->screens.active->hasSavedCursor = true;
    host->screens.active->savedCharacterSetGR = hostileIndex; // OOB index into characterSetG[4]

    auto client = applyCheckpointInto(*host);

    CHECK(client->screens.active->savedCharacterSetGR >= 0 && client->screens.active->savedCharacterSetGR <= 3);

    // ESC 8 copies the saved slot into characterSetGR; a GR byte (0xC0) then enters
    // the GR translate path (gated on characterSetGR != 0) -> characterSetG[characterSetGR].
    const char* decrc = "\033"
                        "8"; // ESC 8 — DECRC
    Ubiquitty::processBytes(client.get(), decrc);
    CHECK(client->characterSetGR >= 0 && client->characterSetGR <= 3);
    Ubiquitty::Modes::setInputEncoding(client.get(), Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
    const char gr[] = { static_cast<char>(0xC0), 0 }; // >=0xA0 -> GR translate path
    Ubiquitty::processBytes(client.get(), std::string_view(gr, static_cast<std::size_t>(1)));

    client.reset();
    host.reset();
}

UBIQUITTY_TEST_MAIN()
