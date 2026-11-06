// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_process_icons.cpp — pins Title::processIconFor and its plate rule: which
// program a pane runs, from its OSC 7501 record's app or its foreground
// process, and when an icon drawn in its own colours needs a plate.

#include "Core/App/ProcessIconPlate.h"
#include "Core/OSC/Activity.h"
#include "Core/Title/ProcessIcons.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

using Ubiquitty::App::processIconNeedsPlate;
using Ubiquitty::OSC::applyProgramStatus;
using Ubiquitty::OSC::ProgramStatusStore;
using Ubiquitty::Title::processIconCatalog;
using Ubiquitty::Title::processIconFor;
using Ubiquitty::Title::ProcessIconInk;
using Ubiquitty::Title::processIconNamed;
using Ubiquitty::Title::processIconWithKey;
using Ubiquitty::Title::TerminalHolder;

namespace {

std::string_view keyFor(const ProgramStatusStore& store, std::string_view foreground, TerminalHolder holder = TerminalHolder::Program)
{
    const auto* icon = processIconFor(store, foreground, holder);
    return icon ? icon->key : std::string_view {};
}

} // namespace

TEST(aProgramIsFoundByEveryNameItRunsUnder)
{
    const ProgramStatusStore none;
    CHECK(keyFor(none, "claude") == "claude-code");
    CHECK(keyFor(none, "codex") == "codex");
    CHECK(keyFor(none, "opencode") == "opencode");
    CHECK(keyFor(none, "pi") == "pi");
    CHECK(keyFor(none, "gemini") == "gemini-cli");
    CHECK(keyFor(none, "grok") == "grok-build");
    CHECK(keyFor(none, "prime-agent") == "prime-agent");
    CHECK(keyFor(none, "dcode") == "deepagents");
    CHECK(keyFor(none, "deepagents-code") == "deepagents");
    CHECK(keyFor(none, "kimi") == "kimi-code");
}

TEST(aNameMatchesWhateverItsCaseAndWindowsSuffix)
{
    CHECK(processIconNamed("Claude") == processIconWithKey("claude-code"));
    CHECK(processIconNamed("CODEX.EXE") == processIconWithKey("codex"));
    CHECK(processIconNamed("codex.exe") == processIconWithKey("codex"));
    CHECK(processIconNamed("codex.ex") == nullptr);
    CHECK(processIconNamed("codexx") == nullptr);
    CHECK(processIconNamed("cod") == nullptr);
    CHECK(processIconNamed("") == nullptr);
    CHECK(processIconNamed(".exe") == nullptr);
    CHECK(processIconNamed("zsh") == nullptr);
    CHECK(processIconNamed("-zsh") == nullptr);
}

TEST(aStatusRecordsAppNamesTheProgramBeforeItsProcessDoes)
{
    ProgramStatusStore store;
    CHECK(applyProgramStatus(store, "state=working:app=claude-code"));
    CHECK(keyFor(store, "node") == "claude-code");
    // The record outranks the process: a wrapper running a program says which.
    CHECK(keyFor(store, "codex") == "claude-code");
}

TEST(aRecordThatOutlivesItsProgramNamesNoOneOnceTheShellIsBack)
{
    ProgramStatusStore store;
    CHECK(applyProgramStatus(store, "state=done:app=claude-code"));
    CHECK(keyFor(store, "claude") == "claude-code");
    // Claude Code has exited; its done record stands, and the shell has the terminal.
    CHECK(keyFor(store, "zsh", TerminalHolder::Shell).empty());
    // A shell named like a program in the catalog is still that name, whatever the record says.
    CHECK(keyFor(store, "codex", TerminalHolder::Shell) == "codex");
}

TEST(aPublishedOrdinalKeysTheProgramAsItsRecordWould)
{
    using Ubiquitty::Title::processIconKeyFor;
    using Ubiquitty::Title::processIconOrdinal;
    const std::uint8_t codex = processIconOrdinal(processIconWithKey("codex"));
    CHECK(processIconKeyFor(codex, "node", TerminalHolder::Program) == "codex");
    CHECK(processIconKeyFor(codex, "bash", TerminalHolder::Shell).empty());
    CHECK(processIconKeyFor(codex, "claude", TerminalHolder::Shell) == "claude-code");
    CHECK(processIconKeyFor(0, "claude", TerminalHolder::Program) == "claude-code");
    CHECK(processIconKeyFor(0, "", TerminalHolder::Program).empty());
}

TEST(aRecordNamingNoCatalogedProgramLeavesItToTheProcess)
{
    ProgramStatusStore store;
    CHECK(applyProgramStatus(store, "state=working:app=build"));
    CHECK(keyFor(store, "opencode") == "opencode");
    CHECK(keyFor(store, "make").empty());
}

TEST(aChildRecordInheritsItsParentsProgram)
{
    ProgramStatusStore store;
    CHECK(applyProgramStatus(store, "id=session:state=idle:app=codex"));
    CHECK(applyProgramStatus(store, "id=session/tool:state=blocked:kind=permission"));
    CHECK(keyFor(store, "zsh") == "codex");
}

TEST(anIconRoundTripsThroughItsPublishedOrdinal)
{
    CHECK(Ubiquitty::Title::processIconOrdinal(nullptr) == 0);
    CHECK(Ubiquitty::Title::processIconAt(0) == nullptr);
    CHECK(Ubiquitty::Title::processIconAt(static_cast<std::uint8_t>(processIconCatalog.size() + 1)) == nullptr);
    for (const auto& icon : processIconCatalog) {
        CHECK(Ubiquitty::Title::processIconAt(Ubiquitty::Title::processIconOrdinal(&icon)) == &icon);
        CHECK(Ubiquitty::Title::processIconKeyAt(Ubiquitty::Title::processIconOrdinal(&icon)) == icon.key);
    }
    CHECK(Ubiquitty::Title::processIconKeyAt(0).empty());
    CHECK(Ubiquitty::Title::processIconKeyAt(static_cast<std::uint8_t>(processIconCatalog.size() + 1)).empty());
    // A published program outranks the process; none leaves it to the process.
    CHECK(Ubiquitty::Title::processIconFor(processIconWithKey("pi"), "codex", TerminalHolder::Program) == processIconWithKey("pi"));
    CHECK(Ubiquitty::Title::processIconFor(nullptr, "codex", TerminalHolder::Program) == processIconWithKey("codex"));
}

TEST(aPaneRunningNoCatalogedProgramHasNoIcon)
{
    const ProgramStatusStore none;
    CHECK(keyFor(none, "vim").empty());
    CHECK(keyFor(none, "").empty());
}

TEST(theCatalogHasOneIconPerNameAndAFileForEach)
{
    std::set<std::string_view> names;
    std::set<std::string_view> keys;
    for (const auto& icon : processIconCatalog) {
        CHECK(keys.insert(icon.key).second);
        CHECK(!icon.title.empty());
        CHECK(icon.file.starts_with(icon.key));
        CHECK(icon.file.ends_with(".svg") || icon.file.ends_with(".png"));
        CHECK(icon.nameCount >= 1 && icon.nameCount <= icon.names.size());
        for (std::size_t i = 0; i < icon.nameCount; ++i) {
            CHECK(names.insert(icon.names[i]).second);
            CHECK(processIconNamed(icon.names[i]) == &icon);
        }
    }
}

TEST(aDrawnIconTooCloseToTheGroundGetsAPlateAndNoOtherDoes)
{
    constexpr std::uint32_t lightGround = 0xF2F2F2;
    constexpr std::uint32_t darkGround = 0x1E1E1E;
    // A light blue drawn bare fades into a light title bar and stands out of a dark one.
    const Ubiquitty::Title::ProcessIcon lightBlue {
        .key = "light-blue",
        .title = "Light Blue",
        .file = "light-blue.svg",
        .ink = ProcessIconInk::Drawn,
        .inkRGB = 0x7FC8FF,
    };
    CHECK(processIconNeedsPlate(lightBlue, lightGround));
    CHECK(!processIconNeedsPlate(lightBlue, darkGround));
    // The same colour on a tile of its own needs nothing more.
    auto tiled = lightBlue;
    tiled.hasOwnPlate = true;
    CHECK(!processIconNeedsPlate(tiled, lightGround));

    const auto* claude = processIconWithKey("claude-code");
    const auto* gemini = processIconWithKey("gemini-cli");
    const auto* deepAgents = processIconWithKey("deepagents");
    const auto* codex = processIconWithKey("codex");
    CHECK(claude && gemini && deepAgents && codex);
    if (!claude || !gemini || !deepAgents || !codex) {
        return;
    }
    // Claude Code's orange reads on both, though on a light ground it
    // measures under the 3:1 WCAG asks of graphics that are not logos.
    CHECK(!processIconNeedsPlate(*claude, lightGround));
    CHECK(!processIconNeedsPlate(*claude, darkGround));
    // Gemini CLI and Deep Agents bring their tiles.
    CHECK(gemini->hasOwnPlate && deepAgents->hasOwnPlate);
    CHECK(!processIconNeedsPlate(*deepAgents, deepAgents->inkRGB));
    // The blue gradient keeps its own colour on either title-bar ground.
    CHECK(codex->ink == ProcessIconInk::Drawn);
    CHECK(!processIconNeedsPlate(*codex, 0x000000));
    CHECK(!processIconNeedsPlate(*codex, 0xFFFFFF));
    CHECK(!processIconNeedsPlate(std::string("no-such-program"), lightGround));
    CHECK(!processIconNeedsPlate(std::string("claude-code"), lightGround));
}

UBIQUITTY_TEST_MAIN()
