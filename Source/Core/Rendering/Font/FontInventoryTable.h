// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontInventory.h"

#include <cstdint>
#include <string_view>

// The font catalogue seam a frontend fills, in the shape a frontend can fill.
//
// `FontInventory` states the same contract as a pure-virtual class, which is a
// contract only C++ can meet: neither ObjC nor Swift can inherit one. The glyph
// seam next door reached that conclusion first and answered it with a table of
// function pointers over an opaque context (`Rendering/GlyphBackend.h`), and the
// argument transfers unchanged — a frontend whose font state lives in CoreText
// or Pango has no way to become a C++ base class without an adapter written per
// frontend, which is the thing the seam exists to remove.
//
// So the table is the seam and the class is one way to meet it. A C++ frontend
// that already has an inventory object keeps it; a frontend in another language
// fills the table and wraps it in `TableFontInventory`, which is the class the
// routing policy consumes. `FontFallbackPolicy` sees one type either way, so
// nothing about the routing decision moves.
//
// Every type crossing here is a plain value — `FontHandle` is two integers and
// the results are that handle plus a glyph and two flags. A borrow-carrying type
// here would be unfillable from Swift, which can neither return a non-escapable
// value nor take a pointer to one, so the entry would be dead for the language
// the table exists to serve. Adding one is how this seam stops being fillable.

namespace Ubiquitty::Rendering::Font {

/// The catalogue entry points, as function pointers over a frontend's context.
///
/// Every member takes `context` first, which is the frontend's own object; Core
/// never dereferences it. Unlike the glyph seam, no entry here has a meaningful
/// default — a catalogue that cannot name a primary font cannot route — so all
/// of them are required and ``isComplete`` is what says so.
struct FontInventoryTable {
    /// The frontend object every entry point receives.
    void* context = nullptr;

    /// Returns the generation the catalogue is on.
    std::uint32_t (*catalogGeneration)(const void* context) = nullptr;
    /// Advances the generation, invalidating handles minted before it.
    void (*bumpCatalogGeneration)(void* context) = nullptr;
    /// Returns the configured family's face for a style and variable weight.
    FontHandle (*primaryFont)(void* context, FontStyle, std::uint8_t variableWeight) = nullptr;
    /// Resolves a family request to a handle, or an invalid one if absent.
    FontHandle (*validateFamily)(void* context, const FontFamilyRequest*) = nullptr;
    /// Reports whether a face has a glyph for one code point, and which.
    GlyphProbeResult (*glyphForCodePoint)(void* context, FontHandle, std::uint32_t codePoint) = nullptr;
    /// Reports whether a face covers a complete cluster without substitution.
    GlyphProbeResult (*glyphForCluster)(void* context, const ClusterProbeRequest*) = nullptr;
    /// Walks the system cascade for one code point.
    CascadeResult (*cascadeForCodePoint)(void* context, const CascadeRequest*) = nullptr;
    /// Walks the system cascade for a shaped run.
    CascadeResult (*cascadeForRun)(void* context, const CascadeRequest*) = nullptr;
    /// Reports whether a handle is the last-resort face.
    bool (*isLastResort)(const void* context, FontHandle) = nullptr;
    /// Returns a face's family name, valid while the catalogue generation holds.
    std::string_view (*familyName)(const void* context, FontHandle) = nullptr;
    /// Returns a face's PostScript name, valid while the catalogue generation holds.
    std::string_view (*postScriptName)(const void* context, FontHandle) = nullptr;
};

/// Whether a table can answer every question routing asks of it.
///
/// Checked once where the table is adopted rather than per call, so a frontend
/// that filled ten of eleven entries fails at the seam instead of dereferencing
/// null on the first code point that takes the missing path — which, for the
/// cascade entries, is the first character outside the primary face.
///
/// - Parameter table: The table to check.
/// - Returns: Whether every entry is filled.
[[nodiscard]] constexpr bool isComplete(const FontInventoryTable& table)
{
    return table.catalogGeneration && table.bumpCatalogGeneration && table.primaryFont
        && table.validateFamily && table.glyphForCodePoint && table.glyphForCluster && table.cascadeForCodePoint
        && table.cascadeForRun && table.isLastResort && table.familyName && table.postScriptName;
}

/// A catalogue backed by a table rather than by inheritance.
///
/// Holds the table by value and the frontend's context by borrow, so it must not
/// outlive whatever filled it. The routing policy takes a `FontInventory&`, and
/// this is what a non-C++ frontend passes it.
class TableFontInventory final : public FontInventory {
public:
    /// Adopts a table.
    ///
    /// - Parameter table: The entry points, which must satisfy ``isComplete``.
    explicit TableFontInventory(const FontInventoryTable& table)
        : m_table(table)
    {
    }

    /// Returns the generation the catalogue is on.
    ///
    /// - Returns: The current generation.
    std::uint32_t catalogGeneration() const override { return m_table.catalogGeneration(m_table.context); }
    /// Advances the catalogue generation, invalidating older handles.
    void bumpCatalogGeneration() override { m_table.bumpCatalogGeneration(m_table.context); }
    /// Returns the configured family's face for a style and weight.
    ///
    /// - Parameters:
    ///   - style: The face to resolve.
    ///   - variableWeight: The `wght` axis value, or zero for the face's own.
    /// - Returns: The face's handle, invalid if the family has none.
    FontHandle primaryFont(FontStyle style, std::uint8_t variableWeight) override
    {
        return m_table.primaryFont(m_table.context, style, variableWeight);
    }
    /// Resolves a family request to a handle.
    ///
    /// - Parameter request: The family and traits wanted.
    /// - Returns: The face's handle, invalid if the family is absent.
    FontHandle validateFamily(const FontFamilyRequest& request) override
    {
        return m_table.validateFamily(m_table.context, &request);
    }
    /// Reports whether a face carries a glyph for one code point.
    ///
    /// - Parameters:
    ///   - font: The face to probe.
    ///   - codePoint: The character wanted.
    /// - Returns: The glyph and whether the face has it.
    GlyphProbeResult glyphForCodePoint(FontHandle font, std::uint32_t codePoint) override
    {
        return m_table.glyphForCodePoint(m_table.context, font, codePoint);
    }
    /// Probes one face for complete cluster coverage through the host callback.
    ///
    /// - Parameters:
    ///   - font: The face to probe.
    ///   - codePoints: The complete cluster.
    ///   - presentation: The text or emoji presentation to require.
    /// - Returns: The face and glyph when the cluster is covered without substitution.
    GlyphProbeResult glyphForCluster(FontHandle font, std::span<const std::uint32_t> codePoints, Presentation presentation) override
    {
        const ClusterProbeRequest request { font, codePoints, presentation };
        return m_table.glyphForCluster(m_table.context, &request);
    }
    /// Walks the system cascade for one code point.
    ///
    /// - Parameter request: The base face, character and language to route.
    /// - Returns: The face that carries it, and its glyph.
    CascadeResult cascadeForCodePoint(const CascadeRequest& request) override
    {
        return m_table.cascadeForCodePoint(m_table.context, &request);
    }
    /// Walks the system cascade for a shaped run.
    ///
    /// - Parameter request: The base face, run and language to route.
    /// - Returns: The face that carries the run, and its glyph.
    CascadeResult cascadeForRun(const CascadeRequest& request) override
    {
        return m_table.cascadeForRun(m_table.context, &request);
    }
    /// Reports whether a handle names the last-resort face.
    ///
    /// - Parameter font: The face to test.
    /// - Returns: Whether it is the last-resort face.
    bool isLastResort(FontHandle font) const override { return m_table.isLastResort(m_table.context, font); }
    /// Returns a face's family name.
    ///
    /// - Parameter font: The face to name.
    /// - Returns: The family name, valid while the generation holds.
    std::string_view familyName(FontHandle font) const override { return m_table.familyName(m_table.context, font); }
    /// Returns a face's PostScript name.
    ///
    /// - Parameter font: The face to name.
    /// - Returns: The PostScript name, valid while the generation holds.
    std::string_view postScriptName(FontHandle font) const override
    {
        return m_table.postScriptName(m_table.context, font);
    }

private:
    // The request structs are passed by pointer rather than by reference: a
    // reference parameter in a function-pointer signature is spelled as a
    // pointer by every language that is not C++, and a table meant to be filled
    // from one of them should say what it means.
    FontInventoryTable m_table;
};

} // namespace Ubiquitty::Rendering::Font
