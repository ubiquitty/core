// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The shell-integration cluster, asserted in one place.
//
// The user-testing sweep filed OSC 133, 633, 1337 and 7 separately as "shows
// nothing", and its epic asked whether they share one cause before anyone fixes
// them one at a time. They do not: Core parses, dispatches and records all
// four. Whatever is missing is downstream of here.
//
// So this case exists to keep that answer true. Each of the three routes that
// can set a working directory is fed and read back, which is the property a
// frontend depends on and which nothing else asserted across the cluster --
// each protocol had its own tests, and none of them compared the routes.
//
// What it does not cover is a frontend displaying any of this: a green
// here says Core did its part.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>
#include <string_view>

// The fixture exposes cells, not rows, so the row is assembled here. Only the
// printable low range matters for these assertions.
static std::string rowText(const Ubiquitty::Terminal& t, int row, int columns)
{
    std::string text;
    for (int column = 0; column < columns; ++column) {
        const Ubiquitty::Cell* c = UbiquittyTest::cell(t, row, column);
        if (!c)
            break;
        const char32_t code = c->codePoint;
        text.push_back(code >= 0x20 && code < 0x7F ? static_cast<char>(code) : ' ');
    }
    return text;
}

TEST(everyWorkingDirectoryRouteReachesTheTerminal)
{
    // OSC 7 -- the original, file:// scheme.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::hardReset(t);
        UbiquittyTest::feed(t, "\033]7;file:///tmp/osc7-route\033\\");
        CHECK(t.currentDirectory == "/tmp/osc7-route");
        // Latched for the frontend to consume, which is how a title bar learns
        // it changed without polling.
        CHECK(t.currentDirectoryChanged);
    }

    // OSC 1337 CurrentDir -- iTerm2's spelling, a bare path with no scheme.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::hardReset(t);
        UbiquittyTest::feed(t, "\033]1337;CurrentDir=/tmp/iterm-route\033\\");
        CHECK(t.currentDirectory == "/tmp/iterm-route");
    }

    // OSC 633 P;Cwd -- VS Code's spelling, a property list.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::hardReset(t);
        UbiquittyTest::feed(t, "\033]633;P;Cwd=/tmp/vscode-route\033\\");
        CHECK(t.currentDirectory == "/tmp/vscode-route");
    }
}

TEST(theOSC133MarkSequenceIsAccepted)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);

    // A whole command cycle: prompt, input, output, exit status. Fed as a shell
    // emits it rather than as four isolated marks, because the ordering is the
    // part a consumer relies on.
    UbiquittyTest::feed(t, "\033]133;A\033\\");
    UbiquittyTest::feed(t, "prompt$ ");
    UbiquittyTest::feed(t, "\033]133;B\033\\");
    UbiquittyTest::feed(t, "echo hi\r\n");
    UbiquittyTest::feed(t, "\033]133;C\033\\");
    UbiquittyTest::feed(t, "hi\r\n");
    UbiquittyTest::feed(t, "\033]133;D;0\033\\");

    // The marks must not reach the screen as text. A terminal that printed the
    // payload would show "A", "B", "C" scattered through the transcript, which
    // is the visible form of "the sequence was not understood".
    const std::string firstRow = rowText(t, 0, 80);
    CHECK(firstRow.find("133") == std::string::npos);
    CHECK(firstRow.find("prompt$") != std::string::npos);
}

// A report belongs to whoever held the terminal after its bytes, not to the
// last probe's reading. `ssh example-host` is typed at the local shell, so the
// last probe names that shell; the remote shell's first prompt reports its
// directory and host before any probe has seen ssh take the terminal. Stamped
// with the stale reading, both would be retired as outlived by the probe that
// finds ssh, and the pane would name the local directory and no host until the
// next prompt.
TEST(aReportParsedBeforeTheProbeSeesItsProgramBelongsToThatProgram)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    Ubiquitty::setSampledDirectoryString(t, "/local/home");
    t.foregroundProcessGroup = 40; // the last probe ran while the local shell held the terminal
    UbiquittyTest::feed(t, "\033]7;file://example-host/srv/app\033\\");
    CHECK(t.currentDirectory == "/srv/app");
    CHECK(t.reportedHost == "example-host");

    // The probe after the bytes finds ssh holding the terminal.
    CHECK_MSG(!Ubiquitty::retireReportedDirectoryIfStale(t, 77), "the directory is ssh's, which still holds the terminal");
    CHECK(t.currentDirectory == "/srv/app");
    CHECK_MSG(!Ubiquitty::retireReportedHostIfStale(t, 77), "and so is the host");
    CHECK(t.reportedHost == "example-host");

    // Once ssh exits, both retire and the sample stands in again.
    CHECK(Ubiquitty::retireReportedDirectoryIfStale(t, 40));
    CHECK(t.currentDirectory == "/local/home");
    CHECK(Ubiquitty::retireReportedHostIfStale(t, 40));
    CHECK(t.reportedHost.empty());
}

UBIQUITTY_TEST_MAIN()
