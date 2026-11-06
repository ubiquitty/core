// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Whether the terminal's foreground process group is the shell's own work or a
// job the user started. Pure logic over facts the platform reads off the group's
// members; every reader of "is the shell in the foreground" — the close
// confirmation, the running-job marker, the at-prompt status, the resize reflow —
// takes its answer from here through `PAL::ProcessFacts::foregroundIsShell`.
//
// A shell hands the terminal to more than the user's jobs. Prompt hooks written
// as `( … & )` fork a subshell, and an interactive shell makes every foreground
// subshell a job in its own process group, so for as long as it lives the group
// is not the shell's. That instant follows every command and every Ctrl-C, when
// closing is most likely. Its members carry the distinction:
//
// - A fork that has not exec'd runs the shell's own code, as the shell does at
//   its prompt. A loop the shell runs in a subshell is no more a job than the
//   same loop run in the shell itself.
// - A program the shell exec'd as its own child is a job: `sleep`, `vim`, `ssh`,
//   a nested shell, and every element of a pipeline.
// - A program a subshell started is a job too, unless it ignores SIGINT. A shell
//   without job control, as inside a subshell, starts `cmd &` that way, so the
//   program a hook backgrounds is told apart from one a subshell waits for,
//   such as the `sleep` in `( sleep 30; true )`.
// - The shell makes a group for each job it starts, led by its own fork. A
//   group led by a fork another program made is that program's work: a nested
//   shell's subshell or prompt hook runs the nested shell's code, and the
//   nested shell is a job.
//
// Facts rather than time: a rule that waited for the group to persist would
// still read a slow hook as a job, and would answer late for a real one.
//
// Who holds the terminal is published beside the foreground's name, and a
// program's catalog key is decided by the one and named by the other. The rule
// that makes a group a job also names it (`foregroundOwner`): by its leader
// when the leader makes it one, and otherwise by the oldest member that does,
// so programs that come and go inside a job do not rename it whatever order
// the platform lists the group in. The shell's own work is named by the shell.
// The platform reads the holder and the name from one reading of the owner
// (`PAL::ProcessFacts::foregroundHolder`). A member is named a moment after
// the owner is read, and the foreground can move in that moment, so a reading
// is settled only when the reading after it finds the same group, holder and
// name (`settleForegroundReading`). The frontend's sampler and the host's
// probe both read the foreground that way.

#include "PAL/Process.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty::Session {

/// Whether one member of the foreground group makes the group a job.
///
/// - Parameters:
///   - member: The member's facts.
///   - shellProcessID: The session's shell, past any `login` wrapper.
/// - Returns: Whether the member is a program running on the user's behalf.
[[nodiscard]] constexpr bool isJobProcess(const PAL::Process::GroupMember& member, std::uint32_t shellProcessID)
{
    if (member.isZombie || !member.hasExecuted)
        return false;
    if (member.parentProcessID == shellProcessID)
        return true;
    return !member.isInterruptIgnored;
}

/// Whether a group's leader makes its group a job by itself.
///
/// A leader that is a job does. So does a live fork another program made: the
/// shell gives the terminal only to groups its own forks lead, so a group led
/// by another program's fork runs that program's code.
///
/// - Parameters:
///   - leader: The facts of the member whose process ID is the group's.
///   - shellProcessID: The session's shell, past any `login` wrapper.
/// - Returns: Whether the leader makes its group a job.
[[nodiscard]] constexpr bool isJobLeader(const PAL::Process::GroupMember& leader, std::uint32_t shellProcessID)
{
    if (leader.isZombie)
        return false;
    return leader.hasExecuted ? isJobProcess(leader, shellProcessID) : leader.parentProcessID != shellProcessID;
}

/// Who owns the terminal's foreground group, and the member that names it.
struct ForegroundOwner {
    /// Whether the group is a job rather than the shell's own work.
    bool isJob = false;
    /// The member a job is named by; zero for the shell's own work, and for a
    /// job none of the members read can name.
    std::uint32_t namingProcessID = 0;
};

/// The oldest of the members a predicate admits, the lower process ID first
/// among members started at once.
///
/// - Parameters:
///   - members: The members read.
///   - isAdmitted: Which members count.
/// - Returns: The member, or nullptr when none is admitted.
template<typename Predicate>
[[nodiscard]] constexpr const PAL::Process::GroupMember* oldestMember(std::span<const PAL::Process::GroupMember> members, Predicate&& isAdmitted)
{
    const PAL::Process::GroupMember* oldest = nullptr;
    for (const PAL::Process::GroupMember& member : members) {
        if (!isAdmitted(member))
            continue;
        if (!oldest || member.startTime < oldest->startTime || (member.startTime == oldest->startTime && member.processID < oldest->processID))
            oldest = &member;
    }
    return oldest;
}

/// Who owns a foreground group, from the members read of it.
///
/// A leader that makes the group a job names it. Otherwise a member that is a
/// job makes the group one, and the oldest such member names it, the one that
/// has held the group longest. A group that could not be read in full is a job
/// whatever the members read are, since a close that asks is recoverable and
/// one that kills a job is not; with no job among them, the oldest that runs a
/// program names it.
///
/// - Parameters:
///   - members: The members read.
///   - group: The group, whose leader is the member with its process ID.
///   - isWholeGroup: Whether `members` is every member of the group.
///   - shellProcessID: The session's shell, past any `login` wrapper.
/// - Returns: The owner.
[[nodiscard]] constexpr ForegroundOwner foregroundOwner(std::span<const PAL::Process::GroupMember> members, std::uint32_t group, bool isWholeGroup, std::uint32_t shellProcessID)
{
    const auto leader = std::ranges::find(members, group, &PAL::Process::GroupMember::processID);
    if (leader != members.end() && isJobLeader(*leader, shellProcessID))
        return { .isJob = true, .namingProcessID = leader->processID };
    if (const auto* job = oldestMember(members, [&](const PAL::Process::GroupMember& member) { return isJobProcess(member, shellProcessID); }))
        return { .isJob = true, .namingProcessID = job->processID };
    if (isWholeGroup)
        return {};
    const auto* program = oldestMember(members, [](const PAL::Process::GroupMember& member) { return member.hasExecuted && !member.isZombie; });
    return { .isJob = true, .namingProcessID = program ? program->processID : 0 };
}

/// Whether the foreground group is the shell's own work.
///
/// A group with no live member reads as the shell's: its job has ended, and the
/// shell is taking the terminal back.
///
/// - Parameters:
///   - members: Every member of the group.
///   - group: The group, whose leader is the member with its process ID.
///   - shellProcessID: The session's shell, past any `login` wrapper.
/// - Returns: Whether the group is no job.
[[nodiscard]] constexpr bool isShellOwnedForegroundGroup(std::span<const PAL::Process::GroupMember> members, std::uint32_t group, std::uint32_t shellProcessID)
{
    return !foregroundOwner(members, group, true, shellProcessID).isJob;
}

/// One reading of who holds a session's terminal and what holds it is named.
struct ForegroundReading {
    /// The foreground's name.
    std::string name;
    /// The foreground process group; zero when none resolves.
    PAL::ProcessID processGroup = 0;
    /// Whether the shell holds the terminal (`PAL::ProcessFacts::foregroundIsShell`).
    bool isShell = true;
    /// Whether the reading after this one found the same group, holder and name.
    bool isSettled = false;

    /// Whether two readings found the same foreground.
    ///
    /// - Parameter other: The other reading.
    /// - Returns: Whether the group, the holder and the name are the same.
    [[nodiscard]] bool findsTheSameForeground(const ForegroundReading& other) const
    {
        return processGroup == other.processGroup && isShell == other.isShell && name == other.name;
    }
};

/// How many times a reading that moved is read again before it is returned
/// unsettled.
inline constexpr int maxForegroundRereads = 3;

/// Settles a reading against the readings after it.
///
/// Each reading after it reads the group, then the holder and the name from one
/// reading of the owner (`readHolder`). A reading the next one finds the same,
/// name and all, is settled. One that moved is replaced by the next and read
/// again, at most `maxForegroundRereads` times, and one still moving is
/// returned unsettled. A reading with no name is never settled, and is not
/// read again once it stands still: a foreground that has not exec'd yet has
/// none to give. A settled reading is the foreground as it stood when it was
/// read; a foreground that moves after that is the next sample's to read.
///
/// - Parameters:
///   - process: The session's process facts.
///   - reading: The first reading. One already settled is returned as it is.
///   - readHolder: Reads who holds the terminal and its name, as
///     `PAL::ProcessFacts::foregroundHolder` does.
/// - Returns: The reading.
template<typename ReadHolder>
[[nodiscard]] ForegroundReading settleForegroundReading(PAL::ProcessFacts& process, ForegroundReading reading, ReadHolder&& readHolder)
{
    for (int reread = 0; !reading.isSettled && reread < maxForegroundRereads; ++reread) {
        ForegroundReading next { .name = {}, .processGroup = process.foregroundProcessGroup().value_or(0), .isShell = true, .isSettled = false };
        PAL::ForegroundHolder holder = readHolder();
        next.isShell = holder.isShell;
        next.name = std::move(holder.name);
        const bool isStill = next.findsTheSameForeground(reading);
        reading = std::move(next);
        if (isStill) {
            reading.isSettled = !reading.name.empty();
            break;
        }
    }
    return reading;
}

/// Holds the name last published for a reading that names nothing.
///
/// A name that did not read cannot say what the program is called: a job read
/// with no name would publish "", and a reader resolving the empty name to the
/// session's launch name would key a program icon on a name the program does
/// not have. The published name stands until a reading that has a name replaces
/// it. The group and the holder stay the reading's own: they were read, and a
/// job that took the terminal is a job whatever its name reads, so the close
/// confirmation, the running-job marker and the size re-send see the takeover
/// at once. Nothing published yet leaves the reading as it is.
///
/// - Parameters:
///   - reading: The reading settled for this sample.
///   - publishedName: The name last published; empty when none has been.
inline void holdUnnamedReading(ForegroundReading& reading, const std::string& publishedName)
{
    if (!reading.name.empty() || publishedName.empty())
        return;
    reading.name = publishedName;
    reading.isSettled = false;
}

} // namespace Ubiquitty::Session
