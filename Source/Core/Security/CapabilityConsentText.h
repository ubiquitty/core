// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// CapabilityConsentText.h — the human-facing words of a capability-consent
// prompt: the title "A program in this session wants to <verb>", and the body
// that says where the asking session is and who is attached to it.
//
// The prompt states three separate facts and keeps them separate, because they
// answer different questions. Where the session sits is certain (SessionOrigin).
// What is running on that PTY is the closest observable stand-in
// for the emitter, and is named as a guess. The principal is the strictest
// driver attached with drive access — the gate's attribution, not evidence that
// it emitted the sequence — so it is named as what holds drive access and never
// as the actor. The one uncertainty the prompt carries is which program emitted
// the sequence, and it says so once, on the line that makes the guess: a prompt
// that disowned its own attribution in the same breath would leave the reader
// with no fact at all.
//
// Each Capability maps to a plain-English
// statement of the real-world EFFECT (not the escape-sequence name), so the
// highest-severity grants (run a command / remap keys / read your screen) read
// as blunt warnings rather than neutral labels. Plain C++ (no ObjC/Swift/AppKit)
// so it is host-tested; every arm's capability-consent dialog consumes it.
// Beside the tier classification it reads, because which grants are
// remembered and which answer a non-answer is that classification's to say.

#include "Core/Protocol/Sync/Consent.h"
#include "Core/Session/SessionOrigin.h"
#include "PAL/Capability.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Security {

// The phrasing for a capability this build states no effect for. A prompt that
// renders this has nothing to say about what it is asking, so the title names
// the capability's numeric code instead — see `capabilityConsentTitle`. Reached
// only by a code outside the enum: the map below covers every enumerator, and
// its switch is total, so a new capability fails to compile without a line.
inline constexpr std::string_view capabilityConsentGenericVerb = "use a terminal capability";

// The consent-prompt verb for `capability`. Covers the prompt-reachable set
// (an agent reaches the HostEscaping + ReadExfil tiers, a remote the LowRiskInfo
// tier); any other value falls back to a generic phrasing so a newly-added
// capability that reaches a prompt still reads sensibly before it earns a line.
[[nodiscard]] inline std::string_view capabilityConsentVerb(Capability capability)
{
    switch (capability) {
    case Capability::ExecuteTrigger:
        return "run a command on your Mac";
    case Capability::ProgramKeys:
        return "remap what your keys type";
    case Capability::ClipboardRead:
        return "read your clipboard";
    case Capability::ClipboardWrite:
        return "write to your clipboard";
    case Capability::ScreenContentRead:
        return "read the text on your screen";
    case Capability::WindowTitleRead:
        return "read this window’s title";
    case Capability::FileReadAccess:
        return "read files from your Mac";
    case Capability::SetFont:
        return "change the terminal font (can spoof glyphs)";
    case Capability::SetColor:
    case Capability::SelectColorPreset:
        return "change the terminal colors";
    case Capability::SetHyperlink:
        return "add clickable links to the output";
    case Capability::ImageRender:
        return "display images in the terminal";
    case Capability::RichTextRender:
        return "display styled text blocks in the terminal";
    case Capability::StealFocus:
        return "take keyboard focus";
    case Capability::RequestAttention:
        return "make this window demand attention";
    case Capability::WindowMutate:
        return "move, resize, or minimize this window";
    case Capability::DesktopNotification:
        return "post a desktop notification";
    case Capability::PlaySound:
        return "play sound through your speakers";
    case Capability::SetWindowTitle:
        return "change this window’s title";
    case Capability::SetWorkingDirectory:
        return "set the reported working directory";
    case Capability::Answerback:
        return "send its identifying answerback string";
    case Capability::SetCurrentFileURL:
        return "report a file as the one this session is editing";
    case Capability::SetUserVariable:
        return "set a session variable other programs can read";
    case Capability::SetRemoteHost:
        return "change the machine this session reports it is on";
    case Capability::SetCursorShape:
        return "change the cursor’s shape";
    case Capability::ProgressIndicator:
        return "show progress in the system Dock or taskbar";
    case Capability::DisplayPower:
        return "turn your display off";
    case Capability::DeviceAttributesRead:
        return "read which terminal and version this is";
    case Capability::StatusReport:
        return "read this terminal’s status";
    case Capability::CursorStateRead:
        return "read where the cursor is and how it is set";
    case Capability::TerminalSizeRead:
        return "read this terminal’s size";
    case Capability::WindowGeometryRead:
        return "read where this window sits on your screen";
    case Capability::ColorRead:
        return "read the terminal’s colors";
    case Capability::ModeStateRead:
        return "read which terminal modes are set";
    case Capability::GraphicsCapabilityRead:
        return "read what graphics this terminal supports";
    case Capability::TerminfoRead:
        return "read this terminal’s capability database";
    case Capability::TextMeasureRead:
        return "measure text in the host’s font";
    case Capability::LocatorReport:
        return "read your pointer’s position";
    case Capability::InputModeReport:
        return "receive your mouse and focus activity";
    case Capability::TektronixAddressRead:
        return "read the Tektronix cursor address";
    case Capability::None:
        return "send a protocol acknowledgement";
    }
    return capabilityConsentGenericVerb;
}

// Whether `code` names a capability this build knows. The enumerators run
// contiguously from zero to `None`, so a code past it is one this build has no
// vocabulary for at all — which is the only case the prompt has to admit it
// cannot describe, because `capabilityConsentVerb` is total over the enum.
[[nodiscard]] inline bool capabilityCodeIsNamed(std::uint16_t code)
{
    return code <= static_cast<std::uint16_t>(Capability::None);
}

// `code` as the human-readable pair a reader can match against a capability
// table and a trace: decimal, then the hex the wire carries.
[[nodiscard]] inline std::string capabilityCodeDescription(std::uint16_t code)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex = "0x";
    for (int shift = 12; shift >= 0; shift -= 4)
        hex += digits[(code >> shift) & 0xF];
    return std::to_string(code) + " (" + hex + ")";
}

// The consent-prompt title for the capability `code` asks for. A described
// capability states its effect; one this build cannot describe names its code,
// because a capability arriving at a prompt unrecognised is itself the thing
// worth showing — "a terminal capability" would leave the reader with neither
// the effect nor the number to look up.
[[nodiscard]] inline std::string capabilityConsentTitle(std::uint16_t code)
{
    if (!capabilityCodeIsNamed(code))
        return "A program in this session wants a terminal capability this version of Ubiquitty can’t name—code " + capabilityCodeDescription(code);
    const std::string_view verb = capabilityConsentVerb(static_cast<Capability>(code));
    return "A program in this session wants to " + std::string(verb);
}

/// Holds what a capability-consent prompt knows about where a request came from.
///
/// The sequence arrives on a session's PTY and carries no author, so these are
/// separate facts and the prompt keeps them separate. `origin` is where the
/// session sits. `foregroundProcesses` is what holds that PTY as the prompt is
/// built, which is the closest thing to the emitter the terminal observes and is
/// a guess: an emitter that has already exited leaves the shell standing there. `principal` is the strictest driver attached with drive access, which
/// is the gate's attribution rather than evidence of who emitted anything, and
/// `agentLabel` is that principal's own claim about itself, never an identity.
/// An empty field is a fact the arm filling this does not hold.
struct CapabilityConsentAttribution {
    Session::SessionOrigin origin;
    std::string foregroundProcesses;
    std::string principal;
    std::string agentLabel;
};

// The prompt's attribution block: where the session is, the best available
// guess at the sender, and the principal holding drive access, each on its own
// line. The uncertainty is stated once and names the fact that is uncertain —
// which program emitted the sequence — so a prompt that did identify a
// principal does not also disown its own attribution.
[[nodiscard]] inline std::string capabilityConsentAttributionText(const CapabilityConsentAttribution& attribution)
{
    std::string text;
    const auto line = [&text](const std::string& sentence) {
        if (!text.empty())
            text += '\n';
        text += sentence;
    };
    if (const std::string where = describeSessionOrigin(attribution.origin); !where.empty())
        line("In " + where + ".");
    if (!attribution.foregroundProcesses.empty()) {
        line("Running there now: " + attribution.foregroundProcesses
            + " — the request came from that terminal. This is a sample taken as this prompt was built, not the program that sent it.");
    } else {
        line("The terminal cannot see what is running there, so it cannot name the program that sent the request.");
    }
    if (!attribution.principal.empty()) {
        std::string held = "Drive access on this session is held by " + attribution.principal + ".";
        if (!attribution.agentLabel.empty())
            held += " It calls itself “" + attribution.agentLabel + "”, which is not verified.";
        line(held);
    }
    return text;
}

// The origin a consent question's `capabilityOrigin` value names. A value outside
// the enumeration reads as Agent, whose host-escaping and exfil grants are all
// single-use: the reading that cannot offer a lifetime the daemon would not keep.
[[nodiscard]] inline CapabilityOrigin capabilityOriginOf(std::uint8_t raw)
{
    switch (raw) {
    case static_cast<std::uint8_t>(CapabilityOrigin::Local):
        return CapabilityOrigin::Local;
    case static_cast<std::uint8_t>(CapabilityOrigin::Remote):
        return CapabilityOrigin::Remote;
    default:
        return CapabilityOrigin::Agent;
    }
}

// The accurate lifetime disclosure for a granted `capability`, per the daemon's
// tier-forced rules: the per-use tiers are re-asked every time (no persistence),
// while a LowRiskInfo Always-grant is saved to disk and survives a host restart.
// The copy must neither over-promise persistence for the forced-OneShot tiers
// nor under-promise it for the disk-persisted Always (the security-material
// direction). Principal-free so it is a stable, host-testable string; the dialog
// names the verified principal separately.
[[nodiscard]] inline std::string_view capabilityConsentLifetimeNote(Capability capability, CapabilityOrigin origin)
{
    if (capabilityGrantIsPerUse(capability, origin))
        return "You’ll be asked each time—this isn’t remembered.";
    return "Always Allow saves this on this host and persists across restarts.";
}

// The title of a capability prompt that is the recipient's own request: the
// program asking runs in a session this device drives, so the person knows
// whose device it is and the prompt names it. `device` is the client's own name
// for its kind ("iPhone", "Mac"); `origin` says how it reaches the host.
[[nodiscard]] inline std::string ownDeviceCapabilityConsentTitle(std::uint16_t code, std::uint8_t origin, const std::string& device)
{
    const std::string where = capabilityOriginOf(origin) == CapabilityOrigin::Remote ? " (remote)" : "";
    const std::string_view verb = capabilityCodeIsNamed(code) ? capabilityConsentVerb(static_cast<Capability>(code)) : capabilityConsentGenericVerb;
    return "Your " + device + where + " asks to " + std::string(verb);
}

// The body of that prompt: who sent it, then the lifetime the daemon enforces.
[[nodiscard]] inline std::string ownDeviceCapabilityConsentDetail(std::uint16_t code, std::uint8_t origin, const std::string& device)
{
    std::string detail = "A program you are running in this session from your " + device + " sent the request.\n";
    detail += capabilityConsentLifetimeNote(static_cast<Capability>(code), capabilityOriginOf(origin));
    return detail;
}

/// The allowing button of a question a client asks of its own policy, for an
/// answer that covers more than the one effect.
///
/// It lasts as long as the connection and nothing is saved, so it is never
/// called "Always".
///
/// - Returns: The button's title.
[[nodiscard]] inline std::string hostEffectConnectionScopeTitle()
{
    return "Allow for This Connection";
}

/// The title of a question a client asks of its own policy about an effect a
/// host on another machine delivered.
///
/// The program runs on that host, and what it asks for happens on this device.
///
/// - Parameter code: The capability the effect needs.
/// - Returns: The title.
[[nodiscard]] inline std::string hostEffectConsentTitle(std::uint16_t code)
{
    const std::string_view verb = capabilityCodeIsNamed(code) ? capabilityConsentVerb(static_cast<Capability>(code)) : capabilityConsentGenericVerb;
    return "A program on a remote host asks to " + std::string(verb);
}

/// The body of that question: why this device asks after the host allowed it,
/// then the lifetime the client keeps.
///
/// - Parameters:
///   - code: The capability the effect needs.
///   - device: The client's own name for its kind ("iPhone", "Mac").
/// - Returns: The body, two lines.
[[nodiscard]] inline std::string hostEffectConsentDetail(std::uint16_t code, const std::string& device)
{
    std::string detail = "The host allowed it. Your " + device + " asks too, because the host is another computer.\n";
    if (capabilityGrantIsPerUse(static_cast<Capability>(code), CapabilityOrigin::Remote))
        detail += "You’ll be asked each time—this isn’t remembered.";
    else
        detail += hostEffectConnectionScopeTitle() + " covers requests like this until the connection ends. Nothing is saved.";
    return detail;
}

// The capability prompt's whole explanatory body: the attribution block, then
// the lifetime the daemon enforces for a grant of `code`. One function so every
// arm's dialog says the same thing in the same order.
[[nodiscard]] inline std::string capabilityConsentDetail(std::uint16_t code, std::uint8_t origin, const CapabilityConsentAttribution& attribution)
{
    std::string detail = capabilityConsentAttributionText(attribution);
    const std::string_view lifetime = capabilityConsentLifetimeNote(static_cast<Capability>(code), capabilityOriginOf(origin));
    if (!detail.empty())
        detail += '\n';
    detail += lifetime;
    return detail;
}

// The consent decision for the chosen dialog button, given its 0-based index in
// display order: 0 = Deny, 1 = Allow Once, 2 = Allow This Session, 3 = Always Allow.
// Any other index — a dismissed sheet, an unexpected NSModalResponse, a stale
// value — is DENY: a consent prompt that was not affirmatively answered must never
// grant (fail-safe: a `default:` falling through to Always would let a
// dismiss GRANT). For a per-use (force-OneShot) capability the session/always
// buttons are never presented, so those indices also fail safe to Deny — a per-use
// grant can never be Session or Always regardless of the index handed in. Pure
// (no AppKit) so the fail-safe is host-tested; the dialog maps its NSModalResponse
// to the index and casts the result to the value-identical UBConsentDecision.
[[nodiscard]] inline Protocol::Sync::ConsentChoice consentDecisionForButtonIndex(int buttonIndex,
    bool isPerUse) // bool-ok: isPerUse is the capability's runtime per-use tier (data), not a call-site constant
{
    switch (buttonIndex) {
    case 1:
        return Protocol::Sync::ConsentChoice::AllowOnce;
    case 2:
        return isPerUse ? Protocol::Sync::ConsentChoice::Deny : Protocol::Sync::ConsentChoice::AllowSession;
    case 3:
        return isPerUse ? Protocol::Sync::ConsentChoice::Deny : Protocol::Sync::ConsentChoice::AllowAlways;
    default: // 0 = Deny, plus dismiss / unknown response → fail safe
        return Protocol::Sync::ConsentChoice::Deny;
    }
}

} // namespace Ubiquitty::Security
