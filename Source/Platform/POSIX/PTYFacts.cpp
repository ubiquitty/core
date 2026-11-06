// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Platform/POSIX/PTYFacts.cpp — the PTYFacts.h process-introspection
// seam, for every Unix the products run on.
//
// Paired with the transport in PTYStream.cpp, which is this file's only caller
// and which supplies the foreground processID via tcgetpgrp and the tty via
// ptsname; this half answers "what is this processID" — the executable name, the
// first child, the live subtree a title bar samples, the cwd fallback for a
// shell that stays OSC 7-silent, and the ADOPT_SESSION ownership gate.
//
// One file with platform legs rather than a file per platform, matching
// PTYStream.cpp beside it, which resolves the same set of operating systems for
// the same seam. A directory per platform would scatter one seam across three of
// them and make each target name a different source; an #elif puts a new system's
// leg next to the code it parallels.
//
// The consequence worth stating: every product that links PTYStream.cpp links
// this, so the app, the daemon and any other embedder answer these questions
// with the same code. A per-embedder file of stubs makes process introspection
// absent by accident of which source a build lists, rather than by any
// decision. Implement a leg once
// and every product on that system gets it.
//
// isProcessAncestorOfSession is a security gate rather than a convenience:
// PTYStream refuses an ADOPT_SESSION handoff unless it passes, because a
// same-uid peer could otherwise hand over a PTY master from a session it does
// not own. A platform without a process-tree walk must fail closed, never
// relax to an unverified accept.
//
// Not in Scripts/portable-core-sources.txt — this needs a real process table
// and wasm has none. Targets list it explicitly, exactly as they list PTYStream.
//

#include "PAL/PTYFacts.h"

#if defined(__APPLE__)

#include "PAL/UnixFileDescriptor.h"

#include "Core/Session/ProcessSubtree.h"

#include <libproc.h>
#include <pwd.h>
#include <sys/event.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <vector>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::PAL {

namespace {

// Exec-path basename of `processID` into `out`. Returns the written length, or
// 0 (out emptied) when the processID can't be resolved.
std::string processBasename(ProcessID processID)
{
    if (processID <= 0)
        return {};
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int n = ::proc_pidpath(processID, path, sizeof(path));
    if (n <= 0)
        return {};
    const char* base = std::strrchr(path, '/');
    return base ? base + 1 : path;
}

struct ProcessArguments {
    std::string argument0Name;
    std::string commandLine;
};

int argumentMaxSize()
{
    static const int cached = [] {
        int maxArgumentSize = 0;
        size_t size = sizeof(maxArgumentSize);
        int argumentMaxMIB[2] = { CTL_KERN, KERN_ARGMAX };
        if (::sysctl(argumentMaxMIB, 2, &maxArgumentSize, &size, nullptr, 0) != 0 || maxArgumentSize <= 0)
            return 0;
        return std::min(maxArgumentSize, 256 * 1024);
    }();
    return cached;
}

ProcessArguments readProcessArguments(ProcessID processID, std::vector<char>& argumentBuffer)
{
    ProcessArguments arguments;
    const int maxArgumentSize = argumentMaxSize();
    if (maxArgumentSize <= 0)
        return arguments;
    if (argumentBuffer.size() < static_cast<size_t>(maxArgumentSize))
        argumentBuffer.resize(static_cast<size_t>(maxArgumentSize));
    int mibArguments[3] = { CTL_KERN, KERN_PROCARGS2, processID };
    size_t size = static_cast<size_t>(maxArgumentSize);
    if (::sysctl(mibArguments, 3, argumentBuffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
        return arguments;

    int argumentCount = 0;
    std::memcpy(&argumentCount, argumentBuffer.data(), sizeof(int));
    const char* begin = argumentBuffer.data();
    const char* end = begin + size;
    const auto basename = [](std::string_view path) {
        const std::size_t slash = path.rfind('/');
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    };
    // The exec path, then its trailing NULs, then argv[0].
    const char* p = begin + sizeof(int);
    while (p < end && *p != '\0')
        ++p;
    const std::string_view executableName = basename({ begin + sizeof(int), static_cast<std::size_t>(p - begin) - sizeof(int) });
    while (p < end && *p == '\0')
        ++p;
    if (p >= end)
        return arguments;

    // argv[0]'s basename, keeping a login shell's leading '-'. Whole when it
    // names the executable, whose name may hold a space; otherwise up to its
    // first space, as a program that rewrites its argv[0] ("sshd: user@ttys000")
    // is named by its first word.
    const char* argument0 = p;
    const char* argument0End = argument0;
    while (argument0End < end && *argument0End != '\0')
        ++argument0End;
    std::string_view name = basename({ argument0, static_cast<std::size_t>(argument0End - argument0) });
    const std::string_view bareName = name.starts_with('-') ? name.substr(1) : name;
    if (bareName != executableName)
        name = name.substr(0, name.find(' '));
    arguments.argument0Name.assign(name);

    // argv joined with single spaces.
    for (int argumentIndex = 0; argumentIndex < argumentCount && p < end; ++argumentIndex) {
        if (argumentIndex > 0)
            arguments.commandLine.push_back(' ');
        const char* argumentStart = p;
        while (p < end && *p != '\0')
            ++p;
        arguments.commandLine.append(argumentStart, static_cast<std::size_t>(p - argumentStart));
        if (p < end)
            ++p;
    }
    return arguments;
}

// Login name for `userID`, or empty on miss.
std::string userIDToName(uid_t userID)
{
    const struct passwd* entry = ::getpwuid(userID);
    return entry && entry->pw_name ? std::string(entry->pw_name) : std::string();
}

// Map the kernel's pbi_status to the portable State. SZOMB/SSTOP/SRUN/
// SIDL/SSLEEP come from <sys/proc.h>; anything else reads as Unknown.
PAL::Process::State mapBSDStatus(uint32_t status)
{
    switch (status) {
    case SZOMB:
        return PAL::Process::State::Zombie;
    case SSTOP:
        return PAL::Process::State::Stopped;
    case SRUN:
        return PAL::Process::State::Running;
    case SSLEEP:
    case SIDL:
        return PAL::Process::State::Sleeping;
    default:
        return PAL::Process::State::Unknown;
    }
}

// Fill `record.state` (and `record.user` when still empty) from PROC_PIDTBSDINFO.
// A zombie/defunct process has no readable argv, so this is the one source the
// Inspector trusts for its run state + owner uid.
void fillStateAndUser(ProcessID processID, PAL::Process::Record& record)
{
    struct proc_bsdinfo bsdInfo;
    if (::proc_pidinfo(processID, PROC_PIDTBSDINFO, 0, &bsdInfo, sizeof(bsdInfo)) < static_cast<int>(sizeof(bsdInfo)))
        return;
    record.state = mapBSDStatus(bsdInfo.pbi_status);
    if (record.user.empty())
        record.user = userIDToName(static_cast<uid_t>(bsdInfo.pbi_uid));
}

// The foreground-owner facts out of one kernel record. P_EXEC is set by exec
// and not inherited by fork; the ignored mask survives exec.
PAL::Process::GroupMember groupMemberOf(const struct kinfo_proc& info)
{
    constexpr std::uint32_t interruptMask = 1U << (SIGINT - 1);
    return {
        .processID = static_cast<std::uint32_t>(info.kp_proc.p_pid),
        .parentProcessID = static_cast<std::uint32_t>(info.kp_eproc.e_ppid),
        .hasExecuted = (info.kp_proc.p_flag & P_EXEC) != 0,
        .isInterruptIgnored = (static_cast<std::uint32_t>(info.kp_proc.p_sigignore) & interruptMask) != 0,
        .isZombie = info.kp_proc.p_stat == SZOMB,
        .startTime = static_cast<std::uint64_t>(info.kp_proc.p_starttime.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(info.kp_proc.p_starttime.tv_usec),
    };
}

} // anonymous namespace

std::string processExecutableName(ProcessID processID) { return processBasename(processID); }

// p_comm, out of the kernel's process table. exec copies it from the name of
// the file it ran, and it stays until the process is reaped. The table reads a
// process of another user, as proc_pidpath does.
std::string processCommandName(ProcessID processID)
{
    if (processID <= 0)
        return {};
    std::array mib { CTL_KERN, KERN_PROC, KERN_PROC_PID, processID };
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), &info, &size, nullptr, 0) != 0 || size < sizeof(info)
        || info.kp_proc.p_pid != processID)
        return {};
    const std::span<const char> command { info.kp_proc.p_comm };
    return { command.begin(), std::ranges::find(command, '\0') };
}

// The executable's name, or p_comm once that no longer reads.
std::string processForegroundName(ProcessID processID)
{
    std::string name = processExecutableName(processID);
    return name.empty() ? processCommandName(processID) : name;
}

std::optional<PTYProcessSnapshot> ptyProcessSnapshot(ProcessID processID)
{
    if (processID <= 1)
        return std::nullopt;
    // The kernel's process table rather than proc_pidinfo: PROC_PIDTBSDINFO
    // refuses a process of another user with EPERM, and the session leader of
    // a login shell is /usr/bin/login, which runs as root. A snapshot that
    // cannot read it cannot vouch for any login shell's terminal. The birth is
    // the same start time PROC_PIDTBSDINFO reports.
    std::array mib { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(processID) };
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), &info, &size, nullptr, 0) != 0 || size != sizeof(info)
        || info.kp_proc.p_pid != processID || info.kp_proc.p_stat == SZOMB)
        return std::nullopt;
    return PTYProcessSnapshot {
        .parentProcessID = static_cast<ProcessID>(info.kp_eproc.e_ppid),
        .processGroupID = static_cast<ProcessID>(info.kp_eproc.e_pgid),
        .birth = { static_cast<std::uint64_t>(info.kp_proc.p_starttime.tv_sec), static_cast<std::uint64_t>(info.kp_proc.p_starttime.tv_usec) },
    };
}

std::optional<PAL::Process::GroupMember> processGroupMember(ProcessID processID)
{
    if (processID <= 0)
        return std::nullopt;
    std::array mib { CTL_KERN, KERN_PROC, KERN_PROC_PID, processID };
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), &info, &size, nullptr, 0) != 0 || size < sizeof(info))
        return std::nullopt;
    return groupMemberOf(info);
}

std::optional<std::size_t> processGroupMembers(ProcessID group, std::span<PAL::Process::GroupMember> outputBuffer)
{
    if (group <= 0)
        return std::nullopt;
    std::array mib { CTL_KERN, KERN_PROC, KERN_PROC_PGRP, group };
    // One read, into a table the caller's buffer bounds. sysctl(3) fills a
    // table the group overflows as far as it holds and fails with ENOMEM, and
    // the length it reports then is not how far (Darwin reports none), so a
    // refused read keeps the table only when every entry is the group's.
    std::vector<struct kinfo_proc> infos(std::max<std::size_t>(outputBuffer.size(), 1));
    size_t size = infos.size() * sizeof(struct kinfo_proc);
    if (::sysctl(mib.data(), static_cast<u_int>(mib.size()), infos.data(), &size, nullptr, 0) == 0) {
        const std::size_t count = size / sizeof(struct kinfo_proc);
        for (std::size_t index = 0; index < count && index < outputBuffer.size(); ++index)
            outputBuffer[index] = groupMemberOf(infos[index]);
        return count;
    }
    if (errno != ENOMEM || !std::ranges::all_of(infos, [group](const struct kinfo_proc& info) { return info.kp_eproc.e_pgid == group; }))
        return std::nullopt;
    for (std::size_t index = 0; index < outputBuffer.size(); ++index)
        outputBuffer[index] = groupMemberOf(infos[index]);
    return outputBuffer.size() + 1;
}

ProcessID firstChildProcess(ProcessID parent)
{
    if (parent <= 0)
        return 0;
    pid_t kids[64];
    int bytes = ::proc_listchildpids(parent, kids, sizeof(kids));
    if (bytes <= 0)
        return 0;
    return kids[0]; // login has exactly one child, the shell
}

// kqueue/NOTE_TRACK watcher: maintains a session shell's live descendant set
// off-thread. One registration on the shell pid; NOTE_TRACK makes the kernel
// attach to each forked descendant and deliver its pid via NOTE_CHILD, while
// NOTE_EXIT clears it — so the title path reads a handful of pids instead of the
// proc_listpids(PROC_ALL_PIDS) scan that stalled new-tab / tab-switch.
class ProcessTracker {
public:
    explicit ProcessTracker(ProcessID root, ProcessExitSource exitSource)
        : m_root(root)
        , m_exitSource(exitSource)
    {
        m_kqueue.reset(::kqueue());
        if (!m_kqueue)
            return;
        struct kevent reg;
        // A user event the destructor triggers to break the blocking kevent().
        EV_SET(&reg, stopIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        ::kevent(m_kqueue.get(), &reg, 1, nullptr, 0, nullptr);
        // Asked for, not assumed. NOTE_TRACK — the note asking the kernel to
        // follow a tracked process's descendants for us — is refused with
        // ENOTSUP on some Darwin releases, and asking for it alongside the
        // others makes the WHOLE registration fail, where EXIT, FORK and EXEC
        // each register and deliver. Bundling them cost every edge.
        //
        // So the capability is probed. Where TRACK is available the kernel keeps
        // the subtree for us; where it is not, this class follows the tree
        // itself by re-descending on each edge and watching what it finds.
        m_followsDescendants = watchProcess(root, WithTrack::Yes);
        if (!m_followsDescendants && !watchProcess(root, WithTrack::No)) {
            m_kqueue.reset();
            return;
        }
        // Descendants alive before registration (none at a fresh launch), plus
        // — without TRACK — a watch on each, since nothing else will report
        // their forks. The thread isn't running yet, so no lock is needed.
        discoverLoginShell();
        seedDescendants(root, m_pids);
        if (!m_followsDescendants) {
            for (ProcessID pid : m_pids)
                watchProcess(pid, WithTrack::No);
            m_watched = m_pids;
        }
        m_thread = std::thread([this] { runLoop(); });
    }

    ~ProcessTracker()
    {
        if (m_kqueue) {
            m_stop.store(true, std::memory_order_relaxed);
            struct kevent trigger;
            EV_SET(&trigger, stopIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            ::kevent(m_kqueue.get(), &trigger, 1, nullptr, 0, nullptr);
        }
        if (m_thread.joinable())
            m_thread.join();
        m_kqueue.reset();
        // After the join no notify can be in flight — the context hand-off is safe.
        releaseNotifierContext();
    }

    // Watch pids someone else discovered. See processTrackerObserveProcesses.
    void observeProcesses(std::span<const ProcessID> processIDs)
    {
        if (!m_kqueue || m_followsDescendants)
            return;
        std::scoped_lock lock(m_mutex);
        for (ProcessID pid : processIDs) {
            if (pid > 0 && m_watched.insert(pid).second) {
                watchProcess(pid, WithTrack::No);
                m_pids.insert(pid);
            }
        }
    }

    enum class WithTrack : std::uint8_t {
        No,
        Yes
    };

    // Registers interest in one process. Returns whether the kernel accepted it.
    bool watchProcess(ProcessID pid, WithTrack track)
    {
        if (!m_kqueue || pid <= 0)
            return false;
        unsigned int notes = NOTE_EXIT | NOTE_FORK | NOTE_EXEC;
        if (pid == m_loginShell) {
            notes |= NOTE_EXITSTATUS;
        }
        if (track == WithTrack::Yes)
            notes |= NOTE_TRACK;
        struct kevent reg;
        EV_SET(&reg, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_CLEAR, notes, 0, nullptr);
        return ::kevent(m_kqueue.get(), &reg, 1, nullptr, 0, nullptr) >= 0;
    }

    // Live descendant pids (including the root) → out; returns the count written.
    int snapshot(ProcessID* outputBuffer, int max) const
    {
        std::scoped_lock lock(m_mutex);
        int i = 0;
        for (ProcessID pid : m_pids) {
            if (i >= max)
                break;
            outputBuffer[i++] = pid;
        }
        return i;
    }

    ChildExit loginShellExit() const
    {
        std::scoped_lock lock(m_mutex);
        return m_loginShellExit;
    }

private:
    void discoverLoginShell()
    {
        if (m_exitSource != ProcessExitSource::LoginShell || m_loginShell > 0) {
            return;
        }
        m_loginShell = firstChildProcess(m_root);
        if (m_loginShell > 0) {
            watchProcess(m_loginShell, WithTrack::No);
        }
    }

    static constexpr uintptr_t stopIdent = 1;

    // Recursive PROC_PPID_ONLY walk — the descendant tree of `root` right now, over
    // only each node's children, never the whole machine.
    static void seedDescendants(ProcessID root, std::set<ProcessID>& set)
    {
        set.insert(root);
        std::vector<ProcessID> stack { root };
        while (!stack.empty()) {
            ProcessID parent = stack.back();
            stack.pop_back();
            ProcessID children[256];
            int bytes = ::proc_listpids(PROC_PPID_ONLY, static_cast<uint32_t>(parent), children, static_cast<int>(sizeof(children)));
            int n = bytes > 0 ? bytes / static_cast<int>(sizeof(ProcessID)) : 0;
            for (int i = 0; i < n; ++i) {
                if (children[i] > 0 && set.insert(children[i]).second)
                    stack.push_back(children[i]);
            }
        }
    }

    void runLoop()
    {
        while (!m_stop.load(std::memory_order_relaxed)) {
            struct kevent events[64];
            int n = ::kevent(m_kqueue.get(), nullptr, 0, events, 64, nullptr);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            const bool treeEdge = std::ranges::any_of(std::span(events, static_cast<std::size_t>(n)), [](const auto& event) {
                return event.filter == EVFILT_PROC;
            });
            const bool shouldDiscover = treeEdge && notifyTreeEdge();
            {
                std::scoped_lock lock(m_mutex);
                discoverLoginShell();
                for (int i = 0; i < n; ++i) {
                    if (events[i].filter == EVFILT_USER)
                        continue; // destructor wakeup
                    const ProcessID pid = static_cast<ProcessID>(events[i].ident);
                    const uint32_t flags = events[i].fflags;
                    // NOTE_CHILD: a tracked process forked `pid`; the kernel already
                    // attached to it (so its descendants keep following). Record it.
                    if (flags & NOTE_CHILD)
                        m_pids.insert(pid);
                    // NOTE_EXIT drops the pid (also fires for the root when the shell dies).
                    if (flags & NOTE_EXIT) {
                        m_pids.erase(pid);
                        if (pid == m_loginShell && (flags & NOTE_EXITSTATUS)) {
                            const int status = static_cast<int>(events[i].data);
                            m_loginShellExit = { true, WIFSIGNALED(status),
                                WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status) };
                        }
                    }
                    // NOTE_TRACKERR: the kernel couldn't attach to a child spawning too
                    // fast — re-seed so we don't lose that subtree.
                    if ((flags & NOTE_TRACKERR) && shouldDiscover)
                        seedDescendants(m_root, m_pids);
                }
                // Without NOTE_TRACK the kernel reports only that a WATCHED
                // process forked, never which child it forked. So the tree is
                // re-descended on the edge and anything new is watched too —
                // edge-driven still, never a tick, and the descent is the same
                // targeted children-of-this-pid walk the title uses.
                if (!m_followsDescendants && shouldDiscover) {
                    seedDescendants(m_root, m_pids);
                    for (ProcessID pid : m_pids) {
                        if (m_watched.insert(pid).second)
                            watchProcess(pid, WithTrack::No);
                    }
                }
            }
        }
    }

    ProcessID m_root;
    ProcessExitSource m_exitSource;
    ProcessID m_loginShell = 0;
    ChildExit m_loginShellExit;
    UnixFileDescriptor m_kqueue;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::set<ProcessID> m_pids;
    // Whether the kernel follows descendants for us (NOTE_TRACK). When it does
    // not, `m_watched` is what this class has registered interest in itself.
    bool m_followsDescendants = false;
    std::set<ProcessID> m_watched;
    std::atomic<bool> m_stop { false };

public:
    // Registration order (context relaxed, then notify release) pairs with the
    // run loop's notify-acquire-then-context load, so a notify pointer is never
    // observed ahead of its context. Once per tracker lifetime; the context is
    // released in the destructor, after the join.
    void setChangeNotifier(bool (*notify)(void*), void* context, void (*releaseContext)(void*))
    {
        m_notifyContext.store(context, std::memory_order_relaxed);
        m_notifyContextRelease = releaseContext;
        m_notify.store(notify, std::memory_order_release);
    }
    // Whether this tracker can actually deliver edges. The constructor drops
    // the kqueue when EV_ADD is refused even without NOTE_TRACK, and then no
    // edge comes: a caller that cannot tell waits for one forever. A shell the
    // caller forked itself is accepted, so a daemon's sessions deliver.
    [[nodiscard]] bool deliversEdges() const { return static_cast<bool>(m_kqueue); }

    bool notifyTreeEdge()
    {
        if (auto notify = m_notify.load(std::memory_order_acquire)) {
            return notify(m_notifyContext.load(std::memory_order_relaxed));
        }
        return true;
    }
    void releaseNotifierContext()
    {
        if (m_notifyContextRelease)
            m_notifyContextRelease(m_notifyContext.load(std::memory_order_relaxed));
        m_notifyContextRelease = nullptr;
    }

private:
    std::atomic<bool (*)(void*)> m_notify { nullptr };
    std::atomic<void*> m_notifyContext { nullptr };
    void (*m_notifyContextRelease)(void*) = nullptr;
};

ProcessTracker* startProcessTracker(ProcessID leader, ProcessExitSource exitSource) { return leader > 0 ? new ProcessTracker(leader, exitSource) : nullptr; }

ChildExit processTrackerLoginShellExit(ProcessTracker* tracker) { return tracker ? tracker->loginShellExit() : ChildExit {}; }

void stopProcessTracker(ProcessTracker* tracker) { delete tracker; }

bool processTrackerDeliversEdges(ProcessTracker* tracker) { return tracker != nullptr && tracker->deliversEdges(); }

void processTrackerObserveProcesses(ProcessTracker* tracker, std::span<const ProcessID> processIDs)
{
    if (tracker)
        tracker->observeProcesses(processIDs);
}

void processTrackerSetChangeNotifier(ProcessTracker* tracker, bool (*notify)(void*), void* context, void (*releaseContext)(void*))
{
    if (!tracker) {
        if (releaseContext)
            releaseContext(context);
        return;
    }
    tracker->setChangeNotifier(notify, context, releaseContext);
}

// Every process in the subtree rooted at `root`, breadth-first, by asking each
// node for its children. Bounded so a pathological tree cannot make a title
// walk unbounded work.
static void collectSubtreeProcessIDs(ProcessID root, std::vector<ProcessID>& out)
{
    constexpr std::size_t subtreeCapacity = 512;
    out.clear();
    if (root <= 0)
        return;
    out.push_back(root);
    for (std::size_t head = 0; head < out.size() && out.size() < subtreeCapacity; ++head) {
        ProcessID children[128];
        const int bytes = ::proc_listpids(PROC_PPID_ONLY, static_cast<uint32_t>(out[head]), children, static_cast<int>(sizeof(children)));
        const int count = bytes > 0 ? bytes / static_cast<int>(sizeof(ProcessID)) : 0;
        for (int i = 0; i < count && out.size() < subtreeCapacity; ++i) {
            if (children[i] > 0)
                out.push_back(children[i]);
        }
    }
}

int gatherSubtree(ProcessID leader, ProcessID foregroundGroup, const std::string& tty, std::span<PAL::Process::Record> outputBuffer, ProcessTracker* tracker)
{
    if (outputBuffer.empty() || leader <= 0)
        return 0;

    // A snapshot reads current arguments even when an exec precedes tracker registration.
    std::vector<char> argumentBuffer;

    // Candidate pids: the leader's subtree, asked of the kernel on every call.
    // The tracker is a NOTIFICATION, never the source of truth. Its live set
    // lags whatever just happened — that is the nature of an edge — so using it
    // as the candidate list silently drops processes that exist right now: a
    // program's freshly spawned child is missing from the walk and the title
    // names the parent.
    //
    // The descent below is cheap enough that there is nothing to trade. Ask the
    // kernel what is there, every time; let the tracker decide only when to ask.
    std::vector<ProcessID> candidates;
    (void)tracker;
    {
        // Descend from the leader instead of enumerating the machine. One
        // PROC_PPID_ONLY call per node visited answers "who are this pid's
        // children", so the cost follows the SESSION's size rather than the
        // host's — a handful of calls against a scan of every process running.
        //
        // This is what Terminal does: it imports proc_listchildpids and no
        // whole-table enumeration at all, which is what lets it re-read often
        // enough to never be wrong instead of guessing when to bother.
        collectSubtreeProcessIDs(leader, candidates);
        if (candidates.empty())
            return 0;
    }

    // Build the FLAT raw-node list the pure classifier walks: one
    // proc_bsdshortinfo read per pid for the {pid,ppid,pgid,uid} scalars (the
    // per-hop proc_pidinfo is platform; the ancestry/foreground/leader policy
    // is Ubiquitty::Session::Process::gatherSubtree's). Keep uid alongside so the
    // post-admission user-name fill doesn't re-stat the table.
    std::vector<Ubiquitty::Session::Process::RawNode> nodes;
    nodes.reserve(candidates.size());
    for (ProcessID processID : candidates) {
        if (processID <= 0)
            continue;
        struct proc_bsdshortinfo info;
        if (::proc_pidinfo(processID, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info)) < static_cast<int>(sizeof(info))) {
            continue;
        }
        Ubiquitty::Session::Process::RawNode node;
        node.processID = static_cast<uint32_t>(processID);
        node.parentProcessID = static_cast<uint32_t>(info.pbsi_ppid);
        node.processGroupID = static_cast<uint32_t>(info.pbsi_pgid);
        node.userID = static_cast<uint32_t>(info.pbsi_uid);
        nodes.push_back(node);
    }

    // Pure policy: admit the subtree + stamp processID/ppid/pgid, isForeground,
    // isSessionLeader, tty. The string buffers come back empty for us to fill.
    int count = Ubiquitty::Session::Process::gatherSubtree(nodes.data(), static_cast<int>(nodes.size()), static_cast<uint32_t>(leader), static_cast<uint32_t>(foregroundGroup), tty, outputBuffer);

    // Fill the per-pid strings the classifier left empty. The uid comes from the
    // matching raw node (so getpwuid runs without a second table walk).
    for (int i = 0; i < count; ++i) {
        PAL::Process::Record& record = outputBuffer[i];
        const ProcessID processID = static_cast<ProcessID>(record.processID);
        // Argument 0's basename is the displayed name (preserves a login shell's
        // "-zsh" dash); fall back to the exec-path basename, then the comm name.
        const auto arguments = readProcessArguments(processID, argumentBuffer);
        record.name = arguments.argument0Name;
        if (record.name.empty()) {
            record.name = processBasename(processID);
            if (record.name.empty()) {
                struct proc_bsdshortinfo info;
                if (::proc_pidinfo(processID, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info)) >= static_cast<int>(sizeof(info)))
                    record.name = info.pbsi_comm;
            }
        }
        // Full command line for Copy Command / Show Arguments; falls back to the name.
        record.commandLine = arguments.commandLine;
        if (record.commandLine.empty())
            record.commandLine = record.name;
        uid_t userID = 0;
        for (const Ubiquitty::Session::Process::RawNode& node : nodes) {
            if (node.processID == record.processID) {
                userID = static_cast<uid_t>(node.userID);
                break;
            }
        }
        record.user = userIDToName(userID);
        // Run state (Zombie/Stopped/...) + owner fallback from the BSD info; a
        // zombie has no argv so this is the authoritative state source.
        fillStateAndUser(processID, record);
    }

    return count;
}

/// The working directory of `processID`, or an empty string when it cannot be
/// read. This is the fallback the cwd resolution uses when a shell stays
/// OSC 7-silent, so an empty result is an ordinary outcome and not an error.
std::string processCurrentDirectory(ProcessID processID)
{
    if (processID <= 0)
        return {};
    struct proc_vnodepathinfo vnodeInfo;
    int n = ::proc_pidinfo(processID, PROC_PIDVNODEPATHINFO, 0, &vnodeInfo, sizeof(vnodeInfo));
    if (n < static_cast<int>(sizeof(vnodeInfo)) || vnodeInfo.pvi_cdir.vip_path[0] == '\0')
        return {};
    return vnodeInfo.pvi_cdir.vip_path;
}

/// Whether `claimedOwner` is `sessionLeader` or reaches it through ancestry.
///
/// The ADOPT_SESSION ownership gate. A caller hands over a PTY master and names
/// the session it claims to own; without this, a same-uid peer could hand over
/// an fd from a session belonging to someone else entirely. Fails closed on any
/// lookup error, including a session leader that is not a live visible process —
/// an fd that cannot be corroborated at all is the case least worth trusting.
///
/// One full table scan, at adopt time rather than on any hot path, so there is
/// no tracker fast path here. The ancestry policy itself is the shared
/// Ubiquitty::Session::Process::processInSubtree; only resolving each process is platform work.
bool isProcessAncestorOfSession(ProcessID claimedOwner, ProcessID sessionLeader)
{
    if (claimedOwner <= 0 || sessionLeader <= 0)
        return false;
    if (claimedOwner == sessionLeader)
        return true; // trivially owns a session it is the leader of

    int total = ::proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (total <= 0)
        return false;
    int slots = total / static_cast<int>(sizeof(ProcessID)) + 16;
    std::vector<ProcessID> candidates(static_cast<size_t>(slots));
    int bytes = ::proc_listpids(PROC_ALL_PIDS, 0, candidates.data(), slots * static_cast<int>(sizeof(ProcessID)));
    candidates.resize(static_cast<size_t>(bytes > 0 ? bytes / static_cast<int>(sizeof(ProcessID)) : 0));

    std::vector<Ubiquitty::Session::Process::RawNode> nodes;
    nodes.reserve(candidates.size());
    Ubiquitty::Session::Process::RawNode sessionLeaderNode {};
    bool foundLeader = false;
    for (ProcessID processID : candidates) {
        if (processID <= 0)
            continue;
        struct proc_bsdshortinfo info;
        if (::proc_pidinfo(processID, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info)) < static_cast<int>(sizeof(info))) {
            continue;
        }
        Ubiquitty::Session::Process::RawNode node;
        node.processID = static_cast<uint32_t>(processID);
        node.parentProcessID = static_cast<uint32_t>(info.pbsi_ppid);
        node.processGroupID = static_cast<uint32_t>(info.pbsi_pgid);
        node.userID = static_cast<uint32_t>(info.pbsi_uid);
        nodes.push_back(node);
        if (processID == sessionLeader) {
            sessionLeaderNode = node;
            foundLeader = true;
        }
    }
    if (!foundLeader)
        return false;

    return Ubiquitty::Session::Process::processInSubtree(nodes.data(), static_cast<int>(nodes.size()), sessionLeaderNode, static_cast<uint32_t>(claimedOwner));
}

} // namespace Ubiquitty::PAL

#else // /proc answers the per-process reads; the tree walk is not written

#include "Core/Session/ProcessSubtree.h"
#include "Platform/POSIX/ProcStatFields.h"

#include "PAL/UnixFileDescriptor.h"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Every entry point answers from /proc: the per-process reads take one file
// each, and the tree walks /proc/<pid>/stat's PPID field through the same chain
// Ubiquitty::Session::Process::processInSubtree already classifies. Only the
// enumeration is platform work — the ancestry and subtree policies are shared
// with the Darwin arm, which enumerates differently and decides identically.

namespace Ubiquitty {
namespace PAL {

// The working directory, from readlink(/proc/<pid>/cwd).
//
// Callers tolerate an empty result: OSC 7 is the primary source and this is the
// fallback. A path that does not fit the buffer returns empty rather than its
// prefix — readlink truncates silently, and a truncated path is not a shorter
// answer but a different directory, which a caller would have no way to notice.
std::string processCurrentDirectory(ProcessID processID)
{
    if (processID <= 0)
        return {};
    std::array<char, 64> path {};
    std::snprintf(path.data(), path.size(), "/proc/%lld/cwd", static_cast<long long>(processID));
    std::array<char, PATH_MAX> resolved {};
    const ssize_t length = ::readlink(path.data(), resolved.data(), resolved.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= resolved.size())
        return {};
    return std::string(resolved.data(), static_cast<std::size_t>(length)); // readlink does not terminate
}

// The first child of `parent`, from /proc/<pid>/task/<pid>/children.
//
// The kernel keeps the list, so this asks rather than scanning the table for a
// matching PPID — one read against one walk of every process, for a question
// asked while resolving a session leader.
//
// Whose children: the thread group's, read through the group leader's own task
// entry. A thread other than the leader has its own children file listing only
// what that thread forked, which is not what a caller asking about a process
// means.
//
// Zero on any miss, which is what the header promises and what
// effectiveShellLeader reads as "no login wrapper here" before falling back to
// the child it already has. A kernel built without CONFIG_PROC_CHILDREN has no
// such file and lands on that same fallback rather than on a guess.
ProcessID firstChildProcess(ProcessID parent)
{
    if (parent <= 0)
        return 0;
    std::array<char, 96> path {};
    std::snprintf(path.data(), path.size(), "/proc/%lld/task/%lld/children",
        static_cast<long long>(parent), static_cast<long long>(parent));
    const int descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return 0;
    std::array<char, 256> children {};
    const ssize_t length = ::read(descriptor, children.data(), children.size());
    ::close(descriptor);
    if (length <= 0)
        return 0;

    // Space-separated pids. The first is taken because the caller's question is
    // about a `login` wrapper, which has exactly one child — the shell. A read
    // that catches the list mid-change can miss an entry, and for a
    // single-child parent there is no second entry to miss.
    const std::string_view list(children.data(), static_cast<std::size_t>(length));
    std::size_t index = 0;
    while (index < list.size() && list[index] == ' ')
        ++index;
    std::uint64_t first = 0;
    const std::size_t start = index;
    while (index < list.size() && list[index] >= '0' && list[index] <= '9') {
        first = first * 10 + static_cast<std::uint64_t>(list[index] - '0');
        if (first > 0x7fffffffULL)
            return 0;
        ++index;
    }
    return index == start ? 0 : static_cast<ProcessID>(first);
}

// No tracker here, and the reason is a measurement rather than a missing leg.
//
// The Darwin tracker exists to skip a scan that stalled new-tab and tab-switch.
// The walk this one would skip is short on a desktop's process table, and it
// runs only when the foreground process group moves, which
// PTYStream::foregroundProcessGroup gates behind one tcgetpgrp. That is not a
// cost a notifier is worth carrying on a change-driven path, and Linux has no
// unprivileged NOTE_TRACK equivalent to carry it with — the netlink proc
// connector wants a privilege a terminal should not hold.
//
// Every caller already handles a null tracker: gatherSubtree takes the full
// walk, which is the same path Darwin takes when its kqueue setup fails. Time
// the walk on a full process table: the fixed cost dominates a small one.
ProcessTracker* startProcessTracker(ProcessID, ProcessExitSource) { return nullptr; }

ChildExit processTrackerLoginShellExit(ProcessTracker*) { return {}; }

bool processTrackerDeliversEdges(ProcessTracker*) { return false; }

void processTrackerObserveProcesses(ProcessTracker*, std::span<const ProcessID>) { }

void processTrackerSetChangeNotifier(ProcessTracker*, bool (*)(void*), void* context, void (*releaseContext)(void*))
{
    // No tracker to own the context: hand it straight back.
    if (releaseContext)
        releaseContext(context);
}

void stopProcessTracker(ProcessTracker*) { }

namespace {

// A /proc path for one pid, formatted into its own storage.
//
// Fixed capacity and no allocation: this runs once per process per walk, and a
// pid plus a short leaf never approaches the bound. format_to_n truncates
// rather than overruns, and the result is NUL-terminated for the ::open that
// consumes it.
[[nodiscard]] std::array<char, 64> procPath(long processID, std::string_view leaf)
{
    std::array<char, 64> path {};
    const auto [end, unused] = std::format_to_n(path.data(), path.size() - 1, "/proc/{}/{}", processID, leaf);
    *end = '\0';
    return path;
}

// The kernel's one-character run state, in the portable vocabulary. An
// unrecognised character reads as Unknown rather than as a guess: a state this
// build has not met is not evidence about the process.
[[nodiscard]] PAL::Process::State processStateForCharacter(char state)
{
    switch (state) {
    case 'Z':
        return PAL::Process::State::Zombie;
    case 'T':
    case 't':
        return PAL::Process::State::Stopped;
    case 'R':
        return PAL::Process::State::Running;
    case 'S':
    case 'D': // uninterruptible sleep is still sleep to a reader
    case 'I': // an idle kernel thread, which the subtree walk will not admit
        return PAL::Process::State::Sleeping;
    default:
        return PAL::Process::State::Unknown;
    }
}

// Login name for a uid, or empty on miss.
[[nodiscard]] std::string userNameForID(std::uint32_t userID)
{
    const struct passwd* entry = ::getpwuid(static_cast<uid_t>(userID));
    return entry && entry->pw_name ? std::string(entry->pw_name) : std::string();
}

// The full command line, from /proc/<pid>/cmdline.
//
// The file separates arguments with NUL and ends with one, which is what makes
// it exact where a `ps` line is ambiguous: an argument holding a space is still
// one argument here. Joined with single spaces to match the Darwin arm's
// KERN_PROCARGS2 walk, which produces the same shape.
//
// Read into the string's own storage and rewritten in place — the separators
// are replaced where they sit rather than the string being grown a byte at a
// time. Bounded because a title bar reads a prefix and an argv can be
// megabytes.
//
// Empty for a kernel thread, which has no argv at all — the caller falls back
// to the executable name.
[[nodiscard]] std::string processCommandLineFromProc(long processID)
{
    constexpr std::size_t commandLineLimit = 8192;
    const UnixFileDescriptor descriptor { ::open(procPath(processID, "cmdline").data(), O_RDONLY | O_CLOEXEC) };
    if (!descriptor)
        return {};
    std::string joined(commandLineLimit, '\0');
    const ssize_t length = ::read(descriptor.get(), joined.data(), joined.size());
    if (length <= 0)
        return {};
    joined.resize(static_cast<std::size_t>(length));
    std::ranges::replace(joined, '\0', ' ');
    // The final NUL became a trailing space; an argv ending in empty arguments
    // leaves more than one.
    while (!joined.empty() && joined.back() == ' ')
        joined.pop_back();
    return joined;
}

// The displayed name: argv[0]'s basename, keeping a login shell's leading dash.
//
// "-zsh" is how a login shell announces itself and the dash is the
// announcement, so it survives here exactly as it does on the Darwin arm. The
// name stops at the first space because argv[0] is the first argument, not the
// whole line.
[[nodiscard]] std::string displayNameFromCommandLine(std::string_view commandLine)
{
    const std::size_t argument0End = commandLine.find(' ');
    const std::string_view argument0 = commandLine.substr(0, argument0End);
    if (argument0.empty())
        return {};
    const std::size_t slash = argument0.rfind('/');
    return std::string(slash == std::string_view::npos ? argument0 : argument0.substr(slash + 1));
}

// One process, from /proc/<pid>/stat plus the directory's owner.
//
// Where the fields sit is parseProcStatFields' problem and is tested against a
// crafted line there — comm is the executable's own name and may hold spaces
// and parentheses. This half does the reading.
//
// The uid comes from the directory rather than /proc/<pid>/status: one stat
// against one open-and-parse, for a scalar the kernel puts in the inode.
//
// Returns a zeroed node on any miss, which the classifier skips — a process
// that exits between the readdir and this read is ordinary, not an error.
// The classifier's node plus the run state the same line already carried.
//
// Kept together because they come from one read: splitting them into parallel
// vectors would pair them by index, and an index is a claim nothing checks.
struct ProcessEntry {
    Ubiquitty::Session::Process::RawNode node;
    char state;
};

// Absent rather than zeroed on a miss. A process that exits between the readdir
// and this read is ordinary, and `nullopt` says so where a zeroed node asks
// every caller to know that zero is the sentinel.
[[nodiscard]] std::optional<ProcessEntry> readProcessEntry(long processID)
{
    // The DIRECTORY is opened, and "stat" opened relative to it, rather than
    // resolving /proc/<pid>/stat directly. Two reasons, and the first is
    // correctness:
    //
    // /proc/<pid>/stat does not carry the process's own uid. The kernel gives
    // the files under a non-dumpable process's directory to root while the
    // DIRECTORY keeps the real owner, so through the stat file every hardened
    // service and every setuid binary reads as uid 0. Measured on an ordinary
    // Fedora desktop:
    //
    //     pid 962: stat uid 0, directory uid 81     (dbus)
    //     pid 973: stat uid 0, directory uid 70     (avahi)
    //     pid 1471: stat uid 0, directory uid 1000  (a plain user process)
    //
    // Second, the directory descriptor pins one process. Resolving the path a
    // second time can land on a different process if the pid recycled in
    // between, which is ordinary on a busy machine.
    const UnixFileDescriptor directory { ::open(procPath(processID, "").data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    if (!directory)
        return std::nullopt;
    const UnixFileDescriptor descriptor { ::openat(directory.get(), "stat", O_RDONLY | O_CLOEXEC) };
    if (!descriptor)
        return std::nullopt;
    std::array<char, 512> line {};
    const ssize_t length = ::read(descriptor.get(), line.data(), line.size());
    if (length <= 0)
        return std::nullopt;

    ProcStatFields fields {};
    if (!parseProcStatFields({ line.data(), static_cast<std::size_t>(length) }, fields))
        return std::nullopt;

    // The owner comes off the DIRECTORY, for the reason given above: the stat
    // file's uid is the kernel's, not the process's, whenever the process is
    // not dumpable. This costs one more syscall per process, and buys an owner
    // that is right.
    struct stat directoryStat {};
    if (::fstat(directory.get(), &directoryStat) != 0)
        return std::nullopt;

    return ProcessEntry {
        .node = {
            .processID = static_cast<std::uint32_t>(processID),
            .parentProcessID = fields.parentProcessID,
            .processGroupID = fields.processGroupID,
            .userID = static_cast<std::uint32_t>(directoryStat.st_uid),
        },
        .state = fields.state,
    };
}

// Every process the table holds, in one walk.
//
// Both callers want the same enumeration and neither wants a second one: the
// ancestry gate reads the ids, the subtree walk reads the ids and the states.
[[nodiscard]] std::vector<ProcessEntry> enumerateProcesses()
{
    std::vector<ProcessEntry> entries;
    DIR* processes = ::opendir("/proc");
    if (!processes)
        return entries;
    entries.reserve(512); // a desktop session sits well under this; a miss costs one growth
    while (const dirent* entry = ::readdir(processes)) {
        // /proc holds named entries beside the numeric ones, and a name that is
        // not all digits is not a process.
        const std::string_view name { entry->d_name };
        long processID = 0;
        const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), processID);
        if (error != std::errc {} || end != name.data() + name.size() || processID <= 0)
            continue;
        if (auto read = readProcessEntry(processID))
            entries.push_back(*read);
    }
    ::closedir(processes);
    return entries;
}

// The classifier takes a contiguous RawNode array, so the nodes are lifted out
// of the entries once rather than the two being kept in step by hand.
[[nodiscard]] std::vector<Ubiquitty::Session::Process::RawNode> nodesOf(std::span<const ProcessEntry> entries)
{
    auto nodes = entries | std::views::transform(&ProcessEntry::node);
    return { nodes.begin(), nodes.end() };
}

} // namespace

namespace {

// Whether argv[0] is a file name rather than a title the program wrote over it.
//
// comm cannot say: a program renames it (prctl PR_SET_NAME, as Node's
// process.title and setproctitle do beside rewriting argv[0]), and a file name
// of 15 bytes or more has a comm that is the head of any title beginning with
// it. What the program cannot rewrite is the file it was run from. A path that
// resolves is a file; so is a bare name equal to the executable's own name.
// The executable's name resolves symbolic links, so only a name no link
// explains is refused by it. An executable that cannot be read, as another
// user's cannot, leaves comm to answer.
[[nodiscard]] bool argument0NamesAFile(std::string_view argument0, std::string_view whole, ProcessID processID)
{
    constexpr std::size_t commandNameLength = 15;
    std::string path;
    if (argument0.starts_with('/'))
        path = std::string(argument0);
    else {
        path = procPath(processID, "cwd").data();
        path += '/';
        path += argument0;
    }
    struct stat fileStat {};
    if (::lstat(path.c_str(), &fileStat) == 0)
        return true;
    std::array<char, PATH_MAX> executable {};
    const ssize_t length = ::readlink(procPath(processID, "exe").data(), executable.data(), executable.size());
    if (length <= 0)
        return whole.substr(0, commandNameLength) == processCommandName(processID);
    std::string_view executablePath { executable.data(), static_cast<std::size_t>(length) };
    constexpr std::string_view deletedSuffix = " (deleted)";
    if (executablePath.ends_with(deletedSuffix))
        executablePath.remove_suffix(deletedSuffix.size());
    return executablePath.substr(executablePath.rfind('/') + 1) == whole;
}

// The name the program was run by, out of the text of /proc/<pid>/cmdline:
// argv[0]'s basename, without a login shell's leading dash, as Darwin's
// executable name has none. Whole when argv[0] is a file name
// (argument0NamesAFile), so a program whose file name holds a space is named
// whole. Otherwise argv[0] is a title the program rewrote ("sshd: user@pts/0",
// "npm run dev"), named by its first word.
[[nodiscard]] std::string argument0Name(std::string_view arguments, ProcessID processID)
{
    const std::string_view argument0 = arguments.substr(0, arguments.find('\0'));
    const auto bareBasename = [](std::string_view path) {
        path.remove_prefix(path.rfind('/') + 1);
        if (path.starts_with('-'))
            path.remove_prefix(1);
        return path;
    };
    if (argument0.find(' ') == std::string_view::npos)
        return std::string(bareBasename(argument0));
    const std::string_view whole = bareBasename(argument0);
    if (argument0NamesAFile(argument0, whole, processID))
        return std::string(whole);
    return std::string(bareBasename(argument0.substr(0, argument0.find(' '))));
}

// A process's argv[0], from the start of /proc/<pid>/cmdline.
struct Argument0 {
    /// Whether the arguments read anything; they read nothing for a process
    /// without memory of its own.
    bool isPresent = false;
    /// Its name (argument0Name).
    std::string name;
};

// argv[0] alone, which a path bounds; nothing for a process that is gone.
[[nodiscard]] std::optional<Argument0> readArgument0(ProcessID processID)
{
    const UnixFileDescriptor descriptor { ::open(procPath(processID, "cmdline").data(), O_RDONLY | O_CLOEXEC) };
    if (!descriptor)
        return std::nullopt;
    std::array<char, PATH_MAX> arguments {};
    const ssize_t length = ::read(descriptor.get(), arguments.data(), arguments.size());
    if (length < 0)
        return std::nullopt;
    return Argument0 { .isPresent = length > 0, .name = argument0Name({ arguments.data(), static_cast<std::size_t>(length) }, processID) };
}

} // namespace

// The name the program was run by, argv[0]'s (argument0Name).
//
// Neither of the kernel's own names will do for a program that runs. The
// executable's path (/proc/<pid>/exe) resolves symbolic links, so `vi` run
// through a link to `vim.basic` would read `vim.basic`, and it needs the
// permission to trace the process. comm is cut at 15 bytes and the process can
// rename it: tmux names its client `tmux: client`. argv[0] is empty for a
// process that is exiting, a zombie, a kernel thread, and one in exec before
// its new image has its arguments; processCommandName answers for those.
std::string processExecutableName(ProcessID processID)
{
    if (processID <= 0)
        return {};
    std::optional<Argument0> argument0 = readArgument0(processID);
    return argument0 ? std::move(argument0->name) : std::string {};
}

// comm, the kernel's own name for the process: the last part of the path exec
// ran, cut at 15 bytes, until the process renames it.
std::string processCommandName(ProcessID processID)
{
    if (processID <= 0)
        return {};
    const UnixFileDescriptor descriptor { ::open(procPath(processID, "comm").data(), O_RDONLY | O_CLOEXEC) };
    if (!descriptor)
        return {};
    std::array<char, 64> name {};
    const ssize_t length = ::read(descriptor.get(), name.data(), name.size());
    if (length <= 0)
        return {};
    // comm arrives newline-terminated.
    std::size_t end = static_cast<std::size_t>(length);
    while (end > 0 && (name[end - 1] == '\n' || name[end - 1] == '\0'))
        --end;
    return std::string(name.data(), end);
}

// argv[0] reads nothing for a process that has let go of its memory, exiting
// or a zombie, whose comm still names it; and for one in exec, from the moment
// its new image is mapped until its arguments are. That stretch holds the
// moment exec has cleared PF_FORKNOEXEC, so the process reads as exec'd, while
// comm still names the program it replaces. Such a process names nothing, and
// the reading after it names the program.
std::string processForegroundName(ProcessID processID)
{
    if (processID <= 0)
        return {};
    std::optional<Argument0> argument0 = readArgument0(processID);
    if (!argument0)
        return {};
    if (argument0->isPresent)
        return argument0->name.empty() ? processCommandName(processID) : std::move(argument0->name);
    const UnixFileDescriptor statDescriptor { ::open(procPath(processID, "stat").data(), O_RDONLY | O_CLOEXEC) };
    if (!statDescriptor)
        return {};
    std::array<char, 512> line {};
    const ssize_t lineLength = ::read(statDescriptor.get(), line.data(), line.size());
    if (lineLength <= 0)
        return {};
    const std::string_view lineText { line.data(), static_cast<std::size_t>(lineLength) };
    ProcStatFields fields {};
    const std::optional<std::uint32_t> flags = parseProcStatFlags(lineText);
    if (!parseProcStatFields(lineText, fields) || !flags)
        return {};
    if (fields.state != 'Z' && (*flags & processFlagExiting) == 0)
        return {};
    return processCommandName(processID);
}

// The live subtree a title bar and the inspector sample.
//
// The classification is the shared Ubiquitty::Session::Process::gatherSubtree,
// which admits the subtree and stamps ids, foreground, leader and tty; this
// enumerates the table for it and fills the strings it leaves empty. Same
// division as the Darwin arm, so both answer with one policy.
//
// No tracker path: startProcessTracker returns nullptr here, so this always
// takes the full walk — the same path Darwin takes when its kqueue setup
// fails, and correct rather than merely acceptable. A tracker would make it
// cheaper, not different.
int gatherSubtree(ProcessID leader, ProcessID foregroundGroup, const std::string& tty, std::span<PAL::Process::Record> outputBuffer, ProcessTracker* /*tracker*/)
{
    if (outputBuffer.empty() || leader <= 0)
        return 0;

    const std::vector<ProcessEntry> entries = enumerateProcesses();
    const std::vector<Ubiquitty::Session::Process::RawNode> nodes = nodesOf(entries);

    const int count = Ubiquitty::Session::Process::gatherSubtree(nodes.data(), static_cast<int>(nodes.size()),
        static_cast<std::uint32_t>(leader), static_cast<std::uint32_t>(foregroundGroup), tty, outputBuffer);

    for (PAL::Process::Record& record : outputBuffer.first(static_cast<std::size_t>(count))) {
        const long processID = static_cast<long>(record.processID);
        record.commandLine = processCommandLineFromProc(processID);
        record.name = displayNameFromCommandLine(record.commandLine);
        if (record.name.empty())
            record.name = processCommandName(static_cast<ProcessID>(processID)); // comm, for a kernel thread
        if (record.commandLine.empty())
            record.commandLine = record.name;
        // The uid and the state come from the entry this walk already read, so
        // neither costs a second pass over the table. Linear over a handful of
        // admitted records against a few hundred entries, which is cheaper than
        // the index the map would need building.
        const auto match = std::ranges::find(entries, record.processID,
            [](const ProcessEntry& entry) { return entry.node.processID; });
        if (match != entries.end()) {
            record.user = userNameForID(match->node.userID);
            record.state = processStateForCharacter(match->state);
        }
    }
    return count;
}

namespace {

// The stat half of one process's foreground-owner facts, and the directory it
// was read through, so a status read after it describes the same process even
// if the pid recycles. With a group given, a process outside it reads as none.
struct GroupMemberStat {
    UnixFileDescriptor directory;
    PAL::Process::GroupMember member;
    char state = 0;
};

[[nodiscard]] std::optional<GroupMemberStat> readGroupMemberStat(long processID, std::optional<std::uint32_t> group)
{
    UnixFileDescriptor directory { ::open(procPath(processID, "").data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    if (!directory)
        return std::nullopt;
    const UnixFileDescriptor statDescriptor { ::openat(directory.get(), "stat", O_RDONLY | O_CLOEXEC) };
    if (!statDescriptor)
        return std::nullopt;
    std::array<char, 512> line {};
    const ssize_t lineLength = ::read(statDescriptor.get(), line.data(), line.size());
    if (lineLength <= 0)
        return std::nullopt;
    const std::string_view lineText { line.data(), static_cast<std::size_t>(lineLength) };
    ProcStatFields fields {};
    const std::optional<std::uint32_t> flags = parseProcStatFlags(lineText);
    if (!parseProcStatFields(lineText, fields) || !flags)
        return std::nullopt;
    if (group && fields.processGroupID != *group)
        return std::nullopt;
    return GroupMemberStat {
        .directory = std::move(directory),
        .member = {
            .processID = static_cast<std::uint32_t>(processID),
            .parentProcessID = fields.parentProcessID,
            .hasExecuted = (*flags & processFlagForkedWithoutExec) == 0,
            .isInterruptIgnored = false,
            .isZombie = false,
            .startTime = parseProcStatBirthTicks(lineText).value_or(0),
        },
        .state = fields.state,
    };
}

// One process's foreground-owner facts: the stat half, then the status file
// through the same directory.
[[nodiscard]] std::optional<PAL::Process::GroupMember> readGroupMember(long processID, std::optional<std::uint32_t> group)
{
    std::optional<GroupMemberStat> stat = readGroupMemberStat(processID, group);
    if (!stat)
        return std::nullopt;
    // An unreadable mask reads as not ignoring, which makes an exec'd member a
    // job: the answer that asks before a close. The mask sits past the groups
    // line, which grows with the user's groups, hence the room.
    //
    // The state is the first thread's, which reads `Z` once that thread has
    // ended even while others run the program on; the thread count tells such a
    // process from one that has ended, which keeps a thread until it is reaped.
    std::optional<std::uint32_t> threadCount;
    if (const UnixFileDescriptor statusDescriptor { ::openat(stat->directory.get(), "status", O_RDONLY | O_CLOEXEC) }) {
        std::array<char, 4096> status {};
        const ssize_t statusLength = ::read(statusDescriptor.get(), status.data(), status.size());
        if (statusLength > 0) {
            const std::string_view statusText { status.data(), static_cast<std::size_t>(statusLength) };
            stat->member.isInterruptIgnored = parseStatusIgnoresInterrupt(statusText).value_or(false);
            threadCount = parseStatusThreadCount(statusText);
        }
    }
    stat->member.isZombie = stat->state == 'Z' && threadCount.value_or(1) <= 1;
    return stat->member;
}

} // namespace

std::optional<PAL::Process::GroupMember> processGroupMember(ProcessID processID)
{
    if (processID <= 0)
        return std::nullopt;
    return readGroupMember(static_cast<long>(processID), std::nullopt);
}

std::optional<PTYProcessSnapshot> ptyProcessSnapshot(ProcessID processID)
{
    if (processID <= 1)
        return std::nullopt;
    const UnixFileDescriptor directory { ::open(procPath(processID, "").data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    if (!directory)
        return std::nullopt;
    const UnixFileDescriptor descriptor { ::openat(directory.get(), "stat", O_RDONLY | O_CLOEXEC) };
    if (!descriptor)
        return std::nullopt;
    std::array<char, 4096> line {};
    const ssize_t length = ::read(descriptor.get(), line.data(), line.size());
    if (length <= 0 || static_cast<std::size_t>(length) == line.size())
        return std::nullopt;
    const std::string_view contents(line.data(), static_cast<std::size_t>(length));
    ProcStatFields fields {};
    const auto birth = parseProcStatBirthTicks(contents);
    if (!parseProcStatFields(contents, fields) || !birth || fields.state == 'Z' || fields.state == 'X')
        return std::nullopt;
    return PTYProcessSnapshot {
        .parentProcessID = static_cast<ProcessID>(fields.parentProcessID),
        .processGroupID = static_cast<ProcessID>(fields.processGroupID),
        .birth = { *birth, 0 },
    };
}

// /proc has no index by group, so this reads process stat lines until it has
// filled the buffer and found one member past it, which is all the contract
// asks of a group the buffer cannot hold; that last member's status is not
// read. The caller asks only when the group's leader alone did not settle the
// answer.
std::optional<std::size_t> processGroupMembers(ProcessID group, std::span<PAL::Process::GroupMember> outputBuffer)
{
    if (group <= 0)
        return std::nullopt;
    DIR* processes = ::opendir("/proc");
    if (!processes)
        return std::nullopt;
    const auto groupID = static_cast<std::uint32_t>(group);
    std::size_t count = 0;
    while (count <= outputBuffer.size()) {
        const dirent* entry = ::readdir(processes);
        if (!entry)
            break;
        const std::string_view name { entry->d_name };
        long processID = 0;
        const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), processID);
        if (error != std::errc {} || end != name.data() + name.size() || processID <= 0)
            continue;
        if (count == outputBuffer.size()) {
            if (readGroupMemberStat(processID, groupID))
                ++count;
            continue;
        }
        if (const std::optional<PAL::Process::GroupMember> member = readGroupMember(processID, groupID))
            outputBuffer[count++] = *member;
    }
    ::closedir(processes);
    return count;
}

// The ADOPT_SESSION ownership gate, over /proc.
//
// Fails closed on every miss, as the Darwin arm does: a session leader absent
// from the table, an unreadable /proc, or a claim that cannot be corroborated
// is the case least worth trusting. The ancestry policy itself is the shared
// Ubiquitty::Session::Process::processInSubtree; only enumerating the table is
// platform work, which is why this reads a flat list and decides nothing.
bool isProcessAncestorOfSession(ProcessID claimedOwner, ProcessID sessionLeader)
{
    if (claimedOwner <= 0 || sessionLeader <= 0)
        return false;
    if (claimedOwner == sessionLeader)
        return true; // trivially owns a session it is the leader of

    const std::vector<ProcessEntry> entries = enumerateProcesses();
    const auto leaderEntry = std::ranges::find(entries, static_cast<std::uint32_t>(sessionLeader),
        [](const ProcessEntry& entry) { return entry.node.processID; });
    // A session leader the table does not hold cannot be corroborated, and an
    // fd whose ownership cannot be corroborated is the one least worth trusting.
    if (leaderEntry == entries.end())
        return false;

    const std::vector<Ubiquitty::Session::Process::RawNode> nodes = nodesOf(entries);
    return Ubiquitty::Session::Process::processInSubtree(nodes.data(), static_cast<int>(nodes.size()),
        leaderEntry->node, static_cast<std::uint32_t>(claimedOwner));
}

} // namespace PAL
} // namespace Ubiquitty

#endif
