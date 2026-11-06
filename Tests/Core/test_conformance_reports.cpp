// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::drain;
using UbiquittyTest::feed;

// REPORTS (Source/Core/VT/Reports.cpp) — the exact wire forms of DA1, DECDSR,
// DECRQSS and DECRQTSR (https://vt100.net/docs/,
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html). A report whose bytes
// drift by one character is as broken as no report at all, and a *missing*
// report costs the requesting host its whole read timeout.

#include "Core/ReplyBuilder.h"

#include "Core/TerminalOperations.h"
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

// Defined in Source/Core/VT/Reports.cpp. Declared here rather than pulled in
// from Core/VT/Internal.h so this file stays a black-box wire-format test.

namespace Ubiquitty::VT {
bool emitDECDSRFixedStatus(Ubiquitty::Terminal& t, int ps, int pid);
int decrqssSettingReport(const Ubiquitty::Terminal& t, std::string_view selector, Ubiquitty::ReplyBuilder& output);
} // namespace Ubiquitty::VT

int runReportsConformanceTests();

// One DECRQSS selector through decrqssSettingReport, compared against the body
// that belongs between the `1$r` header and the ST.
static void checkSetting(Ubiquitty::Terminal& t, const char* selector, const char* expected)
{
    Ubiquitty::ReplyBuilder body;
    const int n = Ubiquitty::VT::decrqssSettingReport(t, selector, body);
    const int expectedLength = static_cast<int>(strlen(expected));
    const std::string_view written { body.span().data(), body.span().size() };
    CHECK(n == expectedLength);
    CHECK(written == std::string_view { expected });
    if (n != expectedLength || written != std::string_view { expected })
        printf("  selector \"%s\": got \"%.*s\", expected \"%s\"\n", selector,
            static_cast<int>(written.size()), written.data(), expected);
}

// The frame the builder writes into, read off the class rather than repeated.
static int replyFrameSize()
{
    Ubiquitty::ReplyBuilder reply;
    for (int i = 0; i < 4096; ++i)
        reply.put('x');
    return reply.length();
}

// DA1 must advertise exactly what Ubiquitty implements: every extension number
// in the reply is backed by a report that actually arrives, because a host that
// trusts the list and then gets silence pays a full read timeout for it.
static void testDa1AdvertisesImplementedCapabilities()
{
    Ubiquitty::Terminal t;
    char outputBuffer[80];
    const char* expected = "\033[?65;1;2;4;6;7;8;9;15;16;17;18;21;22;28;29c";
    Ubiquitty::hardReset(t);
    feed(t, "\033[c");
    int n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, expected) == 0);
    CHECK(n == static_cast<int>(strlen(expected)));
    CHECK(outputBuffer[2] == '?');
    CHECK(outputBuffer[n - 1] == 'c');

    // CSI 0 c is the same request and must produce the identical reply.
    feed(t, "\033[0c");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, expected) == 0);
}

/// DECRQTSR Ps=1 reports DECTSR, and feeding the same body back as DECRSTS must
/// land the terminal on exactly the state that was reported. The format is
/// device-specific by DEC STD 070, so round-trip fidelity is its only contract:
/// drift between the encoder and the decoder is the failure this pins.
static void testDECTSRRoundTripsThroughDECRSTS()
{
    Ubiquitty::Terminal t;
    char saved[96];
    char outputBuffer[96];
    char restore[128];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?6h\033[?7l\033[4h\033[2;20r"); // DECOM, DECAWM off, IRM, DECSTBM
    feed(t, "\033[1$u");
    int n = drain(t, saved);
    CHECK(n > 7);
    CHECK(strncmp(saved, "\033P1$s1;65;1;", 12) == 0);
    CHECK(n > 1 && saved[n - 2] == '\033' && saved[n - 1] == '\\');

    // Perturb every field the report carries, then restore from the saved body
    // (between the DCS 1 $ s header and the ST).
    feed(t, "\033[?6l\033[?7h\033[4l\033[r");
    snprintf(restore, sizeof(restore), "\033P1$p%.*s\033\\", n - 7, saved + 5);
    feed(t, restore);
    feed(t, "\033[1$u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, saved) == 0);

    // Ps values DEC leaves undefined, and a malformed body, must change nothing.
    feed(t, "\033[$u\033[0$u\033[2$u");
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033P1$p1;65;1;bogus\033\\");
    feed(t, "\033[1$u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, saved) == 0);
}

// The device class tracks DECSCL, and VT100 level keeps the classic
// "no options" short form instead of the capability list.
static void testDa1DeviceClassTracksConformanceLevel()
{
    Ubiquitty::Terminal t;
    char outputBuffer[80];
    Ubiquitty::hardReset(t);
    feed(t, "\033[63;1\"p"); // DECSCL VT300
    feed(t, "\033[c");
    drain(t, outputBuffer);
    CHECK(strncmp(outputBuffer, "\033[?63;1;2;4;", 12) == 0);

    feed(t, "\033[61;1\"p"); // DECSCL VT100
    feed(t, "\033[c");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1;2c") == 0);
}

// Each of these DECDSR selectors must answer, and answer byte for byte — some
// reply as CSI, some as DCS.
static void testDECDSRFixedStatuses()
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    CHECK(Ubiquitty::VT::emitDECDSRFixedStatus(t, 62, 0)); // DECMSR — macro space
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[0*{") == 0);

    CHECK(Ubiquitty::VT::emitDECDSRFixedStatus(t, 63, 123)); // DECCKSR — macro checksum
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P123!~0000\033\\") == 0);

    CHECK(Ubiquitty::VT::emitDECDSRFixedStatus(t, 75, 0)); // data integrity
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?70n") == 0);

    CHECK(Ubiquitty::VT::emitDECDSRFixedStatus(t, 85, 0)); // multiple session status
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?83n") == 0);
}

// A selector this helper does not own must stay silent so the caller's own
// chain (and its "unknown ? Ps: no reply" rule) still decides.
static void testDECDSRUnownedSelectorIsSilent()
{
    Ubiquitty::Terminal t;
    char outputBuffer[32];
    Ubiquitty::hardReset(t);
    CHECK(!Ubiquitty::VT::emitDECDSRFixedStatus(t, 6, 0)); // DECXCPR — owned by the DSR handler
    CHECK(!Ubiquitty::VT::emitDECDSRFixedStatus(t, 99, 0));
    CHECK(drain(t, outputBuffer) == 0);
}

// Pid is echoed verbatim into a bare numeric DCS field; a negative parameter
// can never reach the wire as `-1`.
static void testDECDSRChecksumProcessIDStaysNumeric()
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::VT::emitDECDSRFixedStatus(t, 63, -1));
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P0!~0000\033\\") == 0);
}

// DECSLPP and DECSNLS both report the live page geometry, which `CSI Ps t`
// (Ps >= 24) moves.
static void testDECRQSSPageGeometryReports()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.rows == Ubiquitty::defaultRows);
    checkSetting(t, "t", "24t");
    checkSetting(t, "*|", "24*|");

    feed(t, "\033[27t"); // DECSLPP — 27 lines per page
    CHECK(t.rows == 27);
    checkSetting(t, "t", "27t");
    checkSetting(t, "*|", "27*|");
}

// DECSACE Ps 0 and Ps 1 both select the stream extent, so the report has one
// spelling for it: 0.
static void testDECRQSSDECSACEReport()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    checkSetting(t, "*x", "0*x");
    feed(t, "\033[2*x"); // rectangle
    CHECK(t.modes.sace == 2);
    checkSetting(t, "*x", "2*x");
    feed(t, "\033[0*x"); // stream, spelled 0
    CHECK(t.modes.sace == 1);
    checkSetting(t, "*x", "0*x");
    feed(t, "\033[1*x"); // stream, spelled 1 — same setting, same report
    checkSetting(t, "*x", "0*x");
}

static void testDECRQSSStatusDisplayReports()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    checkSetting(t, "$~", "0$~"); // DECSSDT — no status line
    checkSetting(t, "$}", "0$}"); // DECSASD — data screen is active

    feed(t, "\033[2$~"); // host-writable status line
    checkSetting(t, "$~", "2$~");
    feed(t, "\033[1$}"); // direct output to the status line
    checkSetting(t, "$}", "1$}");
    feed(t, "\033[0$}");
    checkSetting(t, "$}", "0$}");
}

// An unowned selector, and a caller buffer too small for the body, both mean
// "no value" — the caller then sends DECRQSS's DCS 0 $ r ST invalid form
// rather than a truncated setting.
static void testDECRQSSUnknownAndUndersizedYieldNothing()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    {
        Ubiquitty::ReplyBuilder body;
        CHECK(Ubiquitty::VT::decrqssSettingReport(t, "z", body) == 0);
        CHECK(Ubiquitty::VT::decrqssSettingReport(t, "*", body) == 0);
        CHECK(Ubiquitty::VT::decrqssSettingReport(t, "$q", body) == 0);
        CHECK(Ubiquitty::VT::decrqssSettingReport(t, "", body) == 0);
        // A selector that owns nothing wrote nothing, so the reply is still empty.
        CHECK(body.length() == 0);
    }
    {
        // A frame with less room than the body needs: "24t" wants three bytes.
        Ubiquitty::ReplyBuilder body;
        const std::string nearlyFull(static_cast<std::size_t>(replyFrameSize() - 2), 'x');
        body.put(std::string_view { nearlyFull });
        CHECK(Ubiquitty::VT::decrqssSettingReport(t, "t", body) == 0);
        CHECK(body.isTruncated());
    }
    {
        // A frame with no room at all.
        Ubiquitty::ReplyBuilder body;
        const std::string full(static_cast<std::size_t>(replyFrameSize()), 'x');
        body.put(std::string_view { full });
        CHECK(Ubiquitty::VT::decrqssSettingReport(t, "t", body) == 0);
        CHECK(body.isTruncated());
    }
}

int runReportsConformanceTests()
{
    testDa1AdvertisesImplementedCapabilities();
    testDECTSRRoundTripsThroughDECRSTS();
    testDa1DeviceClassTracksConformanceLevel();
    testDECDSRFixedStatuses();
    testDECDSRUnownedSelectorIsSilent();
    testDECDSRChecksumProcessIDStaysNumeric();
    testDECRQSSPageGeometryReports();
    testDECRQSSDECSACEReport();
    testDECRQSSStatusDisplayReports();
    testDECRQSSUnknownAndUndersizedYieldNothing();

    if (failures) {
        printf("%d conformance-reports test failure(s)\n", failures);
        return 1;
    }
    printf("conformance reports tests passed\n");
    return 0;
}

// Defined out when several cluster conformance files are linked into one
// binary; that driver supplies main and calls the run*Tests entry points.
#ifndef UBIQUITTY_CONFORMANCE_TESTS_COMBINED
int main() { return runReportsConformanceTests() == 0 ? 0 : 1; }
#endif
