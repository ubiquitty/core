// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sgr_combined_parity.cpp — differential fuzz for SGR::apply's combined
// fast paths. Property: applying a combined SGR (`P1;P2;...;Pn`) equals
// applying the same well-formed parameter GROUPS as separate CSIs in order —
// SGR folding is defined as left-to-right group application on one pen. The
// combined form is what the fast paths intercept (`38;5;A;48;5;B`,
// `X;38;5;A;48;5;B`); the split form goes through the standalone paths and
// the general loop, so any divergence in the interception is a pen mismatch.
// Groups are kept whole (an extended-color spec or colon run never splits),
// because splitting INSIDE a group genuinely changes meaning.
//
// Near-miss shapes (out-of-range index, colon-joined groups, same-plane
// pairs, colon sub-style leads) are generated on purpose: they must fall
// through to the general loop and still match the split reference.
//
//   c++ -std=c++23 -O2 -ISource Tests/Core/test_sgr_combined_parity.cpp
//       <core objects...> -o /tmp/sgrpar && /tmp/sgrpar
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

// xorshift64 — deterministic corpus.
static uint64_t g = 0x2545F4914F6CDD1Dull;
static uint32_t rnd()
{
    g ^= g << 13;
    g ^= g >> 7;
    g ^= g << 17;
    return static_cast<uint32_t>(g >> 32);
}

// One well-formed SGR parameter group (kept whole when splitting).
static std::string makeGroup()
{
    switch (rnd() % 16) {
    case 15: // frame family, plus a long colon run: six parameters with an
             // empty colorspace slot, which must stay one whole group
        switch (rnd() % 3) {
        case 0:
            return "51";
        case 1:
            return "54";
        default:
            return std::string("58:2::") + std::to_string(rnd() % 256) + ":" + std::to_string(rnd() % 256) + ":" + std::to_string(rnd() % 256);
        }
    case 14:
        return "52";
    case 0:
        return "0";
    case 1: { // simple attribute set/clear
        static const char* codes[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "22", "23", "24", "25", "27", "28", "29", "53", "55", "39", "49", "59" };
        return codes[rnd() % (sizeof(codes) / sizeof(codes[0]))];
    }
    case 2: // basic / bright colors
        return std::to_string(30 + (rnd() % 8));
    case 3:
        return std::to_string(40 + (rnd() % 8));
    case 4:
        return std::to_string(90 + (rnd() % 8));
    case 5:
        return std::to_string(100 + (rnd() % 8));
    case 6: { // indexed extended color, semicolon or colon spelling
        const char* intro = (rnd() & 1) ? "38" : "48";
        const char sep = (rnd() & 1) ? ';' : ':';
        // Mostly valid indexes; sometimes out-of-range (must fall through
        // identically: no color stored, group still consumed).
        const int index = (rnd() % 8 == 0) ? 256 + static_cast<int>(rnd() % 700) : static_cast<int>(rnd() % 256);
        return std::string(intro) + sep + "5" + sep + std::to_string(index);
    }
    case 7: { // truecolor extended color
        const char* intro = (rnd() & 1) ? "38" : "48";
        const char sep = (rnd() & 1) ? ';' : ':';
        std::string spec = std::string(intro);
        for (int k = 0; k < 4; ++k)
            spec += sep + std::to_string(k == 0 ? 2 : rnd() % 256);
        return spec;
    }
    case 8: { // underline color (58) — same grammar as 38/48
        const char sep = (rnd() & 1) ? ';' : ':';
        return std::string("58") + sep + "5" + sep + std::to_string(rnd() % 256);
    }
    case 9: // underline style selector 4:n
        return std::string("4:") + std::to_string(rnd() % 7);
    case 10: // variable weight 1:n
        return std::string("1:") + std::to_string(rnd() % 8);
    case 11: // ideogram family
        return std::to_string(60 + (rnd() % 6));
    case 12: // emoji presentation forcing
        return std::string("76:") + std::to_string(rnd() % 3);
    case 13: { // pen fields no other group reaches: font selection (10-20),
               // double underline (21), proportional spacing (26/50), script (73-75)
        static const char* codes[] = { "10", "11", "19", "20", "21", "26", "50", "73", "74", "75" };
        return codes[rnd() % (sizeof(codes) / sizeof(codes[0]))];
    }
    }
    return "0";
}

static void feed(Ubiquitty::Terminal& t, const std::string& s)
{
    Ubiquitty::processBytes(&t, s);
}

static bool pensEqual(Ubiquitty::Terminal& a, Ubiquitty::Terminal& b)
{
    return std::memcmp(&a.screens.active->pen, &b.screens.active->pen, sizeof(Ubiquitty::Pen)) == 0;
}

int main()
{
    Ubiquitty::Terminal combined;
    Ubiquitty::Terminal split;
    Ubiquitty::hardReset(combined);
    Ubiquitty::hardReset(split);

    // Directed: the exact fast-path shapes and their near-misses.
    static const char* directed[] = {
        "38;5;12;48;5;231", // shape A, fg first
        "48;5;231;38;5;12", // shape A, bg first
        "38:5:12;48:5:231", // shape A, colon triples, semicolon join
        "0;38;5;196;48;5;52", // shape B, reset lead
        "1;38;5;196;48;5;52", // shape B, bold lead
        "4;38;5;196;48;5;52", // shape B, underline lead
        "7;38;5;196;48;5;52", // shape B, reverse lead
        "9;38;5;1;48;5;2", // shape B, strike lead
        "6;38;5;1;48;5;2", // lead outside the allowlist -> general loop
        "38;5;300;48;5;2", // out-of-range first index -> general loop
        "38;5;2;48;5;300", // out-of-range second index -> general loop
        "38;5;1;38;5;2", // same-plane pair -> general loop
        "38:5:1:48:5:2", // colon-joined groups: one colon run -> general loop
        "1:3;38;5;9;48;5;8", // colon sub-style lead -> general loop
        "38;2;1;2;3;48;5;7", // truecolor+indexed pc==7 shape mismatch -> general loop
        "0;48;5;16;38;5;255", // shape B with bg-first tail
    };
    for (const char* d : directed) {
        const std::string groups = d;
        feed(combined, std::string("\x1b[") + groups + "m");
        // split at TOP-LEVEL semicolons that start a new group: a ';' that is
        // part of an extended-color spec must stay inside its group, so the
        // directed corpus splits on our own knowledge: rebuild each group by
        // scanning introducers.
        std::string piece;
        std::vector<std::string> parts;
        const char* s = d;
        while (*s) {
            // group = introducer + its known span
            std::string group;
            auto takeInt = [&]() {
                std::string v;
                while (*s && *s != ';' && *s != ':')
                    v.push_back(*s++);
                return v;
            };
            std::string intro = takeInt();
            group = intro;
            if ((intro == "38" || intro == "48" || intro == "58") && *s) {
                const char sep = *s;
                std::string rest;
                // colon spec: swallow the whole colon run; semicolon spec:
                // swallow ;5;N or ;2;r;g;b
                if (sep == ':') {
                    while (*s == ':') {
                        group.push_back(*s++);
                        group += takeInt();
                    }
                } else {
                    const char* save = s;
                    std::string mode;
                    if (*s == ';') {
                        s++;
                        mode = takeInt();
                    }
                    if (mode == "5") {
                        group += ";" + mode;
                        if (*s == ';') {
                            s++;
                            group += ";" + takeInt();
                        }
                    } else if (mode == "2") {
                        group += ";" + mode;
                        for (int k = 0; k < 3 && *s == ';'; ++k) {
                            s++;
                            group += ";" + takeInt();
                        }
                    } else {
                        s = save; // not an extended spec — introducer stands alone
                    }
                }
            } else if (*s == ':') {
                while (*s == ':') { // any other colon sub-style run stays whole
                    group.push_back(*s++);
                    group += takeInt();
                }
            }
            parts.push_back(group);
            if (*s == ';')
                s++;
        }
        for (const std::string& part : parts)
            feed(split, std::string("\x1b[") + part + "m");
        if (!pensEqual(combined, split)) {
            std::printf("FAIL directed: %s\n", d);
            ++failures;
            Ubiquitty::hardReset(combined);
            Ubiquitty::hardReset(split);
        }
    }

    // Fuzz: random well-formed group lists, combined vs split. The combined
    // form must stay within the parser's per-CSI parameter budget
    // (maxParameters = 16) — past it the parser truncates, which makes the
    // combined and split forms LEGITIMATELY diverge.
    const int ITERS = 200000;
    for (int it = 0; it < ITERS; ++it) {
        std::vector<std::string> groups;
        std::string joined;
        // Every 4th iteration: hammer the fast-path shapes themselves —
        // random leads (including out-of-allowlist), random indexes
        // (occasionally out of range), random per-triple spellings.
        if (it % 4 == 0) {
            const bool isBackgroundFirst = rnd() & 1;
            const char sepA = (rnd() & 1) ? ':' : ';';
            const char sepB = (rnd() & 1) ? ':' : ';';
            const int indexA = (rnd() % 10 == 0) ? 256 + static_cast<int>(rnd() % 500) : static_cast<int>(rnd() % 256);
            const int indexB = (rnd() % 10 == 0) ? 256 + static_cast<int>(rnd() % 500) : static_cast<int>(rnd() % 256);
            std::string first = std::string(isBackgroundFirst ? "48" : "38") + sepA + "5" + sepA + std::to_string(indexA);
            std::string second = std::string(isBackgroundFirst ? "38" : "48") + sepB + "5" + sepB + std::to_string(indexB);
            if (rnd() % 3) { // shape B: leading directive 0..10 (10 = outside allowlist)
                groups.push_back(std::to_string(rnd() % 11));
                joined = groups[0] + ";";
            }
            groups.push_back(first);
            groups.push_back(second);
            joined += first + ";" + second;
            feed(combined, "\x1b[" + joined + "m");
            for (const std::string& group : groups)
                feed(split, std::string("\x1b[") + group + "m");
            if (!pensEqual(combined, split)) {
                if (++failures <= 5)
                    std::printf("FAIL shape it=%d: %s\n", it, joined.c_str());
                Ubiquitty::hardReset(combined);
                Ubiquitty::hardReset(split);
            }
            continue;
        }
        const int nGroups = 1 + static_cast<int>(rnd() % 7);
        int totalParameters = 0;
        for (int k = 0; k < nGroups; ++k) {
            std::string group = makeGroup();
            int groupParameters = 1;
            for (char ch : group)
                if (ch == ';' || ch == ':')
                    ++groupParameters;
            if (totalParameters + groupParameters > 16)
                break;
            totalParameters += groupParameters;
            if (k)
                joined += ";";
            joined += group;
            groups.push_back(group);
        }
        if (groups.empty())
            continue;
        feed(combined, "\x1b[" + joined + "m");
        for (const std::string& group : groups)
            feed(split, "\x1b[" + group + "m");
        if (!pensEqual(combined, split)) {
            if (++failures <= 5)
                std::printf("FAIL fuzz it=%d: %s\n", it, joined.c_str());
            Ubiquitty::hardReset(combined);
            Ubiquitty::hardReset(split);
        }
    }

    if (failures == 0) {
        std::printf("sgr combined parity: directed + %d fuzz iterations OK\n", ITERS);
        return 0;
    }
    std::printf("sgr combined parity: %d FAILURES\n", failures);
    return 1;
}
