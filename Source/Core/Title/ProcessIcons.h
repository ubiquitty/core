// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/OSC/Activity.h"
#include "Core/Title/ProcessIconCatalog.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Title {

/// Whether a process or app name is a catalog name, ASCII case aside, and
/// with a Windows executable's ".exe" left off.
///
/// - Parameters:
///   - name: The name a pane reports.
///   - catalogName: A catalog name, lowercase.
/// - Returns: Whether they name the same program.
[[nodiscard]] constexpr bool isProcessName(std::string_view name, std::string_view catalogName)
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (name.size() == catalogName.size() + 4) {
        const std::string_view suffix = name.substr(catalogName.size());
        if (lower(suffix[0]) == '.' && lower(suffix[1]) == 'e' && lower(suffix[2]) == 'x' && lower(suffix[3]) == 'e') {
            name = name.substr(0, catalogName.size());
        }
    }
    if (name.size() != catalogName.size() || name.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (lower(name[i]) != catalogName[i]) {
            return false;
        }
    }
    return true;
}

/// The program a name answers to.
///
/// - Parameter name: An OSC 7501 app or a process name.
/// - Returns: Its catalog entry, or nullptr for a name the catalog does not hold.
[[nodiscard]] constexpr const ProcessIcon* processIconNamed(std::string_view name)
{
    for (const auto& icon : processIconCatalog) {
        for (std::size_t i = 0; i < icon.nameCount; ++i) {
            if (isProcessName(name, icon.names[i])) {
                return &icon;
            }
        }
    }
    return nullptr;
}

/// The program a catalog key names.
///
/// - Parameter key: A catalog key, as ``processIconKeyFor`` gives it.
/// - Returns: Its entry, or nullptr for an unknown key.
[[nodiscard]] constexpr const ProcessIcon* processIconWithKey(std::string_view key)
{
    for (const auto& icon : processIconCatalog) {
        if (icon.key == key) {
            return &icon;
        }
    }
    return nullptr;
}

/// The position a snapshot publishes an icon at: its catalog index plus one.
///
/// - Parameter icon: A catalog entry, or nullptr.
/// - Returns: The ordinal, zero for none.
[[nodiscard]] constexpr std::uint8_t processIconOrdinal(const ProcessIcon* icon)
{
    return icon ? static_cast<std::uint8_t>(icon - processIconCatalog.data() + 1) : 0;
}

/// The icon a published ordinal names.
///
/// - Parameter ordinal: An ``processIconOrdinal``.
/// - Returns: Its entry, or nullptr for zero or a position past the catalog.
[[nodiscard]] constexpr const ProcessIcon* processIconAt(std::uint8_t ordinal)
{
    return ordinal > 0 && ordinal <= processIconCatalog.size() ? &processIconCatalog[ordinal - 1] : nullptr;
}

/// The program a pane's most urgent status record names by its app.
///
/// - Parameter store: The pane's OSC 7501 records.
/// - Returns: The program's entry, or nullptr for no record or an app the catalog does not hold.
[[nodiscard]] inline const ProcessIcon* processIconReported(const OSC::ProgramStatusStore& store)
{
    const auto* record = store.primary();
    return record ? processIconNamed(store.effectiveApp(*record)) : nullptr;
}

/// The program a pane runs: the one its status record names, and failing
/// that the one its foreground process is. Claude Code reports itself as
/// `claude-code` and runs as `claude`, so either finds it. A done, failed or
/// idle record outlives its program, so once the shell holds the terminal
/// again the record names no one and the shell's own name decides.
///
/// - Parameters:
///   - reported: The program the pane's status record names, or nullptr.
///   - foregroundName: The pane's foreground process name.
///   - holder: Who holds the pane's terminal.
/// - Returns: The program's entry, or nullptr for a pane running none.
[[nodiscard]] constexpr const ProcessIcon* processIconFor(const ProcessIcon* reported, std::string_view foregroundName, TerminalHolder holder)
{
    return reported && holder == TerminalHolder::Program ? reported : processIconNamed(foregroundName);
}

/// The catalog key of the program a pane runs; see ``processIconFor``.
///
/// - Parameters:
///   - reportedOrdinal: The snapshot's `reportedProcessIcon`.
///   - foregroundName: The pane's foreground process name.
///   - holder: Who holds the pane's terminal.
/// - Returns: The key, empty for a pane running none.
[[nodiscard]] constexpr std::string_view processIconKeyFor(std::uint8_t reportedOrdinal, std::string_view foregroundName, TerminalHolder holder)
{
    const ProcessIcon* icon = processIconFor(processIconAt(reportedOrdinal), foregroundName, holder);
    return icon ? std::string_view(icon->key) : std::string_view();
}

/// ``processIconFor`` from a pane's records.
///
/// - Parameters:
///   - store: The pane's OSC 7501 records.
///   - foregroundName: The pane's foreground process name.
///   - holder: Who holds the pane's terminal.
/// - Returns: The program's entry, or nullptr for a pane running none.
[[nodiscard]] inline const ProcessIcon* processIconFor(const OSC::ProgramStatusStore& store, std::string_view foregroundName, TerminalHolder holder)
{
    return processIconFor(processIconReported(store), foregroundName, holder);
}

/// The catalog key a published ordinal names, for a frontend walking the
/// catalog without borrowing its entries.
///
/// - Parameter ordinal: An ``processIconOrdinal``.
/// - Returns: The key, empty for zero or a position past the catalog.
[[nodiscard]] inline std::string processIconKeyAt(std::uint8_t ordinal)
{
    const auto* icon = processIconAt(ordinal);
    return icon ? std::string(icon->key) : std::string();
}

/// The bundled file a catalog key's icon is in, for a frontend that holds a key.
///
/// - Parameter key: A catalog key.
/// - Returns: The file name, empty for an unknown key.
[[nodiscard]] inline std::string processIconFile(const std::string& key)
{
    const auto* icon = processIconWithKey(key);
    return icon ? std::string(icon->file) : std::string();
}

/// The program's name as its owner writes it, for a tooltip or a spoken label.
///
/// - Parameter key: A catalog key.
/// - Returns: The name, empty for an unknown key.
[[nodiscard]] inline std::string processIconTitle(const std::string& key)
{
    const auto* icon = processIconWithKey(key);
    return icon ? std::string(icon->title) : std::string();
}

/// Whether a catalog key's icon is painted in the title's ink.
///
/// - Parameter key: A catalog key.
/// - Returns: Whether it is a template; true for an unknown key, which draws nothing.
[[nodiscard]] inline bool isProcessIconTemplate(const std::string& key)
{
    const auto* icon = processIconWithKey(key);
    return !icon || icon->ink == ProcessIconInk::Template;
}

} // namespace Ubiquitty::Title
