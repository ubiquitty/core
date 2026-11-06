// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// PublishedFaces.h — the measurer a host gives a pane it runs: the faces the
// pane's clients draw proportional runs in (Docs/proportional-text.md, "A pane
// a host runs").

#include "Core/OSC/TextSizing.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"
#include "Core/Unicode/Width.h"
#include "PAL/TerminalClients.h"
#include "PAL/TextShaping.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <span>

namespace Ubiquitty::OSC::TextSizing {

/// A host's proportional measurer: the faces the clients that may drive a
/// pane published, so the pane's programs are answered in the faces their
/// runs are drawn in.
///
/// The per-code-point upper envelope dominates every offered face, even when
/// one is widest only at a different weight. It shapes nothing, so selection,
/// carets and text projection keep the run's text on its anchor.
class PublishedFacesMeasurer final : public Ubiquitty::TextMeasureClient {
public:
    /// Whether at least one attached driver has not published its run faces.
    enum class FacePublicationState : std::uint8_t {
        Ready,
        Owed
    };

    /// The most faces one call may offer: one for every client a host holds.
    static constexpr std::size_t maxOffered = maxFaceOffersPerPane;
    /// The bounded answer wait after a program first queries owed faces.
    static constexpr std::uint64_t faceWaitMilliseconds = 250;
    /// The most OSC 66 queries a pane holds for faces at once.
    static constexpr std::size_t maxHeldQueries = 8;
    /// The most replies and literal reply bytes held behind those queries.
    static constexpr std::size_t maxHeldReplies = 32;
    static constexpr std::size_t maxHeldReplyBytes = 64 * 1024;

    /// Records whether an attached driver still owes its face publication.
    ///
    /// A new owed period permits another bounded wait; a completed one lets
    /// `releaseHeld` answer any waiting queries in the current envelope.
    ///
    /// - Parameters:
    ///   - state: Whether a driving offer is still owed.
    ///   - nowMilliseconds: The host's monotonic instant.
    void notePublicationState(FacePublicationState state, std::uint64_t nowMilliseconds)
    {
        const bool areOwed = state == FacePublicationState::Owed;
        if (areOwed && !m_areFacesOwed)
            m_didExpire = false;
        m_areFacesOwed = areOwed;
        m_nowMilliseconds = nowMilliseconds;
    }

    /// Names the next deadline at which held queries must receive grid width.
    ///
    /// - Returns: The monotonic deadline, or zero with no pending query.
    [[nodiscard]] std::uint64_t heldDeadlineMilliseconds() const { return m_deadlineMilliseconds; }

    /// Answers held replies once faces arrive or the wait expires.
    ///
    /// - Parameters:
    ///   - terminal: The pane whose replies are queued.
    ///   - nowMilliseconds: The host's monotonic instant.
    /// - Returns: Whether replies were released for a host drain.
    [[nodiscard]] bool releaseHeld(Ubiquitty::Terminal& terminal, std::uint64_t nowMilliseconds)
    {
        m_nowMilliseconds = nowMilliseconds;
        if (!m_heldCount || (m_areFacesOwed && nowMilliseconds < m_deadlineMilliseconds))
            return false;
        replayHeld(terminal, m_areFacesOwed ? AnswerFace::Grid : AnswerFace::Envelope);
        return true;
    }

    /// Replaces the faces runs are counted in with all adoptable offers.
    ///
    /// The same faces offered again, as a host offers them before every read,
    /// change nothing and cost one comparison a face.
    ///
    /// - Parameters:
    ///   - offered: The faces of the clients that may drive the pane, in any
    ///     order; empty for none, which answers in the grid's width. Faces a
    ///     host cannot count in are dropped, and so is every entry past
    ///     ``maxOffered``.
    ///   - ambiguous: The pane's width for East Asian ambiguous code points.
    void setFaces(std::span<const std::shared_ptr<const ProportionalFaceFacts>> offered, Unicode::Width::AmbiguousWidth ambiguous)
    {
        const auto incoming = offered.first(std::min(offered.size(), maxOffered));
        if (ambiguous == m_ambiguous && std::ranges::equal(incoming, std::span(m_offered).first(m_offeredCount)))
            return;
        bool didRemoveOffer = false;
        for (const auto& prior : std::span(m_offered).first(m_offeredCount)) {
            if (prior && std::ranges::find(incoming, prior) == incoming.end()) {
                didRemoveOffer = true;
                break;
            }
        }
        if (didRemoveOffer)
            m_envelope = {};
        const std::span<const std::shared_ptr<const ProportionalFaceFacts>> counted = std::span(m_offered).first(m_offeredCount);
        for (const auto& entry : incoming) {
            if (!entry || !isAdoptable(*entry))
                continue;
            if (!didRemoveOffer && std::ranges::find(counted, entry) != counted.end())
                continue;
            addToEnvelope(*entry);
        }
        std::ranges::copy(incoming, m_offered.begin());
        std::fill(m_offered.begin() + static_cast<std::ptrdiff_t>(incoming.size()), m_offered.end(), nullptr);
        m_offeredCount = incoming.size();
        m_ambiguous = ambiguous;

        m_count = 0;
        for (std::size_t index = 0; index < incoming.size(); ++index) {
            const auto& entry = incoming[index];
            if (!entry || !isAdoptable(*entry))
                continue;
            const auto preceding = incoming.first(index);
            if (std::ranges::any_of(preceding, [&](const auto& other) { return other && *other == *entry; }))
                continue;
            ++m_count;
        }
    }

    /// How many distinct faces runs are counted in.
    [[nodiscard]] std::size_t sourceCount() const { return m_count; }

    /// The cells a run needs in the envelope, or zero with no faces.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text.
    ///   - style: The face the run is set in.
    /// - Returns: The widest count, or zero for Core to answer in the grid's
    ///   width.
    [[nodiscard]] int proportionalRunCells(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style) override
    {
        if (!m_count)
            return 0;
        if (style.weight > 7)
            return maxProportionalRunCells;
        const auto& advances = style.weight ? m_envelope.weighted[style.faceBits & (runFaceCount - 1)]
                                            : m_envelope.regular[style.faceBits & (runFaceCount - 1)];
        std::uint64_t fractions = 0;
        std::uint32_t widest = 0;
        for (const std::uint32_t codePoint : codePoints) {
            if (const auto index = facedCodePointIndex(codePoint)) {
                fractions += advances[*index];
                continue;
            }
            if (!widest)
                widest = std::ranges::max(advances);
            const bool isWide = Unicode::Width::forCodePoint(codePoint, m_ambiguous) == 2;
            fractions += std::max(widest, isWide ? 2 * ProportionalFaceEnvelope::fractionsPerCell : 0u);
        }
        return static_cast<int>(std::clamp<std::uint64_t>((fractions + ProportionalFaceEnvelope::fractionsPerCell - 1)
                / ProportionalFaceEnvelope::fractionsPerCell,
            1, maxProportionalRunCells));
    }

    /// Holds a program's measure query while an attached driver owes faces.
    ///
    /// - Parameters:
    ///   - terminal: The pane that will send the answer.
    ///   - codePoints: The query text.
    ///   - style: Its face and weight.
    ///   - requestID: The program's query identity.
    ///   - terminator: The reply terminator at parse time.
    /// - Returns: A deferred answer, a bounded grid fallback or an immediate answer.
    [[nodiscard]] Ubiquitty::ProportionalQueryDisposition proportionalQuery(Ubiquitty::Terminal& terminal, std::span<const std::uint32_t> codePoints,
        Ubiquitty::ProportionalRunStyle style, std::uint32_t requestID, std::string_view terminator) override
    {
        if (!m_areFacesOwed)
            return Ubiquitty::ProportionalQueryDisposition::Answer;
        if (m_didExpire)
            return Ubiquitty::ProportionalQueryDisposition::Grid;
        if (m_heldQueryCount == maxHeldQueries || m_heldCount == maxHeldReplies) {
            replayHeld(terminal, AnswerFace::Grid);
            return Ubiquitty::ProportionalQueryDisposition::Grid;
        }
        auto copied = std::unique_ptr<std::uint32_t[]>(new (std::nothrow) std::uint32_t[codePoints.size()]);
        if (!copied) {
            replayHeld(terminal, AnswerFace::Grid);
            return Ubiquitty::ProportionalQueryDisposition::Grid;
        }
        std::ranges::copy(codePoints, copied.get());
        HeldReply& held = m_held[m_heldCount++];
        held.kind = HeldReply::Kind::Measure;
        held.codePoints = std::move(copied);
        held.length = codePoints.size();
        held.style = style;
        held.requestID = requestID;
        held.terminator = terminator;
        held.shouldTransmit8bitC1 = terminal.transmit8bitC1;
        ++m_heldQueryCount;
        if (!m_deadlineMilliseconds)
            m_deadlineMilliseconds = m_nowMilliseconds + faceWaitMilliseconds;
        return Ubiquitty::ProportionalQueryDisposition::Defer;
    }

    /// Holds another permitted terminal reply behind an owed measure answer.
    ///
    /// - Parameters:
    ///   - terminal: The pane whose reply it is.
    ///   - capability: The permission that allowed this reply.
    ///   - bytes: The complete literal reply.
    /// - Returns: Whether the reply is retained in parse order.
    [[nodiscard]] bool holdReply(Ubiquitty::Terminal& terminal, Ubiquitty::Capability capability, std::span<const char> bytes) override
    {
        if (!m_heldCount || m_isReplaying)
            return false;
        if (m_heldCount == maxHeldReplies || bytes.size() > maxHeldReplyBytes - m_heldReplyBytes) {
            replayHeld(terminal, AnswerFace::Grid);
            return false;
        }
        auto copied = std::unique_ptr<char[]>(new (std::nothrow) char[bytes.size()]);
        if (!copied) {
            replayHeld(terminal, AnswerFace::Grid);
            return false;
        }
        std::ranges::copy(bytes, copied.get());
        HeldReply& held = m_held[m_heldCount++];
        held.kind = HeldReply::Kind::Literal;
        held.bytes = std::move(copied);
        held.length = bytes.size();
        held.capability = capability;
        held.shouldTransmit8bitC1 = terminal.transmit8bitC1;
        m_heldReplyBytes += bytes.size();
        return true;
    }

private:
    /// Specifies envelope or grid width for a bounded answer.
    enum class AnswerFace : std::uint8_t {
        Envelope,
        Grid
    };
    /// Retains one measure placeholder or one later literal reply.
    struct HeldReply {
        /// Names which complete reply the slot holds.
        enum class Kind : std::uint8_t {
            Measure,
            Literal
        };
        Kind kind = Kind::Measure;
        std::unique_ptr<std::uint32_t[]> codePoints;
        std::unique_ptr<char[]> bytes;
        std::size_t length = 0;
        Ubiquitty::ProportionalRunStyle style {};
        std::uint32_t requestID = 0;
        std::string_view terminator;
        Ubiquitty::Capability capability = Ubiquitty::Capability::None;
        bool shouldTransmit8bitC1 = false;
    };

    /// Replays measure and ordinary replies in their original parse order.
    ///
    /// - Parameters:
    ///   - terminal: The pane whose replies are queued.
    ///   - face: The envelope or bounded grid fallback to answer in.
    void replayHeld(Ubiquitty::Terminal& terminal, AnswerFace face)
    {
        m_isReplaying = true;
        const bool wasTransmitting8bitC1 = terminal.transmit8bitC1;
        for (std::size_t index = 0; index < m_heldCount; ++index) {
            HeldReply& held = m_held[index];
            terminal.transmit8bitC1 = held.shouldTransmit8bitC1;
            if (held.kind == HeldReply::Kind::Literal) {
                Ubiquitty::respond(terminal, held.capability, std::span(held.bytes.get(), held.length));
            } else {
                const auto codePoints = std::span(held.codePoints.get(), held.length);
                const int cells = face == AnswerFace::Envelope && m_count ? proportionalRunCells(codePoints, held.style) : gridRunCells(codePoints);
                std::array<char, 48> reply {};
                const int length = std::snprintf(reply.data(), reply.size(), "\x1b]66;i=%u:c=%d%.*s", held.requestID, cells,
                    static_cast<int>(held.terminator.size()), held.terminator.data());
                if (length > 0)
                    Ubiquitty::respond(terminal, Ubiquitty::Capability::TextMeasureRead, std::span(reply.data(), static_cast<std::size_t>(length)));
            }
            held = {};
        }
        terminal.transmit8bitC1 = wasTransmitting8bitC1;
        m_isReplaying = false;
        m_heldCount = 0;
        m_heldQueryCount = 0;
        m_heldReplyBytes = 0;
        m_deadlineMilliseconds = 0;
        m_didExpire = face == AnswerFace::Grid;
    }

    /// Counts a run in grid width for a bounded fallback answer.
    ///
    /// - Parameter codePoints: The query text.
    /// - Returns: The clamped grid-width cell count.
    [[nodiscard]] int gridRunCells(std::span<const std::uint32_t> codePoints) const
    {
        std::uint64_t cells = 0;
        for (const std::uint32_t codePoint : codePoints)
            cells += std::max(Unicode::Width::forCodePoint(codePoint, m_ambiguous), 0);
        return static_cast<int>(std::clamp<std::uint64_t>(cells, 1, maxProportionalRunCells));
    }

    /// Raises the envelope by every advance in one adoptable offer.
    ///
    /// - Parameter facts: A validated face offer.
    void addToEnvelope(const ProportionalFaceFacts& facts)
    {
        for (std::size_t face = 0; face < runFaceCount; ++face) {
            for (std::size_t index = 0; index < facedCodePointCount; ++index) {
                // An adoptable offer's cell is at least one pixel, so a 16-bit
                // advance in sixteenths of a cell stays far inside 32 bits.
                const auto regular = static_cast<std::uint32_t>((static_cast<std::uint64_t>(facts.advances[face][index])
                                                                        * ProportionalFaceEnvelope::fractionsPerCell
                                                                    + facts.cellAdvance - 1)
                    / facts.cellAdvance);
                const std::uint32_t weighted = static_cast<std::uint32_t>(facts.weightedAdvances[face][index])
                    * ProportionalFaceEnvelope::fractionsPerCell / weightedAdvanceFractionsPerCell;
                m_envelope.regular[face][index] = std::max(m_envelope.regular[face][index], regular);
                m_envelope.weighted[face][index] = std::max(m_envelope.weighted[face][index], weighted);
            }
        }
    }

    ProportionalFaceEnvelope m_envelope;
    std::array<HeldReply, maxHeldReplies> m_held {};
    std::size_t m_heldCount = 0;
    std::size_t m_heldQueryCount = 0;
    std::size_t m_heldReplyBytes = 0;
    std::uint64_t m_deadlineMilliseconds = 0;
    std::uint64_t m_nowMilliseconds = 0;
    bool m_areFacesOwed = false;
    bool m_didExpire = false;
    bool m_isReplaying = false;
    std::size_t m_count = 0;
    /// What was last offered, held so an unchanged offer is told by pointer
    /// and no address it names can be reused meanwhile.
    std::array<std::shared_ptr<const ProportionalFaceFacts>, maxOffered> m_offered {};
    std::size_t m_offeredCount = 0;
    Unicode::Width::AmbiguousWidth m_ambiguous = Unicode::Width::AmbiguousWidth::Narrow;
};

} // namespace Ubiquitty::OSC::TextSizing
