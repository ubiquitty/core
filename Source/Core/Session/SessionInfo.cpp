// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/SessionInfo.cpp — see SessionInfo.h.

#include "Core/Session/SessionInfo.h"

#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <span>
#include <utility>

namespace Ubiquitty::Session {

// Everything but the subtree. Split out so a caller that already holds a walk
// can fill the rest without provoking a second one.
static void gatherSessionInfoExceptProcesses(const struct Ubiquitty::Terminal& term, Ubiquitty::PAL::Stream& stream, SessionInfo& out)
{
    out.columns = term.columns;
    out.rows = term.rows;
    out.canConfigureAnswerback = !term.mirrorClient;
    out.answerbackDisplay = out.canConfigureAnswerback ? answerbackForDisplay(term) : std::string_view();

    // Default background: Core's 16-bit palette is the authority (the renderer
    // mirrors it). Round each channel down to the 8-bit value the swatch needs.
    const Ubiquitty::PaletteRGB& background = term.palette.defaultBackground;
    out.backgroundRed = Ubiquitty::channel16To8(background.red);
    out.backgroundGreen = Ubiquitty::channel16To8(background.green);
    out.backgroundBlue = Ubiquitty::channel16To8(background.blue);

    Ubiquitty::PAL::ProcessFacts& facts = stream.process();
    out.directory = facts.foregroundCurrentDirectory();
    out.tty = facts.ttyName();

    // command / shell are not portably reachable through PAL::Stream (the launch
    // command line lives in the frontend's window controller); leave them empty
    // for the frontend to fill.
    out.command.clear();
    out.shell.clear();
}

void gatherSessionInfo(const struct Ubiquitty::Terminal& term, Ubiquitty::PAL::Stream& stream, SessionInfo& out)
{
    gatherSessionInfoExceptProcesses(term, stream, out);
    std::array<PAL::Process::Record, maxProcesses> scratch;
    const int processCount = stream.process().gatherProcesses(std::span<PAL::Process::Record>(scratch));
    adoptProcesses(out, std::span<const PAL::Process::Record>(scratch.data(), static_cast<std::size_t>(processCount)));
}

void gatherSessionInfo(const struct Ubiquitty::Terminal& term, Ubiquitty::PAL::Stream& stream, SessionInfo& out,
    std::span<const PAL::Process::Record> walked)
{
    gatherSessionInfoExceptProcesses(term, stream, out);
    adoptProcesses(out, walked);
}

SessionInfo captureSessionInfo(const Terminal* terminal, PAL::Stream* stream,
    const std::vector<PAL::Process::Record>& records)
{
    SessionInfo info;
    if (!terminal) {
        return info;
    }
    if (stream) {
        gatherSessionInfo(*terminal, *stream, info, records);
    } else {
        adoptProcesses(info, records);
    }
    if (const auto* leader = PAL::Process::sessionLeader(records)) {
        info.command = leader->commandLine;
    }
    if (const auto* shell = PAL::Process::effectiveShellLeader(records)) {
        info.shell = PAL::Process::shellCommandDisplayName(*shell);
    }
    info.columns = terminal->columns;
    info.rows = terminal->rows;
    info.canConfigureAnswerback = !terminal->mirrorClient;
    info.answerbackDisplay = info.canConfigureAnswerback ? answerbackForDisplay(*terminal) : std::string_view();
    info.backgroundRed = channel16To8(terminal->palette.defaultBackground.red);
    info.backgroundGreen = channel16To8(terminal->palette.defaultBackground.green);
    info.backgroundBlue = channel16To8(terminal->palette.defaultBackground.blue);
    info.paletteGeneration = terminal->paletteEpoch;
    for (unsigned index = 0; index < 16; ++index) {
        info.colors[index] = terminal->palette.indexed[index];
    }
    info.colors[16] = terminal->palette.defaultForeground;
    info.colors[17] = terminal->palette.defaultBackground;
    info.colors[18] = terminal->palette.cursor;
    if (!terminal->currentDirectory.empty()) {
        info.directory = terminal->currentDirectory;
    }
    return info;
}

bool signalSessionProcess(PAL::Stream* stream, const std::vector<PAL::Process::Record>& records,
    uint32_t processID, int signal)
{
    if (!stream) {
        return false;
    }
    for (const auto& record : records) {
        if (record.processID == processID && record.processGroupID > 1
            && record.processGroupID <= static_cast<uint32_t>(std::numeric_limits<PAL::ProcessID>::max())) {
            return stream->process().sendSignalToProcessGroup(static_cast<PAL::ProcessID>(record.processGroupID), signal);
        }
    }
    return false;
}

void adoptProcesses(SessionInfo& out, std::span<const PAL::Process::Record> walked)
{
    std::vector<std::size_t> indices(walked.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t first, std::size_t second) {
        return walked[first].processID < walked[second].processID;
    });
    std::vector<bool> visited(walked.size());
    std::vector<PAL::Process::Record> ordered;
    ordered.reserve(walked.size());
    auto appendSubtree = [&](auto&& append, std::size_t index) -> void {
        if (visited[index]) {
            return;
        }
        visited[index] = true;
        ordered.push_back(walked[index]);
        if (!walked[index].processID) {
            return;
        }
        for (auto child : indices) {
            if (walked[child].parentProcessID == walked[index].processID) {
                append(append, child);
            }
        }
    };
    for (auto index : indices) {
        if (walked[index].isSessionLeader) {
            appendSubtree(appendSubtree, index);
        }
    }
    for (auto index : indices) {
        const auto parent = walked[index].parentProcessID;
        const bool hasParent = parent && std::ranges::any_of(walked, [parent](const auto& record) { return record.processID == parent; });
        if (!hasParent) {
            appendSubtree(appendSubtree, index);
        }
    }
    // A partial or cyclic process sample still contains useful rows.
    for (auto index : indices) {
        appendSubtree(appendSubtree, index);
    }
    out.processes = std::move(ordered);

    out.foregroundIndex = -1;
    for (int i = 0; i < static_cast<int>(out.processes.size()); ++i) {
        if (out.processes[static_cast<std::size_t>(i)].isForeground) {
            out.foregroundIndex = i;
            break;
        }
    }
}

ProcessPresentation processPresentation(const PAL::Process::Record& record)
{
    ProcessPresentation result;
    result.processID = record.processID;
    result.parentProcessID = record.parentProcessID;
    result.processGroupID = record.processGroupID;
    result.rawName = record.name;
    const bool isLoginWrapper = record.isSessionLeader && record.name == "login";
    result.name = isLoginWrapper ? "(" + record.name + ")" : record.name;
    result.arguments = record.commandLine.empty() ? record.name : record.commandLine;
    result.smartName = isLoginWrapper ? result.name : std::string(PAL::Process::scriptDisplayName(record.name, record.commandLine));
    // Showing arguments preserves the executable and the complete invocation.
    result.smartArguments = result.arguments;
    result.user = record.user;
    result.isForeground = record.isForeground;
    result.isSessionLeader = record.isSessionLeader;
    result.isZombie = record.state == PAL::Process::State::Zombie;
    const auto leader = record.isSessionLeader ? PAL::Process::SessionLeaderStatus::Leader : PAL::Process::SessionLeaderStatus::NonLeader;
    result.isSecondary = isLoginWrapper || !record.isForeground;
    if (!isLoginWrapper) {
        const auto privilege = record.user == "root" ? PAL::Process::UserPrivilege::Root : PAL::Process::UserPrivilege::NonRoot;
        result.marker = PAL::Process::marker(record.name, leader, privilege);
        if (result.marker == "•" && !record.isForeground) {
            result.marker.clear();
        }
    }
    return result;
}

} // namespace Ubiquitty::Session
