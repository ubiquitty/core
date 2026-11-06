// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Session/BackgroundEvents.h"
#include "PAL/Process.h"

#include <algorithm>
#include <span>
#include <utility>

namespace Ubiquitty::Protocol::Control {

/// Serializes a PTY's process walk, bounded below one Sync result frame.
///
/// The caller gathers at most 128 records. A full buffer is conservatively
/// incomplete; unavailable process facts are distinct from an empty tree.
/// - Parameters:
///   - pane: The connection-local pane handle already admitted by the host.
///   - records: The gathered PTY subtree, including background jobs.
///   - isAvailable: Whether the platform supplied a process walk.
/// - Returns: Process facts and explicit availability/completeness flags.
inline Codec::JSON::Value processSnapshot(std::uint32_t pane, std::span<const PAL::Process::Record> records,
    bool isAvailable) // bool-ok: the platform's process-walk availability is data
{
    using Codec::JSON::Value;
    Value result = Value::object();
    Value processes = Value::array();
    bool isComplete = isAvailable && records.size() < 128;
    std::size_t remaining = 48000;
    for (const auto& process : records.first(std::min<std::size_t>(records.size(), 128))) {
        Value row = Value::object();
        row.set("pid", Value::number(process.processID));
        row.set("parentPID", Value::number(process.parentProcessID));
        row.set("groupID", Value::number(process.processGroupID));
        row.set("foreground", Value::boolean(process.isForeground));
        row.set("leader", Value::boolean(process.isSessionLeader));
        const auto name = PAL::Process::scriptDisplayName(process.name, process.commandLine);
        row.set("name", Value::string(Session::backgroundEventText(name, 256)));
        row.set("executable", Value::string(Session::backgroundEventText(process.name, 256)));
        row.set("command", Value::string(Session::backgroundEventText(process.commandLine, 2048)));
        row.set("user", Value::string(Session::backgroundEventText(process.user, 128)));
        row.set("state", Value::string(process.state == PAL::Process::State::Running ? "running" : process.state == PAL::Process::State::Sleeping ? "sleeping"
                                 : process.state == PAL::Process::State::Stopped                                                                  ? "stopped"
                                 : process.state == PAL::Process::State::Zombie                                                                   ? "zombie"
                                                                                                                                                  : "unknown"));
        isComplete = isComplete && name.size() <= 256 && process.name.size() <= 256
            && process.commandLine.size() <= 2048 && process.user.size() <= 128;
        const auto size = Codec::JSON::serialize(row).size() + 1;
        if (size > remaining) {
            isComplete = false;
            break;
        }
        remaining -= size;
        processes.push(std::move(row));
    }
    result.set("pane", Value::number(pane));
    result.set("available", Value::boolean(isAvailable));
    result.set("complete", Value::boolean(isComplete));
    result.set("processes", std::move(processes));
    return result;
}

} // namespace Ubiquitty::Protocol::Control
