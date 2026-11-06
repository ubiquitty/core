// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Title/Format.h"
#include "Core/Title/Truncate.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Title {

/// One version of a label for a width it must fit.
struct FittingStep {
    /// The label, its marks included.
    std::string title;
    /// The directory it opens with, as shortened, for the label to dim; empty
    /// once it is left out.
    std::string lead;
};

/// The fewest characters a fit shortens a program's own title to.
inline constexpr std::size_t shortestTitle = 6;

/// A process chain's foreground program: the one holding the terminal, which
/// leads the chain whichever process stands beside it, its child ("mc ▸ vi")
/// or its parent ("vi ◂ claude").
///
/// - Parameter chain: The chain, as `PAL::Process::formatChain` joins it.
/// - Returns: The program's name, the whole chain when it has one program.
[[nodiscard]] inline std::string_view foregroundOfChain(std::string_view chain)
{
    const std::size_t arrow = std::min(chain.find(PAL::Process::chainArrow), chain.find(PAL::Process::chainParentArrow));
    return arrow == std::string_view::npos ? chain : chain.substr(0, arrow);
}

/// The versions of a tab's or pane's label a narrow one tries in turn, the
/// whole label first and each after shorter than the one before.
///
/// Every part the reader turned on stays: a fit shortens what is shown and
/// never takes a part away, so a label too narrow for its directory still
/// says where it runs. What it most needs to say is which program runs and
/// what it calls itself, so where it runs and the processes under it give
/// way first. The directory shortens from its start as far as the folder it
/// is in (`~` is already that short); the process chain to its foreground
/// program, and that program's name in its middle; and only then the
/// program's own title, in its middle. The
/// readouts that name nothing (the tty, the profile, the size) are the one
/// thing a fit leaves out. A frontend measures each and draws the first that
/// fits, and truncates the last in its middle when even that does not.
///
/// - Parameters:
///   - parts: The label's parts, as woven.
///   - separator: The spelling they are joined with.
///   - marks: The marks that lead the label.
/// - Returns: The versions, longest first; empty when the parts weave to nothing.
[[nodiscard]] inline std::vector<FittingStep> fittingSteps(const ResolvedTitle& parts, PAL::Process::TitleSeparator separator, const TabLabelMarks& marks)
{
    std::vector<FittingStep> steps;
    const auto add = [&](const ResolvedTitle& variant) {
        const std::string joined = joinTitle(variant, separator);
        if (joined.empty())
            return;
        std::string label = markTabLabel(joined, marks);
        if (!steps.empty() && titleScalarLength(label) >= titleScalarLength(steps.back().title))
            return;
        steps.push_back({ .title = std::move(label), .lead = variant.directory });
    };
    add(parts);

    ResolvedTitle shorter = parts;
    shorter.ttyName.clear();
    shorter.profileName.clear();
    shorter.dimensions.clear();
    add(shorter);

    // The directory, as far as the folder it is in: the anchor, a gap and
    // the last component ("~/…/ubiquitty").
    if (const std::size_t length = titleScalarLength(parts.directory); length > 0) {
        const std::string_view path = parts.directory;
        const std::size_t slash = path.rfind('/');
        const std::size_t folder = slash == std::string_view::npos ? length : titleScalarLength(path.substr(slash + 1));
        const std::size_t shortest = std::min(length, folder + 4);
        for (const std::size_t keep : { length * 3 / 4, length / 2, shortest }) {
            if (keep >= shortest && keep < length) {
                shorter.directory = truncatePath(parts.directory, keep);
                add(shorter);
            }
        }
    }

    // The process chain: the programs under the foreground one first, then
    // its name, in its middle.
    if (!parts.chain.empty()) {
        shorter.chain = std::string(foregroundOfChain(parts.chain));
        add(shorter);
        shorter.chain = truncateMiddle(shorter.chain, 12);
        add(shorter);
    }

    // Last, the program's own title and any base title, in their middles, a
    // little at a time, so the label keeps as much of them as its room holds.
    const std::size_t longest = std::max(titleScalarLength(parts.appTitle), titleScalarLength(parts.baseTitle));
    const std::size_t stride = std::max<std::size_t>(1, longest / 24);
    for (std::size_t keep = longest; keep > shortestTitle + stride;) {
        keep -= stride;
        shorter.appTitle = truncateMiddle(parts.appTitle, keep);
        shorter.baseTitle = truncateMiddle(parts.baseTitle, keep);
        add(shorter);
    }
    return steps;
}

} // namespace Ubiquitty::Title
