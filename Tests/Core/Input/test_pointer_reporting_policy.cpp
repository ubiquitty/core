// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pointer_reporting_policy — pins the report-vs-local pointer decision
// every frontend routes through (Source/Core/Input/PointerReportingPolicy.h).
//
// One chord per gesture, and each reaches through an application holding the
// mouse for its own gesture only: Shift reaches a flowing selection, the
// reader's block chord reaches a rectangular one, and Command reaches a LINK
// and no selection at all. The requirement that shaped the rest — a held
// Command takes the LOCAL link path while mouse mode is on — is the first
// reporting-on assertion below.

#include "Core/Input/PointerReportingPolicy.h"

#include <cstdio>

using Ubiquitty::Input::allowsLocalDragSelect;
using Ubiquitty::Input::allowsLocalLinkInteraction;
using Ubiquitty::Input::DragOwner;
using Ubiquitty::Input::isOpenableURLScheme;
using Ubiquitty::Input::localDragIsRectangular;
using Ubiquitty::Input::plainClickOpenableUnderReporting;
using Ubiquitty::Input::plainDragDefaultOwner;
using Ubiquitty::Input::PointerReportingState;

TEST(pointerReportingPolicy)
{

    // --- No mouse reporting: local always wins, regardless of modifiers. ---
    {
        PointerReportingState s; // nothing armed; nobody is asking for the pointer
        CHECK(allowsLocalLinkInteraction(s));
        CHECK(allowsLocalDragSelect(s));
        CHECK(!localDragIsRectangular(s)); // no block chord armed
    }

    // --- Mouse reporting ON, the link chord armed → LOCAL link path. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isLinkOverrideEnabled = false; // the link chord's own doing, not the setting's
        // No chord: the app owns the pointer — no local link, no local drag.
        CHECK(!allowsLocalLinkInteraction(s));
        CHECK(!allowsLocalDragSelect(s));
        s.isLinkModifierArmed = true;
        CHECK(allowsLocalLinkInteraction(s));
        // And a LINK only. Command reaching a selection as well makes two
        // gestures out of one chord: a ⌘-drag over a tracking application both
        // opens the link under the press and selects the text it sits in.
        CHECK(!allowsLocalDragSelect(s));
        CHECK(!localDragIsRectangular(s));
    }

    // --- Shift reaches a FLOWING selection through a tracking application. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isSelectionOverrideModifierArmed = true;
        CHECK(allowsLocalDragSelect(s));
        CHECK(!localDragIsRectangular(s)); // Shift is the flow, not the block
        CHECK(!allowsLocalLinkInteraction(s)); // and it is not a link gesture
    }

    // --- The block chord reaches a RECTANGULAR one the same way. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isBlockSelectionOverrideModifierArmed = true;
        CHECK(allowsLocalDragSelect(s));
        CHECK(localDragIsRectangular(s));
        CHECK(!allowsLocalLinkInteraction(s)); // a block chord never lights links
    }

    // --- A link chord and a selection chord both armed stay two gestures:
    //     the link opens, and the drag is the selection chord's shape. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isLinkModifierArmed = true;
        s.isBlockSelectionOverrideModifierArmed = true;
        CHECK(allowsLocalLinkInteraction(s));
        CHECK(allowsLocalDragSelect(s));
        CHECK(localDragIsRectangular(s));
    }

    // --- -CC pane: a plain drag is LOCAL even while the pane app reports the
    //     mouse (the surface is ours) — the default-owner flip. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true; // vim/htop inside the pane turned it on
        s.isControlModePane = true;
        CHECK(plainDragDefaultOwner(s) == DragOwner::Terminal);
        CHECK(allowsLocalDragSelect(s)); // plain drag: no modifier needed
        CHECK(!localDragIsRectangular(s)); // ...and it's linear
        // The link chord still means local here too (never inverts to "forward").
        s.isLinkModifierArmed = true;
        CHECK(allowsLocalLinkInteraction(s));
        CHECK(allowsLocalDragSelect(s));
    }

    // --- Non-CC surface is the App's owner under reporting (unchanged): a plain
    //     drag forwards, only a modifier reaches through. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isControlModePane = false;
        CHECK(plainDragDefaultOwner(s) == DragOwner::App);
        CHECK(!allowsLocalDragSelect(s)); // plain drag forwards to the app
    }

    // --- The -CC flip is drag-only: a bare (no-⌘) hover does not light links in
    //     a -CC pane whose app reports the mouse — the affordance stays ⌘-gated. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isControlModePane = true;
        CHECK(!allowsLocalLinkInteraction(s)); // no ⌘ → no link affordance
    }

    // --- links_override_reporting (default ON): unmodified link interaction stays
    //     LOCAL even under mouse reporting — but link-only, not drag-select. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isLinkOverrideEnabled = true;
        CHECK(allowsLocalLinkInteraction(s)); // no ⌘ needed → links stay local under reporting
        CHECK(!allowsLocalDragSelect(s)); // link-only: a plain drag still forwards to the app
        CHECK(plainDragDefaultOwner(s) == DragOwner::App); // owner unchanged — the setting doesn't steal drags
    }

    // --- Setting OFF reverts to the modifier gate, and ⌘ still reaches links. ---
    {
        PointerReportingState s;
        s.isMouseReporting = true;
        s.isLinkOverrideEnabled = false;
        CHECK(!allowsLocalLinkInteraction(s)); // OFF → back to the ⌘-gated behavior
        s.isLinkModifierArmed = true;
        CHECK(allowsLocalLinkInteraction(s)); // ⌘ (with its override) still works when OFF
    }

    // --- isOpenableURLScheme: the {http, https, file} scheme allowlist for the no-⌘
    //     AUTO-DETECTED open/hover under reporting. Case-insensitive; every other
    //     scheme and a schemeless / empty reference are rejected. An explicit scheme
    //     prefix is unambiguous; only a BARE path (isFile) is the false-positive class. ---
    {
        CHECK(isOpenableURLScheme("http://x"));
        CHECK(isOpenableURLScheme("https://x"));
        CHECK(isOpenableURLScheme("HTTP://x")); // case-insensitive scheme
        CHECK(isOpenableURLScheme("HttPs://x")); // mixed case too
        CHECK(isOpenableURLScheme("https://www.foo.com")); // www. materializes to https:// upstream
        CHECK(isOpenableURLScheme("file://x")); // explicit file:// Is in scope (a bare path is the isFile candidate, kept ⌘-only)
        CHECK(isOpenableURLScheme("FILE://x")); // case-insensitive
        CHECK(!isOpenableURLScheme("ftp://x"));
        CHECK(!isOpenableURLScheme("ssh://x"));
        CHECK(!isOpenableURLScheme("mailto:x"));
        CHECK(!isOpenableURLScheme("example.com")); // schemeless (no ':') → no scheme
        CHECK(!isOpenableURLScheme("")); // empty
        CHECK(!isOpenableURLScheme(":no-scheme")); // ':' at index 0 → empty scheme
        CHECK(!isOpenableURLScheme("httpx://y")); // near-miss scheme is not http
        CHECK(!isOpenableURLScheme("https")); // no ':' → not a scheme'd URL
    }

    // --- plainClickOpenableUnderReporting: the SHARED kind/scheme verdict the
    //     click resolver and the hover acceptance both route through (lockstep).
    //     OSC 8 always opens; a file candidate never does; otherwise only a
    //     http/https auto-detected URL. ---
    {
        // The verdict takes a PlainOpenableLink struct (named bool FIELDS, not
        // bool arguments). The extra parens shield the brace-init commas from the
        // CHECK macro's argument split.
        // OSC 8 (app-stamped) qualifies regardless of the file/scheme flags.
        CHECK((plainClickOpenableUnderReporting({ .isOSC8 = true, .isFile = false, .hasOpenableScheme = false })));
        CHECK((plainClickOpenableUnderReporting({ .isOSC8 = true, .isFile = false, .hasOpenableScheme = true })));
        CHECK((plainClickOpenableUnderReporting({ .isOSC8 = true, .isFile = true, .hasOpenableScheme = false }))); // OSC 8 wins over the file flag
        CHECK((plainClickOpenableUnderReporting({ .isOSC8 = true, .isFile = true, .hasOpenableScheme = true })));
        // A file-path candidate (non-OSC 8) never qualifies — loose ⌘-path only.
        CHECK((!plainClickOpenableUnderReporting({ .isOSC8 = false, .isFile = true, .hasOpenableScheme = false })));
        CHECK((!plainClickOpenableUnderReporting({ .isOSC8 = false, .isFile = true, .hasOpenableScheme = true }))); // the scheme flag can't rescue a file
        // Non-OSC 8, non-file: gated strictly on the http/https scheme flag.
        CHECK((plainClickOpenableUnderReporting({ .isOSC8 = false, .isFile = false, .hasOpenableScheme = true }))); // http/https auto-detected URL
        CHECK((!plainClickOpenableUnderReporting({ .isOSC8 = false, .isFile = false, .hasOpenableScheme = false }))); // other scheme / schemeless
    }
}

// How much of the link scan a pointer event is worth running. The whole scan
// flattens a soft-wrap group and sweeps it twice; a modifier-less hover happens
// on every move over any text, so it asks only what the program declared.
TEST(theLinkProbeIsAsNarrowAsTheRevealPolicyAllows)
{
    using Ubiquitty::Input::LinkProbe;
    using Ubiquitty::Input::linkProbe;

    // The ordinary screen: nothing is reporting, a bare hover reveals a
    // declared link, and that is all it can find.
    Ubiquitty::Input::PointerReportingState plain;
    CHECK_MSG(linkProbe(plain, true) == LinkProbe::Declared,
        "a bare hover asks for what the program declared");

    // Command is the reader asking by name, so it takes the whole scan.
    Ubiquitty::Input::PointerReportingState commanded = plain;
    commanded.isLinkModifierArmed = true;
    CHECK_MSG(linkProbe(commanded, true) == LinkProbe::Any, "Command asks for a guess too");

    // A tracking application owns the pointer, so no link interaction is local
    // and nothing is scanned at all.
    Ubiquitty::Input::PointerReportingState reporting = plain;
    reporting.isMouseReporting = true;
    CHECK_MSG(linkProbe(reporting, true) == LinkProbe::None, "a tracking application scans nothing");

    // links_override_reporting keeps the hover local, and the set it may
    // reveal there is the auto-detected one, so the scan widens with it.
    Ubiquitty::Input::PointerReportingState overridden = reporting;
    overridden.isLinkOverrideEnabled = true;
    CHECK_MSG(linkProbe(overridden, true) == LinkProbe::Any,
        "the override reveals guesses under reporting, so it scans for them");

    // With the bare reveal switched off there is nothing for a bare hover to
    // show, so it scans nothing; Command still reaches through.
    CHECK_MSG(linkProbe(plain, false) == LinkProbe::None, "no bare reveal, no bare scan");
    CHECK_MSG(linkProbe(commanded, false) == LinkProbe::Any, "Command reaches through regardless");
}

UBIQUITTY_TEST_MAIN()
