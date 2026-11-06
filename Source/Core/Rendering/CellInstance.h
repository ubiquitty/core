// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

// CellInstance — backend-neutral POD per-frame GPU instance payloads. These
// mirror the macOS Metal renderer's instance structs field-for-field and
// byte-for-byte but carry no Apple / simd type, so any backend uploads the same
// bytes. The layout must stay identical to the Metal / SIMD originals:
// simd_float2 is 8-byte aligned and simd_float4 16-byte aligned, so Vec2 / Vec4
// replicate that and the natural padding reproduces every field offset. The
// static_asserts below pin the sizes so any drift is a compile error.
// Pure C++, no exceptions / RTTI, host-testable.

namespace Ubiquitty::Rendering {

/// An 8-byte aligned 2-vector, laid out to match `simd_float2` and Metal's
/// `float2`.
///
/// Escapable: two floats and no pointer, so a copy outlives whatever produced it.
struct UBIQUITTY_ESCAPABLE alignas(8) Vec2 {
    float x, y;
};

/// A 16-byte aligned 4-vector, laid out to match `simd_float4` and Metal's
/// `float4`.
///
/// Escapable: four floats and no pointer, so a copy outlives whatever produced
/// it. Swift builds these for the colour fields the frame options carry.
struct UBIQUITTY_ESCAPABLE alignas(16) Vec4 {
    float x, y, z, w;

    // Explicit rather than left implicit -- see Size's identical note in
    // Geometry.h. No member initializers, so this states the same trivial
    // zero-fill the implicit default already performed; the difference is
    // that it is a declaration the Swift importer can see and trust,
    // instead of a fallback it deprecates as unsafe.
    //
    // The memberwise constructor beside it is required, not decorative: adding
    // any constructor un-aggregates the type, and Vec4 { x, y, z, w } is the
    // hot instance-stream path in CellInstanceBuilder.cpp.
    constexpr Vec4() = default;
    constexpr Vec4(float x, float y, float z, float w) UBIQUITTY_SWIFT_NAME(init(x:y:z:w:))
        : x(x)
        , y(y)
        , z(z)
        , w(w)
    {
    }
};

// Canonical float-channel → byte conversion (clamp, round); shared by the
// instance builder and software compositor so both quantize colors identically.
inline std::uint8_t channelToByte(float v)
{
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

/// Cell-background pass instance (one per visible cell). `atlasTopLeft ==
/// atlasBottomRight` marks a blank cell.
///
/// Escapable: plain scalars, so a frontend hands a frame's worth across the
/// language seam as a span and the copies stand alone.
struct UBIQUITTY_ESCAPABLE CellInstance {
    Vec2 gridPosition; // (column, row)
    float widthCells; // 1 narrow, 2 wide lead
    Vec4 foreground; // foreground RGBA, 0..1
    Vec4 background; // background RGBA, 0..1
    Vec2 atlasTopLeft; // glyph rect top-left, atlas pixels
    Vec2 atlasBottomRight; // glyph rect bottom-right, atlas pixels
};

/// Subpixel glyph pass instance (a pixel-snapped quad alpha-blended over the
/// cell background).
///
/// Escapable for the same reason as ``CellInstance``.
struct UBIQUITTY_ESCAPABLE GlyphInstance {
    Vec2 originInPixels; // pixel-snapped top-left
    Vec2 sizeInPixels; // glyph quad size in pixels
    Vec4 foreground;
    Vec4 background; // the cell background this glyph blends over
    Vec2 atlasTopLeft;
    Vec2 atlasBottomRight;
};

/// Cursor / selection / box-fill / Powerline-separator quad.
///
/// Escapable for the same reason as ``CellInstance``.
struct UBIQUITTY_ESCAPABLE OverlayInstance {
    Vec2 topLeftInPixels;
    Vec2 bottomRightInPixels;
    Vec4 color; // RGBA, 0..1; alpha drives the blend
    float cornerRadius; // >0 = rounded-rect corners, device px
    float cornerStyle; // 0 = circular arc, 1 = continuous (superellipse)
    float strokeWidth; // >0 = hollow outline of this width, device px; for the pill ring (-3), how far the pill's top sits below its cells' box's
    float shadeDensity; // >0 = phase-locked ordered-dither shade at density; for the pill ring (-3), how far the pill's bottom sits above its cells' box's
    float shapeKind; // >0 = analytic Powerline SDF; -1 = mosaic mask; -2 = Braille coverage (ink bounds in plA); -3 = focused search pill ring (color.a packs the ground as r*65536+g*256+b); -4 = toolkit caret (color.a is its 8-bit mask / 255)
    std::uint32_t usesBackgroundColors; // Replaces the cell with a shaped transition between color and background.
    float foregroundCoverage; // Blink/conceal coverage, independent of a transition's background opacity.
    Vec4 plA; // shape parameters (unit cell coords); for the pill ring, the pill's rect: left, top, right, bottom in frame pixels
    Vec4 plB;
    Vec4 background; // The other side of a background transition, including native coverage.
};

/// Underline / overline / strikethrough / box rule. The first 80 bytes (through
/// pad1) are the rule geometry + OWN-cell skip-ink mask the current GPU shaders
/// read; the trailing 80 bytes (OWN-cell slot clamp + cross-cell neighbour
/// glyphs) are additive and zero-filled by the builder, so a shader reading only
/// the first 80 bytes renders unchanged. Matches macOS DecorationInstance.
///
/// Escapable for the same reason as ``CellInstance``.
struct UBIQUITTY_ESCAPABLE DecorationInstance {
    Vec2 topLeftInPixels;
    Vec2 bottomRightInPixels;
    Vec4 color;
    float kind; // DECO_* selector (matches the shader)
    float dashPeriodInPixels; // pattern period for dotted / dashed / curly
    float thicknessInPixels; // stroke thickness (curl / box)
    float skipInk; // 1 = cut the rule where glyph ink crosses it
    Vec2 inkTopLeft; // glyph cell box in atlas pixels (skip-ink coverage tap)
    Vec2 inkBottomRight;
    Vec2 padInAtlas; // skip-ink dilation pad, atlas pixels
    // .x: the dilation's tap count; .y: three base-8 sheet selectors for self,
    // left, and right. Each digit is mono 0, proportional strip 1, ligature
    // strip 2, the proportional glyph sheet 3, or the two-cell sheet 4, so a
    // shaped run or a wide glyph samples the pixels it actually draws.
    Vec2 pad1;
    // OWN-cell full atlas-slot clamp: dilation taps reaching past the slot
    // would sample a neighbour glyph, so the advanced skip-ink pass clamps to
    // this rect. .x >= .y (a zeroed pair) marks "unset".
    Vec2 glyphSlotTopLeftUV;
    Vec2 glyphSlotBottomRightUV;
    // Cross-cell skip-ink: an underline rule is per-cell, so the neighbouring
    // cell's rule must cut a descender's halo leaning in from the adjacent cell.
    // A zeroed *Box (.x >= .y) marks an absent neighbour.
    //   *Ink = (cellBoxU min, cellBoxU max, band v top, band v bottom)
    //   *Box = (slot clamp min.x, min.y, max.x, max.y)
    Vec4 leftInk;
    Vec4 leftBox;
    Vec4 rightInk;
    Vec4 rightBox;
};

/// Per-frame uniforms shared by every pass.
///
/// Escapable for the same reason as ``CellInstance``.
struct UBIQUITTY_ESCAPABLE FrameUniforms {
    // Explicit rather than left implicit -- see Size's identical note in
    // Geometry.h. No memberwise constructor: nothing in the tree brace-inits
    // a FrameUniforms (it is always default-constructed, then field-assigned),
    // so there is no aggregate-init call site to preserve.
    constexpr FrameUniforms() = default;

    Vec2 viewportSizeInPixels;
    Vec2 cellSizeInPixels;
    Vec2 originInPixels; // top-left of the grid (y measured from top)
    float backgroundOpacity; // cell-background alpha; <1 shows the desktop through
    Vec4 defaultBackground; // terminal default background (sRGB 0..1)
    // DECSET 7402: glyphless cells paint the letterbox opaquely. Trailing, and
    // it pads the struct from 68 to 80 — a backend that stops at
    // defaultBackground hands the shader a buffer 16 bytes short and this field
    // reads whatever follows it, which is a letterbox that turns itself on.
    std::int32_t edgeToEdge;
    // Whether this frame's face is one Core rasterises itself (GNU Unifont):
    // 1 for the bitmap tier, 0 for a CoreText or FreeType face. The
    // procedural passes read it to composite partial coverage the way the
    // software renderer does, so the bitmap tier's golden is one picture on
    // every arm; a real font keeps the system's own blend. Sits in the
    // padding after edgeToEdge, so the stride stays 64 bytes.
    std::int32_t bitmapTier;
};

/// One image placement's quad, in device pixels with its source sub-rect.
///
/// Kitty graphics and sixel placements both land here, resolved by the
/// frontend to the destination rect the shader draws and the UV window it
/// samples. The texture is not a field: a placement's pixels are a backend
/// object, and the instance carries only what the vertex stage reads, so the
/// backend binds one texture per quad beside the buffer.
///
/// Escapable: eight floats and no pointer, so a copy outlives the placement it
/// was resolved from — which is what lets a frontend hand a whole frame's
/// worth across a language seam as a plain span.
struct UBIQUITTY_ESCAPABLE ImageInstance {
    Vec2 topLeftInPixels;
    Vec2 bottomRightInPixels;
    Vec2 uv0;
    Vec2 uv1;
};

/// One Tek 4014 vector segment: an oriented stroke quad.
///
/// A segment per solid LINE, per dash run within a dashed one, per POINT, and
/// one per GIN crosshair arm. The quad is oriented rather than axis-aligned
/// because a vector display draws at any angle. The shader grows it by
/// ``halfThickness`` and the antialiasing feather on every side, ends included,
/// and shades the capsule round the segment, so a run's ends are round and a
/// point's degenerate segment is the beam's spot.
///
/// The pad after ``halfThickness`` is written, not merely reserved: the colour
/// that follows is 16-byte aligned, so the compiler would insert the space
/// regardless and a member spells what the shader's own struct spells.
struct UBIQUITTY_ESCAPABLE TekLineInstance {
    Vec2 startInPixels;
    Vec2 endInPixels;
    float halfThickness;
    float pad0;
    Vec4 color;
};

// Layout contract — these bytes are shared with the Metal / SIMD originals
// and the GPU shaders, so a drift here is a cross-backend rendering bug.
static_assert(sizeof(Vec2) == 8 && alignof(Vec2) == 8, "Vec2 must match simd_float2");
static_assert(sizeof(Vec4) == 16 && alignof(Vec4) == 16, "Vec4 must match simd_float4");

static_assert(sizeof(CellInstance) == 64, "CellInstance stride must match the shader");
static_assert(offsetof(CellInstance, gridPosition) == 0);
static_assert(offsetof(CellInstance, widthCells) == 8);
static_assert(offsetof(CellInstance, foreground) == 16);
static_assert(offsetof(CellInstance, background) == 32);
static_assert(offsetof(CellInstance, atlasTopLeft) == 48);
static_assert(offsetof(CellInstance, atlasBottomRight) == 56);
// Trivially copyable and standard layout, which is what licences the memcpy
// upload path and the `offsetof` pins above: `offsetof` on a non-standard-layout
// type is ill-formed, and a member made non-trivial would take these asserts out
// of the language rather than merely changing their answers.
static_assert(std::is_standard_layout_v<CellInstance>);
static_assert(std::is_trivially_copyable_v<CellInstance>);

static_assert(sizeof(ImageInstance) == 32, "ImageInstance stride must match the shader");
static_assert(offsetof(ImageInstance, topLeftInPixels) == 0);
static_assert(offsetof(ImageInstance, bottomRightInPixels) == 8);
static_assert(offsetof(ImageInstance, uv0) == 16);
static_assert(offsetof(ImageInstance, uv1) == 24);
static_assert(std::is_standard_layout_v<ImageInstance>);
static_assert(std::is_trivially_copyable_v<ImageInstance>);

static_assert(sizeof(TekLineInstance) == 48, "TekLineInstance stride must match the shader");
static_assert(offsetof(TekLineInstance, startInPixels) == 0);
static_assert(offsetof(TekLineInstance, endInPixels) == 8);
static_assert(offsetof(TekLineInstance, halfThickness) == 16);
static_assert(offsetof(TekLineInstance, pad0) == 20);
// The colour's own alignment, not a chosen offset: Vec4 is 16-byte aligned, so
// the eight bytes between the pad and here are the compiler's and the shader
// carries them too.
static_assert(offsetof(TekLineInstance, color) == 32);
static_assert(std::is_standard_layout_v<TekLineInstance>);
static_assert(std::is_trivially_copyable_v<TekLineInstance>);

static_assert(sizeof(GlyphInstance) == 64, "GlyphInstance stride must match the shader");
static_assert(offsetof(GlyphInstance, foreground) == 16);
static_assert(offsetof(GlyphInstance, atlasTopLeft) == 48);

static_assert(sizeof(OverlayInstance) == 112, "OverlayInstance stride must match the shader");
static_assert(offsetof(OverlayInstance, color) == 16);
static_assert(offsetof(OverlayInstance, cornerRadius) == 32);
// Every scalar between cornerRadius and plA, not the ends alone: a field
// inserted among them keeps both anchors and moves everything between, which is
// the layout drift a sizeof or a two-anchor check cannot see.
static_assert(offsetof(OverlayInstance, cornerStyle) == 36);
static_assert(offsetof(OverlayInstance, strokeWidth) == 40);
static_assert(offsetof(OverlayInstance, shadeDensity) == 44);
static_assert(offsetof(OverlayInstance, shapeKind) == 48);
static_assert(offsetof(OverlayInstance, usesBackgroundColors) == 52);
static_assert(offsetof(OverlayInstance, foregroundCoverage) == 56);
static_assert(offsetof(OverlayInstance, plA) == 64);
static_assert(offsetof(OverlayInstance, plB) == 80);
static_assert(offsetof(OverlayInstance, background) == 96);

static_assert(sizeof(DecorationInstance) == 160, "DecorationInstance stride must match the shader");
static_assert(offsetof(DecorationInstance, topLeftInPixels) == 0);
static_assert(offsetof(DecorationInstance, bottomRightInPixels) == 8);
static_assert(offsetof(DecorationInstance, color) == 16);
static_assert(offsetof(DecorationInstance, dashPeriodInPixels) == 36);
static_assert(offsetof(DecorationInstance, thicknessInPixels) == 40);
static_assert(offsetof(DecorationInstance, skipInk) == 44);
static_assert(offsetof(DecorationInstance, kind) == 32);
static_assert(offsetof(DecorationInstance, inkTopLeft) == 48);
static_assert(offsetof(DecorationInstance, inkBottomRight) == 56);
static_assert(offsetof(DecorationInstance, padInAtlas) == 64);
static_assert(offsetof(DecorationInstance, pad1) == 72);
static_assert(offsetof(DecorationInstance, glyphSlotTopLeftUV) == 80);
static_assert(offsetof(DecorationInstance, glyphSlotBottomRightUV) == 88);
static_assert(offsetof(DecorationInstance, leftInk) == 96);
static_assert(offsetof(DecorationInstance, leftBox) == 112);
static_assert(offsetof(DecorationInstance, rightInk) == 128);
static_assert(offsetof(DecorationInstance, rightBox) == 144);
// The same pair that licences the pins above: `offsetof` on a non-standard-layout
// type is ill-formed, so a member made non-trivial removes the asserts from the
// language rather than changing what they answer.
static_assert(std::is_standard_layout_v<DecorationInstance>);
static_assert(std::is_trivially_copyable_v<DecorationInstance>);

// Every field is pinned rather than the size alone. A size assert checks the
// struct against a number, not against the shader: a struct that lacks a
// trailing field the shader reads asserts its own shorter size and passes.
static_assert(sizeof(FrameUniforms) == 64, "FrameUniforms stride must match the shader");
static_assert(offsetof(FrameUniforms, viewportSizeInPixels) == 0);
static_assert(offsetof(FrameUniforms, cellSizeInPixels) == 8);
static_assert(offsetof(FrameUniforms, originInPixels) == 16);
static_assert(offsetof(FrameUniforms, backgroundOpacity) == 24);
static_assert(offsetof(FrameUniforms, defaultBackground) == 32);
static_assert(offsetof(FrameUniforms, edgeToEdge) == 48);
static_assert(offsetof(FrameUniforms, bitmapTier) == 52);
static_assert(std::is_standard_layout_v<FrameUniforms>);
static_assert(std::is_trivially_copyable_v<FrameUniforms>);

} // namespace Ubiquitty::Rendering
