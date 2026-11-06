// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/Font/FontFallbackCache.h"
#include "Core/Rendering/Font/FontFallbackPolicy.h"
#include "Core/Rendering/Font/FontInventoryTable.h"

// Source/Core/Rendering/Font/FontRoutingSession.h — the routing caches as one
// value, and the route call as one function.
//
// FontFallbackPolicyCaches holds seven C++ references, which is the right
// shape for a C++ caller with the caches as members and an impossible one for
// a Swift caller: a struct of references cannot be assembled across the
// interop seam. This aggregate owns the seven by value at one stable address,
// and routeGlyph builds the reference struct and the stack policy inside the
// call — the seam carries a pointer and PODs, and the policy stays exactly
// the one every backend routes through.

namespace Ubiquitty::Rendering::Font {

// The named alias Swift constructs a request's code-point view through: the
// importer hands a NAMED span alias the buffer-pointer initialiser, where the
// direct spelling imports only as an opaque generic.
using CodePointSpan = std::span<const std::uint32_t>;

/// The per-renderer fallback-routing caches, as one owned value.
///
/// Capacities match the Metal arm's: the resolved cascades a session touches
/// are a handful of scripts wide, and the negative cache runs deeper because
/// a miss is retried per glyph until something records it.
struct FontRoutingSession {
    HanFallbackCache han { 64 };
    ScriptFallbackCache script { 64 };
    GenericFallbackCache generic { 16 };
    EmojiFallbackCache emoji { 64 };
    SymbolFallbackCache symbol { 64 };
    RangeMapCache rangeMap { 64 };
    NegativeRouteCache negative { 128 };

    /// Clears every cache.
    ///
    /// A font or metrics rebuild strands entries keyed on old faces; a caller
    /// that feeds a constant catalogue generation clears here instead.
    void reset()
    {
        han.clear();
        script.clear();
        generic.clear();
        emoji.clear();
        symbol.clear();
        rangeMap.clear();
        negative.clear();
    }
};

/// Routes one request through the shared fallback policy.
///
/// Takes the table rather than a `FontInventory&`: the adapter over it is
/// built inside the call, so a Swift caller holds neither a struct of C++
/// references nor a virtual-bearing C++ class — the seam carries the table,
/// the session pointer and PODs.
///
/// - Parameters:
///   - session: The caller's caches, consulted and filled.
///   - table: The catalogue's entry points; must satisfy ``isComplete``.
///   - request: What is being drawn, assembled per the policy's contract.
///   - preferences: The configured fallback families.
/// - Returns: The route, whose font handle may be invalid on a total miss.
inline GlyphRoute routeGlyph(FontRoutingSession& session, const FontInventoryTable& table,
    const FontRequest& request, const FontFallbackPreferences& preferences)
{
    TableFontInventory inventory(table);
    FontFallbackPolicy policy(inventory,
        { session.han, session.script, session.generic, session.emoji,
            session.symbol, session.rangeMap, session.negative });
    return policy.route(request, preferences);
}

} // namespace Ubiquitty::Rendering::Font
