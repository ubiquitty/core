// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Native attachment requests, directory rows, clipboard values, and resource
// policy shared by the daemon and frontends. Canonical framing and admission
// belong to Sync/Envelope.h and Sync/Messages.h.

#include "Base/Bits/OptionSet.h"
#include "Core/Protocol/Control/FileRead.h"
#include "Core/Protocol/Sync/AppVersion.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Scope.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Session/NameLimits.h"
#include "Core/Transport/CarrierCapacity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// Native attachment intent flags translated into a canonical admission request.
enum class HelloIntent : uint32_t {
    /// The connection is a workspace carrier: the host binds every pane of
    /// the workspace on it, and a pane's frames ride its handle.
    AttachesWorkspace = 0x01,
    /// The client answers CLIPBOARD_REQUEST from its own pasteboard. Stated
    /// only when the user turned it on: serving means every process on the
    /// host that runs xclip can read what the user copied.
    ServesClipboard = 0x02,
    /// A control-only connection that administers the host, which takes the
    /// Manage scope rather than Control.
    RequestsManagement = 0x04,
};

// Pulled into this namespace so HelloIntents/ScopeSet and call sites read unqualified.
using Ubiquitty::Bits::OptionSet;

/// The native intents translated into a canonical Greet.
using HelloIntents = OptionSet<HelloIntent>;

/// Fixed headroom in the connection's retained staging allowance.
constexpr std::size_t frameHeaderLength = 5;

/// Retained payload capacity shared by image, effect, and carrier staging.
constexpr std::size_t maxPayloadLength = 72u * 1024u * 1024u;

/// Which end of a version refusal runs the older build.
///
/// The values are explicit because the Swift mirrors pin them.
enum class OlderSide : uint8_t {
    /// The versions do not say: the refusal was not about a version, the host
    /// did not report one, or both ends report the same.
    Unknown = 0,
    /// The host runs the older build; updating it is the remedy.
    Host = 1,
    /// This client runs the older build; nothing on the host can fix it.
    Client = 2,
};

/// The created session ends when its last human detaches cleanly.
constexpr uint8_t helloSessionEndWhenDetached = 0x01;

/// The selected session must already exist; an absent identity never creates one.
constexpr uint8_t helloSessionAttachOnly = 0x02;

/// Maximum bytes in a workspace name, checked before lookup or mutation.
constexpr std::size_t maxWorkspaceNameLength = Session::maxWorkspaceNameBytes;

/// Maximum bytes retained from an untrusted claimed client label.
constexpr std::size_t maxAgentLabelLength = 256;

/// Maximum capability-token bytes accepted before hashing.
constexpr std::size_t maxAuthTokenLength = 256;

/// Flags describing the compiler settings of the serving binary.
enum class HostBuildFlag : uint8_t {
    Optimised = 0x01, ///< The compiler was invoked at -O1 or above.
    AssertionsEnabled = 0x02, ///< NDEBUG was absent, so assert() is live.
};
using HostBuildFlags = OptionSet<HostBuildFlag>;

/// This build's own flags, resolved where the code was compiled.
constexpr HostBuildFlags localHostBuildFlags()
{
    HostBuildFlags flags;
#ifdef __OPTIMIZE__
    flags.add(HostBuildFlag::Optimised);
#endif
#ifndef NDEBUG
    flags.add(HostBuildFlag::AssertionsEnabled);
#endif
    return flags;
}

/// Native file-read result status.
using FileDataStatus = Control::FileReadStatus;

// A raw file.read result fits the carrier's staging guarantee; the encoded
// canonical Result may travel as a typed bulk transfer.
static_assert(Control::maxReadFileBytes <= Transport::guaranteedOutboundFrameBytes,
    "the raw file result must fit within every carrier's outbound guarantee");

/// What a CLIPBOARD_REQUEST is asking the client for.
enum class ClipboardRequestKind : std::uint8_t {
    /// Which MIME types the pasteboard holds, no bytes.
    ///
    /// The probe a tool runs before reading: xclip spells it TARGETS and
    /// wl-paste spells it --list-types, and both callers only grep the names.
    /// Answering it without moving the bytes keeps a probe cheap, which matters
    /// because a shell prompt may run one per keystroke.
    Types = 0,
    /// The bytes of one named type.
    Item = 1,
};

/// Why a clipboard answer carries nothing.
enum class ClipboardServeStatus : std::uint8_t {
    /// The items are the answer.
    OK = 0,
    /// The pasteboard holds nothing, or nothing of the type asked for.
    Empty = 1,
    /// The client will not serve it: the user has not turned it on.
    ///
    /// Distinct from Empty because they mean different things to the person at
    /// the keyboard — one says copy something, the other says change a setting.
    Refused = 2,
    /// The payload exceeds the clipboard answer's byte budget.
    TooLarge = 3,
};

/// One typed payload from the client's pasteboard.
struct ClipboardServeItem {
    std::string mime;
    /// Empty for a ``ClipboardRequestKind/Types`` answer, which names types
    /// without moving them.
    std::vector<std::uint8_t> data;
};

/// A clipboard item borrowed for synchronous framing, without another payload copy.
struct ClipboardServeItemView {
    std::string_view mime;
    std::span<const std::uint8_t> data;
};

/// A frontend clipboard request, delivered by the typed pane protocol.
struct ClipboardRequestMessage {
    uint32_t requestID {}; // echoed by the typed reply
    ClipboardRequestKind kind = ClipboardRequestKind::Types;
    /// The type wanted, empty for a ``ClipboardRequestKind/Types`` request.
    std::string mime;
};

/// An owned frontend answer, not an alternate wire encoding.
struct ClipboardDataMessage {
    uint32_t requestID {}; // the request this answers
    ClipboardServeStatus status = ClipboardServeStatus::Empty;
    /// The types, or the one item, depending on what was asked. Empty unless
    /// `status` is Ok.
    std::vector<ClipboardServeItem> items;
};

/// The most data bytes one clipboard answer carries.
///
/// MIME metadata is bounded separately. Pane replies use typed bulk delivery;
/// local helper replies use the same canonical bundle on local IPC.
inline constexpr std::size_t maxClipboardServeBytes = Transport::guaranteedOutboundFrameBytes;

/// The most types one answer names.
///
/// A pasteboard offering more than this is a program writing every flavour it
/// can think of, and the reader greps the list — it does not need all of them.
inline constexpr std::size_t maxClipboardServeItems = 32;

/// Per-client staging allowance, including bounded clipboard MIME metadata.
inline constexpr std::size_t maxClientFramePayloadLength = maxClipboardServeBytes + maxClipboardServeItems * (0xFFFFu + 2u + 4u) + 16u;
static_assert(maxClientFramePayloadLength <= maxPayloadLength);
static_assert(maxClientFramePayloadLength >= 4u + 1u + 2u + maxClipboardServeItems * (2u + 0xFFFFu + 4u) + maxClipboardServeBytes,
    "the complete clipboard bundle and correlation fit the carrier budget");

/// Builds a complete bounded clipboard answer, or refuses the answer whole.
/// - Parameters:
///   - requestID: The exact request this answers.
///   - status: The result, whose items are retained only on OK.
///   - items: Owned types and data; moved into the result only within its budget.
/// - Returns: The complete answer, or TooLarge without partial contents.
[[nodiscard]] ClipboardDataMessage makeClipboardData(
    uint32_t requestID, ClipboardServeStatus status, std::vector<ClipboardServeItem> items);

/// Reads two builds' wire versions for which one is older.
///
/// - Parameters:
///   - local: The version this client speaks.
///   - host: The version the host speaks.
/// - Returns: The side with the lower version, or ``OlderSide::Unknown`` when
///   the two are equal or either is unknown.
[[nodiscard]] constexpr OlderSide olderSideOfVersions(std::uint16_t local, std::uint16_t host)
{
    if (!local || !host || local == host)
        return OlderSide::Unknown;
    return host < local ? OlderSide::Host : OlderSide::Client;
}

/// Reads two builds for which speaks the older wire version.
///
/// App versions are display facts. Equal wire versions name no older side,
/// including when the handshake refuses different fingerprints.
///
/// - Parameters:
///   - local: This client's build.
///   - host: What the host said about its build.
/// - Returns: The older side, or ``OlderSide::Unknown`` when the host said
///   nothing readable or neither build orders before the other.
[[nodiscard]] constexpr OlderSide olderSideOfBuilds(const Sync::PeerBuild& local, const Sync::PeerBuild& host)
{
    if (!local.isKnown || !host.isKnown)
        return OlderSide::Unknown;
    return olderSideOfVersions(local.wireVersion, host.wireVersion);
}

/// Reads a version refusal for which side is older.
///
/// - Parameters:
///   - reason: The refusal.
///   - host: What the host said about its build.
///   - local: This client's build.
/// - Returns: The older side, or ``OlderSide::Unknown`` when the refusal is
///   not about a version or the builds do not say.
[[nodiscard]] inline OlderSide olderSideOfRefusal(Sync::ByeReason reason, const Sync::PeerBuild& host, const Sync::PeerBuild& local = localPeerBuild())
{
    return reason == Sync::ByeReason::VersionMismatch ? olderSideOfBuilds(local, host) : OlderSide::Unknown;
}

} // namespace Ubiquitty::Protocol::Sync
