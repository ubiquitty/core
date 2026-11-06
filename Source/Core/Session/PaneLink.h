// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Liveness.h"
#include "Core/Protocol/Sync/Messages.h"

#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace Ubiquitty::Runtime {
struct KeyPress;
}

namespace Ubiquitty {
struct PixelGeometry;
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {
class HistoryCache;
struct PaneFacts;
}

namespace Ubiquitty::Session {

class PaneLinkObserver;

/// The lifetime of a pane's attachment, independent of its carrier.
enum class LinkPhase : std::uint8_t {
    Connecting,
    Live,
    Lost,
    Ended,
};

/// The attachment's phase beside the two independently judged stall reasons.
struct LinkState {
    LinkPhase phase = LinkPhase::Connecting;
    Protocol::Sync::LinkState transport {};

    [[nodiscard]] bool isStalled() const { return transport.isStalled(); }
};

/// Borrowed authoritative state, not a second terminal or a render capture.
/// The caller holds the model's read lock while using the terminal. All
/// pointers expire at the next link mutation; a local pane has no remote
/// history cache or host record. Its Terminal owns its history and status.
struct PaneView {
    Ubiquitty::Terminal* terminal = nullptr;
    const Protocol::Sync::HistoryCache* history = nullptr;
    const Protocol::Sync::PaneFacts* facts = nullptr;
    /// An externally adopted model has its own publisher; its adapter cannot
    /// claim coherent frames from the unused local runtime.
    bool hasFrameNotifications = true;
};

/// One synchronous input handoff, with no owned buffer or wire encoding.
/// The caller owns every borrowed value until `PaneLink::send` returns.
struct PaneInput {
    /// Already-encoded input: IME text, macros, or a terminal-specific report.
    struct Raw { };
    /// A physical key transition, independent of this mirror's current modes.
    struct Key {
        const Runtime::KeyPress& press;
    };
    /// A complete paste, before terminal normalization and delimiters.
    struct Paste {
        std::string_view text;
    };
    /// A pointer event the local selection/reporting ladder routed to the program.
    struct Pointer {
        Protocol::Sync::PointerEvent event;
    };

    std::variant<Raw, Key, Paste, Pointer> intent;
    /// Bytes already resolved under the locked local model, or the raw input.
    /// A local/tmux endpoint writes these without re-encoding or allocating a
    /// protocol event. A remote endpoint ignores these for semantic input:
    /// even an empty local encoding cannot override the host's live modes.
    std::span<const char> localEncoding;
};

/// The viewport a frontend reports, not authority to resize a remote replica.
/// Local placement retains the resize's anchoring and live-drag urgency.
struct PaneViewport {
    int rows = 0;
    int columns = 0;
    int widthInPixels = 0;
    int heightInPixels = 0;
    Modes::ResizeUrgency urgency = Modes::ResizeUrgency::Settled;
    Modes::AnchorMode anchor = Modes::AnchorMode::Top;
};

/// The pane's state and input endpoint, independent of where its program runs.
/// Implementations must consume or copy borrowed input before returning and
/// must not require the caller to hold the terminal model lock while sending.
class PaneLink {
public:
    virtual ~PaneLink() = default;

    /// Hands input to the in-process program or the remote attachment.
    ///
    /// Raw input with no bytes is refused on every link: it writes nothing,
    /// and the wire has no empty `Raw` to carry it.
    ///
    /// - Parameter input: Borrowed semantic input and its local encoding.
    /// - Returns: Whether the route admitted the handoff, not delivery proof.
    virtual bool send(const PaneInput& input) = 0;

    /// Borrows this pane's model and the history and records beside it.
    [[nodiscard]] virtual PaneView view() = 0;

    /// Reads the PTY erase character under the model lock.
    /// - Returns: VERASE, absent when disabled, or DEL when unknown.
    [[nodiscard]] virtual std::optional<std::uint8_t> lineEraseCharacter() const { return 0x7f; }

    /// Reports whether this pane is visible; it never changes another pane.
    virtual void setView(Protocol::Sync::ViewState) = 0;

    /// Reports a client focus fact through the model's owning route.
    virtual void setFocus(Ubiquitty::Modes::FocusState) = 0;

    /// Reports the visible grid through the owner; only a local owner reflows now.
    virtual void setViewport(const PaneViewport&) = 0;

    /// Stages the client's complete pixel metrics for the next viewport report.
    /// A local owner publishes them to its model; a remote link keeps them
    /// separately from the host's verified picture.
    virtual void setPixelGeometry(const PixelGeometry&) = 0;

    /// Reports an updated pixel extent after cell/drawable metrics are published.
    virtual void setWindowSize(int columns, int rows, int widthInPixels, int heightInPixels) = 0;

    /// Requests logical history lines at one priority. A local terminal
    /// already owns its history; it validates the ranges but sends nothing.
    /// - Returns: Whether the request was admitted, not whether lines arrived.
    [[nodiscard]] virtual bool wantHistory(Protocol::Sync::HistoryPriority, std::span<const Protocol::Sync::LineRange>) = 0;

    /// Reports where the frontend's view sits: the rows of history it shows
    /// above the live tail. A remote link asks the host for the lines a view
    /// near the oldest row it holds reaches past them (Docs/sync.md §6.6
    /// "On demand"), one want per gap however often the view moves within
    /// it. A local terminal already owns its history and sends nothing.
    /// - Parameter rowsAboveTail: Rows above the tail; zero is the tail.
    virtual void setViewOffset(int rowsAboveTail) = 0;

    /// Judges the attachment on its owner's monotonic clock. Local panes
    /// never stall, even when their program is not reading its input.
    [[nodiscard]] virtual LinkState linkState(std::uint64_t now) const = 0;

    /// The same monotonic clock the owner uses for receipts and publication.
    [[nodiscard]] virtual std::uint64_t clockMicroseconds() const = 0;

    /// Borrows one observer until it is replaced or destroyed. Registration,
    /// polling, and observer destruction belong to the presentation owner.
    virtual void observe(PaneLinkObserver&) = 0;

    /// Captures the newest notification state and delivers it without holding
    /// the model lock. The presentation owner calls this after draining events.
    ///
    /// A frame is reported only while the view is `Live`. A `Dormant` or
    /// `RecordsOnly` view presents no screen and is told of no frame, not
    /// even one its terminal already holds; once `Live` again it is told of
    /// the newest.
    ///
    /// - Parameter now: The owner's monotonic clock.
    virtual void pollEvents(std::uint64_t now) = 0;
};

} // namespace Ubiquitty::Session
