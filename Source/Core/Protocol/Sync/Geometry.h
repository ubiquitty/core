// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// Whose size a pane takes when several clients show it (Docs/sync.md
/// §6.10): the size follows the hands.
///
/// - The owner is the driver whose typing is newest: keys, paste or raw
///   bytes, never pointer motion. Agents and viewers never own.
/// - Until anyone types, the newest viewport a driver reported applies, as a
///   lone client's window resize does.
/// - A pin freezes the size whoever types.
/// - An override, an agent's explicit resize, applies once and holds until
///   the owner takes the size back by typing or by reporting a new size.
/// - A size is applied at most once per coalesce window, the trailing value
///   at the window's end: an owner's drag, and two clients typing in turn,
///   each cost the program one SIGWINCH per window.

#include "Core/Protocol/Sync/Messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// How a pane's geometry follows its owner.
struct GeometryPolicy {
    /// The window an owner's resizes coalesce over, the trailing one applied.
    std::uint64_t coalesceMicroseconds;
};

/// The default geometry-coalescing policy.
///
/// - Returns: The policy.
[[nodiscard]] constexpr GeometryPolicy measuredGeometryPolicy()
{
    // Coalesces successive resizes before publishing the latest geometry.
    return { .coalesceMicroseconds = 50'000 };
}

/// A pane's size in cells.
struct PaneGeometry {
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;

    friend constexpr bool operator==(const PaneGeometry&, const PaneGeometry&) = default;
};

/// Who sets a pane's size, as every client is told so a client whose window
/// does not match can say why. The arbiter decides it; a frontend shows it.
struct GeometryStatus {
    /// The attachment whose size applies, absent when none has reported one.
    std::optional<ID128> owner;
    /// Whether a pin holds the size, whoever types.
    bool isPinned = false;
    /// Whether `owner` set the size by an override the owner has not yet
    /// taken back.
    bool isOverridden = false;
    /// The attachment that pinned the size, when a pin holds it and the pin
    /// named one; a frontend says "size set by" this client.
    std::optional<ID128> pinnedBy;

    friend bool operator==(const GeometryStatus&, const GeometryStatus&) = default;
};

/// What an attachment may do to a pane's size.
enum class GeometryStanding : std::uint8_t {
    /// A human driver: its typing takes the size.
    Driver,
    /// An agent or a viewer, or a client without the Drive scope: it never
    /// sets the size.
    Bystander,
};

/// The kind an attachment stands as: what it declares, never more than the
/// host verified. A client may declare itself less than it is, a human
/// as an agent or a viewer; a client the host cannot verify as a person
/// stands as what the host can verify, whatever it declares.
///
/// - Parameters:
///   - declared: The kind its `Greet` names.
///   - verified: The most the host verified of it: a token's own kind, or
///     the origin check of a local binary.
/// - Returns: The kind it stands as.
[[nodiscard]] constexpr ClientKind standingKind(ClientKind declared, ClientKind verified)
{
    return declared == ClientKind::Human && verified != ClientKind::Human ? verified : declared;
}

/// Whether an attachment of a kind, with or without the Drive scope, may own
/// a pane's size.
///
/// - Parameters:
///   - kind: The client's kind.
///   - mayDrive: Whether its authority may drive the pane.
/// - Returns: Its standing.
[[nodiscard]] constexpr GeometryStanding geometryStanding(ClientKind kind, bool mayDrive)
{
    return kind == ClientKind::Human && mayDrive ? GeometryStanding::Driver : GeometryStanding::Bystander;
}

/// Where a pane stands in a window showing it.
enum class PanePlacement : std::uint8_t {
    /// The pane is all the window shows, so the window's size is the pane's.
    WholeWindow,
    /// The pane shares the window with others, and its size is its place
    /// among them.
    BesideOthers,
};

/// One pane's geometry owner and the size it applies.
class GeometryArbiter final {
public:
    /// The attachments one pane tracks; past it the one heard from longest
    /// ago is forgotten.
    static constexpr std::size_t maxHands = 64;

    /// Owns the ordered hands and the exact coalescing clock, without callbacks.
    struct Continuation {
        /// Retains the facts and timing for one attachment in physical order.
        struct Hand {
            ID128 attachmentID;
            GeometryStanding standing = GeometryStanding::Bystander;
            std::optional<PaneGeometry> viewport;
            std::optional<PaneGeometry> windowViewport;
            std::optional<std::uint64_t> typedAt;
            std::uint64_t viewportAt = 0;
            std::uint64_t windowViewportAt = 0;
            std::uint64_t heardAt = 0;
        };
        /// Retains an explicit size and its requesting attachment.
        struct Override {
            ID128 attachmentID;
            PaneGeometry geometry;
        };
        GeometryPolicy policy {};
        std::array<Hand, maxHands> hands {};
        std::uint8_t handCount = 0;
        std::optional<PaneGeometry> pin;
        std::optional<ID128> pinnedBy;
        std::optional<Override> override;
        std::optional<PaneGeometry> applied;
        std::optional<std::uint64_t> appliedAt;
        PanePlacement placement = PanePlacement::WholeWindow;
    };

    /// Copies the exact physical order and absolute host timestamps.
    /// - Returns: Owned continuation, or nothing for invalid or exhausted timing state.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Restores ownership and pending geometry without applying a size.
    ///
    /// The receiver policy must match; timestamps are never rebased. Candidate
    /// hands retain the arbiter's bounded STL allocation contract.
    /// - Parameter state: Complete, field-wise decoded continuation.
    /// - Returns: False without mutation on invalid state or a different policy.
    [[nodiscard]] bool restoreContinuation(const Continuation& state);

    explicit GeometryArbiter(const GeometryPolicy& policy = measuredGeometryPolicy());

    /// Records the size the pane has, so a report of the same size applies
    /// nothing.
    ///
    /// - Parameter geometry: The pane's size.
    void seed(PaneGeometry geometry);

    /// Records an attachment's typing into the pane. A driver's makes it the
    /// owner.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - standing: What it may do to the size.
    ///   - now: The host clock.
    void noteTyping(ID128 attachmentID, GeometryStanding standing, std::uint64_t now);

    /// Records the viewport an attachment shows the pane in. A size of zero,
    /// one the client has not measured, is ignored.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - standing: What it may do to the size.
    ///   - viewport: Its size in cells.
    ///   - now: The host clock.
    void noteViewport(ID128 attachmentID, GeometryStanding standing, PaneGeometry viewport, std::uint64_t now);

    /// Records the size of the window an attachment shows the pane in, as
    /// against the size it shows the pane itself at. It counts as the pane's
    /// viewport only while the pane is the whole window: a pane laid out
    /// beside others takes its size from its place among them, and a window
    /// says nothing of that. A size of zero is ignored.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - standing: What it may do to the size.
    ///   - viewport: The window's size in cells.
    ///   - now: The host clock.
    void noteWindowViewport(ID128 attachmentID, GeometryStanding standing, PaneGeometry viewport, std::uint64_t now);

    /// Says where the pane stands in the windows showing it. Beside others,
    /// no window's size is its own; the windows' sizes are kept, and count
    /// again when the pane is alone.
    ///
    /// - Parameter placement: Where the pane stands.
    void setPlacement(PanePlacement placement) { m_placement = placement; }

    /// Forgets an attachment that left; the owner passes to the driver that
    /// typed most recently before it.
    ///
    /// - Parameter attachmentID: The attachment.
    void forget(ID128 attachmentID);

    /// Freezes the pane's size.
    ///
    /// - Parameters:
    ///   - geometry: The size.
    ///   - by: The attachment that asked, absent when the host pinned it
    ///     on nobody's behalf.
    void pin(PaneGeometry geometry, std::optional<ID128> by = std::nullopt);

    /// Lets the owner's size apply again.
    void unpin();

    /// Sets the size once, for an attachment that may not own it but asked
    /// explicitly: an agent's resize. It holds until a driver types or
    /// reports a size different from the one it had.
    ///
    /// - Parameters:
    ///   - attachmentID: Who asked.
    ///   - geometry: The size.
    ///   - now: The host clock.
    void overrideSize(ID128 attachmentID, PaneGeometry geometry, std::uint64_t now);

    /// Rekeys an attachment: a client's events arrived under a provisional
    /// key before its attachment was known.
    ///
    /// - Parameters:
    ///   - from: The provisional key.
    ///   - to: The attachment.
    void rekey(ID128 from, ID128 to);

    /// Takes the size to apply now, when it differs from the one applied and
    /// the coalesce window since the last is over.
    ///
    /// - Parameter now: The host clock.
    /// - Returns: The size, or `nullopt` when nothing is due.
    [[nodiscard]] std::optional<PaneGeometry> due(std::uint64_t now);

    /// Takes the size to apply before an attachment's input is written: the
    /// input was typed against the window the attachment reported, so when
    /// it owns the size and reported a new one since the last applied, that
    /// size goes first, whatever the window. Anything else waits as `due`
    /// has it, so typists in turn still cost one signal per window.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment whose input is about to be written.
    ///   - now: The host clock.
    /// - Returns: The size, or `nullopt` when nothing is due.
    [[nodiscard]] std::optional<PaneGeometry> dueBeforeInput(ID128 attachmentID, std::uint64_t now);

    /// When a size waiting for its window is due, or `nullopt` when nothing
    /// waits.
    [[nodiscard]] std::optional<std::uint64_t> nextDueMicroseconds() const;

    /// Who sets the size.
    [[nodiscard]] GeometryStatus status() const;

    /// The driver whose size applies when nothing pins or overrides it: the
    /// newest typist, or before anyone types the newest viewport.
    [[nodiscard]] std::optional<ID128> drivingOwner() const;

    /// The drivers that have typed, the newest first.
    [[nodiscard]] std::vector<ID128> typistsNewestFirst() const;

    /// The size last applied, absent before the first.
    [[nodiscard]] std::optional<PaneGeometry> applied() const { return m_applied; }

private:
    /// One attachment showing the pane.
    struct Hand {
        ID128 attachmentID;
        GeometryStanding standing = GeometryStanding::Bystander;
        /// The size it shows the pane itself at.
        std::optional<PaneGeometry> viewport;
        /// The size of the window it shows the pane in.
        std::optional<PaneGeometry> windowViewport;
        std::optional<std::uint64_t> typedAt;
        std::uint64_t viewportAt = 0;
        std::uint64_t windowViewportAt = 0;
        std::uint64_t heardAt = 0;
    };

    /// A size a hand shows the pane at, and when it said so.
    struct Shown {
        PaneGeometry geometry;
        std::uint64_t at = 0;
    };

    /// The size a hand shows the pane at: the newer of the pane's own and
    /// its window's, and the pane's own alone while the pane is laid out.
    ///
    /// - Parameter hand: The hand.
    /// - Returns: The size, or `nullopt` when the hand has given none that
    ///   counts.
    [[nodiscard]] std::optional<Shown> shown(const Hand& hand) const;

    /// The hand an attachment is, made when it is new.
    ///
    /// - Parameters:
    ///   - attachmentID: The attachment.
    ///   - standing: Its standing, which replaces the one recorded.
    ///   - now: The host clock.
    /// - Returns: The hand.
    Hand& hand(ID128 attachmentID, GeometryStanding standing, std::uint64_t now);

    /// The hand whose size applies, or null.
    [[nodiscard]] const Hand* owner() const;

    /// The size that should apply, or `nullopt` when none is known.
    [[nodiscard]] std::optional<PaneGeometry> target() const;

    /// An override and who asked for it.
    struct Override {
        ID128 attachmentID;
        PaneGeometry geometry;
    };

    GeometryPolicy m_policy;
    std::vector<Hand> m_hands;
    std::optional<PaneGeometry> m_pin;
    std::optional<ID128> m_pinnedBy;
    std::optional<Override> m_override;
    std::optional<PaneGeometry> m_applied;
    std::optional<std::uint64_t> m_appliedAt;
    PanePlacement m_placement = PanePlacement::WholeWindow;
};

} // namespace Ubiquitty::Protocol::Sync
