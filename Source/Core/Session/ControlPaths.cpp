// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Session/ControlPaths.cpp — see ControlPaths.h.
//

#include "Core/Session/ControlPaths.h"

#include "PAL/PAL.h"
#include "PAL/Socket.h"

#include <cstdio>
#include <cstdlib>
#include <span>

namespace Ubiquitty::Session {

std::size_t controlDirectory(std::span<char> output)
{
    char* const outputBuffer = output.data();
    const std::size_t capacity = output.size();
    if (!outputBuffer || capacity == 0)
        return 0;
    outputBuffer[0] = '\0';
    // $UBIQUITTY_HOME overrides ~/.ubiquitty (lets tests run an isolated daemon
    // without touching the real control directory); else $HOME/.ubiquitty.
    const char* overrideHome = ::getenv("UBIQUITTY_HOME");
    if (overrideHome && overrideHome[0]) {
        int n = std::snprintf(outputBuffer, capacity, "%s", overrideHome);
        return (n > 0 && static_cast<std::size_t>(n) < capacity) ? static_cast<std::size_t>(n) : 0;
    }
    char home[1024];
    if (PAL::systemHomeDirectory(home, sizeof(home)) <= 0)
        return 0;
    int n = std::snprintf(outputBuffer, capacity, "%s/.ubiquitty", home);
    return (n > 0 && static_cast<std::size_t>(n) < capacity) ? static_cast<std::size_t>(n) : 0;
}

std::size_t controlFilePath(const char* leaf, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const std::size_t capacity = output.size();
    char directory[1024];
    if (!leaf || leaf[0] == '\0' || controlDirectory(directory) == 0) {
        if (outputBuffer && capacity)
            outputBuffer[0] = '\0';
        return 0;
    }
    int n = std::snprintf(outputBuffer, capacity, "%s/%s", directory, leaf);
    return (n > 0 && static_cast<std::size_t>(n) < capacity) ? static_cast<std::size_t>(n) : 0;
}

bool ensureControlDirectory()
{
    char directory[1024];
    if (controlDirectory(directory) == 0)
        return false;
    return PAL::ensurePrivateSocketDirectory(directory);
}

std::expected<void, std::string> prepareSocketPathForBind(const std::string& path)
{
    return PAL::prepareLocalSocketPathForBind(path);
}

} // namespace Ubiquitty::Session
