// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "TerminalFixture.h"

#include "Core/Rendering/StatusRow.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace UbiquittyTest {

/// Seeds every variable-sized checkpoint owner without a host or transport.
/// - Parameter terminal: A complete terminal with a nonempty grid.
/// - Returns: Whether its fallible text, cluster, and status owners were admitted.
inline bool seedCheckpointOwners(Ubiquitty::Terminal& terminal)
{
    terminal.titleUTF8 = std::string(200, 't');
    terminal.iconNameUTF8 = std::string(100, 'i');
    terminal.titleUTF8Stack[3] = std::string(150, 's');
    feed(terminal, "\x1b[31m\x1b]8;id=fixture;https://example.test/path\x1b\\linked\x1b]8;;\x1b\\\x1b[0m");
    const std::array<std::uint32_t, 3> cluster { 'a', 0x301, 0x302 };
    if (!terminal.clusterPool.appendDecoded(1, cluster))
        return false;
    const std::array<std::uint32_t, 3> run { 'r', 'u', 'n' };
    if (!terminal.runTextStore.tryAppendDecoded(9, run))
        return false;
    Ubiquitty::OSC::Context context;
    context.id = "command-1";
    context.fields[0] = "command";
    context.fields[static_cast<std::size_t>(Ubiquitty::OSC::ContextField::CommandLine)] = std::string(200, 'c');
    terminal.contexts.active.push_back(context);
    terminal.contexts.lastEnded = context;
    terminal.programStatus.records.push_back({ "job", "builder", "Building", std::string(900, 'm'), Ubiquitty::OSC::ProgramState::Working,
        Ubiquitty::OSC::ProgramBlockKind::None, 10 });
    Ubiquitty::RichText::Block block;
    block.serial = 17;
    block.width = 7;
    block.height = 2;
    block.document.text.assign(3, 'a');
    block.document.runs.push_back({ .length = 3 });
    block.visibleRects.push_back({ .width = 3, .height = 1 });
    terminal.richTextBlocks.restore(Ubiquitty::ScreenMode::Primary, std::move(block));
    terminal.statusLineType = 2;
    auto status = Ubiquitty::Rendering::ensureStatusRow(terminal);
    if (status.empty())
        return false;
    std::fill(status.begin(), status.end(), Ubiquitty::defaultBlankCell);
    status.front().codePoint = 'H';
    status.back().codePoint = 'T';
    terminal.udk = std::make_unique<Ubiquitty::VT::UserDefinedKeys>();
    terminal.udk->count = 1;
    terminal.udk->entries[0].keyCode = 11;
    terminal.udk->entries[0].length = 3;
    std::memcpy(terminal.udk->entries[0].value, "key", 3);
    return true;
}

} // namespace UbiquittyTest
