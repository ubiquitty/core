// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Headless Core ingest throughput micro-bench. Feeds a fixed byte
// pattern through Terminal::processBytes and reports MB/s — no GUI, no
// render, so it isolates Core parser+CellGrid cost for before/after A/B.
// Not part of the suite; built ad hoc.
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static double feedMBps(const std::string& chunk, long targetBytes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t); // default 80x24
    const char* buffer = chunk.data();
    const int length = static_cast<int>(chunk.size());
    Ubiquitty::processBytes(&t, std::string_view(buffer, static_cast<std::size_t>(length))); // warm up
    long fed = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (fed < targetBytes) {
        Ubiquitty::processBytes(&t, std::string_view(buffer, static_cast<std::size_t>(length)));
        fed += length;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(fed) / 1e6 / secs;
}

int main()
{
    const long target = 800L * 1024 * 1024; // 800 MB

    // ascii_fill-like: plain printable ASCII rows (hits emitASCIIRun).
    std::string ascii;
    for (int r = 0; r < 2000; r++) {
        for (int c = 0; c < 80; c++)
            ascii += static_cast<char>('A' + (c % 26));
        ascii += "\r\n";
    }
    // dense_sgr256-like: a 256-color SGR before each cell.
    std::string sgr;
    for (int r = 0; r < 1000; r++) {
        for (int c = 0; c < 80; c++) {
            char esc[24];
            std::snprintf(esc, sizeof(esc), "\x1b[48;5;%dm ", (c * 7 + r) & 255);
            sgr += esc;
        }
        sgr += "\x1b[0m\r\n";
    }
    // unicode_cjk: wide CJK ideographs (U+4E00.. as 3-byte UTF-8), 40 per row.
    std::string cjk;
    for (int r = 0; r < 1500; r++) {
        for (int c = 0; c < 40; c++) {
            unsigned cp = 0x4E00 + ((c + r) & 0x1FF);
            cjk += static_cast<char>(0xE0 | (cp >> 12));
            cjk += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            cjk += static_cast<char>(0x80 | (cp & 0x3F));
        }
        cjk += "\r\n";
    }
    // cjk_overwrite: full 24x80 wide-CJK screen repainted in place (CUP home,
    // no LF/scroll/scrollback) — isolates emitWideRun's target (emitCharacter +
    // writeWideCell) from scroll + scrollback-append cost.
    std::string cjkov = "\x1b[H";
    for (int r = 0; r < 24; r++) {
        for (int c = 0; c < 40; c++) {
            unsigned cp = 0x4E00 + ((c + r) & 0x1FF);
            cjkov += static_cast<char>(0xE0 | (cp >> 12));
            cjkov += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            cjkov += static_cast<char>(0x80 | (cp & 0x3F));
        }
        if (r < 23)
            cjkov += "\r\n"; // last row: no LF, so the screen never scrolls
    }
    // cjk_mixed: 20 wide CJK (40 columns) + 40 ASCII (40 columns) per row — forces
    // run-boundary switching between the wide-batch and emitASCIIRun paths, so
    // a batch fast-path can't win by assuming a homogeneous run.
    std::string mix;
    for (int r = 0; r < 1500; r++) {
        for (int c = 0; c < 20; c++) {
            unsigned cp = 0x4E00 + ((c + r) & 0x1FF);
            mix += static_cast<char>(0xE0 | (cp >> 12));
            mix += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            mix += static_cast<char>(0x80 | (cp & 0x3F));
        }
        for (int c = 0; c < 40; c++)
            mix += static_cast<char>('A' + (c % 26));
        mix += "\r\n";
    }
    // scroll_flood: single char + LF per line — 2 bytes per scroll, the
    // max-scroll-pressure case (TUI line streaming, `yes`, log tails). Each
    // LF at the bottom margin triggers Ubiquitty::Screen::scrollUp(1): scrollback append + ring
    // rotate + blank. Isolates the per-scroll fixed cost.
    std::string scroll;
    for (int r = 0; r < 4000; r++) {
        scroll += 'y';
        scroll += "\r\n";
    }

    // box_drawing: U+2500.. line-drawing (3-byte UTF-8), 80 per row.
    std::string box;
    for (int r = 0; r < 1500; r++) {
        for (int c = 0; c < 80; c++) {
            unsigned cp = 0x2500 + ((c + r) % 0x80);
            box += static_cast<char>(0xE0 | (cp >> 12));
            box += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            box += static_cast<char>(0x80 | (cp & 0x3F));
        }
        box += "\r\n";
    }
    // combining: base 'e' + combining acute U+0301 (2-byte), 80 per row.
    std::string comb;
    for (int r = 0; r < 1500; r++) {
        for (int c = 0; c < 80; c++) {
            comb += 'e';
            comb += static_cast<char>(0xCC);
            comb += static_cast<char>(0x81);
        }
        comb += "\r\n";
    }
    // emoji: U+1F600.. (4-byte UTF-8), VS-less, 40 per row.
    std::string emoji;
    for (int r = 0; r < 1500; r++) {
        for (int c = 0; c < 40; c++) {
            unsigned cp = 0x1F600 + ((c + r) % 0x40);
            emoji += static_cast<char>(0xF0 | (cp >> 18));
            emoji += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            emoji += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            emoji += static_cast<char>(0x80 | (cp & 0x3F));
        }
        emoji += "\r\n";
    }

    /// diverse_symbols: vtebench's unicode/symbols feed — ~138 KB of every
    /// diverse Unicode symbol back-to-back, through emitTextRun. Falls back to a
    /// generated diverse-BMP stream if the suite file is absent.
    std::string symbols;
    {
        std::ifstream f(std::string(getenv("HOME") ? getenv("HOME") : "") + "/vtebench-suite/benchmarks/unicode/symbols", std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            symbols = ss.str();
        } else {
            for (int r = 0; r < 1500; r++) {
                for (unsigned cp = 0xA1; cp < 0xA1 + 80; cp++) {
                    symbols += char(0xC0 | (cp >> 6));
                    symbols += char(0x80 | (cp & 0x3F));
                }
                symbols += "\r\n";
            }
        }
    }

    std::printf("ascii_fill   : %7.1f MB/s\n", feedMBps(ascii, target));
    std::printf("dense_sgr256 : %7.1f MB/s\n", feedMBps(sgr, target));
    std::printf("unicode_cjk  : %7.1f MB/s\n", feedMBps(cjk, target));
    std::printf("cjk_overwrite: %7.1f MB/s   (emit-only, no scroll)\n", feedMBps(cjkov, target));
    std::printf("cjk_mixed    : %7.1f MB/s   (wide/ascii run boundaries)\n", feedMBps(mix, target));
    std::printf("scroll_flood : %7.1f MB/s   (1 scroll / 3 bytes)\n", feedMBps(scroll, target));
    std::printf("box_drawing  : %7.1f MB/s\n", feedMBps(box, target));
    std::printf("combining    : %7.1f MB/s\n", feedMBps(comb, target));
    std::printf("emoji        : %7.1f MB/s\n", feedMBps(emoji, target));
    std::printf("diverse_symbols: %7.1f MB/s   (vtebench unicode/symbols)\n", feedMBps(symbols, target));
    return 0;
}
