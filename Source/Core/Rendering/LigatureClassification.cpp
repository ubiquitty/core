// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/LigatureClassification.h"

#include <algorithm>
#include <array>

namespace Ubiquitty::Rendering {

LigatureClassificationSession::LigatureClassificationSession()
    : m_candidateTags(ligatureCandidateFeatureTags())
{
}

bool LigatureClassificationSession::isAwaitingProbe() const
{
    return m_phase != Phase::Done;
}

LigatureClassificationSession::ProbeRequest LigatureClassificationSession::pendingProbe() const
{
    switch (m_phase) {
    case Phase::BaseAlphanumerics:
        return { ligatureAlphanumericProbe, {}, 0 };
    case Phase::BaseOperators:
        return { ligatureOperatorProbe, {}, 0 };
    case Phase::CandidateAlphanumerics:
        return { ligatureAlphanumericProbe, m_candidateTags[m_candidate], 1 };
    case Phase::CandidateOperators:
        return { ligatureOperatorProbe, m_candidateTags[m_candidate], 1 };
    case Phase::FallbackOperators:
        return { ligatureOperatorProbe, m_candidateTags[m_candidate], 0 };
    case Phase::Done:
        break;
    }
    return {};
}

void LigatureClassificationSession::advanceCandidate()
{
    ++m_candidate;
    if (m_phase == Phase::FallbackOperators) {
        if (m_candidate >= m_candidateTags.size())
            m_phase = Phase::Done;
        return;
    }
    if (m_candidate >= m_candidateTags.size())
        beginFallbackOrFinish();
    else
        m_phase = Phase::CandidateAlphanumerics;
}

void LigatureClassificationSession::beginFallbackOrFinish()
{
    // calt/liga are on by default, so a pure-calt face (Fira, Cascadia)
    // qualifies nothing in the candidate pass — its ligatures already ride
    // the default shaping. Forcing each candidate OFF and watching the
    // operator probe change is what detects that face.
    if (m_classification.doesLigate || m_candidateTags.empty()) {
        m_phase = Phase::Done;
        return;
    }
    m_candidate = 0;
    m_phase = Phase::FallbackOperators;
}

void LigatureClassificationSession::submitGlyphs(const ProbeGlyphs& glyphs)
{
    switch (m_phase) {
    case Phase::BaseAlphanumerics:
        m_baseAlphanumerics = glyphs;
        m_phase = Phase::BaseOperators;
        return;
    case Phase::BaseOperators:
        m_baseOperators = glyphs;
        m_phase = m_candidateTags.empty() ? Phase::Done : Phase::CandidateAlphanumerics;
        return;
    case Phase::CandidateAlphanumerics:
        m_candidateAlphanumerics = glyphs;
        m_hasCandidateAlphanumerics = true;
        m_phase = Phase::CandidateOperators;
        return;
    case Phase::CandidateOperators: {
        // A feature qualifies when it moves the operator probe and leaves
        // plain text alone: ligatures turn on, letter alternates stay off.
        const bool didAlterAlphanumerics = !m_hasCandidateAlphanumerics
            || m_candidateAlphanumerics != m_baseAlphanumerics;
        const bool didAlterOperators = glyphs != m_baseOperators;
        if (didAlterOperators && !didAlterAlphanumerics) {
            m_classification.features.push_back({ m_candidateTags[m_candidate], 1 });
            m_classification.doesLigate = true;
        }
        m_hasCandidateAlphanumerics = false;
        advanceCandidate();
        return;
    }
    case Phase::FallbackOperators:
        if (glyphs != m_baseOperators) {
            m_classification.doesLigate = true;
            m_phase = Phase::Done;
            return;
        }
        advanceCandidate();
        return;
    case Phase::Done:
        return;
    }
}

void LigatureClassificationSession::submitUnavailable()
{
    switch (m_phase) {
    case Phase::BaseAlphanumerics:
    case Phase::BaseOperators:
        // Without a baseline nothing can be compared; the face classifies as
        // one that ligates nothing, which is what both arms always answered
        // for a shaper that refused.
        m_phase = Phase::Done;
        return;
    case Phase::CandidateAlphanumerics:
        m_phase = Phase::CandidateOperators;
        m_hasCandidateAlphanumerics = false;
        return;
    case Phase::CandidateOperators:
        m_hasCandidateAlphanumerics = false;
        advanceCandidate();
        return;
    case Phase::FallbackOperators:
        advanceCandidate();
        return;
    case Phase::Done:
        return;
    }
}

LigatureClassification LigatureClassificationSession::classification() const
{
    return m_classification;
}

namespace {

// Room for either probe with margin; a probe outgrowing it is a Core edit
// and lands here beside the bound it must raise.
constexpr int probeGlyphCapacity = 256;

} // namespace

LigatureClassification classifyLigatureFeatures(ShapeProbe shape, void* context)
{
    if (!shape)
        return {};
    // One policy: this form only drives the session, so a caller reaching the
    // classification through either spelling runs the same loop.
    LigatureClassificationSession session;
    std::array<std::uint32_t, probeGlyphCapacity> scratch;
    while (session.isAwaitingProbe()) {
        const LigatureClassificationSession::ProbeRequest request = session.pendingProbe();
        const int count = shape(context, request.text.c_str(),
            request.featureTag.empty() ? nullptr : request.featureTag.c_str(),
            request.featureValue, ProbeGlyphSpan { scratch });
        if (count < 0) {
            session.submitUnavailable();
            continue;
        }
        session.submitGlyphs(ProbeGlyphs(scratch.begin(), scratch.begin() + count));
    }
    return session.classification();
}

FontFeatureSettings mergeFontFeatureSettings(
    const FontFeatureSettings& base, const FontFeatureSettings& overrides)
{
    FontFeatureSettings folded = base;
    for (const FontFeatureSetting& override_ : overrides) {
        bool didReplace = false;
        for (FontFeatureSetting& setting : folded) {
            if (setting.tag == override_.tag) {
                setting.value = override_.value;
                didReplace = true;
                break;
            }
        }
        if (!didReplace)
            folded.push_back(override_);
    }
    return folded;
}

} // namespace Ubiquitty::Rendering
