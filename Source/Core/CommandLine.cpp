// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// CommandLine.cpp — see Core/CommandLine.h.
//

#include "Core/CommandLine.h"

namespace Ubiquitty {

namespace {

bool isCommandSpace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}

} // namespace

std::vector<std::string> splitCommandWords(std::string_view command)
{
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < command.size()) {
        while (i < command.size() && isCommandSpace(command[i]))
            ++i;
        if (i >= command.size())
            break;
        const std::size_t start = i;
        while (i < command.size() && !isCommandSpace(command[i]))
            ++i;
        words.emplace_back(command.substr(start, i - start));
    }
    return words;
}

std::string commandProgramName(std::string_view command)
{
    const std::vector<std::string> words = splitCommandWords(command);
    return words.empty() ? std::string {} : words.front();
}

} // namespace Ubiquitty
