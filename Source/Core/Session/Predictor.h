// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"
#include "Core/Keys/KeyEvent.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Session/PredictionExposure.h"
#include "Core/Unicode/Width.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace Ubiquitty::Session {

/// A storage bound, not a display threshold or a negotiated wire allowance.
inline constexpr std::size_t maxPredictionEdits = 64;

/// Pins speculative state to one authenticated attachment's exact pane.
struct PredictionIdentity {
    Protocol::Sync::ID128 hostInstance;
    Protocol::Sync::ID128 clientInstance;
    Protocol::Sync::ID128 attachment;
    Protocol::Sync::ID128 pane;
    /// Changes on every reconnect, even if the same attachment resumes.
    std::uint64_t connectionEpoch = 0;

    friend bool operator==(const PredictionIdentity&, const PredictionIdentity&) = default;
};

/// Borrows one verified row under the authoritative model's read lock.
/// No pointer or span is retained after a predictor call. An empty known-cell
/// mask means the complete row is known; otherwise one byte names each cell.
struct PredictionSnapshot {
    PredictionIdentity identity;
    std::uint64_t generation = 0;
    std::uint64_t inputSettled = 0;
    /// Advances on shell prompt boundaries, not only on an AtPrompt bit change.
    std::uint64_t promptEpoch = 0;
    int rows = 0;
    int columns = 0;
    int cursorRow = 0;
    int cursorColumn = 0;
    std::span<const Cell> row;
    std::span<const std::uint8_t> knownCells;
    Protocol::Sync::EchoContext echoContext;
    Pen inputPen {};
    Modes::ScreenChoice screen = Modes::ScreenChoice::Primary;
    Unicode::Width::AmbiguousWidth ambiguousWidth = Unicode::Width::AmbiguousWidth::Narrow;
    bool hasEchoContext = false;
    bool hasExplicitSecureInput = false;
    bool isCursorVisible = false;
    /// The owner has ruled out non-normal row width, insert mode, custom
    /// character translation, margins, pending wrap, and other editing modes.
    bool canPredictSimpleEditing = false;
};

enum class PredictionError : std::uint8_t {
    InvalidLimits,
    InvalidExposurePolicy,
    InvalidSnapshot,
    ForeignIdentity,
    InvalidSequence,
    ClockRegressed,
    CapacityExceeded,
    EpochExhausted,
};

enum class PredictionInputOutcome : std::uint8_t {
    Ignored,
    Vetoed,
    Tentative,
    ConfirmedEpoch,
    Invalidated,
};

enum class PredictionFrameOutcome : std::uint8_t {
    Unchanged,
    Reset,
    Reconciled,
    Refuted,
};

/// Reconciliation evidence without speculative text or diagnostic payloads.
struct PredictionReconciliation {
    PredictionFrameOutcome outcome = PredictionFrameOutcome::Unchanged;
    std::size_t settledInputCount = 0;
    std::optional<std::uint64_t> echoLatencyMicroseconds {};
};

/// External changes that cannot be inferred from the borrowed screen row.
enum class PredictionInvalidation : std::uint8_t {
    Reconnect,
    PromptChanged,
    Resize,
    CursorMoved,
    SecureInput,
    UnmodelledInput,
    SettingChanged,
    PaneEnded,
};

/// One disposable rendering replacement, never an authoritative grid write.
struct PredictedCell {
    int row = 0;
    int column = 0;
    Cell cell {};
};

struct PredictionCursor {
    int row = 0;
    int column = 0;
};

/// Borrows render-only storage until the next mutable predictor call or its death.
/// Consumers must not insert these cells into selection, clipboard, accessibility,
/// search, scrollback, persistent state, or diagnostic text.
struct PredictionOverlay {
    std::span<const PredictedCell> cells;
    std::optional<PredictionCursor> cursor;
    std::uint64_t epoch = 0;
    PredictionTreatment treatment;
};

/// Models bounded simple edits in hidden epochs until an authoritative frame
/// confirms a changed nonblank glyph. It owns no Terminal and cannot send input.
/// All calls belong to one presentation owner; the injected timestamps are monotonic.
class Predictor {
public:
    Predictor() = default;
    ~Predictor();
    Predictor(const Predictor&) = delete;
    Predictor& operator=(const Predictor&) = delete;
    Predictor(Predictor&&) = delete;
    Predictor& operator=(Predictor&&) = delete;

    /// Sets the pending-edit allowance and an optional measured display policy.
    /// Zero or excessive storage allowance refuses and disables prediction.
    [[nodiscard]] std::expected<void, PredictionError> configure(std::size_t editLimit, std::optional<PredictionExposurePolicy>);

    /// Reconciles only verified screen frames, never a standalone receipt.
    [[nodiscard]] std::expected<PredictionReconciliation, PredictionError> acceptFrame(const PredictionSnapshot&, Protocol::Sync::FrameClass, std::uint64_t now);

    /// Models one already-admitted numbered key batch, without retaining its keys.
    /// Non-key input uses `unmodelledInput` under its actual input sequence.
    [[nodiscard]] std::expected<PredictionInputOutcome, PredictionError> predictKeys(std::uint64_t inputSeq, std::span<const Keys::KeyEvent>, const PredictionSnapshot&, std::uint64_t now);

    /// Ends confidence at the exact sequence of a paste, pointer, or raw event.
    [[nodiscard]] std::expected<void, PredictionError> unmodelledInput(std::uint64_t inputSeq, std::uint64_t now);

    /// Removes retained text immediately and requires a fresh authoritative frame.
    void invalidate(PredictionInvalidation);

    /// Borrows visible render replacements; hidden epochs return no text or cursor.
    [[nodiscard]] PredictionOverlay overlay(std::uint64_t now) UBIQUITTY_LIFETIMEBOUND;

    /// Returns no wake for an idle, vetoed, or unconfirmed epoch.
    [[nodiscard]] std::optional<std::uint64_t> nextWakeDelay(std::uint64_t now);

    /// Counts retained edits without exposing their text to diagnostics.
    [[nodiscard]] std::size_t pendingEditCount() const { return m_pendingCount; }
    [[nodiscard]] bool hasConfirmedEpoch() const { return m_hasConfirmedEpoch; }

private:
    struct Edit {
        std::uint64_t inputSeq = 0;
        std::uint64_t sentAt = 0;
        int column = 0;
        int cursorAfter = 0;
        Cell before {};
        Cell after {};
        bool canConfirm = false;
        bool endsKey = false;
    };

    [[nodiscard]] static bool isValidSnapshot(const PredictionSnapshot&);
    [[nodiscard]] static bool isSafeContext(const PredictionSnapshot&);
    [[nodiscard]] static bool isKnownCell(const PredictionSnapshot&, int column);
    [[nodiscard]] static bool isSimpleCell(const Cell&, Unicode::Width::AmbiguousWidth);
    [[nodiscard]] bool matchesContext(const PredictionSnapshot&) const;
    [[nodiscard]] bool isSameSnapshot(const PredictionSnapshot&) const;
    [[nodiscard]] std::expected<void, PredictionError> noteTime(std::uint64_t now);
    [[nodiscard]] std::expected<void, PredictionError> admitSequence(std::uint64_t inputSeq);
    [[nodiscard]] bool openEpoch();
    void erasePending();
    void takeBaseline(const PredictionSnapshot&);
    void rebuildOverlay();
    [[nodiscard]] Cell projectedCell(const PredictionSnapshot&, int column) const;
    [[nodiscard]] std::optional<std::uint64_t> visiblePendingSince() const;

    std::array<Edit, maxPredictionEdits> m_pending {};
    std::array<PredictedCell, maxPredictionEdits> m_overlay {};
    std::size_t m_editLimit = 0;
    std::size_t m_pendingCount = 0;
    std::size_t m_overlayCount = 0;
    PredictionIdentity m_identity;
    Protocol::Sync::EchoContext m_echoContext;
    PredictionExposure m_exposure;
    std::optional<std::uint64_t> m_lastNow;
    std::uint64_t m_generation = 0;
    std::uint64_t m_promptEpoch = 0;
    std::uint64_t m_lastInputSeq = 0;
    std::uint64_t m_lastSettled = 0;
    std::uint64_t m_epoch = 0;
    int m_rows = 0;
    int m_columns = 0;
    int m_row = 0;
    int m_authoritativeColumn = 0;
    int m_pendingBaseColumn = 0;
    int m_projectedColumn = 0;
    Unicode::Width::AmbiguousWidth m_ambiguousWidth = Unicode::Width::AmbiguousWidth::Narrow;
    bool m_hasBaseline = false;
    bool m_hasIdentity = false;
    bool m_hasConfirmedEpoch = false;
    bool m_needsFreshFrame = true;
    bool m_isEpochExhausted = false;
};

} // namespace Ubiquitty::Session
