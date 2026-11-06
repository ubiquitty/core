// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include <cstddef>
#include <type_traits>

// Geometry value types — a width/height pair and an x/y pair, in whatever
// scalar the caller measures in.
//
// What they buy, which is not brevity. A signature like
//
//     gridFromViewport(float viewportWidth, float viewportHeight, EdgeInsets, //                      float reservedHeight, float cellWidth, float cellHeight)
//
// takes four floats that mean two pairs, and every transposition of a pair
// compiles. The names are the only thing keeping them apart, and a name is not
// checked. Passing the pairs as values makes the same mistake a type error.
//
// They cost nothing, and that is measured rather than assumed. On arm64 a
// two-float aggregate is a homogeneous floating-point aggregate, so it arrives
// in the same registers two bare floats would. Compiled at -O3, the scalar and
// the typed spelling of one function emit identical instructions; so do a call
// through a C function pointer and a call across the ObjC++ boundary. Nothing
// is packed, unpacked, or spilled at a seam.
//
// Not for GPU upload. ``Rendering::Vec2`` is a different type on purpose: it is
// `alignas(8)` to match `simd_float2`, and the instance structs it appears in
// have their field offsets pinned by static_assert because a shader reads those
// bytes. These have natural alignment and no ABI obligation. Using one where
// the other belongs is a wrong picture, not a compile error, so they stay apart.
//
// Grid space is not PIXEL space. A cell coordinate is `(row, column)` and a grid
// size is `(columns, rows)` — the two orders differ, which is exactly the
// mistake worth making impossible. Those keep their own types
// (``CellPosition``, ``GridSize``) rather than becoming
// `IntPoint` and `IntSize`, so a grid quantity cannot be assigned to a pixel one.

namespace Ubiquitty {

/// A width and a height, in the caller's unit.
///
/// - Parameter T: The scalar the dimensions are measured in.
template<typename T>
struct UBIQUITTY_ESCAPABLE Size {
    T width {};
    T height {};

    // Explicit rather than left implicit: the Swift importer's fallback for an
    // implicitly-declared default constructor is a raw zero-fill it now
    // deprecates as unsafe in general, since it cannot know whether zero is a
    // valid default for every C++ type it applies to. Here it always is -- the
    // member initializers above already guarantee it -- so this constructor
    // states that explicitly rather than leaving Swift to guess.
    //
    // Declaring a constructor un-aggregates the type in C++, which would
    // break every `Size { w, h }` call site across the tree, so the memberwise
    // constructor beside it is not optional -- it is what keeps that syntax
    // resolving to a real overload instead of falling back to aggregate rules
    // that no longer apply.
    constexpr Size() = default;
    // The label annotation below pins the argument-label set the aggregate's
    // synthesized memberwise init already carried (`Size(width:height:)`):
    // Swift does not import a hand-written C++ constructor's parameter names
    // as argument labels on its own, and every existing
    // `FloatSize(width:, height:)` call site across the tree depends on them.
    constexpr Size(T width, T height) UBIQUITTY_SWIFT_NAME(init(width:height:))
        : width(width)
        , height(height)
    {
    }

    friend constexpr bool operator==(Size, Size) = default;
};

/// An x and a y, in the caller's unit.
///
/// The type is `Point`; `origin` is a role a point plays. A rect's corner is an
/// `origin` of type `FloatPoint`, which is the arrangement that leaves a point
/// that is not a corner still nameable.
///
/// - Parameter T: The scalar the axes are measured in.
template<typename T>
struct UBIQUITTY_ESCAPABLE Point {
    T x {};
    T y {};

    // Explicit rather than left implicit -- see Size's identical note above,
    // which applies here for the identical reason: a declared special member
    // (the defaulted comparison operator below) is what the Swift importer's
    // deprecated-fallback zero-fill treats as reason enough to ask for an
    // explicit default, whether or not this constructor is the one it read
    // that signal from.
    constexpr Point() = default;
    // See Size's identical note: the label set below pins what the aggregate's
    // synthesized memberwise init carries, since a hand-written constructor
    // does not import its parameter names as Swift labels on its own.
    constexpr Point(T x, T y) UBIQUITTY_SWIFT_NAME(init(x:y:))
        : x(x)
        , y(y)
    {
    }

    friend constexpr bool operator==(Point, Point) = default;
};

/// An origin and an extent — a laid-out box.
///
/// Composed of a ``Point`` and a ``Size`` rather than four loose scalars, for
/// the reason the pair types exist at all: `originX, originY, width, height` is
/// four arguments of one type where two are a position and two are an extent,
/// and nothing distinguishes them at a call site.
///
/// - Parameter T: The scalar the box is measured in.
template<typename T>
struct UBIQUITTY_ESCAPABLE Rect {
    Point<T> origin {};
    Size<T> size {};

    friend constexpr bool operator==(Rect, Rect) = default;
};

/// A count of columns and rows — the grid's extent.
///
/// Not `IntSize`, and the difference is the point. Grid space is indexed
/// `(row, column)` while its extent is written `(columns, rows)`, so the two
/// orders disagree and a pixel `Size` assigned to a grid quantity would be
/// silently transposed. Distinct types make that a compile error.
struct UBIQUITTY_ESCAPABLE GridSize {
    int columns {};
    int rows {};

    friend constexpr bool operator==(GridSize, GridSize) = default;
};

/// One cell's position, row first.
///
/// Row before column, matching how the grid is indexed and stored, and against
/// ``GridSize``'s columns-first order on purpose — the pair that reads
/// backwards is the pair a reader must not assume.
struct UBIQUITTY_ESCAPABLE CellPosition {
    int row {};
    int column {};

    friend constexpr bool operator==(CellPosition, CellPosition) = default;
};

/// Pixel geometry, continuous — a laid-out box or position.
using FloatSize = Size<float>;
/// Pixel geometry, continuous — a laid-out box or position.
using FloatPoint = Point<float>;
/// Pixel geometry, whole — a surface or texture the driver allocates.
using IntSize = Size<int>;
/// Pixel geometry, whole — a surface or texture the driver allocates.
using IntPoint = Point<int>;
/// Pixel geometry, continuous — a laid-out box.
using FloatRect = Rect<float>;
/// Pixel geometry, whole — a laid-out box.
using IntRect = Rect<int>;

// The seam obligations, pinned. A frontend hands these across a C function
// pointer and an ObjC++ boundary, so the layout is a contract and not an
// implementation detail: a member added here, or reordered, changes what the
// other side reads while every count stays right.
static_assert(std::is_standard_layout_v<FloatSize> && std::is_trivially_copyable_v<FloatSize>);
static_assert(std::is_standard_layout_v<FloatPoint> && std::is_trivially_copyable_v<FloatPoint>);
static_assert(sizeof(FloatSize) == 2 * sizeof(float) && sizeof(FloatPoint) == 2 * sizeof(float));
static_assert(sizeof(IntSize) == 2 * sizeof(int) && sizeof(IntPoint) == 2 * sizeof(int));
static_assert(offsetof(FloatSize, width) == 0 && offsetof(FloatSize, height) == sizeof(float));
static_assert(offsetof(FloatPoint, x) == 0 && offsetof(FloatPoint, y) == sizeof(float));
static_assert(std::is_standard_layout_v<FloatRect> && std::is_trivially_copyable_v<FloatRect>);
static_assert(sizeof(FloatRect) == 4 * sizeof(float));
static_assert(offsetof(FloatRect, origin) == 0 && offsetof(FloatRect, size) == 2 * sizeof(float));

} // namespace Ubiquitty
