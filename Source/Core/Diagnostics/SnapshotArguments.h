// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Diagnostics {

/// Describes a headless snapshot. String pointers borrow the launch argument vector.
struct SnapshotArguments {
    bool requested = false;
    bool shouldUsePortableBackend = false;
    std::string outPath; // empty: none given
    int columns = 80;
    int rows = 24;
    // --reflow-from-columns N: the feed is written at N columns, then the grid
    // is resized to `columns` and rewrapped before the frame. 0 = no resize.
    int reflowFromColumns = 0;
    std::string configPath; // empty: no configuration file
    std::string fontName; // empty → the shipped default (JetBrains Mono)
    double fontSize = 0.0; // 0 → renderer default
    std::string themeName; // empty → theme 0
    std::string feed; // bytes fed to the parser
    bool haveFeed = false;
    std::string feedFile; // empty: no feed file
    double scale = 2.0; // backing scale; 2 ≈ Retina
    double paddingX = 4.0; // left+right inset, points
    bool havePaddingX = false;
    double paddingY = 4.0; // top+bottom inset, points
    bool havePaddingY = false;
    double backgroundOpacity = 1.0; // window background alpha, 0.2..1.0
    bool haveBackgroundOpacity = false;
    std::string backgroundImage; // --bg-image <path>: backdrop behind the grid; empty for none
    double backgroundImageOpacity = 1.0; // --bg-image-opacity <0..1>
    int backgroundImageMode = 0; // --bg-image-mode fill|fit|stretch|tile|center
    // Post-feed actions, replicating the macOS frontend so the OSC 133 prompt
    // marks can be rendered (real Metal pipeline) in their cleared / scrolled
    // states — the states the gutter brackets were drawn wrong in.
    bool shouldClearToMark = false; // Cmd-L (clearToPreviousMark)
    bool shouldClearPreservingInput = false; // Cmd-K (clearBuffer / clearBufferPreserveInput)
    bool drawsSizedText = false; // --draws-sized-text: the OSC 66 gate, sized blocks and proportional runs both
    bool shouldPrintMetrics = false; // --print-metrics: one line of cell geometry on stdout, for a reference drawn beside the render
    // Renders the frame as an unfocused pane, which is the only route to the
    // hollow-outline cursor: focus is window state, and a headless snapshot
    // has no window to lose it from.
    bool isUnfocused = false;
    int viewOffset = 0; // scroll the viewport up by N rows before rendering
    bool haveViewOffset = false;
    bool shouldGlowTekVectors = false; // --tek-glow: additive bloom on Tek 4014 vectors (lit-CRT look)
    // --tek-color-mode 1 + --tek-beam/--tek-bg: drive the standalone Tek custom
    // color path (tekColorMode/tekBeamColor/tekBackgroundColor), exercising the
    // exact renderer override the [tek] config feeds in the live app.
    int tekColorMode = 0;
    uint32_t tekBeamColor = 0x33FF66u;
    // Whether --tek-beam or --tek-bg was actually passed: both are read only
    // under --tek-color-mode 1, and a stored value nothing consumes is a
    // silence the person who typed the flag cannot distinguish from success.
    bool haveTekColorArgument = false;
    uint32_t tekBackgroundColor = 0x0A0A0Au;
    uint32_t beamColor = 0; // --beam RRGGBB: override the default foreground (Tek phosphor / beam)
    bool haveBeamColor = false;
    uint32_t backgroundColor = 0; // --bg RRGGBB: override the default background
    bool haveBackgroundColor = false;
    // --select-rect r0,c0,r1,c1: a char-mode selection over the given visible
    // cell range, so a golden can exercise the selection overlay + OSC 19
    // selection-foreground recolor headlessly. Absent = no selection.
    bool haveSelectRect = false;
    int selectRow0 = 0, selectColumn0 = 0, selectRow1 = 0, selectColumn1 = 0;
    // --select-points x0,y0,x1,y1: a plain drag between two points, in cells
    // across and rows down, unrounded, run through the pointer ladder's caret
    // gesture, so a golden can show a selection that starts and ends inside a
    // rich text block's lines. Absent = no such selection.
    bool haveSelectPoints = false;
    double selectPointX0 = 0, selectPointY0 = 0, selectPointX1 = 0, selectPointY1 = 0;
    // The literal a search runs for, or empty for no search. A snapshot that
    // renders a focused match needs the terminal's own search state, because the
    // renderer reads the active match from it rather than being handed spans.
    std::string searchLiteral;
    // --link-style-per-kind: turn on per-kind OSC 8 underline distinction
    // (file:// dotted, mailto: dashed, url/other base) so a golden can show
    // the three kinds rendered distinctly. Off = all kinds use link_style.
    bool linkStylePerKind = false;
    // --hover-rect r0,c0,r1,c1[,id]: a Cmd-hovered link run over the given VISIBLE
    // cell range, so a golden can render the hover affordance headlessly. The
    // affordance is mouse state rather than terminal state, so no feed can reach
    // it and without this the hovered presentation has no test at all.
    // --no-subpixel: render with subpixel text off, the way a user who turned the
    // setting off sees it. The atlas keeps its margins either way, so this is the
    // only way to reach the cell-baked path on a build whose atlas offers phases.
    bool disableSubpixelText = false;
    bool haveHoverRect = false;
    int hoverRow0 = 0, hoverColumn0 = 0, hoverRow1 = 0, hoverColumn1 = 0;
    int hoverLinkIdentifier = 0;
    // Extra frames rendered from the same process, after the first: each
    // --also-feed is processed and the paired --also-snapshot written, in the
    // order the pairs appear.
    //
    // One process rather than several because that is the only way to reach a
    // frame that shares a glyph atlas, a grid cache and a damage baseline with
    // the frame before it. A per-frame process gives every frame a cold
    // everything, so a state one frame leaves behind for the next — an atlas
    // epoch, a retained instance buffer, a dirty-row set — is never exercised.
    std::vector<std::string> alsoFeeds;
    std::vector<std::string> alsoSnapshots;
};

/// Drains the replies a snapshot's feed left queued, as one line of text: a
/// byte outside printable ASCII, or a backslash, written as `\xHH`.
///
/// A snapshot has no program to read what the terminal answered, so the
/// driver prints it and a case asserts on the line; a feed that asks and then
/// draws with the answer is held to both.
///
/// - Parameter terminal: The terminal the feed was processed into.
/// - Returns: The replies, escaped; empty when nothing was answered.
[[nodiscard]] inline std::string drainRepliesForReport(Ubiquitty::Terminal& terminal)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string report;
    std::array<char, 1024> chunk {};
    int length = 0;
    while ((length = Ubiquitty::drainOutput(&terminal, chunk)) > 0) {
        for (const char character : std::span(chunk).first(static_cast<std::size_t>(length))) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte >= 0x20 && byte < 0x7f && byte != '\\') {
                report.push_back(character);
                continue;
            }
            report += "\\x";
            report.push_back(digits[byte >> 4]);
            report.push_back(digits[byte & 0xf]);
        }
    }
    return report;
}

}
