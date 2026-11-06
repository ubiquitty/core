// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// No input shape costs more than linear time in its own dimension.
//
// Every throughput lane feeds a fixed byte count, so a path whose cost grows
// with the length of the thing it is parsing — a line, an OSC payload, a
// parameter list, a combining stack — reads there as merely slow. A quadratic
// one would too, right up until a program emits a long enough one and the
// terminal stops responding.
//
// Each case feeds one total byte count at two dimensions and compares the times.
// A path linear in total bytes holds flat; one quadratic in the dimension climbs
// by the ratio between them. The dimensions are 4x apart, so a linear path reads
// near 1 and a quadratic one near 4, and the bound sits at 2 with a doubling of
// room to either side. The times are the thread's CPU time, not the wall's: the
// runners share a box, and a wall-clock ratio counts the neighbours' turns.

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "ThreadCPUClock.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace {

constexpr std::size_t totalBytes = 512u * 1024u;
// Both dimensions are large on purpose. A small one is dominated by per-command
// setup — at 16 bytes a payload the same total carries 64x more commands than at
// 1024 — and that falls as the dimension grows while a superlinear payload cost
// rises, so the two cancel and the comparison says nothing: a quadratic spliced
// into the OSC scan can even read as faster at the larger dimension.
constexpr std::size_t smallDimension = 1024;
constexpr std::size_t largeDimension = 4096;
// Quadratic across this 4x span reads as 4x and a linear path reads as ~1, so
// two sits with margin on both sides. A quadratic spliced into the OSC scan
// reads above it here.
constexpr double superlinearRatio = 2.0;

double bestSecondsFor(const std::string& input)
{
    double best = 1e9;
    for (int repeat = 0; repeat < 5; ++repeat) {
        auto term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*term, 50, 200);
        const double start = UbiquittyTest::threadCPUSeconds();
        Ubiquitty::processBytes(term.get(), std::string_view(input));
        best = std::min(best, UbiquittyTest::threadCPUSeconds() - start);
    }
    return best;
}

std::string repeatTo(const std::string& unit, std::size_t bytes)
{
    std::string out;
    out.reserve(bytes + unit.size());
    while (out.size() < bytes)
        out += unit;
    return out;
}

std::string longLine(std::size_t lineLength) { return repeatTo(std::string(lineLength, 'x') + "\r\n", totalBytes); }
std::string oscPayload(std::size_t length) { return repeatTo("\x1b]0;" + std::string(length, 'T') + "\x1b\\", totalBytes); }

std::string sgrParameters(std::size_t count)
{
    std::string unit = "\x1b[";
    for (std::size_t i = 0; i < count; ++i)
        unit += (i ? ";1" : "1");
    return repeatTo(unit + "m", totalBytes);
}

std::string combiningMarks(std::size_t count)
{
    std::string unit = "e";
    for (std::size_t i = 0; i < count; ++i)
        unit += "\xcc\x81"; // U+0301 COMBINING ACUTE ACCENT
    return repeatTo(unit, totalBytes);
}

void checkLinearInItsOwnDimension(const char* name, std::string (*build)(std::size_t))
{
    const double small = bestSecondsFor(build(smallDimension));
    const double large = bestSecondsFor(build(largeDimension));
    const double ratio = small > 0.0 ? large / small : 0.0;
    char message[192];
    std::snprintf(message, sizeof(message),
        "%s: %zu wide took %.2fms, %zu wide took %.2fms (x%.2f, superlinear past x%.1f)",
        name, smallDimension, small * 1e3, largeDimension, large * 1e3, ratio, superlinearRatio);
    CHECK_MSG(ratio < superlinearRatio, message);
}

TEST(noInputShapeIsSuperlinearInItsOwnDimension)
{
    checkLinearInItsOwnDimension("long line", longLine);
    checkLinearInItsOwnDimension("OSC payload", oscPayload);
    checkLinearInItsOwnDimension("SGR parameters", sgrParameters);
    checkLinearInItsOwnDimension("combining marks", combiningMarks);
}

} // namespace

UBIQUITTY_TEST_MAIN()
