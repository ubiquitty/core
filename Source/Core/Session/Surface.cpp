// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Surface.cpp — see Surface.h.

#include "Core/Session/Surface.h"

#include <utility>

namespace Ubiquitty::Session {

SurfaceID SurfaceRegistry::create(const Transport::Origin& origin, std::string title)
{
    const SurfaceID id = m_next++;
    Surface surface;
    surface.id = id;
    surface.origin = origin;
    surface.title = std::move(title);
    m_surfaces.emplace(id, std::move(surface));
    return id;
}

bool SurfaceRegistry::remove(SurfaceID id) { return m_surfaces.erase(id) != 0; }

const Surface* SurfaceRegistry::find(SurfaceID id) const
{
    const auto it = m_surfaces.find(id);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

Surface* SurfaceRegistry::findMutable(SurfaceID id)
{
    const auto it = m_surfaces.find(id);
    return it == m_surfaces.end() ? nullptr : &it->second;
}

bool SurfaceRegistry::setHealth(SurfaceID id, Health health)
{
    Surface* surface = findMutable(id);
    if (!surface)
        return false;
    surface->health = health;
    return true;
}

bool SurfaceRegistry::setTitle(SurfaceID id, std::string title)
{
    Surface* surface = findMutable(id);
    if (!surface)
        return false;
    surface->title = std::move(title);
    return true;
}

std::vector<const Surface*> SurfaceRegistry::all() const
{
    std::vector<const Surface*> out;
    out.reserve(m_surfaces.size());
    for (const auto& entry : m_surfaces) // std::map iterates in ascending key order
        out.push_back(&entry.second);
    return out;
}

} // namespace Ubiquitty::Session
