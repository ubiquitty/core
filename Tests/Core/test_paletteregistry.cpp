// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_paletteregistry.cpp — pins the public registration seam
// (Core/CommandPalette/PaletteRegistry): providers merge in order, an id
// collision is deduped with the EARLIER provider winning (a menu command
// shadows a later duplicate), and a custom PaletteProvider contributes just like
// a precomputed list. Header-only; bare libc++.

#include "Core/CommandPalette/PaletteRegistry.h"

#include <cstdio>
#include <string>
#include <vector>

using Ubiquitty::CommandPalette::PaletteCommand;
using Ubiquitty::CommandPalette::PaletteProvider;
using Ubiquitty::CommandPalette::PaletteRegistry;

namespace {
void check(bool condition, const char* what)
{
    CHECK_MSG(condition, what);
}

PaletteCommand command(const char* id, const char* title)
{
    PaletteCommand out;
    out.id = id;
    out.title = title;
    return out;
}

std::vector<std::string> titles(const std::vector<PaletteCommand>& commands)
{
    std::vector<std::string> out;
    for (const PaletteCommand& command : commands)
        out.push_back(command.title);
    return out;
}

// A custom provider (not the StaticPaletteProvider adapter), proving the
// abstract seam is what a subsystem implements.
class OneShotProvider final : public PaletteProvider {
public:
    explicit OneShotProvider(PaletteCommand command)
        : command_(std::move(command))
    {
    }
    [[nodiscard]] std::vector<PaletteCommand> commands() const override { return { command_ }; }

private:
    PaletteCommand command_;
};

} // namespace

TEST(paletteregistry)
{

    // Providers merge in registration order.
    {
        PaletteRegistry registry;
        registry.addCommands({ command("menu.new", "New Tab"), command("menu.close", "Close Tab") });
        registry.addCommands({ command("set.font", "Font Size") });
        std::vector<std::string> want { "New Tab", "Close Tab", "Font Size" };
        check(titles(registry.commands()) == want, "providers merge in order");
    }

    // An id collision dedups with the EARLIER provider winning.
    {
        PaletteRegistry registry;
        registry.addCommands({ command("shared", "Menu Wins"), command("menu.only", "Menu Only") });
        registry.addCommands({ command("shared", "Setting Loses"), command("set.only", "Set Only") });
        std::vector<PaletteCommand> merged = registry.commands();
        std::vector<std::string> want { "Menu Wins", "Menu Only", "Set Only" };
        check(titles(merged) == want, "earlier provider wins an id collision");
    }

    // A custom PaletteProvider contributes through the same seam.
    {
        PaletteRegistry registry;
        registry.addProvider(std::make_shared<OneShotProvider>(command("plug.x", "Plugin Command")));
        registry.addCommands({ command("menu.y", "Menu Command") });
        std::vector<std::string> want { "Plugin Command", "Menu Command" };
        check(titles(registry.commands()) == want, "a custom provider joins the merge");
    }
}

UBIQUITTY_TEST_MAIN()
