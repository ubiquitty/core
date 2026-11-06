// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Ingest-side replay of the ubench generator's dominant scenario shapes,
// through the real parser (Terminal::processBytes) like
// Tests/Core/bench_sgr.cpp. The generator lives in the separate ubench
// repository that Scripts/run-parse-scenarios.sh builds from UBENCH_DIR; the
// byte streams here mirror it so CPU-side wins translate to the end-to-end
// harness.
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static double feedMBps(const std::string& chunk, long target, int rows = 24, int columns = 80)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    if (rows > 0 && columns > 0 && (rows != t.rows || columns != t.columns))
        Ubiquitty::resize(t, rows, columns);
    const char* buffer = chunk.data();
    const int length = (int)chunk.size();
    Ubiquitty::processBytes(&t, std::string_view(buffer, static_cast<std::size_t>(length))); // warm up
    long fed = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (fed < target) {
        Ubiquitty::processBytes(&t, std::string_view(buffer, static_cast<std::size_t>(length)));
        fed += length;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    return (double)fed / 1e6 / secs;
}

static std::string cup(int row, int column) { return "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H"; }

int main(int argc, char** argv)
{
    // Optional: argv[1] selects a single scenario; argv[2] multiplies the
    // byte target (useful to keep one scenario hot under a profiler).
    const char* only = argc > 1 ? argv[1] : nullptr;
    const long targetMultiplier = argc > 2 ? std::atol(argv[2]) : 1;
    const long target = 60L * 1024 * 1024 * (targetMultiplier > 0 ? targetMultiplier : 1);
    const int columns = 80, rows = 24;

    // dense_sgr256: per-row CUP, then per cell a 256-color background + space.
    std::string dense256;
    for (int f = 0; f < 4; ++f) {
        dense256 += "\x1b[H";
        for (int y = 0; y < rows; ++y) {
            dense256 += cup(y + 1, 1);
            for (int x = 0; x < columns; ++x)
                dense256 += "\x1b[48;5;" + std::to_string(16 + (x + y + f) % 216) + "m ";
        }
        dense256 += "\x1b[0m";
    }

    // dense_truecolor: same with 24-bit backgrounds.
    std::string denseTc;
    for (int f = 0; f < 4; ++f) {
        denseTc += "\x1b[H";
        for (int y = 0; y < rows; ++y) {
            denseTc += cup(y + 1, 1);
            for (int x = 0; x < columns; ++x) {
                int r = (x * 4 + f) & 0xFF, g = (y * 8 + f) & 0xFF, b = (x + y + f) & 0xFF;
                denseTc += "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m ";
            }
        }
        denseTc += "\x1b[0m";
    }

    // sgr_churn: every cell flips attribute + fg + bg in one combined SGR.
    std::string churn;
    for (int f = 0; f < 4; ++f) {
        churn += "\x1b[H";
        for (int y = 0; y < rows; ++y) {
            churn += cup(y + 1, 1);
            for (int x = 0; x < columns; ++x) {
                static const int attributes[4] = { 0, 1, 4, 7 };
                int a = attributes[(x + y + f) & 3];
                int fg = 16 + (x * 3 + f) % 216, bg = 16 + (y * 5 + f) % 216;
                churn += "\x1b[" + std::to_string(a) + ";38;5;" + std::to_string(fg) + ";48;5;" + std::to_string(bg) + "m";
                churn += (char)('A' + (x + y) % 26);
            }
        }
        churn += "\x1b[0m";
    }

    // cursor_random: CUP + 256-color bg + one glyph, LCG-addressed.
    std::string cursorRandom;
    unsigned long long state = 0x12345;
    for (int i = 0; i < 4096; ++i) {
        state = (state * 1103515245ULL + 12345ULL) & 0x7FFFFFFF;
        int r = (int)((state >> 8) % rows) + 1;
        int c = (int)((state >> 16) % columns) + 1;
        cursorRandom += cup(r, c) + "\x1b[48;5;" + std::to_string(16 + i % 216) + "m" + (char)('@' + i % 26);
    }
    cursorRandom += "\x1b[0m";

    // pager-style full-screen redraw: per row CUP + EL + ASCII text.
    std::string pager;
    for (int f = 0; f < 4; ++f) {
        for (int y = 0; y < rows; ++y) {
            pager += cup(y + 1, 1) + "\x1b[K";
            for (int x = 0; x < columns - 10; ++x)
                pager += (char)('a' + (x + y + f) % 26);
        }
    }

    // scroll_plain-style: full-width text lines streamed with CRLF.
    std::string scroll;
    for (int line = 0; line < 100; ++line) {
        for (int x = 0; x < columns; ++x)
            scroll += (char)('a' + (x + line) % 26);
        scroll += "\r\n";
    }

    // scroll_sgr_runs-style: log lines with a few stable color spans.
    std::string scrollSGR;
    for (int line = 0; line < 100; ++line) {
        scrollSGR += "\x1b[38;5;245m12:00:0" + std::to_string(line % 10) + "\x1b[0m \x1b[1;38;5;81mINFO\x1b[0m worker thread pool size adjusted ok\r\n";
    }

    // kitty_random_images-style: small RGBA images transmitted + placed via
    // the kitty graphics APC protocol (f=32 raw, base64, 4096-char chunks,
    // 8 live image ids recycled with deletes) — mirrors the ubench
    // generator's kitty_random_images byte shape.
    std::string kittyImages;
    {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        unsigned long long lcg = 0x4d61635465726dULL;
        const int side = 32, imageBytes = side * side * 4;
        std::string pixels(imageBytes, '\0'), payload;
        for (int id = 0; id < 256; ++id) {
            for (int j = 0; j < imageBytes; ++j) {
                lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
                pixels[j] = (char)(lcg >> 32);
            }
            for (int j = 3; j < imageBytes; j += 4)
                pixels[j] = (char)0xFF;
            const long imageID = 100000 + (id % 8);
            const int row = 1 + (id * 3) % (rows - 2 + 1);
            const int col = 1 + (id * 7) % (columns - 4 + 1);
            if (id >= 8)
                kittyImages += "\x1b_Ga=d,d=I,i=" + std::to_string(imageID) + ",q=2;\x1b\\";
            kittyImages += cup(row, col);
            payload.clear();
            for (int j = 0; j + 3 <= imageBytes; j += 3) {
                unsigned n = ((unsigned char)pixels[j] << 16) | ((unsigned char)pixels[j + 1] << 8) | (unsigned char)pixels[j + 2];
                payload += b64[(n >> 18) & 63];
                payload += b64[(n >> 12) & 63];
                payload += b64[(n >> 6) & 63];
                payload += b64[n & 63];
            } // imageBytes % 3 == 1 -> 2-pad tail
            {
                unsigned n = (unsigned char)pixels[imageBytes - 1] << 16;
                payload += b64[(n >> 18) & 63];
                payload += b64[(n >> 12) & 63];
                payload += "==";
            }
            for (size_t off = 0, first = 1; off < payload.size(); first = 0) {
                size_t end = std::min(off + (size_t)4096, payload.size());
                int more = end < payload.size() ? 1 : 0;
                if (first)
                    kittyImages += "\x1b_Ga=T,t=d,f=32,s=32,v=32,i=" + std::to_string(imageID) + ",p=" + std::to_string(imageID) + ",c=4,r=2,C=1,q=2,m=" + std::to_string(more) + ";";
                else
                    kittyImages += "\x1b_Gm=" + std::to_string(more) + ";";
                kittyImages.append(payload, off, end - off);
                kittyImages += "\x1b\\";
                off = end;
            }
        }
        for (int slot = 0; slot < 8; ++slot)
            kittyImages += "\x1b_Ga=d,d=I,i=" + std::to_string(100000 + slot) + ",q=2;\x1b\\";
    }

    // variation_selector-style: dual-presentation bases with alternating
    // VS16/VS15, the per-cell cluster + presentation-width path.
    std::string variationSelector;
    {
        static const char* bases[] = { "#", "*", "0", "1", "2", "\xc2\xa9", "\xc2\xae", "\xe2\x80\xbc", "\xe2\x81\x89", "\xe2\x84\xa2", "\xe2\x84\xb9", "\xe2\x86\x94", "\xe2\x86\x95", "\xe2\x86\xa9", "\xe2\x86\xaa", "\xe2\x8c\x9a", "\xe2\x8c\x9b", "\xe2\x8f\xa9", "\xe2\x8f\xaa", "\xe2\x96\xaa",
            "\xe2\x98\x80", "\xe2\x98\x81", "\xe2\x98\x8e", "\xe2\x98\x91", "\xe2\x98\x94", "\xe2\x99\xa0", "\xe2\x99\xa3", "\xe2\x99\xa5", "\xe2\x99\xa6", "\xe2\x99\xa8" };
        const char* vs15 = "\xef\xb8\x8e";
        const char* vs16 = "\xef\xb8\x8f";
        for (int f = 0; f < 4; ++f) {
            variationSelector += "\x1b[H";
            for (int y = 0; y < rows; ++y) {
                variationSelector += cup(y + 1, 1);
                for (int x = 0; x < columns; ++x) {
                    variationSelector += bases[(x + y + f) % 30];
                    variationSelector += ((x + y) % 2 == 0) ? vs16 : vs15;
                }
            }
        }
    }
    // truecolor_text (in-app sweep geometry, 202x49): per-cell 24-bit
    // foreground + one ASCII glyph, full lines streamed with CRLF (scrolls).
    // Mirrors the ubench generator's truecolor_text.
    const int sweepColumns = 202, sweepRows = 49;
    std::string truecolorText;
    {
        const int lineCells = sweepColumns - 1;
        for (long i = 0; i < 128; ++i) {
            for (int x = 0; x < lineCells; ++x) {
                long r = (x * 3 + i) & 0xFF, g = (x * 5 + i) & 0xFF, b = (x * 7 + i) & 0xFF;
                truecolorText += "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
                truecolorText += (char)('A' + (x + i) % 26);
            }
            truecolorText += "\x1b[0m\r\n";
        }
    }

    // sgr_churn at the in-app sweep geometry (202x49).
    std::string churnSweep;
    for (long f = 0; f < 4; ++f) {
        churnSweep += "\x1b[H";
        for (int y = 0; y < sweepRows; ++y) {
            churnSweep += cup(y + 1, 1);
            for (int x = 0; x < sweepColumns; ++x) {
                static const int attributes[4] = { 0, 1, 4, 7 };
                long a = attributes[(x + y + f) & 3];
                long fg = 16 + (x * 3 + f) % 216, bg = 16 + (y * 5 + f) % 216;
                churnSweep += "\x1b[" + std::to_string(a) + ";38;5;" + std::to_string(fg) + ";48;5;" + std::to_string(bg) + "m";
                churnSweep += (char)('A' + (x + y) % 26);
            }
        }
        churnSweep += "\x1b[0m";
    }

    // zwj_storm-style: every visible cell is a multi-codepoint ZWJ emoji
    // cluster (families, skin-tone handshakes, flag sequences) — mirrors
    // The ubench generator's zwj_storm byte shape at the in-app sweep's 202x49.
    const int zwjColumns = 202, zwjRows = 49;
    std::string zwjStorm;
    long zwjCells = 0;
    {
        static const char* sequences[] = {
            "\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466", // family MWGB
            "\U0001F469\u200D\U0001F469\u200D\U0001F466\u200D\U0001F466", // family WWBB
            "\U0001F9D1\U0001F3FB\u200D\U0001F91D\u200D\U0001F9D1\U0001F3FD", // handshake skintones
            "\U0001F468\U0001F3FF\u200D\U0001F4BB", // dark-skin technologist
            "\U0001F469\U0001F3FC\u200D\u2764\uFE0F\u200D\U0001F468\U0001F3FE", // couple, mixed tones
            "\U0001F3F3\uFE0F\u200D\U0001F308", // pride flag
            "\U0001F3F4\u200D\u2620\uFE0F", // pirate flag
            "\U0001F9D1\u200D\U0001F9B2", // bald person
        };
        const int half = zwjColumns / 2;
        for (int f = 0; f < 4; ++f) {
            zwjStorm += "\x1b[H";
            for (int y = 0; y < zwjRows; ++y) {
                zwjStorm += cup(y + 1, 1);
                for (int x = 0; x < half; ++x)
                    zwjStorm += sequences[(x + y + f) % 8];
            }
            zwjCells += (long)half * zwjRows;
        }
    }

    struct {
        const char* name;
        std::string* s;
        int rows = 24;
        int columns = 80;
        double bytesPerCell = 0; // 0 = byte throughput only
    } S[] = {
        { "dense_sgr256", &dense256 },
        { "dense_truecolor", &denseTc },
        { "sgr_churn", &churn },
        { "cursor_random", &cursorRandom },
        { "pager_redraw", &pager },
        { "scroll_plain", &scroll },
        { "scroll_sgr_runs", &scrollSGR },
        { "kitty_images", &kittyImages },
        { "variation_selector", &variationSelector },
        { "truecolor_text", &truecolorText, sweepRows, sweepColumns, (double)truecolorText.size() / (128.0 * (sweepColumns - 1)) },
        { "sgr_churn_sweep", &churnSweep, sweepRows, sweepColumns, (double)churnSweep.size() / (4.0 * sweepRows * sweepColumns) },
    };
    std::printf("%-18s %10s %10s\n", "scenario", "MB/s", "Mcells/s");
    for (auto& sc : S) {
        if (only && std::strcmp(only, sc.name) != 0)
            continue;
        double mbps = feedMBps(*sc.s, target, sc.rows, sc.columns);
        if (sc.bytesPerCell > 0)
            std::printf("%-18s %10.1f %10.2f\n", sc.name, mbps, mbps / sc.bytesPerCell);
        else
            std::printf("%-18s %10.1f %10s\n", sc.name, mbps, "-");
    }
    if (!only || std::strcmp(only, "zwj_storm") == 0) {
        const double megabytesPerSecond = feedMBps(zwjStorm, target, zwjRows, zwjColumns);
        const double cellsPerByte = (double)zwjCells / (double)zwjStorm.size();
        std::printf("%-18s %10.1f  (%.2f Mcells/s)\n", "zwj_storm", megabytesPerSecond, megabytesPerSecond * cellsPerByte);
    }
    return 0;
}
