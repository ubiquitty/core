// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/TypographyFacts.h — the metrics a frontend publishes for Core to
// lay rich text out by (Docs/rich-text.md, "The measurement model").
//
// Core owns every answer a program observes — the fit reply, the selection,
// which character a pointer is on — and the frontend owns the fonts. The
// table is the fonts as facts: per face, the advances of a dense code point
// range in em units, so an advance at any size is one multiplication and the
// layout is exact integer arithmetic on every arm. A local pane's frontend
// publishes it to its Core; a remote pane's client publishes it to the host.

#include "Core/RichText/Document.h"
#include "Core/RichText/FontCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::RichText {

/// The first and last code points a face's advances cover.
inline constexpr std::uint32_t firstTabledCodePoint = 0x20;
inline constexpr std::uint32_t lastTabledCodePoint = 0x24F;
/// How many advances a face carries.
inline constexpr std::size_t tabledCodePointCount = lastTabledCodePoint - firstTabledCodePoint + 1;
/// The most faces one table may publish.
inline constexpr int maxFaces = 16;
/// The units a face's metrics are in, per em.
inline constexpr std::uint32_t unitsPerEm = 2048;
/// The largest cell a table may describe, in 1/64 point: 100 points.
inline constexpr std::uint16_t maxCellSize = 6400;

/// One face's metrics.
struct FaceMetrics {
    /// The UTF-8 family name RTF's font table matches; empty for the class
    /// face every family of `familyClass` falls back to.
    std::array<char, maxFamilyNameBytes> name {};
    std::uint8_t nameLength = 0;
    FamilyClass familyClass = FamilyClass::SansSerif;
    /// `StyleBit::Bold` and `StyleBit::Italic`, the style the face measures.
    std::uint8_t styles = 0;
    /// In 1/2048 em. The line height is the distance between baselines.
    std::uint16_t ascent = 0;
    std::uint16_t descent = 0;
    std::uint16_t lineHeight = 0;
    /// The advance an estimate takes for a narrow code point outside the
    /// tabled range, in 1/2048 em.
    std::uint16_t averageAdvance = 0;
    /// The advance of each code point from ``firstTabledCodePoint``, in
    /// 1/2048 em.
    std::array<std::uint16_t, tabledCodePointCount> advances {};

    [[nodiscard]] std::string_view familyName() const { return { name.data(), nameLength }; }

    friend bool operator==(const FaceMetrics&, const FaceMetrics&) = default;
};

/// What a frontend publishes: its cell and its faces.
struct TypographyFacts {
    /// Identifies this pane's native font resolution and shaping configuration.
    /// A renderer rebuild advances it even when dense metrics compare equal.
    std::uint64_t rendererRevision = 1;
    /// The grid's cell as the frontend draws it, in 1/64 point.
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    /// Baseline below the cell top and current proportional em size, in 1/64 point.
    /// Zero denotes a publisher without terminal-line metrics.
    std::uint16_t baseline = 0;
    std::uint16_t proportionalSize = 0;
    /// The grid font's current em size, in 1/64 point. Terminal-aligned
    /// monospace runs use this size independently of proportional prose.
    std::uint16_t monospaceSize = 0;
    /// The pane's presentation draws retained rich-text blocks.
    std::uint8_t rendersBlocks = 0;
    /// The publishing attachment can shape complete styled runs.
    std::uint8_t shaping = 0;
    /// The native catalog available to this pane's application queries.
    FontCatalogScope fontCatalogScope = FontCatalogScope::Unavailable;
    std::uint8_t faceCount = 0;
    std::array<FaceMetrics, maxFaces> faces {};

    [[nodiscard]] std::span<const FaceMetrics> faceTable() const { return { faces.data(), faceCount }; }

    /// Returns the configured terminal em size for a font class.
    ///
    /// - Parameter familyClass: The resolved font class.
    /// - Returns: Its size in 1/64 point, or zero without published metrics.
    [[nodiscard]] std::uint16_t terminalSize(FamilyClass familyClass) const
    {
        return familyClass == FamilyClass::Monospace ? monospaceSize : proportionalSize;
    }

    friend bool operator==(const TypographyFacts&, const TypographyFacts&) = default;
};

/// Fills a table one face at a time.
///
/// For a frontend whose language reaches a C++ structure more easily through
/// calls than through nested arrays: the Swift renderer measures each face it
/// draws blocks in and hands Core the result through this.
class TypographyFactsBuilder {
public:
    /// Identifies the native renderer configuration that produced these facts.
    /// - Parameter revision: A nonzero revision owned by the pane's renderer.
    void setRendererRevision(std::uint64_t revision) { m_facts.rendererRevision = revision; }

    /// Sets the grid's cell.
    ///
    /// - Parameters:
    ///   - width: In 1/64 point.
    ///   - height: In 1/64 point.
    void setCell(std::uint16_t width, std::uint16_t height)
    {
        m_facts.cellWidth = width;
        m_facts.cellHeight = height;
    }

    /// Publishes the baseline and both zoomed font sizes in 1/64 point.
    ///
    /// - Parameters:
    ///   - baseline: Distance from the cell top to its baseline.
    ///   - proportionalSize: The current proportional em size.
    ///   - monospaceSize: The current grid font's em size.
    void setTerminalMetrics(std::uint16_t baseline, std::uint16_t proportionalSize, std::uint16_t monospaceSize)
    {
        m_facts.baseline = baseline;
        m_facts.proportionalSize = proportionalSize;
        m_facts.monospaceSize = monospaceSize;
    }

    /// Publishes whether this attachment can shape complete styled paragraphs.
    ///
    /// - Parameter shaping: One for native shaping, zero when unavailable.
    void setShaping(std::uint8_t shaping) { m_facts.shaping = shaping; }

    /// Publishes whether this pane draws the protocol's retained blocks.
    /// - Parameter rendering: One for block rendering, zero when unavailable.
    void setRendering(std::uint8_t rendering) { m_facts.rendersBlocks = rendering; }

    /// Publishes the usable native catalog's coverage (FontCatalogScope).
    /// - Parameter scope: A ``FontCatalogScope`` value.
    void setFontCatalogScope(std::uint8_t scope) { m_facts.fontCatalogScope = static_cast<FontCatalogScope>(scope); }

    /// Starts a class face; the advances that follow are its.
    ///
    /// - Parameters:
    ///   - familyClass: The class the face stands for.
    ///   - styles: `StyleBit::Bold` and `StyleBit::Italic`.
    ///   - ascent: In 1/2048 em.
    ///   - descent: In 1/2048 em.
    ///   - lineHeight: In 1/2048 em.
    ///   - averageAdvance: In 1/2048 em.
    /// - Returns: False, adding nothing, when the table holds ``maxFaces``.
    bool addFace(FamilyClass familyClass, std::uint8_t styles, std::uint16_t ascent, std::uint16_t descent, std::uint16_t lineHeight, std::uint16_t averageAdvance)
    {
        if (m_facts.faceCount == maxFaces)
            return false;
        FaceMetrics& face = m_facts.faces[m_facts.faceCount++];
        face = {};
        face.familyClass = familyClass;
        face.styles = styles;
        face.ascent = ascent;
        face.descent = descent;
        face.lineHeight = lineHeight;
        face.averageAdvance = averageAdvance;
        return true;
    }

    /// Sets one advance of the face last added.
    ///
    /// - Parameters:
    ///   - codePoint: A code point in the tabled range; any other is ignored.
    ///   - advance: In 1/2048 em.
    void setAdvance(std::uint32_t codePoint, std::uint16_t advance)
    {
        if (!m_facts.faceCount || codePoint < firstTabledCodePoint || codePoint > lastTabledCodePoint)
            return;
        m_facts.faces[m_facts.faceCount - 1u].advances[codePoint - firstTabledCodePoint] = advance;
    }

    /// The table filled so far.
    [[nodiscard]] const TypographyFacts& facts() const { return m_facts; }

private:
    TypographyFacts m_facts;
};

/// Whether a table is one Core can lay out by: a cell of 1/64 point to
/// ``maxCellSize`` each way, at most ``maxFaces`` faces, each with a nonzero
/// line height and a known class and style.
///
/// - Parameter facts: The table.
/// - Returns: Whether the table may be adopted.
[[nodiscard]] bool isAdoptable(const TypographyFacts& facts);

/// The face a run's font entry is measured and drawn in.
///
/// The published face whose family matches the entry's name, ASCII
/// case-insensitively, in the run's bold and italic; failing that, the class
/// face of the entry's family class in that style; failing that, the
/// sans-serif class face in that style; failing that, the regular sans-serif
/// class face; failing that, the first face.
///
/// - Parameters:
///   - facts: The table, holding at least one face.
///   - document: The document whose font table the run names.
///   - run: The run.
/// - Returns: The face's index in the table.
[[nodiscard]] std::size_t resolveFace(const TypographyFacts& facts, const Document& document, const Run& run);

} // namespace Ubiquitty::RichText
