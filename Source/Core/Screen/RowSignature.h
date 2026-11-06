// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

// RowSignature.h — the live grid's rows as one line a resize census reads.
//
// A frame's cost says nothing about what it showed; the rows do. Each row
// contributes one token: a size marker painted at columns 4-10 (`36x140`, the
// resize probe's, so a frame mixing two paints reads as torn from its rows
// alone), `|` followed by every vertical divider's column (a panel border —
// mc's rows disagreeing on theirs is a tear, a body whose first divider is not
// column 0 is a wrap cascade), `.` for other text and `_` for a blank row.
// Equal neighbours run-length encode. Every arm's phase log prints the same
// line, so the census reads both with one rule.

#include "Core/Cell.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"

#include <string>

namespace Ubiquitty::Screen {

/// The live grid's rows as one line a resize census reads: one token per row
/// (a size marker, `|` and its divider columns, `.` for text, `_` for a blank
/// row), equal neighbours run-length encoded.
///
/// - Parameters:
///   - terminal: The terminal whose grid is read.
/// - Returns: The signature line every arm's phase log prints.
inline std::string rowSignature(const Ubiquitty::Terminal& terminal)
{
    std::string out;
    std::string lastToken;
    int runLength = 0;
    auto flush = [&] {
        if (runLength == 0)
            return;
        if (!out.empty())
            out.push_back(',');
        out += lastToken;
        out.push_back('*');
        out += std::to_string(runLength);
    };
    const int columnLimit = terminal.columns < 300 ? terminal.columns : 300;
    for (int row = 0; row < terminal.rows; ++row) {
        std::string marker;
        std::string dividers;
        int dividerCount = 0;
        bool isBlank = true;
        for (int column = 0; column < columnLimit; ++column) {
            const Ubiquitty::Cell* cell = visibleCell(&terminal, 0, row, column);
            if (!cell)
                continue;
            const std::uint32_t code = cell->codePoint;
            if (code != 0 && code != ' ')
                isBlank = false;
            if (column >= 4 && column < 11 && ((code >= '0' && code <= '9') || code == 'x'))
                marker.push_back(static_cast<char>(code));
            if (dividerCount < 8 && (code == 0x2502 || code == 0x2551 || code == '|')) {
                if (!dividers.empty())
                    dividers.push_back('/');
                dividers += std::to_string(column);
                ++dividerCount;
            }
        }
        std::string token;
        if (marker.size() >= 4 && marker.find('x') != std::string::npos)
            token = marker;
        else if (!dividers.empty())
            token = "|" + dividers;
        else
            token = isBlank ? "_" : ".";
        if (token == lastToken) {
            ++runLength;
        } else {
            flush();
            lastToken = token;
            runLength = 1;
        }
    }
    flush();
    return out;
}

} // namespace Ubiquitty::Screen
