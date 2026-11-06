// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_surface.cpp — the portable composable-surface model.
//
// A Surface is one rendered pane the frontend composes; the SurfaceRegistry is the
// origin-agnostic source of truth for the set of live surfaces and each one's
// Ubiquitty::Transport::Origin / Ubiquitty::Session::Health / title — what Workspaces arranges + restores, independent of
// where each surface's bytes come from. These drive the model's contract.

#include "Core/Session/Surface.h"

#include <cstdio>
#include <string>

using namespace Ubiquitty::Session;

namespace {
void check(bool ok, const char* what)
{
    CHECK_MSG(ok, what);
}

Ubiquitty::Transport::Origin local() { return { Ubiquitty::Transport::OriginKind::Local, "" }; }
Ubiquitty::Transport::Origin ssh(const char* host) { return { Ubiquitty::Transport::OriginKind::SSH, host }; }
Ubiquitty::Transport::Origin daemon(const char* host) { return { Ubiquitty::Transport::OriginKind::Daemon, host }; }

TEST(test_create_assigns_stable_increasing_ids)
{
    SurfaceRegistry registry;
    const SurfaceID a = registry.create(local(), "shell");
    const SurfaceID b = registry.create(ssh("devbox"), "ssh-tmux");
    check(a != invalidSurface && b != invalidSurface, "ids are valid");
    check(a != b && b > a, "ids are distinct + increasing");
    check(registry.size() == 2, "registry holds both");
}

TEST(test_find_returns_origin_and_title)
{
    SurfaceRegistry registry;
    const SurfaceID id = registry.create(daemon("box"), "remote");
    const Surface* s = registry.find(id);
    check(s != nullptr, "find returns the surface");
    check(s->origin.transport == Ubiquitty::Transport::OriginKind::Daemon, "origin transport preserved");
    check(s->origin.host == "box", "origin host preserved");
    check(s->origin.isRemote(), "daemon surface is remote");
    check(s->title == "remote", "title preserved");
    check(s->health == Ubiquitty::Session::Health::Connected, "default health is Connected");
    check(registry.find(invalidSurface) == nullptr, "unknown id finds nothing");
}

TEST(test_mixed_origins_coexist)
{
    SurfaceRegistry registry;
    registry.create(local());
    registry.create(ssh("a"));
    registry.create(daemon("b"));
    check(registry.size() == 3, "mixed-origin surfaces coexist in one registry");
    int remote = 0;
    for (const Surface* s : registry.all())
        if (s->origin.isRemote())
            ++remote;
    check(remote == 2, "two of the three are remote");
}

TEST(test_update_health_and_title)
{
    SurfaceRegistry registry;
    const SurfaceID id = registry.create(daemon("box"), "old");
    check(registry.setHealth(id, Ubiquitty::Session::Health::Reconnecting), "setHealth applies");
    check(registry.find(id)->health == Ubiquitty::Session::Health::Reconnecting, "health updated");
    check(registry.setTitle(id, "new"), "setTitle applies");
    check(registry.find(id)->title == "new", "title updated");
    check(!registry.setHealth(invalidSurface, Ubiquitty::Session::Health::Failed), "setHealth on unknown is a no-op");
    check(!registry.setTitle(invalidSurface, "x"), "setTitle on unknown is a no-op");
}

TEST(test_remove)
{
    SurfaceRegistry registry;
    const SurfaceID a = registry.create(local());
    const SurfaceID b = registry.create(ssh("h"));
    check(registry.remove(a), "remove returns true for a live surface");
    check(registry.find(a) == nullptr, "removed surface is gone");
    check(registry.find(b) != nullptr, "the other surface survives");
    check(registry.size() == 1, "size reflects the removal");
    check(!registry.remove(a), "removing again is false");
}

TEST(test_ids_are_not_reused)
{
    SurfaceRegistry registry;
    const SurfaceID a = registry.create(local());
    registry.remove(a);
    const SurfaceID b = registry.create(local());
    check(b != a, "a fresh surface never reuses a retired id");
}

TEST(test_all_is_ordered_by_id)
{
    SurfaceRegistry registry;
    const SurfaceID a = registry.create(local());
    const SurfaceID b = registry.create(local());
    const SurfaceID c = registry.create(local());
    registry.remove(b);
    const std::vector<const Surface*> all = registry.all();
    check(all.size() == 2, "all() excludes the removed surface");
    check(all[0]->id == a && all[1]->id == c, "all() is in ascending id order");
}

} // namespace

UBIQUITTY_TEST_MAIN()
