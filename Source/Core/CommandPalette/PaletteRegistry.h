// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// PaletteRegistry.h — the public registration seam a subsystem uses to
// contribute commands to the palette. A source implements PaletteProvider (or
// wraps a precomputed list in StaticPaletteProvider) and is added to a
// PaletteRegistry; the registry merges every provider's commands into one list,
// deduped by id with earlier providers winning a collision. A command with any
// reach is still a small local provider, not an edit to the engine — this is
// the stable API a frontend or a kit registers against.
//

#include "Core/CommandPalette/PaletteCommand.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ubiquitty::CommandPalette {

// The seam every command source implements. `commands()` returns this source's
// contribution; the registry owns cross-source ordering + dedup.
class PaletteProvider {
public:
    virtual ~PaletteProvider() = default;
    [[nodiscard]] virtual std::vector<PaletteCommand> commands() const = 0;
};

// A provider backed by a fixed, precomputed list — the adapter a frontend uses
// when it built the commands itself (e.g. a platform menu walk) yet still wants
// them merged + deduped through the registry.
class StaticPaletteProvider final : public PaletteProvider {
public:
    explicit StaticPaletteProvider(std::vector<PaletteCommand> commands)
        : commands_(std::move(commands))
    {
    }

    [[nodiscard]] std::vector<PaletteCommand> commands() const override { return commands_; }

private:
    std::vector<PaletteCommand> commands_;
};

// Aggregates providers into one deduped command list. Earlier providers win a
// collision on id, so a menu's own command shadows any duplicate from a later
// source.
class PaletteRegistry {
public:
    void addProvider(std::shared_ptr<PaletteProvider> provider)
    {
        if (provider)
            providers_.push_back(std::move(provider));
    }

    // Convenience: register a precomputed command list as a provider.
    void addCommands(std::vector<PaletteCommand> commands) { addProvider(std::make_shared<StaticPaletteProvider>(std::move(commands))); }

    // The merged, deduped command list in provider then intrinsic order.
    [[nodiscard]] std::vector<PaletteCommand> commands() const
    {
        std::vector<PaletteCommand> out;
        std::unordered_set<std::string> seen;
        for (const std::shared_ptr<PaletteProvider>& provider : providers_) {
            for (PaletteCommand& command : mutableCommands(provider)) {
                if (seen.insert(command.id).second)
                    out.push_back(std::move(command));
            }
        }
        return out;
    }

private:
    // Snapshot a provider's contribution into an owned, movable buffer so the
    // merge can steal the survivors' storage.
    static std::vector<PaletteCommand> mutableCommands(const std::shared_ptr<PaletteProvider>& provider) { return provider->commands(); }

    std::vector<std::shared_ptr<PaletteProvider>> providers_;
};

} // namespace Ubiquitty::CommandPalette
