// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_capability_consent_text — pins the words a capability-consent prompt
// shows. Each prompt-reachable Capability states its real-world effect; a code
// with no effect line names its number instead; and the body names where the
// session sits, the foreground chain as the nearest guess at the sender, and
// the principal holding drive access as three separate facts, with the one
// uncertainty stated once. Off AppKit — the words are plain C++.

#include "Core/Security/CapabilityConsentText.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

using Ubiquitty::Capability;
using Ubiquitty::capabilityGrantIsPerUse;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Security::capabilityConsentLifetimeNote;
using Ubiquitty::Security::capabilityConsentVerb;

TEST(capabilityConsentText)
{

    // The highest-severity grants read as blunt effect statements, not labels.
    CHECK(capabilityConsentVerb(Capability::ExecuteTrigger) == "run a command on your Mac");
    CHECK(capabilityConsentVerb(Capability::ProgramKeys) == "remap what your keys type");
    CHECK(capabilityConsentVerb(Capability::ScreenContentRead) == "read the text on your screen");
    // Reads / writes / act-beyond-grid each get a plain-English effect.
    CHECK(capabilityConsentVerb(Capability::ClipboardRead) == "read your clipboard");
    CHECK(capabilityConsentVerb(Capability::ClipboardWrite) == "write to your clipboard");
    CHECK(capabilityConsentVerb(Capability::FileReadAccess) == "read files from your Mac");
    CHECK(capabilityConsentVerb(Capability::SetRemoteHost) == "change the machine this session reports it is on");
    CHECK(capabilityConsentVerb(Capability::WindowMutate) == "move, resize, or minimize this window");
    // Color set and preset-select share one verb.
    CHECK(capabilityConsentVerb(Capability::SetColor) == capabilityConsentVerb(Capability::SelectColorPreset));
    CHECK(capabilityConsentVerb(Capability::SetColor) == "change the terminal colors");
    // Every enumerator states an effect. The map is total over the enum, so the
    // generic phrasing is reachable only by a code outside it — which is what
    // lets the prompt's title tell the reader, truthfully, when this build has
    // no vocabulary for what it is being asked.
    for (std::uint16_t code = 0; code <= static_cast<std::uint16_t>(Capability::None); ++code) {
        const std::string_view verb = capabilityConsentVerb(static_cast<Capability>(code));
        CHECK(!verb.empty());
        CHECK(verb != "use a terminal capability");
    }
    CHECK(capabilityConsentVerb(static_cast<Capability>(0xBEEF)) == "use a terminal capability");

    // --- Lifetime disclosure keyed to the daemon's tier-forced rules. The copy
    //     must not over-promise persistence for the force-OneShot tiers, nor
    //     under-promise it for the disk-persisted LowRiskInfo Always. ---
    // HostEscaping + ReadExfil are force-gated to a single use → per-use, not remembered.
    CHECK(capabilityGrantIsPerUse(Capability::ExecuteTrigger, CapabilityOrigin::Agent)); // HostEscaping
    CHECK(capabilityGrantIsPerUse(Capability::ClipboardWrite, CapabilityOrigin::Agent)); // HostEscaping
    CHECK(capabilityGrantIsPerUse(Capability::ScreenContentRead, CapabilityOrigin::Agent)); // ReadExfil
    CHECK(capabilityGrantIsPerUse(Capability::WindowTitleRead, CapabilityOrigin::Agent)); // ReadExfil
    for (Capability c : { Capability::ExecuteTrigger, Capability::ScreenContentRead }) {
        const std::string_view note = capabilityConsentLifetimeNote(c, CapabilityOrigin::Agent);
        CHECK(note.find("asked each time") != std::string_view::npos); // re-prompted every use
        CHECK(note.find("restart") == std::string_view::npos); // never claims persistence
    }
    // A remote owner's own clipboard writes and notifications may be
    // remembered; the rest of their tier over Remote, and the same two for an
    // agent, stay single-use.
    CHECK(!capabilityGrantIsPerUse(Capability::ClipboardWrite, CapabilityOrigin::Remote));
    CHECK(!capabilityGrantIsPerUse(Capability::DesktopNotification, CapabilityOrigin::Remote));
    CHECK(capabilityGrantIsPerUse(Capability::ExecuteTrigger, CapabilityOrigin::Remote));
    CHECK(capabilityGrantIsPerUse(Capability::ClipboardRead, CapabilityOrigin::Remote));
    CHECK(capabilityGrantIsPerUse(Capability::DesktopNotification, CapabilityOrigin::Agent));
    CHECK(capabilityConsentLifetimeNote(Capability::ClipboardWrite, CapabilityOrigin::Remote).find("persists across restarts") != std::string_view::npos);
    // An origin byte outside the enumeration reads as the single-use one.
    CHECK(Ubiquitty::Security::capabilityOriginOf(0xEE) == CapabilityOrigin::Agent);
    CHECK(Ubiquitty::Security::capabilityOriginOf(std::to_underlying(CapabilityOrigin::Remote)) == CapabilityOrigin::Remote);

    // A request from the recipient's own device names the device.
    const std::uint16_t clipboardWrite = std::to_underlying(Capability::ClipboardWrite);
    CHECK(Ubiquitty::Security::ownDeviceCapabilityConsentTitle(clipboardWrite, std::to_underlying(CapabilityOrigin::Remote), "iPhone")
        == "Your iPhone (remote) asks to write to your clipboard");
    const std::string ownDetail = Ubiquitty::Security::ownDeviceCapabilityConsentDetail(clipboardWrite, std::to_underlying(CapabilityOrigin::Remote), "iPhone");
    CHECK(ownDetail.find("from your iPhone") != std::string::npos);
    CHECK(ownDetail.find("persists across restarts") != std::string::npos);
    CHECK(ownDetail.find("Verified") == std::string::npos);

    // LowRiskInfo grants can persist (Always saves to disk across restarts) → the
    // copy must disclose that (the security-material under-promise direction).
    CHECK(!capabilityGrantIsPerUse(Capability::SetWorkingDirectory, CapabilityOrigin::Agent)); // LowRiskInfo
    CHECK(!capabilityGrantIsPerUse(Capability::SetHyperlink, CapabilityOrigin::Agent)); // LowRiskInfo
    {
        const std::string_view note = capabilityConsentLifetimeNote(Capability::SetWorkingDirectory, CapabilityOrigin::Agent);
        CHECK(note.find("persists across restarts") != std::string_view::npos);
    }

    // --- Fail-safe consent mapping: the dialog's response→decision must
    //     Deny on a dismiss / unexpected response, never grant; a per-use capability
    //     can never yield Session/Always. (A `default:` that fell through to
    //     AllowAlways would let a dismissed prompt GRANT.) ---
    using Ubiquitty::Security::consentDecisionForButtonIndex;
    using D = Ubiquitty::Protocol::Sync::ConsentChoice;
    // The four explicit buttons, in display order.
    CHECK(consentDecisionForButtonIndex(0, /*isPerUse*/ false) == D::Deny);
    CHECK(consentDecisionForButtonIndex(1, false) == D::AllowOnce);
    CHECK(consentDecisionForButtonIndex(2, false) == D::AllowSession);
    CHECK(consentDecisionForButtonIndex(3, false) == D::AllowAlways);
    // FAIL-SAFE: a dismissed sheet (NSModalResponse < NSAlertFirstButtonReturn →
    // negative index), or any out-of-range / unknown response, is Deny — not a grant.
    CHECK(consentDecisionForButtonIndex(-1, false) == D::Deny);
    CHECK(consentDecisionForButtonIndex(-1000, false) == D::Deny);
    CHECK(consentDecisionForButtonIndex(4, false) == D::Deny);
    CHECK(consentDecisionForButtonIndex(99, false) == D::Deny);
    // A per-use (force-OneShot) capability never presents Session/Always; those
    // indices — and a dismiss — fail safe to Deny, so a per-use grant is never
    // Session/Always regardless of the index handed in.
    CHECK(consentDecisionForButtonIndex(1, /*isPerUse*/ true) == D::AllowOnce);
    CHECK(consentDecisionForButtonIndex(2, true) == D::Deny);
    CHECK(consentDecisionForButtonIndex(3, true) == D::Deny);
    CHECK(consentDecisionForButtonIndex(-1, true) == D::Deny);
}

// The origin descriptor every prompt and banner names a place with. Shared with
// the notification-origin work, which needs the same workspace/window/tab/pane
// path — one descriptor, so the same terminal reads the same way wherever it is
// named.
// A question a client asks of its own policy about an effect its remote host
// delivered keeps nothing, so it names the connection as the widest lifetime,
// never "Always" or a grant saved on the host; a per-use capability says it is
// asked each time.
TEST(aClientHeldQuestionPromisesOnlyTheConnection)
{
    using Ubiquitty::Security::hostEffectConnectionScopeTitle;
    using Ubiquitty::Security::hostEffectConsentDetail;
    using Ubiquitty::Security::hostEffectConsentTitle;
    const auto code = [](Capability capability) { return static_cast<std::uint16_t>(std::to_underlying(capability)); };
    CHECK(hostEffectConnectionScopeTitle() == "Allow for This Connection");
    CHECK(hostEffectConsentTitle(code(Capability::ClipboardWrite)) == "A program on a remote host asks to write to your clipboard");
    CHECK(hostEffectConsentTitle(code(Capability::RequestAttention)) == "A program on a remote host asks to make this window demand attention");
    for (const Capability capability : { Capability::ClipboardWrite, Capability::DesktopNotification }) {
        const std::string detail = hostEffectConsentDetail(code(capability), "Mac");
        CHECK(detail.starts_with("The host allowed it. Your Mac asks too"));
        CHECK(detail.find("Allow for This Connection covers requests like this until the connection ends. Nothing is saved.") != std::string::npos);
        CHECK(detail.find("Always") == std::string::npos);
        CHECK(detail.find("persists") == std::string::npos);
    }
    for (const Capability capability : { Capability::PlaySound, Capability::RequestAttention }) {
        const std::string detail = hostEffectConsentDetail(code(capability), "iPhone");
        CHECK(detail.find("Your iPhone asks too") != std::string::npos);
        CHECK(detail.find("You’ll be asked each time") != std::string::npos);
        CHECK(detail.find("This Connection") == std::string::npos);
    }
}

TEST(sessionOriginDescription)
{
    using Ubiquitty::Session::describeSessionOrigin;
    using Ubiquitty::Session::SessionOrigin;

    // Nothing known: no words, so the caller says so itself rather than
    // printing an empty line.
    CHECK(SessionOrigin {}.isEmpty());
    CHECK(describeSessionOrigin(SessionOrigin {}).empty());

    // Every part labelled, outermost place first.
    SessionOrigin full;
    full.workspace = "Beam";
    full.window = "Build";
    full.tab = "src";
    full.pane = "2";
    full.session = "deploy";
    CHECK(!full.isEmpty());
    CHECK(describeSessionOrigin(full) == "workspace “Beam” › window “Build” › tab “src” › pane “2” › session “deploy”");

    // A part nobody holds is omitted; the separator never leads or doubles.
    SessionOrigin partial;
    partial.window = "Build";
    partial.pane = "2";
    const std::string description = describeSessionOrigin(partial);
    CHECK(description == "window “Build” › pane “2”");
    CHECK(description.find("workspace") == std::string::npos);
    CHECK(!description.starts_with(" "));

    // A window title is set by the program in the session, which for a consent
    // prompt is the program the reader is being asked about. A title carrying
    // this line's own quotation marks and separator renders as the prompt's own
    // sentences unless they are taken out of it first.
    SessionOrigin forged;
    forged.window = "Build” › pane “1”. Drive access is held by /usr/bin/ssh";
    const std::string sanitized = describeSessionOrigin(forged);
    CHECK(sanitized.find("›") == std::string::npos); // one part, so no separator survives
    CHECK(sanitized.find("pane “") == std::string::npos);
    CHECK(sanitized.starts_with("window “"));
    CHECK(sanitized.ends_with("”"));

    // C0 bytes and runs of blanks fold, so a title cannot push the labelled
    // parts off the line or open a second one.
    SessionOrigin noisy;
    noisy.window = "a\r\n\tb    c ";
    CHECK(describeSessionOrigin(noisy) == "window “a b c”");

    // A title runs to thousands of bytes; the line shows a name, and cuts on a
    // UTF-8 boundary so the ellipsis never lands mid-sequence.
    SessionOrigin long_;
    long_.window = std::string(40, 'x') + "日本語日本語日本語日本語日本語";
    const std::string cut = describeSessionOrigin(long_);
    CHECK(cut.size() < long_.window.size());
    CHECK(cut.ends_with("…”"));
}

// The capability prompt's title and body. Three separate facts — the place, the
// foreground chain as the guess at the sender, the principal holding drive
// access — one statement of the single thing that is uncertain, and the
// capability's numeric code whenever its effect cannot be stated. A prompt that
// names no program and no capability, and disclaims its own attribution beside
// them, asks a question nobody can answer.
TEST(capabilityConsentAttribution)
{
    using Ubiquitty::Security::capabilityCodeDescription;
    using Ubiquitty::Security::capabilityCodeIsNamed;
    using Ubiquitty::Security::CapabilityConsentAttribution;
    using Ubiquitty::Security::capabilityConsentAttributionText;
    using Ubiquitty::Security::capabilityConsentDetail;
    using Ubiquitty::Security::capabilityConsentTitle;

    const auto clipboardRead = static_cast<std::uint16_t>(std::to_underlying(Capability::ClipboardRead));

    // A described capability states its effect.
    CHECK(capabilityConsentTitle(clipboardRead) == "A program in this session wants to read your clipboard");

    // A code with no effect line names the code — decimal and the hex the wire
    // carries — instead of the generic phrasing, because a capability reaching
    // a prompt unrecognised is itself the thing worth showing.
    CHECK(capabilityCodeDescription(0xBEEF) == "48879 (0xbeef)");
    CHECK(!capabilityCodeIsNamed(0xBEEF));
    const std::string unknownTitle = capabilityConsentTitle(0xBEEF);
    CHECK(unknownTitle.find("48879 (0xbeef)") != std::string::npos);
    CHECK(unknownTitle.find("use a terminal capability") == std::string::npos);

    // Every enumerator states an effect, so the numeric title is reserved for a
    // code this build has no vocabulary for. A prompt that answers "code 19" for
    // a capability the build names is worse than a vague phrase: it hands the
    // reader a number and a false statement.
    for (std::uint16_t code = 0; code <= static_cast<std::uint16_t>(Capability::None); ++code) {
        CHECK(capabilityCodeIsNamed(code));
        const std::string title = capabilityConsentTitle(code);
        CHECK(title.find("cannot name") == std::string::npos);
        CHECK(title.find("use a terminal capability") == std::string::npos);
    }
    CHECK(capabilityConsentTitle(static_cast<std::uint16_t>(std::to_underlying(Capability::DisplayPower)))
        == "A program in this session wants to turn your display off");

    // Everything known: the place, the foreground chain as the best guess at the
    // sender, and the principal as the holder of drive access. The principal is
    // never phrased as the sender, and the prompt does not tell the reader its
    // own attribution is worthless.
    CapabilityConsentAttribution full;
    full.origin.window = "Build";
    full.origin.tab = "src";
    full.foregroundProcesses = "zsh › node";
    full.principal = "/usr/bin/agent (pid 4211)";
    full.agentLabel = "Backup Helper";
    const std::string text = capabilityConsentAttributionText(full);
    CHECK(text.find("window “Build”") != std::string::npos);
    CHECK(text.find("tab “src”") != std::string::npos);
    CHECK(text.find("zsh › node") != std::string::npos);
    CHECK(text.find("/usr/bin/agent (pid 4211)") != std::string::npos);
    CHECK(text.find("Backup Helper") != std::string::npos);
    CHECK(text.find("not verified") != std::string::npos);
    CHECK(text.find("cannot tell which program") == std::string::npos);
    CHECK(text.find("unidentified process") == std::string::npos);
    // The uncertainty is stated once, and about the sender rather than about
    // the prompt: two "closest" clauses would be two disclaimers.
    CHECK(text.find("a sample taken as this prompt was built") != std::string::npos);
    CHECK(text.find("a sample taken", text.find("a sample taken") + 1) == std::string::npos);

    // The worst case — nothing attached, nothing sampled. The prompt names the
    // one fact it lacks instead of asking about an unidentified process.
    const std::string bare = capabilityConsentAttributionText(CapabilityConsentAttribution {});
    CHECK(bare.find("cannot name the program that sent the request") != std::string::npos);
    CHECK(bare.find("unidentified") == std::string::npos);
    CHECK(bare.find("Drive access") == std::string::npos); // no principal invented
    CHECK(bare.find('\n') == std::string::npos); // one line, one unknown

    // A principal with no foreground sample still separates the two facts: the
    // terminal admits it cannot see the sender, and names who holds drive access.
    CapabilityConsentAttribution principalOnly;
    principalOnly.principal = "agent";
    const std::string held = capabilityConsentAttributionText(principalOnly);
    CHECK(held.find("cannot name the program that sent the request") != std::string::npos);
    CHECK(held.find("Drive access on this session is held by agent.") != std::string::npos);
    CHECK(held.find("“") == std::string::npos); // no empty quotation where a label would be

    // The body carries the lifetime the daemon enforces, after the attribution.
    const std::string detail = capabilityConsentDetail(clipboardRead, std::to_underlying(CapabilityOrigin::Agent), full);
    CHECK(detail.starts_with(text));
    CHECK(detail.find("asked each time") != std::string::npos);
    // An unrecognised code falls to the strictest tier, so its body promises no
    // persistence either.
    const std::string unknownDetail = capabilityConsentDetail(0xBEEF, std::to_underlying(CapabilityOrigin::Remote), CapabilityConsentAttribution {});
    CHECK(unknownDetail.find("asked each time") != std::string::npos);
    CHECK(unknownDetail.find("persists across restarts") == std::string::npos);
}

UBIQUITTY_TEST_MAIN()
