// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// PaletteFrecency.cpp — see PaletteFrecency.h.
//

#include "Core/CommandPalette/PaletteFrecency.h"

#include <cmath>

namespace Ubiquitty::CommandPalette {

double frecencyScore(const UsageHistory& history, std::int64_t now)
{
    double score = 0.0;
    for (std::int64_t stamp : history.timestamps) {
        const std::int64_t age = stamp > now ? 0 : now - stamp;
        score += std::exp2(-static_cast<double>(age) / frecencyHalfLifeSeconds);
    }
    return score;
}

} // namespace Ubiquitty::CommandPalette
