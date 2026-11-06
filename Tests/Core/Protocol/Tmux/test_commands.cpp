// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_commands.cpp — host tests for the portable tmux control-command
// vocabulary (Protocol/Tmux/Commands.h) and the Panes::Tree → layout serializer
// (Protocol/Tmux/PaneLock.h emitPaneLayout). These run the same code every -CC
// frontend and the host emit + decode, so a drift in a `#{…}` field or a
// sentinel breaks here in one place.
//
// Capture recovery is pinned in test_orchestration.cpp; these cases cover the
// protocol command builders independently of session orchestration.

#include "Core/Protocol/Tmux/Commands.h"
// Session.h sets up the include order (mutex, LayoutNode) and pulls in
// PaneLock.h where emitPaneLayout + the layout parse/build helpers live.
#include "Core/Protocol/Tmux/Session.h"

#include "Core/Panes/PaneTree.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Tmux;

TEST(savedSessionReferencesRejectMalformedIdentities)
{
    const auto reference = parseSessionReference("UBSESSION 321 1700000000 $0 1700000001 /tmp/tmux user's socket");
    CHECK(reference.has_value());
    CHECK(reference->serverProcessID == 321);
    CHECK(reference->sessionID == 0);
    CHECK_STR_EQ(reference->socketPath, "/tmp/tmux user's socket");
    for (const auto invalid : {
             "UBSESSION 0 1700000000 $0 1700000001 /tmp/s",
             "UBSESSION 321 0 $0 1700000001 /tmp/s",
             "UBSESSION 321 1700000000 $0 0 /tmp/s",
             "UBSESSION 321 1700000000 0 1700000001 /tmp/s",
             "UBSESSION -1 1700000000 $0 1700000001 /tmp/s",
             "UBSESSION 18446744073709551616 1700000000 $0 1700000001 /tmp/s",
             "UBSESSION 321 1700000000 $0;kill-server 1700000001 /tmp/s",
             "UBSESSION 321 1700000000 $0 1700000001 relative/socket",
             "UBSESSION 321 1700000000 $0 1700000001 /tmp/s\nsecond line" }) {
        CHECK(!parseSessionReference(invalid));
    }
    CHECK(!parseSessionReference("UBSESSION 321 1700000000 $0 1700000001 /" + std::string(4096, 'x')));
    std::string embeddedNull = "UBSESSION 321 1700000000 $0 1700000001 /tmp/";
    embeddedNull.push_back('\0');
    CHECK(!parseSessionReference(embeddedNull));
}

TEST(savedSessionAttachPinsServerAndNeverCreatesASession)
{
    const SessionReference reference { 321, 1700000000, 0, 1700000001, "/tmp/tmux user's socket" };
    const auto arguments = restoreSessionArguments("/opt/homebrew/bin/tmux", reference);
    CHECK(arguments.size() == 11);
    CHECK_STR_EQ(arguments[2], reference.socketPath);
    CHECK_STR_EQ(arguments[7], "$0:");
    CHECK_STR_EQ(arguments[8], "#{&&:#{&&:#{==:#{pid},321},#{==:#{start_time},1700000000}},#{==:#{session_created},1700000001}}");
    CHECK_STR_EQ(arguments[9], "attach-session -t $0");
    CHECK(restoreSessionArguments("tmux", {}).empty());
    CHECK(restoreSessionArguments("", reference).empty());

    Session session;
    CHECK(session.consumeSessionReference("UBSESSION 321 1700000000 $0 1700000001 /tmp/tmux user's socket"));
    CHECK(session.sessionReference() == reference);
    CHECK(!session.consumeSessionReference("unrelated response"));
    CHECK(session.sessionReference() == reference);
    CHECK(session.consumeSessionReference("UBSESSION broken"));
    CHECK(!session.sessionReference());
    CHECK(session.consumeSessionReference("UBSESSION 321 1700000000 $0 1700000001 /tmp/s"));
    session.clearSessionReference();
    CHECK(!session.sessionReference());
    CHECK(session.consumeSessionReference("UBSESSION 321 1700000000 $0 1700000001 /tmp/s"));
    session.retireAll();
    CHECK(!session.sessionReference());
}

// ---- vocabulary strings ----

TEST(testPaneListFormat)
{
    // The exact #{…} enumeration order parsePaneRecord decodes:
    // @<win> %<pane> <w> <h> <active> <layout> <session> <window_name>
    const std::string fmt = "\"#{window_id} #{pane_id} #{pane_width} #{pane_height} #{pane_active} "
                            "#{window_layout} #{session_name} #{window_name}\"";
    CHECK_STR_EQ(std::string(paneListFormat), fmt);
    CHECK_STR_EQ(listPanesForSession(), "list-panes -s -F " + fmt);
    CHECK_STR_EQ(listPanesForWindow(7), "list-panes -t @7 -F " + fmt);
}

TEST(testRefreshClientSize)
{
    CHECK_STR_EQ(refreshClientSize(80, 24), "refresh-client -C 80x24");
    CHECK_STR_EQ(refreshClientSize(212, 51), "refresh-client -C 212x51");
}

TEST(testRecoveryQueries)
{
    // The mode query stamps the UBMODES reply sentinel + the fixed 6-field order
    // decodePaneModeReport decodes.
    CHECK_STR_EQ(paneModeQuery(3),
        "display-message -p -t %3 \"UBMODES #{pane_id} "
        "#{?mouse_standard_flag,1,0}#{?mouse_button_flag,1,0}#{?mouse_all_flag,1,0}"
        "#{?mouse_sgr_flag,1,0}#{?mouse_utf8_flag,1,0}#{?keypad_cursor_flag,1,0}\"");
    // The scope digit: 0 = CurrentScreen (the default), 1 = FullHistory; then
    // the form digit: 0 = Text (the default), 1 = FlaggedRows.
    CHECK_STR_EQ(captureBeginMarker(3, 0), "display-message -p -t %3 \"UBCAPBEGIN 3 #{cursor_x} #{cursor_y} 0 0 0\"");
    CHECK_STR_EQ(captureBeginMarker(12, 2), "display-message -p -t %12 \"UBCAPBEGIN 12 #{cursor_x} #{cursor_y} 2 0 0\"");
    CHECK_STR_EQ(captureBeginMarker(3, 0, CaptureScope::CurrentScreen), "display-message -p -t %3 \"UBCAPBEGIN 3 #{cursor_x} #{cursor_y} 0 0 0\"");
    CHECK_STR_EQ(captureBeginMarker(3, 0, CaptureScope::FullHistory), "display-message -p -t %3 \"UBCAPBEGIN 3 #{cursor_x} #{cursor_y} 0 1 0\"");
    CHECK_STR_EQ(captureBeginMarker(3, 1, CaptureScope::FullHistory, CaptureForm::FlaggedRows), "display-message -p -t %3 \"UBCAPBEGIN 3 #{cursor_x} #{cursor_y} 1 1 1\"");
    CHECK_STR_EQ(capturePane(3), "capture-pane -t %3 -e -p");
    CHECK_STR_EQ(capturePane(3, CaptureForm::FlaggedRows), "capture-pane -t %3 -e -p -N -T -F");
    // capturePaneWithHistory: `-S -` / `-E -` are plain literal dashes (man
    // tmux: "start of the history" / "end of the visible pane"), resolved
    // server-side — no client-side history-size or row-count query needed.
    // Confirmed live over the actual -CC control protocol that this differs
    // from embedding a `#{...}` format string in the same position, which
    // either kills the session with a parse error or silently desyncs -E's
    // association.
    CHECK_STR_EQ(capturePaneWithHistory(3), "capture-pane -t %3 -e -p -S - -E -");
    CHECK_STR_EQ(capturePaneWithHistory(12), "capture-pane -t %12 -e -p -S - -E -");
    CHECK_STR_EQ(capturePaneWithHistory(12, CaptureForm::FlaggedRows), "capture-pane -t %12 -e -p -N -T -F -S - -E -");
    CHECK_STR_EQ(serverVersionQuery(), "display-message -p \"UBVERSION #{version}\"");
    // The reply sentinels keep recovery replies clear of pane records (trailing
    // space matters — see the prefix-match router).
    CHECK_STR_EQ(std::string(paneModeReplyPrefix), "UBMODES ");
    CHECK_STR_EQ(std::string(captureBeginPrefix), "UBCAPBEGIN ");
    CHECK_STR_EQ(std::string(serverVersionReplyPrefix), "UBVERSION ");
}

// capture-pane prints its line flags from tmux 3.8; a release before it, or a
// version this does not read, captures text alone, which every tmux prints.
TEST(testCaptureFormFollowsTheServerVersion)
{
    for (const char* version : { "3.7", "3.7c", "3.8", "3.8a", "3.10", "4.0", "next-3.9", "next-3.7" })
        CHECK_MSG(captureFormForVersion(version) == CaptureForm::FlaggedRows, version);
    for (const char* version : { "3.6", "3.6b", "3.2a", "2.9", "next-3.6", "openbsd-7.7", "master", "", "3", "3.", ".8", "99999999999.1" })
        CHECK_MSG(captureFormForVersion(version) == CaptureForm::Text, version);
}

TEST(testClipboardPull)
{
    // The marker's reply self-identifies (UBCLIPBEGIN %<pane>); show-buffer's
    // response body is the buffer content the bridge mirrors to the clipboard.
    CHECK_STR_EQ(clipboardPullMarker(3), "display-message -p -t %3 \"UBCLIPBEGIN #{pane_id}\"");
    CHECK_STR_EQ(clipboardPullShowBuffer(), "show-buffer");
    CHECK_STR_EQ(std::string(clipboardPullPrefix), "UBCLIPBEGIN ");

    // Sync decision: write only on permitted + non-empty + changed.
    CHECK(shouldSyncClipboardPull("hello", "", true)); // fresh copy → write
    CHECK(shouldSyncClipboardPull("hello", "world", true)); // changed → write
    CHECK(!shouldSyncClipboardPull("hello", "hello", true)); // unchanged → skip (no loop)
    CHECK(!shouldSyncClipboardPull("", "hello", true)); // empty buffer → never clear
    CHECK(!shouldSyncClipboardPull("hello", "", false)); // write disabled → skip
    CHECK(!shouldSyncClipboardPull("", "", true)); // empty + unchanged → skip
}

// The pane id in the clipboard-pull marker ("UBCLIPBEGIN %<pane>") is
// SERVER-controlled, so its parse must be bounded and reject malformed input.
TEST(testClipboardPullMarkerPane)
{
    CHECK(parseClipboardPullMarkerPane("%3") == 3);
    CHECK(parseClipboardPullMarkerPane("%12") == 12);
    CHECK(parseClipboardPullMarkerPane("%5 trailing") == 5); // stops at the first non-digit
    CHECK(parseClipboardPullMarkerPane("") == -1); // empty
    CHECK(parseClipboardPullMarkerPane("%") == -1); // no digits
    CHECK(parseClipboardPullMarkerPane("3") == -1); // missing '%'
    CHECK(parseClipboardPullMarkerPane("%abc") == -1); // non-numeric
    CHECK(parseClipboardPullMarkerPane("%99999999999999999999") == -1); // bounded, no overflow
}

// The untrusted -CC clipboard pull must FAIL CLOSED when the (server-controlled)
// pane id doesn't resolve — a hostile server dodging attribution must not get an
// ungated write. A resolved pane with no policy is the standalone no-op; with a
// policy, the policy decides.
TEST(testClipboardPullGate)
{
    // Fail closed: pane didn't resolve (bogus/omitted id) — never write, whatever
    // the (unused) policy fields say.
    CHECK(!clipboardPullShouldWrite(PaneAttribution::Unresolved, ClipboardWriteGate::Refused));
    CHECK(!clipboardPullShouldWrite(PaneAttribution::Unresolved, ClipboardWriteGate::Allowed));
    // Resolved → the gate decides, and an unset policy answers PAL's default inside it.
    CHECK(clipboardPullShouldWrite(PaneAttribution::Resolved, ClipboardWriteGate::Allowed));
    CHECK(!clipboardPullShouldWrite(PaneAttribution::Resolved, ClipboardWriteGate::Refused));
}

// ---- decodePaneModeReport ----

TEST(testDecodeModeReport)
{
    PaneID paneID = -1;
    // Body is what follows the "UBMODES " sentinel: "%<pane> <bits>".
    // Order: 1000 1002 1003 1006 1005 DECCKM.
    // Standard mouse + SGR coords + app cursor keys → bits 1,0,0,1,0,1.
    std::string body = "%5 100101";
    std::string seq = decodePaneModeReport(body, paneID);
    CHECK(paneID == 5);
    CHECK_STR_EQ(seq, std::string("\x1b[?1000h") + "\x1b[?1006h" + "\x1b[?1h");

    // All bits set → all six DECSETs in canonical order.
    body = "%9 111111";
    seq = decodePaneModeReport(body, paneID);
    CHECK(paneID == 9);
    CHECK_STR_EQ(seq, std::string("\x1b[?1000h") + "\x1b[?1002h" + "\x1b[?1003h" + "\x1b[?1006h" + "\x1b[?1005h" + "\x1b[?1h");

    // No bits set → empty (caller sends nothing).
    body = "%2 000000";
    seq = decodePaneModeReport(body, paneID);
    CHECK(paneID == 2);
    CHECK(seq.empty());

    // Button-event tracking only (bit 1).
    body = "%1 010000";
    seq = decodePaneModeReport(body, paneID);
    CHECK(paneID == 1);
    CHECK_STR_EQ(seq, std::string("\x1b[?1002h"));

    // Malformed prefix (no leading '%') → empty, paneID reset.
    body = "5 100000";
    paneID = 42;
    seq = decodePaneModeReport(body, paneID);
    CHECK(paneID == 0);
    CHECK(seq.empty());

    // Truncated bit field: only the present bits decode (the rest read as 0).
    // "%7 1" → only bit 0 (1000h) is present.
    body = "%7 1";
    seq = decodePaneModeReport(body, paneID);
    CHECK(paneID == 7);
    CHECK_STR_EQ(seq, std::string("\x1b[?1000h"));

    // Empty / null guards.
    seq = decodePaneModeReport({}, paneID);
    CHECK(seq.empty());
    CHECK(paneID == 0);
}

// ---- emit ∘ parse layout round-trip ----

static std::map<int, PaneDimensions>* g_dimensions = nullptr;
static PaneDimensions lookupDimensions(void* context, PaneID paneID)
{
    (void)context;
    auto it = g_dimensions->find(paneID);
    return it != g_dimensions->end() ? it->second : PaneDimensions { 1, 1 };
}

TEST(testEmitSinglePaneUnsplit)
{
    // A single-leaf tree still serializes a parseable rect.
    std::map<int, PaneDimensions> dims = { { 1, { 80, 24 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    std::string out;
    PaneLayoutExtent ext = emitPaneLayout(tree, tree.root(), lookupDimensions, nullptr, out);
    CHECK_STR_EQ(out, "80x24,0,0,1");
    CHECK(ext.columns == 80 && ext.rows == 24);
}

TEST(testEmitColumnsSplit)
{
    // {} = Vertical (columns) side-by-side: width = sum, height = max.
    std::map<int, PaneDimensions> dims = { { 1, { 40, 24 } }, { 2, { 39, 24 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    tree.split(Panes::Orientation::Vertical, 2, 0.5f);
    std::string out;
    PaneLayoutExtent ext = emitPaneLayout(tree, tree.root(), lookupDimensions, nullptr, out);
    CHECK_STR_EQ(out, "79x24,0,0{40x24,0,0,1,39x24,0,0,2}");
    CHECK(ext.columns == 79 && ext.rows == 24);
}

TEST(testEmitRowsSplit)
{
    // [] = Horizontal (rows) stacked: height = sum, width = max.
    std::map<int, PaneDimensions> dims = { { 1, { 80, 12 } }, { 2, { 80, 11 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    tree.split(Panes::Orientation::Horizontal, 2, 0.5f);
    std::string out;
    PaneLayoutExtent ext = emitPaneLayout(tree, tree.root(), lookupDimensions, nullptr, out);
    CHECK_STR_EQ(out, "80x23,0,0[80x12,0,0,1,80x11,0,0,2]");
    CHECK(ext.columns == 80 && ext.rows == 23);
}

// Serialize a tree, prefix the constant checksum, parse it back, rebuild, and
// re-serialize — the wire form must be identical (the round-trip the
// capture/restore path relies on).
static std::string roundTrip(const Panes::Tree& tree)
{
    std::string out;
    emitPaneLayout(tree, tree.root(), lookupDimensions, nullptr, out);
    const std::string full = "0000," + out;
    LayoutNode root;
    if (!parseLayoutTree(full, root))
        return "<parse-failed>";
    Panes::Tree rebuilt(leftmostPaneID(root));
    buildPaneTreeInto(rebuilt, root);
    std::string out2;
    emitPaneLayout(rebuilt, rebuilt.root(), lookupDimensions, nullptr, out2);
    CHECK(rebuilt.paneCount() == tree.paneCount());
    return out2 == out ? out : ("<mismatch> " + out + " != " + out2);
}

TEST(testRoundtripColumns)
{
    std::map<int, PaneDimensions> dims = { { 1, { 40, 24 } }, { 2, { 39, 24 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    tree.split(Panes::Orientation::Vertical, 2, 0.5f);
    CHECK_STR_EQ(roundTrip(tree), "79x24,0,0{40x24,0,0,1,39x24,0,0,2}");
}

TEST(testRoundtripNested)
{
    // Right-nested 3-way column split: 1 | (2 | 3).
    std::map<int, PaneDimensions> dims = { { 1, { 20, 24 } }, { 2, { 20, 24 } }, { 3, { 20, 24 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    tree.split(Panes::Orientation::Vertical, 2, 0.33f);
    tree.focusPane(2);
    tree.split(Panes::Orientation::Vertical, 3, 0.5f);
    const std::string result = roundTrip(tree);
    // Shape (and pane ids / dimensions) survive the round-trip identically.
    CHECK_STR_EQ(result, "60x24,0,0{20x24,0,0,1,40x24,0,0{20x24,0,0,2,20x24,0,0,3}}");
}

TEST(testRoundtripMixedOrientation)
{
    // Vertical at the root, the right child split Horizontally: 1 | (2 / 3).
    std::map<int, PaneDimensions> dims = { { 1, { 40, 24 } }, { 2, { 39, 12 } }, { 3, { 39, 11 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    tree.split(Panes::Orientation::Vertical, 2, 0.5f);
    tree.focusPane(2);
    tree.split(Panes::Orientation::Horizontal, 3, 0.5f);
    const std::string result = roundTrip(tree);
    // Right child stacks 2 (39x12) over 3 (39x11): height 12+11=23, width max=39.
    // Root is Vertical: width 40+39=79, height max(24,23)=24.
    CHECK_STR_EQ(result, "79x24,0,0{40x24,0,0,1,39x23,0,0[39x12,0,0,2,39x11,0,0,3]}");
}

TEST(testEmitMissingDimensionsFallback)
{
    // A pane with no registry entry (or zero dims) falls back to 1x1 so the
    // emitted layout still parses.
    std::map<int, PaneDimensions> dims = { { 1, { 0, 0 } } };
    g_dimensions = &dims;
    Panes::Tree tree(1);
    std::string out;
    emitPaneLayout(tree, tree.root(), lookupDimensions, nullptr, out);
    CHECK_STR_EQ(out, "1x1,0,0,1");
}

TEST(testPaneTransfersAddressStablePaneIDs)
{
    using namespace Ubiquitty::Protocol::Tmux;
    using namespace Ubiquitty::Panes;
    CHECK(movePane(7, 9, Orientation::Vertical, InsertSide::First) == "select-window -t %9 ; run-shell -d 0 ; move-pane -d -h -b -l 50% -s %7 -t %9");
    CHECK(movePane(7, 9, Orientation::Horizontal, InsertSide::Second) == "select-window -t %9 ; run-shell -d 0 ; move-pane -d -v -l 50% -s %7 -t %9");
    CHECK(movePane(7, 7, Orientation::Vertical, InsertSide::First).empty());
    CHECK(breakPane(7) == "break-pane -s %7");
    CHECK(breakPane(-1).empty());
    Tree tree(7);
    tree.split(Orientation::Horizontal, 8, 0.3f);
    CHECK(moveLayout(tree, 9, Orientation::Vertical, InsertSide::Second)
        == "select-window -t %9 ; run-shell -d 0 ; move-pane -d -h -l 50% -s %7 -t %9 ; select-window -t %7 ; run-shell -d 0 ; move-pane -d -v -l 70% -s %8 -t %7 ; select-pane -t %8");
    CHECK(moveLayout(tree, 7, Orientation::Vertical, InsertSide::First).empty());
    CHECK(movePane(7, 9, Orientation::Horizontal, InsertSide::First, 50, InsertTarget::Root)
        == "select-window -t %9 ; run-shell -d 0 ; move-pane -d -v -f -b -l 50% -s %7 -t %9");
    CHECK(moveLayout(tree, 9, Orientation::Vertical, InsertSide::Second, InsertTarget::Root)
        == "select-window -t %9 ; run-shell -d 0 ; move-pane -d -h -f -l 50% -s %7 -t %9 ; select-window -t %7 ; run-shell -d 0 ; move-pane -d -v -l 70% -s %8 -t %7 ; select-pane -t %8");
}

UBIQUITTY_TEST_MAIN()
