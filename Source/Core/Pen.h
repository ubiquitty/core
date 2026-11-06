// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/VT/Constants.h"
#include <array>
#include <cstdint>
#include <type_traits>

namespace Ubiquitty {

/// Identifies the shell section that produced a cell.
enum class SemanticRole : uint16_t {
    Output,
    Prompt,
    Input,
};

// SGR pen: attributes plus fg/bg/underline colors. An *Index of colorRGB uses
// the matching r/g/b fields; colorDefault uses the renderer default, except on
// underlineIndex where it means "follow text color" (the SGR 59 / SGR 0 reset).
struct Pen {
    Ubiquitty::VT::AttributeSet attributes; // OptionSet<Attribute> — serializes as its raw uint16_t
    uint16_t foregroundIndex;
    uint16_t backgroundIndex;
    uint16_t underlineIndex : 9;
    SemanticRole semanticRole : 2;
    // Whether the foreground or background index is the DECAC normal text
    // colour standing in for the default one rather than a colour SGR selected.
    // The Sync row form carries both in bits 11 and 12 of the underline word.
    uint16_t isForegroundAssignedDefault : 1;
    uint16_t isBackgroundAssignedDefault : 1;
    uint16_t underlineReserved : 3;
    uint8_t foregroundRed, foregroundGreen, foregroundBlue;
    uint8_t backgroundRed, backgroundGreen, backgroundBlue;
    uint8_t underlineRed, underlineGreen, underlineBlue;
    uint8_t underlineStyle; // packed shape / script / ideogram (SGR 60-65); layout in
                            // Pen*Field. Access via penUnderlineShape / penScript / penIdeogram.
    Ubiquitty::VT::ExtendedAttributes extendedAttributes; // OptionSet<ExtendedAttribute> flags + a weight subfield
    // Packed font style (SGR 10-20) / proportional (SGR 26) / text band
    // (bits 0-2, the row a multi-row sized block cell draws) / rich text cover
    // (bit 3); layout in Pen*Field. Access via penFontStyle / penProportional /
    // penTextBand / penRichTextCovered. Bit 4 carries ECMA guarded-area
    // membership; access it through penIsGuarded / penSetGuarded.
    uint8_t font;
};

// Pen embeds OptionSet fields (non-trivial default ctor), so it must never be
// memset/calloc'd (UB); it stays trivially copyable for the whole-cell memcpy.
static_assert(std::is_trivially_copyable_v<Pen>, "Pen must stay trivially copyable (whole-cell memcpy / snapshot wire)");

// Underline indices need nine bits (0..257); semantic roles and the two
// assigned-default flags share that word without enlarging Cell.
// underlineReserved stays zero for memcmp equality.
static_assert(sizeof(Pen) == 20, "the cell pen occupies twenty bytes");

// Two bytes carry several unrelated SGR properties between them, and the packing
// is the point: `Pen` is 20 bytes and the static_assert above says what widening
// one costs. These accessors are the only supported way in — a caller reading
// `underlineStyle` or `font` directly sees a value with more than one field in it.

/// Returns the underline shape (`underlineNone`..`underlineDashed`, SGR 4:n).
///
/// - Parameter p: The pen to read.
/// - Returns: The shape, within the range `PenUnderlineShapeField` allows.
static inline uint8_t penUnderlineShape(const Pen* p) { return Ubiquitty::VT::PenUnderlineShapeField::get(p->underlineStyle); }

/// Returns the superscript/subscript selection (`scriptNone`..`scriptSub`, SGR 73-75).
///
/// - Parameter p: The pen to read.
/// - Returns: The script, within the range `PenScriptField` allows.
static inline uint8_t penScript(const Pen* p) { return Ubiquitty::VT::PenScriptField::get(p->underlineStyle); }

/// Sets the underline shape, leaving the script and ideogram fields alone.
///
/// - Parameters:
///   - p: The pen to modify.
///   - shape: One of `underlineNone`..`underlineDashed`. A value too wide for
///     the field is truncated to it rather than spilling into its neighbours.
static inline void penSetUnderlineShape(Pen* p, uint8_t shape) { p->underlineStyle = Ubiquitty::VT::PenUnderlineShapeField::set(p->underlineStyle, shape); }

/// Sets the script selection, leaving the underline shape and ideogram alone.
///
/// - Parameters:
///   - p: The pen to modify.
///   - script: One of `scriptNone`..`scriptSub`.
static inline void penSetScript(Pen* p, uint8_t script) { p->underlineStyle = Ubiquitty::VT::PenScriptField::set(p->underlineStyle, script); }

/// Returns the ideogram decoration (`ideogramNone`..`ideogramStress`, SGR 60-65).
///
/// - Parameter p: The pen to read.
/// - Returns: The decoration, within the range `PenIdeogramField` allows.
static inline uint8_t penIdeogram(const Pen* p) { return Ubiquitty::VT::PenIdeogramField::get(p->underlineStyle); }

/// Sets the ideogram decoration, leaving the underline shape and script alone.
///
/// - Parameters:
///   - p: The pen to modify.
///   - ideogram: One of `ideogramNone`..`ideogramStress`.
static inline void penSetIdeogram(Pen* p, uint8_t ideogram) { p->underlineStyle = Ubiquitty::VT::PenIdeogramField::set(p->underlineStyle, ideogram); }

/// Returns the font selection (`fontPrimary`..`fontFraktur`, SGR 10-20).
///
/// - Parameter p: The pen to read.
/// - Returns: The font, within the range `PenFontStyleField` allows.
static inline uint8_t penFontStyle(const Pen* p) { return Ubiquitty::VT::PenFontStyleField::get(p->font); }

/// Sets the font selection, leaving the proportional flag alone.
///
/// - Parameters:
///   - p: The pen to modify.
///   - fontStyle: One of `fontPrimary`..`fontFraktur`.
static inline void penSetFontStyle(Pen* p, uint8_t fontStyle) { p->font = Ubiquitty::VT::PenFontStyleField::set(p->font, fontStyle); }

/// Returns whether the cell is proportionally spaced (SGR 26).
///
/// - Parameter p: The pen to read.
/// - Returns: `true` when proportional spacing is on.
static inline bool penProportional(const Pen* p) { return Ubiquitty::VT::PenProportionalField::get(p->font) != 0; }

/// The vertical band this cell draws of its multi-row sized block.
///
/// - Parameter p: The pen to read.
/// - Returns: Zero for a single-row cell or a block's top row, else the
///   0-based row within the block, within what `PenTextBandField` allows.
static inline uint8_t penTextBand(const Pen* p) { return Ubiquitty::VT::PenTextBandField::get(p->font); }
/// Sets proportional spacing, leaving the font style alone.
///
/// - Parameters:
///   - p: The pen to modify.
///   - isProportional: Whether proportional spacing applies.
static inline void penSetProportional(Pen* p, bool isProportional) { p->font = Ubiquitty::VT::PenProportionalField::set(p->font, isProportional ? 1 : 0); } // bool-ok: SGR 26 and SGR 50 are the on and off of one property, and the field they write is one bit wide

/// Whether a rich text block covers the cell.
///
/// - Parameter p: The pen to read.
/// - Returns: `true` on a cell of a block's rectangle.
static inline bool penRichTextCovered(const Pen* p) { return Ubiquitty::VT::PenRichTextCoveredField::get(p->font) != 0; }

/// Reports whether ECMA guarded-area protection applies to the cell.
/// - Parameter p: The pen to read.
/// - Returns: Whether ordinary erasure preserves this cell.
static inline bool penIsGuarded(const Pen* p) { return Ubiquitty::VT::PenGuardedField::get(p->font) != 0; }

/// Changes ECMA guarding without affecting DEC protection or rendition.
/// - Parameters:
///   - p: The pen to update.
///   - isGuarded: Whether subsequent characters belong to a guarded area.
static inline void penSetGuarded(Pen* p, bool isGuarded) { p->font = Ubiquitty::VT::PenGuardedField::set(p->font, isGuarded ? 1 : 0); } // bool-ok: this stores a single cell property

/// Marks or unmarks a cell as covered by a rich text block.
///
/// - Parameters:
///   - p: The pen to write.
///   - isCovered: Whether a block covers the cell.
static inline void penSetRichTextCovered(Pen* p, bool isCovered) { p->font = Ubiquitty::VT::PenRichTextCoveredField::set(p->font, isCovered ? 1 : 0); } // bool-ok: the field is one bit, and the two callers are the block writer and the block's removal

/// Stamps the vertical band of a multi-row sized block cell.
///
/// - Parameters:
///   - p: The pen to write.
///   - band: The 0-based row within the block.
static inline void penSetTextBand(Pen* p, uint8_t band) { p->font = Ubiquitty::VT::PenTextBandField::set(p->font, band); }

// Power-on defaults: all zero, the three color indices at colorDefault. The one
// definition SGR 0 and the screen-reset paths share: a hand copy drifts, and a
// drifted one leaves a palette-0 underline color after RIS/DECSTR.
inline void penReset(Pen* p)
{
    // Value-init, not memset: Pen embeds non-trivial OptionSet fields (memset is
    // UB). Relies on Pen{} zeroing everything but the three indices set below.
    *p = Pen {};
    p->foregroundIndex = Ubiquitty::colorDefault;
    p->backgroundIndex = Ubiquitty::colorDefault;
    p->underlineIndex = Ubiquitty::colorDefault;
}

/// The colours DECAC assigns to one of its items: the foreground palette index,
/// then the background one.
///
/// Each is 0 to 15 once assigned; `unassignedColors` says the terminal's own
/// colours stand. DECAC assigns the two together.
using ColorAssignment = std::array<uint16_t, 2>;

/// The assignment of an item DECAC has not assigned.
inline constexpr ColorAssignment unassignedColors { Ubiquitty::colorDefault, Ubiquitty::colorDefault };

/// The attribute combinations DECATC assigns colours to: 0 is normal text, 1 to 15 the
/// combinations of bold, reverse, underline and blink the VT520 numbers.
inline constexpr std::size_t alternateTextCombinationCount = 16;

/// The colours DECATC assigns to each attribute combination, indexed by the combination.
using AlternateTextColors = std::array<ColorAssignment, alternateTextCombinationCount>;

/// The table of a terminal DECATC has not assigned anything to.
inline constexpr AlternateTextColors unassignedAlternateTextColors = [] {
    AlternateTextColors table {};
    table.fill(unassignedColors);
    return table;
}();

/// Returns whether DECAC has assigned an item its colours.
///
/// - Parameter colors: The item's assignment.
/// - Returns: `true` when both indices name one of the 16 colours DECAC selects from.
[[nodiscard]] inline bool isColorAssigned(const ColorAssignment& colors)
{
    constexpr uint16_t lastAssignableIndex = 15;
    return colors[0] <= lastAssignableIndex && colors[1] <= lastAssignableIndex;
}

/// Gives a pen the colours DECAC assigned to normal text in place of the default ones.
///
/// The VT520 DECAC normal text colours are the default foreground and
/// background, so they are resolved into the pen and every cell it writes from
/// then on, and into the cells already written in the default colours. Each
/// colour resolved is marked as the default, so a later assignment replaces it
/// while a colour SGR selected explicitly stays, whatever its index.
///
/// - Parameters:
///   - p: The pen to resolve.
///   - assigned: The normal text colours now assigned.
inline void penApplyNormalTextColors(Pen* p, const ColorAssignment& assigned)
{
    if (!isColorAssigned(assigned))
        return;
    if (p->foregroundIndex == Ubiquitty::colorDefault || p->isForegroundAssignedDefault) {
        p->foregroundIndex = assigned[0];
        p->isForegroundAssignedDefault = 1;
    }
    if (p->backgroundIndex == Ubiquitty::colorDefault || p->isBackgroundAssignedDefault) {
        p->backgroundIndex = assigned[1];
        p->isBackgroundAssignedDefault = 1;
    }
}

/// Returns a pen's DECAC normal text colours to the default ones they stand in for.
///
/// SGR reads the pen this way, so a colour it selects is explicit even when its
/// index is the assigned one, and `penApplyNormalTextColors` then resolves the
/// colours it left at the default.
///
/// - Parameter p: The pen to return.
inline void penReturnNormalTextColorsToDefault(Pen* p)
{
    if (p->isForegroundAssignedDefault) {
        p->foregroundIndex = Ubiquitty::colorDefault;
        p->isForegroundAssignedDefault = 0;
    }
    if (p->isBackgroundAssignedDefault) {
        p->backgroundIndex = Ubiquitty::colorDefault;
        p->isBackgroundAssignedDefault = 0;
    }
}

/// Resets SGR attributes, preserving protection and the prompt or input section.
/// - Parameter p: The pen to reset.
inline void penResetRendition(Pen* p)
{
    const SemanticRole role = p->semanticRole;
    const auto protection = p->attributes & VT::Attribute::Protected;
    const auto guarded = VT::PenGuardedField::get(p->font);
    penReset(p);
    p->semanticRole = role;
    p->attributes.add(protection);
    p->font = VT::PenGuardedField::set(p->font, guarded);
}

// penReset's zero invariant: a value-init Pen must be all-zero except the
// indices it sets, so its OptionSet fields must default to the empty set.
static_assert(Ubiquitty::VT::AttributeSet {}.toRaw() == 0 && Ubiquitty::VT::ExtendedAttributes {}.flags.toRaw() == 0, "a value-init OptionSet must be empty for penReset's zero invariant");

} // namespace Ubiquitty
