// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The portable composable-surface model. A Surface is one rendered pane the
// frontend composes into windows/tabs/Workspaces. The SurfaceRegistry is the
// origin-agnostic source of truth for live surfaces and each one's Origin +
// Health + title; the frontend reads these to render a badge and arrange
// surfaces, never branching on origin. Single-threaded by contract (UI thread);
// owns no transport, never touches a Terminal.

#include "Core/Session/Health.h"
#include "Core/Transport/Origin.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

// A stable, never-reused handle for a surface. 0 is "none". (A different
// ID space from Session::Layout::SurfaceID, the u32 layout-tree handle — see the
// note in Session/LayoutTree.h.)
using SurfaceID = std::uint64_t;
inline constexpr SurfaceID invalidSurface = 0;

// One composable surface's portable metadata (the Terminal/session lives in the
// frontend, keyed by id).
struct Surface {
    SurfaceID id = invalidSurface;
    Transport::Origin origin; // where-from (static); rendered as a badge, never branched on
    Health health = Health::Connected; // live link state (dynamic)
    std::string title;
};

class SurfaceRegistry {
public:
    SurfaceRegistry() = default;

    /// Registers a new surface and returns its id.
    ///
    /// Ids are monotonic and never reused, so a stale id names nothing rather
    /// than a later surface.
    ///
    /// - Parameters:
    ///   - origin: Where the surface's bytes come from; rendered as a badge.
    ///   - title: The initial title, empty when the surface has none yet.
    /// - Returns: The new surface's stable id.
    SurfaceID create(const Transport::Origin& origin, std::string title = {});

    // Drop a surface; true if it existed.
    bool remove(SurfaceID);

    // Non-owning lookup; nullptr if gone. Stable across other surfaces'
    // create/remove (map storage) — invalidated only by removing this id.
    [[nodiscard]] const Surface* find(SurfaceID) const;

    // Update a surface's live state; no-op returning false if the id is gone.
    bool setHealth(SurfaceID, Health);
    bool setTitle(SurfaceID, std::string title);

    // All live surfaces, in ascending id order (creation order). Pointers obey
    // find()'s lifetime rule.
    [[nodiscard]] std::vector<const Surface*> all() const;
    [[nodiscard]] std::size_t size() const { return m_surfaces.size(); }

private:
    Surface* findMutable(SurfaceID);

    SurfaceID m_next = 1; // next id to hand out (0 reserved for "none")
    std::map<SurfaceID, Surface> m_surfaces; // ordered by id; stable element addresses
};

} // namespace Ubiquitty::Session
