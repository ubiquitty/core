// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>

// RenderPathMarker — names the code that produced a frame, for a harness that
// has to know which implementation it measured.
//
// A snapshot arm is named for a frontend, but a frontend chooses at run time
// between implementations that share a geometry: cell instances built by Core
// or by an inline copy, a frame composited on the GPU or in software. A
// rendered PNG says none of that, so an arm that falls through to another path
// produces a correct-looking image and reports as the path it was asked for.
// The two axes are independent — the macOS frontend composites in Metal while
// building its instances through Core — so one field cannot express which code
// ran and this carries both.
//
// Pure C++, no platform types: every frontend spells these names from here
// rather than writing its own string, or the harness compares vocabularies
// instead of paths.

namespace Ubiquitty::Rendering {

/// Names which implementation built a frame's cell instances.
///
/// A frontend may carry its own copy of the instance geometry alongside the
/// call into Core. The copies drift, and a copy that no arm renders drifts
/// silently, so which one ran is a property a test needs to be able to read.
enum class CellBuilder : std::uint8_t {
    /// `buildCellInstances`, the portable builder in Core.
    Core = 0,
    /// Retired: a cell walk private to a frontend, duplicating Core's
    /// geometry. No frontend carries one; the value stays so an old marker
    /// line keeps its meaning and a later entry cannot take this number.
    FrontendInline = 1,
};

/// Names which implementation drew a frame's instances into pixels.
///
/// The compositor is chosen independently of the builder: the same instances
/// reach a GPU pipeline on one frontend and a CPU rasteriser on another, and
/// the edge pixels differ enough that they are compared against separate
/// references.
enum class FrameCompositor : std::uint8_t {
    /// `SoftwareRenderer`, the portable CPU rasteriser in Core.
    Software = 0,
    /// The macOS Metal pipeline.
    Metal = 1,
    /// The Linux Vulkan pipeline.
    Vulkan = 2,
    /// The web frontend's 2D canvas rasteriser.
    Canvas = 3,
    /// The web frontend's WebGL pipeline.
    WebGL = 4,
};

/// Returns the harness-readable spelling of `builder`.
///
/// - Parameter builder: The implementation that built the cell instances.
/// - Returns: A stable lowercase token; never empty.
[[nodiscard]] inline const char* cellBuilderName(CellBuilder builder)
{
    switch (builder) {
    case CellBuilder::Core:
        return "core";
    case CellBuilder::FrontendInline:
        return "frontend-inline";
    }
    return "unknown";
}

/// Which encode drew the instances, where a frontend carries more than one.
///
/// A frontend with a single encode reports ``Native``. One that carries its
/// own encode beside the Swift renderer module says which drew the frame: a
/// pixel record that cannot say is a recording of an unknown path, because the
/// two agree and nothing else in the frame distinguishes them.
enum class FrameEncoder : std::uint8_t {
    /// The frontend's own encode, in whatever language the frontend is written.
    Native = 0,
    /// The Swift renderer module.
    Swift = 1,
};

/// Returns the harness-readable spelling of `encoder`.
///
/// - Parameter encoder: The encode that drew the instances.
/// - Returns: A stable lowercase token; never empty.
[[nodiscard]] inline const char* frameEncoderName(FrameEncoder encoder)
{
    switch (encoder) {
    case FrameEncoder::Native:
        return "native";
    case FrameEncoder::Swift:
        return "swift";
    }
    return "unknown";
}

/// Returns the harness-readable spelling of `compositor`.
///
/// - Parameter compositor: The implementation that drew the instances.
/// - Returns: A stable lowercase token; never empty.
[[nodiscard]] inline const char* frameCompositorName(FrameCompositor compositor)
{
    switch (compositor) {
    case FrameCompositor::Software:
        return "software";
    case FrameCompositor::Metal:
        return "metal";
    case FrameCompositor::Vulkan:
        return "vulkan";
    case FrameCompositor::Canvas:
        return "canvas";
    case FrameCompositor::WebGL:
        return "webgl";
    }
    return "unknown";
}

/// Returns the one line a snapshot writes to name the path it rendered through.
///
/// Emitted on stdout beside the "wrote" line rather than into the PNG, so a
/// harness reads it without decoding an image and a frontend that forgets to
/// emit it is distinguishable from one that emits the wrong path. Carries no
/// trailing newline.
///
/// The encoder axis takes no default. A frontend that carries two encodes and
/// forgets to name one would emit a marker a harness accepts and cannot
/// attribute, which is the state this axis exists to end.
///
/// - Parameter builder: The implementation that built the cell instances.
/// - Parameter compositor: The implementation that drew them.
/// - Parameter encoder: The encode that drove the compositor.
/// - Returns: A line of the form
///   `ubiquitty-render-path: builder=… compositor=… encoder=…`.
[[nodiscard]] inline std::string renderPathMarkerLine(CellBuilder builder, FrameCompositor compositor, FrameEncoder encoder)
{
    return std::string("ubiquitty-render-path: builder=") + cellBuilderName(builder)
        + " compositor=" + frameCompositorName(compositor)
        + " encoder=" + frameEncoderName(encoder);
}

} // namespace Ubiquitty::Rendering
