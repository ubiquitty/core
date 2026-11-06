// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_checkpoint_ceiling — the largest checkpoint bodies a legal
// terminal produces at the largest legal geometry, against the host's
// checkpoint cap (Docs/sync-measurements.md, "Memory admission").
//
// Each case fills both screens of a terminal with the content that makes its
// encoded rows longest: a distinct pen per cell, a hyperlink per cell, long
// grapheme clusters, and the widest code points. The cap must admit every one
// at maxRows x maxColumns, so no terminal a program can legally draw is
// unbound for size; a body past it can only come from a defect.
//
// The suite runs a 256 x 256 terminal and scales its body by the cell ratio,
// which overstates the capped dictionaries and so errs high. The recorded
// figures are the full geometry's: set UBQ_CHECKPOINT_CEILING_FULL=1 to
// measure it (minutes in a Debug build), and UBQ_PRINT_CHECKPOINT_CEILING=1
// to print each body.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/GridBounds.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::feed;

namespace {

/// Encodes a terminal's checkpoint as the host would send it.
std::size_t checkpointBytes(const Ubiquitty::Terminal& terminal)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(terminal) || !publisher.checkpoint(terminal, 1, body, baseline))
        return 0;
    return body.size();
}

/// Appends UTF-8 for one code point.
void appendUTF8(std::string& output, char32_t codePoint)
{
    if (codePoint < 0x80) {
        output += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        output += static_cast<char>(0xC0 | (codePoint >> 6));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        output += static_cast<char>(0xE0 | (codePoint >> 12));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        output += static_cast<char>(0xF0 | (codePoint >> 18));
        output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

/// Fills both screens of a largest-geometry terminal, a row at a time, with
/// the text `cell` writes for each column, and measures its checkpoint.
/// Returns the body scaled to the largest legal geometry.
std::size_t measure(const char* name, const std::function<void(std::string&, int row, int column)>& cell)
{
    const bool isFull = std::getenv("UBQ_CHECKPOINT_CEILING_FULL") != nullptr;
    const int rows = isFull ? Ubiquitty::maxRows : 256;
    const int columns = isFull ? Ubiquitty::maxColumns : 256;
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(terminal.get(), 0);
    Ubiquitty::resize(*terminal, rows, columns);
    std::string text;
    for (int screen = 0; screen < 2; ++screen) {
        // DECAWM off keeps each row's last cell on its row.
        feed(*terminal, screen == 0 ? "\x1b[?7l\x1b[H" : "\x1b[?1049h\x1b[?7l\x1b[H");
        for (int row = 0; row < rows; ++row) {
            text.clear();
            text += "\x1b[" + std::to_string(row + 1) + ";1H";
            for (int column = 0; column < columns; ++column)
                cell(text, row + screen * rows, column);
            feed(*terminal, text);
        }
    }
    const std::size_t measured = checkpointBytes(*terminal);
    const std::size_t scale = static_cast<std::size_t>(Ubiquitty::maxRows / rows) * static_cast<std::size_t>(Ubiquitty::maxColumns / columns);
    const std::size_t bytes = measured * scale;
    if (std::getenv("UBQ_PRINT_CHECKPOINT_CEILING"))
        std::printf("checkpoint-ceiling: %-28s %zu bytes at %dx%d, %.2f MiB at the largest geometry\n", name, measured, rows, columns, static_cast<double>(bytes) / (1 << 20));
    return measured ? bytes : 0;
}

/// A pen of its own per cell, cycling the 65,535 a dictionary epoch holds.
void distinctPen(std::string& text, int row, int column)
{
    const std::uint32_t index = static_cast<std::uint32_t>(row * Ubiquitty::maxColumns + column) % 65'535;
    text += "\x1b[1;3;4;9;38;2;" + std::to_string(index & 0xFF) + ';' + std::to_string((index >> 8) & 0xFF) + ";7;48;2;"
        + std::to_string(255 - (index & 0xFF)) + ';' + std::to_string(index % 251) + ";3;58;2;1;2;3m";
    appendUTF8(text, 0x10000 + (index % 0xF0000));
}

/// A hyperlink per cell, each with a long URI and its own id.
void distinctLink(std::string& text, int row, int column)
{
    const std::uint32_t index = static_cast<std::uint32_t>(row * Ubiquitty::maxColumns + column);
    text += "\x1b]8;id=" + std::to_string(index) + ";https://example.com/" + std::string(200, 'p') + std::to_string(index) + "\x1b\\";
    appendUTF8(text, 0x4E00 + (index % 0x5000));
    text += "\x1b]8;;\x1b\\";
}

/// A long grapheme cluster per cell: a base and combining marks.
void longCluster(std::string& text, int row, int column)
{
    const std::uint32_t index = static_cast<std::uint32_t>(row * Ubiquitty::maxColumns + column);
    appendUTF8(text, 'a' + index % 26);
    for (int mark = 0; mark < 12; ++mark)
        appendUTF8(text, 0x0300 + ((index + mark) % 0x70));
}

/// A pen of its own and a long cluster in every cell.
void penAndCluster(std::string& text, int row, int column)
{
    distinctPen(text, row, column);
    for (int mark = 0; mark < 12; ++mark)
        appendUTF8(text, 0x0300 + ((row * Ubiquitty::maxColumns + column + mark) % 0x70));
}

/// Pens, links and clusters at once.
void everything(std::string& text, int row, int column)
{
    distinctPen(text, row, column);
    if (column % 2)
        distinctLink(text, row, column);
    else
        longCluster(text, row, column);
}

} // namespace

TEST(theHostCheckpointLimitAdmitsTheLargestLegalTerminals)
{
    const std::size_t cap = measuredHostPolicy(0).maxCheckpointBytes;
    // The client must take every body the host may send.
    CHECK(cap <= measuredClientPolicy().maxCheckpointBytes);
    for (const auto& [name, cell] : std::vector<std::pair<const char*, std::function<void(std::string&, int, int)>>> {
             { "distinct pen per cell", distinctPen },
             { "hyperlink per cell", distinctLink },
             { "long cluster per cell", longCluster },
             { "pen and cluster per cell", penAndCluster },
             { "pens, links and clusters", everything },
         }) {
        const std::size_t bytes = measure(name, cell);
        CHECK(bytes > 0);
        CHECK(bytes <= cap);
    }
}

UBIQUITTY_TEST_MAIN()
