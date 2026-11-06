// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Canonical capability vocabulary for the Core <-> host boundary. Every grid-
// escaping side effect (writes host state, reads it back to the program, or acts
// on the host window/filesystem) is one of these. The host supplies a
// CapabilityPolicy (PAL/TerminalClients.h) that decides each request,
// default-deny. One enum shared by Core, the host daemon, and the agent MCP
// surface so parallel allow-lists cannot drift. Lives in Source/Platform (not
// Source/Protocol) so Core can include it without an up-dependency.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace Ubiquitty {

enum class Capability : std::uint16_t {
    // ── exec / injection (highest severity) ──
    ExecuteTrigger, // TriggerClient run= — arbitrary host exec from output content
    ProgramKeys, // DECUDK — reprograms what a key sends (input-injection)

    // ── writes (perturb host UI/state; the program learns nothing) ──
    SetWindowTitle, // OSC 0/1/2, XTWINOPS title push
    SetWorkingDirectory, // OSC 7, OSC 1337 CurrentDir=
    SetCurrentFileURL, // OSC 6
    SetHyperlink, // OSC 8
    SetUserVariable, // OSC 1337 SetUserVar=
    SetRemoteHost, // OSC 1337 RemoteHost=, OSC 7 authority
    SetCursorShape, // OSC 1337 CursorShape=, DECSCUSR
    SetColor, // OSC 4/10/11/12/13/14/17/19 set, OSC 22 restore, OSC 1337 SetColors=
    SetFont, // OSC 50 set, DECDLD soft font (remote glyphs = display-spoofing)
    ClipboardWrite, // OSC 52 set, OSC 1337 Copy=
    ImageRender, // sixel, Kitty placement, OSC 1337 inline-image decode+store

    // ── act-beyond-grid (host window / desktop / display) ──
    DesktopNotification, // OSC 9 iTerm, OSC 99 kitty, OSC 777 urxvt
    ProgressIndicator, // External Dock/taskbar progress, not pane/tab metadata.
    RequestAttention, // OSC 1337 RequestAttention=
    StealFocus, // OSC 1337 StealFocus
    WindowMutate, // XTWINOPS move/resize/raise/lower/min/max/fullscreen
    SelectColorPreset, // OSC 1337 SetColors=preset=
    DisplayPower, // Linux CSI 9/13/14 ] (DPMS)
    PlaySound, // DECPS CSI , ~ play-sound, DCS + s sample download — the one
               //   effect that leaves the screen and reaches the room

    // ── reads: the program reads host state back (strictest tier) ──
    DeviceAttributesRead, // DA1/DA2/DA3, XTVERSION, DECREQTPARM, VT52 ident
    StatusReport, // DSR n/?n, focus report
    CursorStateRead, // CPR, DECXCPR, DECCIR, DECRQSS
    TerminalSizeRead, // XTWINOPS 18/19 t, DECRQDE
    WindowGeometryRead, // XTWINOPS 13/14/15/16 t (pixel positions/sizes)
    WindowTitleRead, // XTWINOPS 20/21 t
    ColorRead, // OSC 4/10/11/12/13/14/17/19 query, DCS +q color
    ScreenContentRead, // DECRQCRA checksum, DECRQPSR — reconstruct cells never written
    ModeStateRead, // DECRQM, kitty-kbd ?u, xterm modifyKeys
    GraphicsCapabilityRead, // XTSMGRAPHICS
    TerminfoRead, // XTGETTCAP (DCS +q)
    TextMeasureRead, // OSC 66 p=1:q=1, OSC 7404 a=q — what text needs in the viewer's faces
    LocatorReport, // DECRQLP, DEC locator
    InputModeReport, // mouse 1000-1006/1015/1016 + focus 1004 + bracketed-paste 2004
    //   (ongoing exfil; gate at the mode ENABLE, not each report)
    Answerback, // ENQ -> answerback string
    TektronixAddressRead, // Tek 4014 ENQ status / GIN address report
    ClipboardRead, // OSC 52 ? — reads the host clipboard
    FileReadAccess, // Kitty t=f/t=t — reads the host filesystem

    // ── grid content, appended after the reads so no persisted code moves ──
    RichTextRender, // OSC 7404 a=p / a=d — place or remove a rich text block

    // A reply that reveals zero host state — a pure protocol ACK. Routed through
    // check() as the Benign tier (always-allow), never a check()-bypass sentinel:
    // every responder still consults the policy and stays auditable.
    None,
};

/// Who crossed the boundary. The same capability gets a different verdict by
/// principal (e.g. FileReadAccess: prompt-for-Local, hard-deny-Remote). Local
/// input is all Core sees directly; the agent MCP surface stamps Agent, a
/// consent-granted attach stamps Remote.
enum class CapabilityOrigin : std::uint8_t {
    Local,
    Agent,
    Remote
};

enum class CapabilityDecision : std::uint8_t {
    Deny, // forbidden — Core no-ops the side effect / drops the reply
    Allow, // permitted
    Prompt, // ask the user; Core must not block, so it parks (treats as Deny-for-now)
            //   and the frontend surfaces the grant prompt, then the host re-grants
};

// Severity tier — backbone of the default verdict, read through tierOf(). Lives
// here (not the daemon's policy header) so the daemon grant gate and every
// consent-copy disclosure key on one tier source — no drift.
enum class CapabilityTier : std::uint8_t {
    HostEscaping, // exec, file reads, clipboard writes, host window/desktop acts — top blast radius
    ReadExfil, // reads more than feature/cursor: arbitrary cells / off-screen title / the clipboard
    LowRiskInfo, // display-metadata writes (cwd/host/file/title) the host shows trivially
    Benign, // feature/cursor reads + a pure display-string write — reveal nothing past the rendered screen
    Rendering, // grid content, not a host escape
};

// Maps each Capability to its tier. The classification is security's semantics;
// an unmapped value falls through to the strictest tier (fail-closed).
[[nodiscard]] constexpr CapabilityTier tierOf(Capability capability)
{
    switch (capability) {
    case Capability::ExecuteTrigger:
    case Capability::ProgramKeys:
    case Capability::ClipboardWrite:
    case Capability::FileReadAccess:
    case Capability::Answerback:
    case Capability::SetUserVariable:
    case Capability::DesktopNotification:
    case Capability::ProgressIndicator:
    case Capability::RequestAttention:
    case Capability::StealFocus:
    case Capability::WindowMutate:
    case Capability::DisplayPower:
    // Sound escapes further than any other effect here: it reaches a person who
    // is not looking at the screen, and people beside them. A program can hold
    // eight channels and ten-second samples, so an ungated one is a speaker a
    // remote peer owns.
    case Capability::PlaySound:
        return CapabilityTier::HostEscaping;
    // True exfil: read content beyond the rendered/teleported screen. DECRQCRA
    // checksums arbitrary cells; the title carries cwd/command/user@host and is
    // window chrome a viewer often cannot see; the clipboard holds whatever the
    // user copied last, off-screen secrets included.
    case Capability::ClipboardRead:
    case Capability::ScreenContentRead:
    case Capability::WindowTitleRead:
        return CapabilityTier::ReadExfil;
    // Feature/cursor reads — reveal nothing past the already-rendered screen the
    // attached peer sees, and programs BLOCK on them for capability detection, so
    // a blanket Remote-Deny would hang vim/tmux on a teleport-viewed session.
    // SetWindowTitle is a pure display-string write (its readback stays Deny);
    // None is a reply that reveals zero host state (a pure protocol ACK).
    case Capability::DeviceAttributesRead:
    case Capability::StatusReport:
    case Capability::CursorStateRead:
    case Capability::TerminalSizeRead:
    case Capability::WindowGeometryRead:
    case Capability::ColorRead:
    case Capability::ModeStateRead:
    case Capability::GraphicsCapabilityRead:
    case Capability::TerminfoRead:
    case Capability::TextMeasureRead:
    case Capability::LocatorReport:
    case Capability::InputModeReport:
    case Capability::TektronixAddressRead:
    case Capability::SetWindowTitle:
    case Capability::None:
        return CapabilityTier::Benign;
    // CellGrid metadata: the program INFORMS the terminal of cwd/host/file/colour;
    // no host escape. Allowed for every origin by default; a configuration that
    // distrusts its remote programs' presentation (fg=bg hiding, remote-supplied
    // DECDLD glyphs) restricts this tier to Prompt or Deny.
    case Capability::SetWorkingDirectory:
    case Capability::SetCurrentFileURL:
    case Capability::SetRemoteHost:
    case Capability::SetHyperlink:
    case Capability::SetCursorShape:
    case Capability::SetColor:
    case Capability::SetFont:
    case Capability::SelectColorPreset:
        return CapabilityTier::LowRiskInfo;
    case Capability::ImageRender:
    case Capability::RichTextRender:
        return CapabilityTier::Rendering;
    }
    return CapabilityTier::HostEscaping; // unmapped capability — strictest, fail-closed
}

// The per-origin default when no grant exists and the configuration names no
// override. Local trusts its own programs (they already run with the user's OS
// privileges). Agent is semi-trusted: the first host-escaping or exfil use
// prompts the human. Remote is the owner's own device attached over SSH or QUIC
// and is least-trusted for what leaves the grid: host-escaping and exfil are
// denied without an explicit human grant. Its informational tier is allowed —
// a hyperlink, a cursor shape, a colour, a cwd note changes nothing outside the
// grid the reader is already looking at, and a prompt for each would train the
// reflexive "allow" that consent exists to prevent. A configuration restricts
// any of the three configurable tiers per origin (CapabilityDecisionTable), and
// the per-capability overload below refines two remote host-escaping effects.
[[nodiscard]] constexpr CapabilityDecision defaultDecision(CapabilityTier tier, CapabilityOrigin origin)
{
    switch (origin) {
    case CapabilityOrigin::Local:
        return CapabilityDecision::Allow;
    case CapabilityOrigin::Agent:
        switch (tier) {
        case CapabilityTier::HostEscaping:
        case CapabilityTier::ReadExfil:
            return CapabilityDecision::Prompt;
        case CapabilityTier::LowRiskInfo:
        case CapabilityTier::Benign:
        case CapabilityTier::Rendering:
            return CapabilityDecision::Allow;
        }
        break;
    case CapabilityOrigin::Remote:
        switch (tier) {
        case CapabilityTier::HostEscaping:
        case CapabilityTier::ReadExfil:
            return CapabilityDecision::Deny;
        case CapabilityTier::LowRiskInfo:
        case CapabilityTier::Benign:
        case CapabilityTier::Rendering:
            return CapabilityDecision::Allow;
        }
        break;
    }
    return CapabilityDecision::Deny; // fail-closed
}

// The two host-escaping effects a remote session's owner is asked about rather
// than refused. A clipboard write and a desktop notification from a remote
// session are the owner's own device acting for the owner — a yank in a remote
// editor, a build saying it finished — and a Deny fails them silently on the
// device that asked. They stay host-escaping (a write can stage paste-injection
// data, a notification can impersonate the system), so the owner is asked, and
// the answer may be remembered: a yank that re-asked every time would be a
// prompt per copy. The rest of the tier and all of ReadExfil keep the tier's
// Deny and its single-use grants.
[[nodiscard]] constexpr bool isAskedOfRemoteOwner(Capability capability)
{
    return capability == Capability::ClipboardWrite || capability == Capability::DesktopNotification;
}

// The per-capability default: the tier's, refined for isAskedOfRemoteOwner.
[[nodiscard]] constexpr CapabilityDecision defaultDecision(Capability capability, CapabilityOrigin origin)
{
    if (origin == CapabilityOrigin::Remote && isAskedOfRemoteOwner(capability))
        return CapabilityDecision::Prompt;
    return defaultDecision(tierOf(capability), origin);
}

/// The per-capability default for a policy that asks nobody.
///
/// PAL's default with a Prompt refused. A Prompt promises that a person is
/// asked and that a grant may follow; a policy that holds no consent question
/// keeps neither promise, so it answers no. A frontend policy answers this:
/// a non-Local origin keeps the replies its programs block on and the
/// informational and rendering effects that stay on the grid, and is refused
/// every host-escaping and exfiltrating effect, the two a remote owner would be
/// asked about included.
///
/// - Parameters:
///   - capability: The side effect.
///   - origin: Who drives the session.
/// - Returns: Allow or Deny, never Prompt.
[[nodiscard]] constexpr CapabilityDecision defaultDecisionWithoutConsent(Capability capability, CapabilityOrigin origin)
{
    const CapabilityDecision decision = defaultDecision(capability, origin);
    return decision == CapabilityDecision::Prompt ? CapabilityDecision::Deny : decision;
}

// Whether a grant for `capability` driven from `origin` is good for one use
// only, whatever lifetime the human picked. HostEscaping and ReadExfil are: the
// human cannot see the program's later autonomous uses, so a remembered grant
// would authorize more than they looked at. A remote owner's clipboard writes
// and notifications are not — they are the owner's own, and the prompt names
// the device that asks.
[[nodiscard]] constexpr bool capabilityGrantIsPerUse(Capability capability, CapabilityOrigin origin)
{
    if (origin == CapabilityOrigin::Remote && isAskedOfRemoteOwner(capability))
        return false;
    const CapabilityTier tier = tierOf(capability);
    return tier == CapabilityTier::HostEscaping || tier == CapabilityTier::ReadExfil;
}

// Whether a configuration may override the tier's default. Benign and Rendering
// are not: a feature read or a grid write that a blanket Deny suppressed would
// hang programs that block on the reply and blank the screen the peer is
// viewing, with no host effect to protect against.
[[nodiscard]] constexpr bool isConfigurableTier(CapabilityTier tier)
{
    switch (tier) {
    case CapabilityTier::HostEscaping:
    case CapabilityTier::ReadExfil:
    case CapabilityTier::LowRiskInfo:
        return true;
    case CapabilityTier::Benign:
    case CapabilityTier::Rendering:
        return false;
    }
    return false;
}

/// The verdict for one tier's side effects from one origin, given the
/// configured cell if the file names one.
///
/// The rule every reader of a tier's verdict shares: a configured cell holds
/// its verdict, an unconfigured one answers the tier's default, and a value
/// cast in from outside the enumerations is refused.
///
/// - Parameters:
///   - configured: The file's verdict for the tier, if any.
///   - origin: Who drives the session.
///   - tier: The side effect's tier.
/// - Returns: The verdict.
[[nodiscard]] constexpr CapabilityDecision capabilityDecisionFor(std::optional<CapabilityDecision> configured, CapabilityOrigin origin, CapabilityTier tier)
{
    if (std::to_underlying(origin) > std::to_underlying(CapabilityOrigin::Remote) || std::to_underlying(tier) > std::to_underlying(CapabilityTier::Rendering))
        return CapabilityDecision::Deny; // a value cast in from outside the enumerations: fail-closed
    return configured.value_or(defaultDecision(tier, origin));
}

/// The verdict for one capability from one origin, given the configured cell
/// for its tier if the file names one.
///
/// The rule every reader of a capability's verdict shares: a configured cell
/// covers every capability in its tier, outranking the per-capability
/// refinement of the default, and an origin cast in from outside its
/// enumeration is refused. ``tierOf`` places every capability value in a
/// tier, an unmapped one in the strictest.
///
/// - Parameters:
///   - configured: The file's verdict for the capability's tier, if any.
///   - origin: Who drives the session.
///   - capability: The side effect.
/// - Returns: The verdict.
[[nodiscard]] constexpr CapabilityDecision capabilityDecisionFor(std::optional<CapabilityDecision> configured, CapabilityOrigin origin, Capability capability)
{
    if (std::to_underlying(origin) > std::to_underlying(CapabilityOrigin::Remote))
        return CapabilityDecision::Deny; // a value cast in from outside the enumeration: fail-closed
    return configured.value_or(defaultDecision(capability, origin));
}

/// Holds the verdict for every (origin, tier) pair when no grant exists.
///
/// A cell the configuration names holds its verdict; every other cell answers
/// the default. Overrides are folded in through `replacing`. Immutable once
/// built, so a session policy holding a copy keeps the verdicts it was created
/// under; a changed configuration reaches the sessions created after it.
class CapabilityDecisionTable {
public:
    /// Returns a copy with one cell replaced.
    ///
    /// A tier that is not configurable keeps its default, so no table can hold
    /// a Benign or Rendering verdict other than Allow.
    ///
    /// - Parameters:
    ///   - origin: Who drives the session.
    ///   - tier: The side effect's tier.
    ///   - decision: The verdict to hold for that pair.
    /// - Returns: The new table; this one is unchanged.
    [[nodiscard]] constexpr CapabilityDecisionTable replacing(CapabilityOrigin origin, CapabilityTier tier, CapabilityDecision decision) const
    {
        CapabilityDecisionTable result = *this;
        const std::size_t index = indexOf(origin, tier);
        if (isConfigurableTier(tier) && index < result.m_configured.size())
            result.m_configured[index] = decision;
        return result;
    }

    /// Returns the verdict for one (origin, tier) pair.
    ///
    /// - Parameters:
    ///   - origin: Who drives the session.
    ///   - tier: The side effect's tier.
    /// - Returns: The configured cell, the tier's default, or Deny for a value
    ///   outside the enumerations.
    [[nodiscard]] constexpr CapabilityDecision decision(CapabilityOrigin origin, CapabilityTier tier) const
    {
        const std::size_t index = indexOf(origin, tier);
        return capabilityDecisionFor(index < m_configured.size() ? m_configured[index] : std::nullopt, origin, tier);
    }

    /// Returns the verdict for one capability.
    ///
    /// A configured cell covers every capability in its tier: the owner's
    /// written verdict outranks the per-capability refinement of the default.
    ///
    /// - Parameters:
    ///   - origin: Who drives the session.
    ///   - capability: The side effect.
    /// - Returns: The configured cell for its tier, or the capability's default.
    [[nodiscard]] constexpr CapabilityDecision decision(CapabilityOrigin origin, Capability capability) const
    {
        const std::size_t index = indexOf(origin, tierOf(capability));
        return capabilityDecisionFor(index < m_configured.size() ? m_configured[index] : std::nullopt, origin, capability);
    }

    constexpr bool operator==(const CapabilityDecisionTable&) const = default;

    /// Reads an explicit override without collapsing it into a tier default.
    ///
    /// Handover preserves this distinction because individual capabilities
    /// refine an unset tier's default.
    /// - Parameters:
    ///   - origin: The driving principal class.
    ///   - tier: The capability tier.
    /// - Returns: The configured override, or no override for an unset/invalid cell.
    [[nodiscard]] constexpr std::optional<CapabilityDecision> configuredDecision(CapabilityOrigin origin, CapabilityTier tier) const
    {
        const auto index = indexOf(origin, tier);
        return index < m_configured.size() ? m_configured[index] : std::nullopt;
    }

private:
    static constexpr std::size_t originCount = std::to_underlying(CapabilityOrigin::Remote) + 1;
    static constexpr std::size_t tierCount = std::to_underlying(CapabilityTier::Rendering) + 1;

    [[nodiscard]] static constexpr std::size_t indexOf(CapabilityOrigin origin, CapabilityTier tier)
    {
        const std::size_t originIndex = std::to_underlying(origin);
        const std::size_t tierIndex = std::to_underlying(tier);
        if (originIndex >= originCount || tierIndex >= tierCount)
            return originCount * tierCount;
        return originIndex * tierCount + tierIndex;
    }

    std::array<std::optional<CapabilityDecision>, originCount * tierCount> m_configured {}; // unset: the default answers
};

} // namespace Ubiquitty
