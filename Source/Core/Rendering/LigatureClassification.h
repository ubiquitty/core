// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Rendering/FontFeatureSpecification.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Rendering {

/// A probe's glyph ids, written into the classification's buffer.
///
/// Named because Swift imports a function pointer's span parameter only
/// through a named alias.
using ProbeGlyphSpan = std::span<std::uint32_t>;

/// The one platform question the ligature classification asks.
///
/// Writes the glyph ids of `text` shaped as one run under the arm's font
/// carrying `featureTag` at `featureValue`; a null or empty tag shapes the
/// face's defaults. Answers the count written, or -1 when the arm cannot
/// shape — which the classification treats as a face that ligates nothing.
///
/// The ids only ever meet ids from the same shaper, so their values are the
/// arm's business; what the classification reads is equality.
using ShapeProbe = int (*)(void* context, const char* text,
    const char* featureTag, int featureValue, ProbeGlyphSpan outGlyphs);

/// What the classification found in a face.
struct LigatureClassification {
    /// The features that move the operator probe and leave plain text alone.
    FontFeatureSettings features;
    /// Whether the face ligates at all, its own defaults included.
    bool doesLigate = false;
};

/// Classifies a face's OpenType features into the ligature set.
///
/// One loop in Core rather than one per arm, because the loop is the policy:
/// which candidates are tried, what disqualifies one, and the defaults
/// fallback below. Arms that each run their own amount of it render every
/// Monaspace-class face differently, and the shared golden tells them apart.
///
/// A feature qualifies when it changes ``ligatureOperatorProbe``'s glyphs and
/// leaves ``ligatureAlphanumericProbe`` untouched: ligatures turn on, letter
/// alternates stay off. A face that qualifies nothing may still ligate by
/// default (calt fonts — Fira, Cascadia): forcing every candidate off and
/// seeing the operator probe change is what detects that.
///
/// - Parameters:
///   - shape: The arm's shaper.
///   - context: Handed back to `shape` on every call.
/// - Returns: The classification.
[[nodiscard]] LigatureClassification classifyLigatureFeatures(ShapeProbe shape, void* context);

/// The glyph ids one probe produced.
///
/// Named because Swift builds one by value and hands it over with no pointer
/// in sight — the vector's element type is a template argument the importer
/// only follows through an alias.
using ProbeGlyphs = std::vector<std::uint32_t>;

/// The classification as a session the shaper drives, one probe at a time.
///
/// The pull shape is the interop shape: a function pointer crossing the
/// language seam drags a context pointer and an unmanaged cast with it, while
/// a session is values in both directions — the caller asks what to shape,
/// shapes it however its platform does, and hands back the ids. The loop, the
/// candidates and the verdict stay in here, which is the point: two shells
/// that each run their own amount of the loop classify one font two ways.
///
/// The protocol: while ``isAwaitingProbe`` answers true, shape
/// ``pendingProbe`` and call ``submitGlyphs`` — or ``submitUnavailable`` for
/// a probe the platform cannot shape — then read ``classification``. A
/// session is one classification; classify another face with another
/// session.
class LigatureClassificationSession {
public:
    /// One probe the session asks for.
    struct ProbeRequest {
        /// The text to shape as one run.
        std::string text;
        /// The OpenType tag to carry; empty shapes the face's defaults.
        std::string featureTag;
        /// The tag's value; zero disables it.
        int featureValue = 0;
    };

    /// Opens a session at its first probe.
    LigatureClassificationSession();

    /// Whether the session still needs a probe shaped.
    [[nodiscard]] bool isAwaitingProbe() const;

    /// The probe to shape next.
    ///
    /// Meaningful only while ``isAwaitingProbe`` answers true; afterwards it
    /// answers an empty request rather than reading past the plan.
    ///
    /// - Returns: The request.
    [[nodiscard]] ProbeRequest pendingProbe() const;

    /// Answers the pending probe with the shaped glyph ids.
    ///
    /// The ids only ever meet ids from the same shaper, so their values are
    /// the platform's business; the session reads equality.
    ///
    /// - Parameter glyphs: The ids, in run order.
    void submitGlyphs(const ProbeGlyphs& glyphs);

    /// Answers the pending probe with "this platform cannot shape it".
    ///
    /// A base probe failing ends the session as a face that ligates nothing;
    /// a candidate failing skips that candidate, the way the arms always
    /// treated a feature the shaper refused.
    void submitUnavailable();

    /// The verdict, once no probe is awaited.
    [[nodiscard]] LigatureClassification classification() const;

private:
    /// Which probe the session is on.
    enum class Phase : std::uint8_t {
        BaseAlphanumerics,
        BaseOperators,
        CandidateAlphanumerics,
        CandidateOperators,
        FallbackOperators,
        Done,
    };

    /// Steps to the next candidate, or onward when they are exhausted.
    void advanceCandidate();

    /// Enters the default-on fallback, or finishes when it has nothing to ask.
    void beginFallbackOrFinish();

    Phase m_phase = Phase::BaseAlphanumerics;
    std::size_t m_candidate = 0;
    std::vector<std::string> m_candidateTags;
    ProbeGlyphs m_baseAlphanumerics;
    ProbeGlyphs m_baseOperators;
    ProbeGlyphs m_candidateAlphanumerics;
    bool m_hasCandidateAlphanumerics = false;
    LigatureClassification m_classification;
};

/// Folds override settings over a base set.
///
/// The strip font is the probe's verdict with the reader's `font.features`
/// laid on top: an override names the same tag, its value wins — `-ss01`
/// turns off a set the probe enabled — and a new tag appends. One spelling of
/// the fold, because two arms merging in different orders enable different
/// features from identical configuration.
///
/// - Parameters:
///   - base: The probe's settings.
///   - overrides: The reader's settings.
/// - Returns: The folded settings, base order first.
[[nodiscard]] FontFeatureSettings mergeFontFeatureSettings(
    const FontFeatureSettings& base, const FontFeatureSettings& overrides);

} // namespace Ubiquitty::Rendering
