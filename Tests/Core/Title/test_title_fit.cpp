// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_title_fit.cpp — pins Title::fittingSteps, the order a narrow tab or
// pane label gives up its parts in: the directory it opens with first, then
// the process segments it closes with, and the program's own title last.

#include "Core/Title/Fit.h"

#include <string>

using Ubiquitty::PAL::Process::TitleSeparator;
using Ubiquitty::Title::fittingSteps;
using Ubiquitty::Title::ResolvedTitle;
using Ubiquitty::Title::TabLabelMarks;
using Ubiquitty::Title::titleScalarLength;

TEST(aFitShortensEveryPartAndRemovesNone)
{
    ResolvedTitle parts;
    parts.directory = "~/Projects/ubiquitty/Source";
    parts.appTitle = "Claude Code";
    parts.chain = "craft-bridge-warmup.py ‹ python3 ◂ claude";
    const auto steps = fittingSteps(parts, TitleSeparator::Spaced, {});
    CHECK(steps.size() >= 4);
    CHECK_STR_EQ(steps.front().title, "~/Projects/ubiquitty/Source — Claude Code — craft-bridge-warmup.py ‹ python3 ◂ claude");
    for (std::size_t i = 1; i < steps.size(); ++i)
        CHECK_MSG(titleScalarLength(steps[i].title) < titleScalarLength(steps[i - 1].title), "each step is shorter than the one before");
    for (const auto& step : steps) {
        CHECK_MSG(step.title.starts_with("~/"), "the directory stays, however short");
        CHECK_MSG(step.lead.ends_with("Source"), "as far as the folder it is in");
        CHECK_MSG(step.title.find("warmup") != std::string::npos || step.title.find("craft") != std::string::npos, "the foreground program stays, however short");
    }
    // Where it runs and what runs under it give way before the program's own title.
    std::size_t firstShortTitle = steps.size();
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].title.find("Claude Code") == std::string::npos) {
            firstShortTitle = i;
            break;
        }
    }
    CHECK(firstShortTitle > 0 && firstShortTitle < steps.size());
    const std::string& lastWhole = steps[firstShortTitle - 1].title;
    CHECK_MSG(lastWhole.starts_with("~/…/Source — Claude Code — craft"), "the title shortens only once the rest is at its shortest");
    CHECK_MSG(lastWhole.find("claude") == std::string::npos, "the chain down to the program holding the terminal, the one beside it gone first");
}

TEST(aLongTitleShortensALittleAtATime)
{
    ResolvedTitle parts;
    parts.appTitle = "OC | Troubleshooting the build with bun run dev and its watcher";
    parts.chain = "opencode";
    const auto steps = fittingSteps(parts, TitleSeparator::Spaced, {});
    std::size_t shortened = 0;
    std::size_t previous = 0;
    for (const auto& step : steps) {
        const std::size_t length = titleScalarLength(step.title);
        if (step.title.find("…") != std::string::npos) {
            ++shortened;
            if (previous > 0)
                CHECK_MSG(previous - length <= 4, "no step drops more than a few characters of the title");
        }
        previous = length;
    }
    CHECK_MSG(shortened >= 10, "the title offers many lengths to fit a room");
    CHECK(titleScalarLength(steps.back().title) >= Ubiquitty::Title::shortestTitle);
}

TEST(aProgramStatusKeepsItsPlaceThroughTheFittingWalk)
{
    ResolvedTitle parts;
    parts.directory = "~";
    parts.appTitle = "Running the build";
    parts.programStatus = "Linking";
    parts.chain = "make ◂ zsh";
    parts.dimensions = "201×48";
    const auto steps = fittingSteps(parts, TitleSeparator::Spaced, {});
    CHECK(!steps.empty());
    CHECK_STR_EQ(steps.front().title, "~ — Running the build — Linking — make ◂ zsh — 201×48");
    for (const auto& step : steps) {
        const std::size_t status = step.title.find("Linking");
        const std::size_t process = step.title.find("make");
        CHECK(status != std::string::npos && process != std::string::npos && status < process);
    }
}

TEST(theHomeDirectoryAlwaysStays)
{
    ResolvedTitle parts;
    parts.directory = "~";
    parts.appTitle = "Claude Code";
    parts.chain = "craft-bridge-warmup.py ◂ claude";
    for (const auto& step : fittingSteps(parts, TitleSeparator::Spaced, {}))
        CHECK(step.title.starts_with("~ — "));
}

TEST(aLabelWithNoTitleOfItsOwnKeepsItsProgramsName)
{
    ResolvedTitle parts;
    parts.directory = "~";
    parts.chain = "codex";
    const auto steps = fittingSteps(parts, TitleSeparator::Spaced, {});
    CHECK(steps.size() == 1);
    CHECK_STR_EQ(steps.back().title, "~ — codex");
}

TEST(theReadoutsThatNameNothingAreTheOneThingLeftOut)
{
    ResolvedTitle parts;
    parts.directory = "~";
    parts.chain = "vim";
    parts.ttyName = "ttys003";
    parts.dimensions = "80×24";
    const auto steps = fittingSteps(parts, TitleSeparator::Spaced, {});
    CHECK_STR_EQ(steps.back().title, "~ — vim");
}

TEST(everyStepKeepsTheMarksThatLeadTheLabel)
{
    ResolvedTitle parts;
    parts.directory = "~/src";
    parts.appTitle = "a long title the program gave itself";
    parts.shellCommand = "zsh";
    TabLabelMarks marks;
    marks.isPinned = true;
    const auto steps = fittingSteps(parts, TitleSeparator::Spaced, marks);
    const std::string prefix = Ubiquitty::Title::tabLabelMarkPrefix(marks);
    CHECK(!prefix.empty() && steps.size() > 1);
    for (const auto& step : steps)
        CHECK(step.title.starts_with(prefix));
}

TEST(aLabelOfNothingHasNoSteps)
{
    CHECK(fittingSteps(ResolvedTitle {}, TitleSeparator::Spaced, {}).empty());
}

UBIQUITTY_TEST_MAIN()
