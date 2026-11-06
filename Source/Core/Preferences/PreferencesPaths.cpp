// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/PreferencesPaths.h"

#include "PAL/PAL.h"

#include <array>
#include <cstdlib>
#include <format>
#include <string_view>

namespace Ubiquitty::Preferences {

namespace {

/// Joins `base` and `suffix` with exactly one slash, whatever `base` ends in.
///
/// Trailing slashes are trimmed rather than doubled, and a root of `/` keeps
/// the one slash it is: `/ubiquitty`, not `//ubiquitty` or `ubiquitty`.
std::string joined(std::string_view base, std::string_view suffix)
{
    while (base.size() > 1 && base.back() == '/')
        base.remove_suffix(1);
    if (base.ends_with('/'))
        return std::format("{}{}", base, suffix);
    return std::format("{}/{}", base, suffix);
}

} // namespace

std::optional<std::string> directoryPath()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] == '/')
        return joined(xdg, "ubiquitty");
    std::array<char, 1024> home {};
    if (PAL::systemHomeDirectory(home.data(), static_cast<int>(home.size())) <= 0)
        return std::nullopt;
    return joined(home.data(), ".config/ubiquitty");
}

std::optional<std::string> filePath()
{
    const auto directory = directoryPath();
    if (!directory)
        return std::nullopt;
    return joined(*directory, "config.toml");
}

} // namespace Ubiquitty::Preferences
