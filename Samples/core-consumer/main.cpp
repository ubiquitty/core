// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Samples/core-consumer/main.cpp — the standalone libUbiquittyCore consumer.
//
// Proves the portable Core is reusable without a frontend: this program links
// only the `ubiquitty-core` static library plus the tiny STUB PAL below (no
// AppKit / GTK / Metal / windowing, and no POSIX beyond what the library's
// bundled backend already provides). It boots a Terminal, feeds VT bytes, reads a
// snapshot row through the supported public API, and prints it.
//
// Build (from the repo root):
//   cmake -S . -B build/core -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF
//   cmake --build build/core --target core-consumer
//   ./build/core/core-consumer
//
// The whole program speaks through Source/UbiquittyCore/UbiquittyCore.h alone — the curated
// public umbrella — never an internal module header. Both halves of a frontend
// are exercised for that reason: bytes into the model and a key event out
// through the encoder, so a gap in either half of the published surface fails
// this build rather than the embedder's.
//

#include "UbiquittyCore/UbiquittyCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// STUB PAL — the "system services" half of the PAL contract a caller must
// supply (the free functions in namespace Ubiquitty::PAL that the library's
// bundled POSIX backend, Source/Platform/POSIX/SystemPAL.cpp, does not provide). A real
// frontend wires these to its clock / clipboard / log / bell / preferences;
// here they are trivial no-ops, which is all a headless byte-in / grid-out
// consumer needs. This is the entire OS surface the sample touches.
// ---------------------------------------------------------------------------

namespace Ubiquitty::PAL {

Ticks monotonicTicks() { return Ticks(0); }
uint32_t monotonicMicros() { return 0; }

void diagWrite(const char* message, int length)
{
    if (message && length > 0)
        std::fwrite(message, 1, static_cast<size_t>(length), stderr);
}

void bellRing(BellMode) { }

void clipboardSetText(const char*, int) { }
int clipboardGetText(char*, int) { return -1; }

int preferencesLoad(PreferencesBlobKey, void*, int) { return -1; }
int preferencesSaveAtomic(PreferencesBlobKey, const void*, int) { return 0; }

} // namespace Ubiquitty::PAL

// ---------------------------------------------------------------------------

namespace {

constexpr int kRows = 24;
constexpr int kColumns = 80;

// Formatted output that cannot throw.
//
// `std::print` raises `std::system_error` on a failed write, and this sample is
// built `-fno-exceptions`, where that is a process abort. `std::format` throws
// nothing here and the write's failure is a return value.

template<typename... Arguments>
void printText(std::format_string<Arguments...> format, Arguments&&... arguments)
{
    const std::string text = std::format(format, std::forward<Arguments>(arguments)...);
    (void)std::fwrite(text.data(), 1, text.size(), stdout);
}

template<typename... Arguments>
void printLine(std::format_string<Arguments...> format, Arguments&&... arguments)
{
    std::string line = std::format(format, std::forward<Arguments>(arguments)...);
    line.push_back('\n');
    (void)std::fwrite(line.data(), 1, line.size(), stdout);
}

// Render one snapshot row as UTF-8 to stdout, decoding each cell's base
// codePoint. A wide-character continuation cell carries codePoint 0 and is
// skipped (its lead cell already emitted the glyph). Blank trailing cells are
// trimmed so the printed row is the written extent, not 80 padded spaces.
void printRow(const Ubiquitty::Terminal& term, int row)
{
    Ubiquitty::Cell cells[kColumns];
    Ubiquitty::Screen::getDisplayRow(term, /*viewOffset=*/0, row, cells, kColumns);

    int lastInked = -1;
    for (int column = 0; column < kColumns; ++column) {
        if (cells[column].codePoint != 0)
            lastInked = column;
    }

    printText("row {:2}: \"", row);
    for (int column = 0; column <= lastInked; ++column) {
        uint32_t codePoint = cells[column].codePoint;
        if (codePoint == 0) {
            std::putchar(' '); // blank or wide continuation inside the run
            continue;
        }
        // Minimal UTF-8 encode of the base codePoint (clusters / combining
        // marks are reachable via cells[column].combining; omitted for brevity).
        char buffer[4];
        int length = 0;
        if (codePoint < 0x80) {
            buffer[length++] = static_cast<char>(codePoint);
        } else if (codePoint < 0x800) {
            buffer[length++] = static_cast<char>(0xC0 | (codePoint >> 6));
            buffer[length++] = static_cast<char>(0x80 | (codePoint & 0x3F));
        } else if (codePoint < 0x10000) {
            buffer[length++] = static_cast<char>(0xE0 | (codePoint >> 12));
            buffer[length++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            buffer[length++] = static_cast<char>(0x80 | (codePoint & 0x3F));
        } else {
            buffer[length++] = static_cast<char>(0xF0 | (codePoint >> 18));
            buffer[length++] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
            buffer[length++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            buffer[length++] = static_cast<char>(0x80 | (codePoint & 0x3F));
        }
        std::fwrite(buffer, 1, static_cast<size_t>(length), stdout);
    }
    printLine("\"");
}

} // namespace

int main()
{
    printLine("libUbiquittyCore consumer sample (public API v{})",
        Ubiquitty::coreVersionString);

    Ubiquitty::Terminal term;
    Ubiquitty::resize(term, kRows, kColumns);

    // Feed a byte stream through the real VT parser: plain text, then an SGR
    // bold-green run, a reset, more text, and a newline-positioned second line.
    static const char kStream[] = "Hello from \x1b[1;32mlibUbiquittyCore\x1b[0m!\r\n"
                                  "Fed \x1b[4mbytes\x1b[0m, read a snapshot row.";
    Ubiquitty::processBytes(&term, kStream);

    // Read the rendered grid back through the supported snapshot API and print
    // the two written rows. This is the byte-in -> model -> grid-out proof.
    printLine("--- snapshot ---");
    printRow(term, 0);
    printRow(term, 1);

    // The other direction: a key event becomes the bytes a shell reads. A
    // frontend is both halves, and the encoder is the only supported way to
    // spell the second — the arrow keys alone answer to the cursor-key mode,
    // the Kitty flags and the xterm modify-keys settings.
    printLine("--- key encoding ---");
    Ubiquitty::Keys::KeyEvent up {};
    up.virtualKey = Ubiquitty::Keys::VirtualKey::UpArrow;
    // The encoder reads the key's Kitty identity from keyCode; VirtualKey names
    // the key and functionalKeyCodePoint is the bridge, which is the step every
    // frontend takes and the reason the identity table is published beside the
    // encoder.
    up.keyCode = Ubiquitty::Keys::functionalKeyCodePoint(up.virtualKey);
    up.eventType = Ubiquitty::Keys::KeyEventType::Press;
    const Ubiquitty::Kitty::KeyFlagSet noKittyFlags {};
    char encoded[64];
    const int encodedLength = Ubiquitty::Keys::Keymap::encodeKeyEvent(up,
        Ubiquitty::Modes::CursorKeyMode::Normal, noKittyFlags, 0, 0, 0, 0, 0, encoded);
    printText("UpArrow -> ");
    for (int i = 0; i < encodedLength; ++i) {
        const unsigned char byte = static_cast<unsigned char>(encoded[i]);
        if (byte == 0x1B)
            printText("ESC ");
        else
            printText("{} ", static_cast<char>(byte));
    }
    printLine("({} bytes)", encodedLength);

    printLine("--- ok ---");
    return 0;
}
