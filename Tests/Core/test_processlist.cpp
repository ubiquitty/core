// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_processlist.cpp — pins the portable foreground-selection + title
// formatting policy in Source/PAL/Process.cpp against synthetic
// process lists (no PTY, no libproc). Compiles against bare libc++.

#include "PAL/Process.h"
#include "PAL/ProcessSignal.h"

#include <csignal>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

using Ubiquitty::PAL::Process::Record;

namespace {
void check(bool cond, const char* what)
{
    CHECK_MSG(cond, what);
}

void checkString(std::string_view got, std::string_view want, const char* what)
{
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "%s — got \"%.*s\" want \"%.*s\"", what, static_cast<int>(got.size()), got.data(), static_cast<int>(want.size()), want.data());
        CHECK_MSG(false, ubqMessage);
    }
}

Record makeRec(unsigned processID, unsigned parentProcessID, const char* name, bool foreground, bool leader)
{
    Record r;
    r.processID = processID;
    r.parentProcessID = parentProcessID;
    r.name = name;
    r.isForeground = foreground;
    r.isSessionLeader = leader;
    return r;
}

} // namespace

TEST(processlist)
{

    std::string buffer;

    // 0. Process-kind policy (the marker column) + signal table, all pure.
    {
        namespace Signal = Ubiquitty::PAL::Process::Signal;
        using Ubiquitty::PAL::Process::isLoginShell;
        using Ubiquitty::PAL::Process::marker;
        using Ubiquitty::PAL::Process::safeToClose;
        using Ubiquitty::PAL::Process::SessionLeaderStatus;
        using Ubiquitty::PAL::Process::UserPrivilege;

        // Login shell: leading argv[0] dash, or the session leader.
        check(isLoginShell("-zsh", SessionLeaderStatus::NonLeader), "isLoginShell: dash");
        check(isLoginShell("zsh", SessionLeaderStatus::Leader), "isLoginShell: leader");
        check(!isLoginShell("zsh", SessionLeaderStatus::NonLeader), "isLoginShell: plain not");
        check(!isLoginShell("vim", SessionLeaderStatus::NonLeader), "isLoginShell: tool not");

        // Whitelist (leading dash stripped before matching).
        check(safeToClose("zsh"), "safeToClose: zsh");
        check(safeToClose("-bash"), "safeToClose: -bash strips dash");
        check(safeToClose("ssh"), "safeToClose: ssh");
        check(safeToClose("tmux"), "safeToClose: tmux");
        check(safeToClose("et"), "safeToClose: et");
        check(safeToClose("login"), "safeToClose: login");
        check(!safeToClose("claude"), "safeToClose: claude not");
        check(!safeToClose("vim"), "safeToClose: vim not");
        check(!safeToClose(""), "safeToClose: empty not");

        // Markers: $ login, # root login, • prompt-before-closing, none whitelisted.
        checkString(marker("-zsh", SessionLeaderStatus::NonLeader, UserPrivilege::NonRoot), "$", "marker: login $");
        checkString(marker("-zsh", SessionLeaderStatus::NonLeader, UserPrivilege::Root), "#", "marker: root login #");
        checkString(marker("zsh", SessionLeaderStatus::Leader, UserPrivilege::Root), "#", "marker: root leader #");
        checkString(marker("claude", SessionLeaderStatus::NonLeader, UserPrivilege::NonRoot), "\xE2\x80\xA2", "marker: non-whitelist •");
        checkString(marker("ssh", SessionLeaderStatus::NonLeader, UserPrivilege::NonRoot), "", "marker: whitelisted none");
        checkString(marker("(login)", SessionLeaderStatus::NonLeader, UserPrivilege::NonRoot), "\xE2\x80\xA2", "marker: (login) literal not whitelisted");

        // A non-root login shell never gets the • even though it's whitelisted
        // — login takes precedence and shows $.
        checkString(marker("-bash", SessionLeaderStatus::NonLeader, UserPrivilege::NonRoot), "$", "marker: login wins over whitelist");

        // Signal table: order, lookup, unknown.
        const std::span<const Signal::Entry> signalTable = Signal::table();
#ifdef SIGINFO
        check(signalTable.size() == 14, "Signal::table includes platform INFO");
#else
        check(signalTable.size() == 13, "Signal::table excludes unavailable INFO");
#endif
        checkString(signalTable.front().mnemonic, "TSTP", "Signal::table opens with TSTP, the Suspend entry");
        check(Signal::numberForMnemonic("TERM") == SIGTERM, "TERM uses platform signal");
        check(Signal::numberForMnemonic("KILL") == SIGKILL, "KILL uses platform signal");
#ifdef SIGINFO
        check(Signal::numberForMnemonic("INFO") == SIGINFO, "INFO uses platform signal");
#else
        check(Signal::numberForMnemonic("INFO") == 0, "unavailable INFO is absent");
#endif
        check(Signal::numberForMnemonic("WINCH") == SIGWINCH, "WINCH uses platform signal");
        check(Signal::numberForMnemonic("nope") == 0, "an unlisted mnemonic maps to the 0 sentinel");
    }

    // 1. Shell only at a prompt: no foreground job → chain names the shell.
    {
        Record recs[] = {
            makeRec(100, 1, "zsh", /*foreground*/ true, /*leader*/ true),
        };
        check(Ubiquitty::PAL::Process::foregroundLeafProcessID(std::span(recs, static_cast<std::size_t>(1))) == 0, "shell-only: no fg leaf");
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(1)));
        checkString(buffer, "zsh", "shell-only chain");
    }

    // 2. Single foreground job: zsh → vim. Leaf is vim; parent is the shell
    //    (leader) so no arrow — just "vim" (the shell is omitted).
    {
        Record recs[] = {
            makeRec(100, 1, "zsh", true, true),
            makeRec(200, 100, "vim", true, false),
        };
        check(Ubiquitty::PAL::Process::foregroundLeafProcessID(std::span(recs, static_cast<std::size_t>(2))) == 200, "vim is fg leaf");
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)));
        checkString(buffer, "vim", "single-job chain");
    }

    // 2b. Foreground process with arguments: the leaf shows its argv tail
    //     ("ssh example-host"), not the bare executable name.
    {
        Record recs[] = {
            makeRec(100, 1, "-zsh", true, true),
            makeRec(200, 100, "ssh", true, false),
        };
        recs[1].commandLine = "/usr/bin/ssh example-host";
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)));
        checkString(buffer, "ssh example-host", "leaf shows command + args");

        // The option governs arguments, and where a session leads is not one
        // of them: the flags go, the destination stays, so a long invocation
        // reads as the machine it reached rather than collapsing to the
        // program's name. Finding `example-host` means stepping over the values of
        // `-o` and `-L`, neither of which is it.
        recs[1].commandLine = "/usr/bin/ssh -o ProxyJump=bastion.internal.example.com -L 8080:localhost:8080 example-host";
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)));
        checkString(buffer, "ssh example-host", "the destination outlives the arguments option");
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)), Ubiquitty::PAL::Process::ChainArguments::IncludeArguments);
        checkString(buffer, "ssh -o ProxyJump=bastion.internal.example.com -L 8080:localhost:8080 example-host", "IncludeArguments keeps the full argv tail");
        // A small tail renders identically in both modes (the smart-name default
        // already keeps it).
        recs[1].commandLine = "/usr/bin/ssh example-host";
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)), Ubiquitty::PAL::Process::ChainArguments::IncludeArguments);
        checkString(buffer, "ssh example-host", "small tail identical under IncludeArguments");

        // 2c. login(1)-wrapped session (macOS PTY shape): leader is "login", the
        //     user's shell is its login-shell child. effectiveShellLeader resolves
        //     the CHILD; its display name drops the login dash; the chain suppresses
        //     the shell parent (transient subshell renders "-zsh", never
        //     "-zsh ◂ -zsh") and names the effective shell at a prompt, not login.
        {
            Record recs[] = {
                makeRec(100, 1, "login", /*foreground*/ false, /*leader*/ true),
                makeRec(200, 100, "-zsh", /*foreground*/ false, /*leader*/ false),
                makeRec(300, 200, "-zsh", /*foreground*/ true, /*leader*/ false), // startup subshell
            };
            const auto span3 = std::span(recs, static_cast<std::size_t>(3));
            const Ubiquitty::PAL::Process::Record* shell = Ubiquitty::PAL::Process::effectiveShellLeader(span3);
            check(shell != nullptr && shell->processID == 200, "effectiveShellLeader skips login to the login-shell child");
            check(Ubiquitty::PAL::Process::shellCommandDisplayName(*shell) == "zsh", "shell display name drops the login dash");
            buffer = Ubiquitty::PAL::Process::formatChain(span3);
            checkString(buffer, "-zsh", "shell parent suppressed under a login wrapper");
            // The REALISTIC at-prompt steady state (the default macOS shape): the
            // subshell is gone and the shell itself is the foreground pgroup. The
            // leaf resolves to the shell (only the leader is skipped), its parent
            // is the login leader — which must be suppressed, or every prompt
            // renders "-zsh ◂ login".
            {
                Record prompt[] = {
                    makeRec(100, 1, "login", /*foreground*/ false, /*leader*/ true),
                    makeRec(200, 100, "-zsh", /*foreground*/ true, /*leader*/ false),
                };
                buffer = Ubiquitty::PAL::Process::formatChain(std::span(prompt, static_cast<std::size_t>(2)));
                checkString(buffer, "-zsh", "at-prompt under login: leader parent suppressed, never '-zsh ◂ login'");
            }
            recs[1].isForeground = false;
            recs[2].isForeground = false; // degenerate: no foreground record at all
            buffer = Ubiquitty::PAL::Process::formatChain(span3);
            checkString(buffer, "-zsh", "no-foreground fallback names the effective shell, not login");
        }

        Record bare[] = {
            makeRec(100, 1, "-zsh", true, true),
            makeRec(200, 100, "vim", true, false),
        };
        bare[1].commandLine = "vim";
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(bare, static_cast<std::size_t>(2)));
        checkString(buffer, "vim", "no-args leaf is bare name");
    }

    // 2c. Long arguments collapse to just the process name: a path/flag-heavy
    //     command would overflow the title, so only the binary name shows.
    {
        Record recs[] = {
            makeRec(100, 1, "-zsh", true, true),
            makeRec(200, 100, "claude", true, false),
        };
        recs[1].commandLine = "claude --dangerously-skip-permissions /Users/user/Projects/ubiquitty";
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)));
        checkString(buffer, "claude", "long args collapse to process name");
    }

    // 3. Nested non-shell chain: zsh → claude → beam.smp. Leaf beam.smp,
    //    parent claude (non-leader) → "beam.smp ◂ claude": beam.smp holds the
    //    terminal, so it leads, and the arrow points back at it as the child.
    {
        Record recs[] = {
            makeRec(100, 1, "zsh", true, true),
            makeRec(200, 100, "claude", true, false),
            makeRec(300, 200, "beam.smp", true, false),
        };
        check(Ubiquitty::PAL::Process::foregroundLeafProcessID(std::span(recs, static_cast<std::size_t>(3))) == 300, "beam.smp is fg leaf");
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(3)));
        checkString(buffer, "beam.smp \xE2\x97\x82 claude", "the holder leads and the arrow points back at it");
    }

    // 3a. A program that holds the terminal while PARKING a child of its own.
    //     mc keeps a subshell in the background: mc is the deepest FOREGROUND
    //     process, but the session is sitting on that shell, and a walk that
    //     stopped at the foreground named only "mc" for a window the reader can
    //     see is running it. The walk descends, so the title says "mc ▸ zsh".
    {
        Record recs[] = {
            makeRec(100, 1, "login", /*foreground*/ false, /*leader*/ true),
            makeRec(200, 100, "-zsh", false, false),
            makeRec(300, 200, "mc", true, false),
            makeRec(400, 300, "zsh", false, false),
        };
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(4)));
        checkString(buffer, "mc \xE2\x96\xB8 zsh", "a parked background child is named, not stopped at");
    }

    // 3a-ii. Two background children is genuinely ambiguous — the walk stops
    //     rather than picking one and presenting it as the answer.
    {
        Record recs[] = {
            makeRec(100, 1, "zsh", /*foreground*/ true, /*leader*/ true),
            makeRec(200, 100, "mc", true, false),
            makeRec(300, 200, "zsh", false, false),
            makeRec(400, 200, "helper", false, false),
        };
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(4)));
        checkString(buffer, "mc", "two background children: the walk stops rather than guessing");
    }

    // 3a-iii. The REAL tree, read off a live session with ps and Activity
    //     Monitor: mc opens a pty of its OWN for its subshell, so the shell and
    //     the editor sit in a different foreground group from the session's,
    //     and only mc is foreground on the session's tty. The ancestry is still
    //     login -> -zsh -> mc -> zsh -> vi, which is what the walk follows.
    {
        Record recs[] = {
            makeRec(34559, 1, "-login", /*foreground*/ false, /*leader*/ true),
            makeRec(34560, 34559, "-zsh", false, false),
            makeRec(34642, 34560, "mc", true, false),
            makeRec(34644, 34642, "zsh", false, false),
            makeRec(34945, 34644, "vi", false, false),
        };
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(5)));
        checkString(buffer, "mc \xE2\x96\xB8 vi", "the editor beneath mc's parked subshell is what the reader means");
    }

    // 3a-iv. A step that names the same program twice says nothing. Neovim's
    //     interface runs its editor as a child of the same name, so the walk
    //     that descends to it would read "nvim ▸ nvim"; the chain names it
    //     once, whether or not arguments are shown, since the descendant is
    //     spelled without them in both.
    {
        Record recs[] = {
            makeRec(100, 1, "-zsh", /*foreground*/ true, /*leader*/ true),
            makeRec(200, 100, "nvim", true, false),
            makeRec(300, 200, "nvim", false, false),
        };
        recs[1].commandLine = "nvim src/cursor.cpp";
        recs[2].commandLine = "nvim --embed";
        const auto span3 = std::span(recs, static_cast<std::size_t>(3));
        buffer = Ubiquitty::PAL::Process::formatChain(span3);
        checkString(buffer, "nvim", "a child of the same name is not named again");
        buffer = Ubiquitty::PAL::Process::formatChain(span3, Ubiquitty::PAL::Process::ChainArguments::IncludeArguments);
        checkString(buffer, "nvim src/cursor.cpp", "nor when the holder is spelled with its arguments");
    }

    // 3a-v. Saying nothing, the repeated step leaves the chain free to say what
    //     it otherwise would: where the program was started from.
    {
        Record recs[] = {
            makeRec(100, 1, "-zsh", /*foreground*/ false, /*leader*/ true),
            makeRec(200, 100, "claude", false, false),
            makeRec(300, 200, "nvim", true, false),
            makeRec(400, 300, "nvim", false, false),
        };
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(4)));
        checkString(buffer, "nvim \xE2\x97\x82 claude", "past a same-named child, the parent is named");
    }

    // 3a-vi. The same holds looking up: a recursive make reads once.
    {
        Record recs[] = {
            makeRec(100, 1, "-zsh", /*foreground*/ false, /*leader*/ true),
            makeRec(200, 100, "make", true, false),
            makeRec(300, 200, "make", true, false),
        };
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(3)));
        checkString(buffer, "make", "a parent of the same name is not named again");
    }

    // 3b. Login-shell dash is KEPT for display: the PAL preserves a login
    //     shell's "-zsh" argv[0] (the leading '-' is the login convention), and
    //     the title shows it — "-zsh" — so the interactive login shell reads
    //     distinct from a bare "zsh" subprocess.
    {
        Record recs[] = {
            makeRec(100, 1, "-zsh", /*foreground*/ true, /*leader*/ true),
        };
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(1)));
        checkString(buffer, "-zsh", "login-shell dash kept for display");
    }

    // 3c. The dash is kept on a non-leader leaf too (a login shell re-exec'd as
    //     a foreground job): "-bash".
    {
        Record recs[] = {
            makeRec(100, 1, "zsh", true, true),
            makeRec(200, 100, "-bash", true, false),
        };
        recs[1].commandLine = "-bash";
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)));
        checkString(buffer, "-bash", "leaf login-shell dash kept");
    }

    // 4. Background job must not count: a non-foreground child is ignored.
    {
        Record recs[] = {
            makeRec(100, 1, "zsh", true, true),
            makeRec(200, 100, "make", false, false), // backgrounded
        };
        check(Ubiquitty::PAL::Process::foregroundLeafProcessID(std::span(recs, static_cast<std::size_t>(2))) == 0, "backgrounded job not fg");
        buffer = Ubiquitty::PAL::Process::formatChain(std::span(recs, static_cast<std::size_t>(2)));
        checkString(buffer, "zsh", "bg-job falls back to shell");
    }

    // 5. Window title (includeDimensions=true): lead + OSC + chain + dims.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("user", "Claude Code", "beam.smp \xE2\x97\x82 claude", 80, 29, Ubiquitty::PAL::Process::DimensionFormat::IncludeDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer,
            "user—Claude Code—"
            "beam.smp \xE2\x97\x82 claude—80\xC3\x97"
            "29",
            "window title: lead+osc+chain+dims");
    }

    // 6. Tab title (includeDimensions=false): same input drops only the dims.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("user", "Claude Code", "beam.smp \xE2\x97\x82 claude", 80, 29, Ubiquitty::PAL::Process::DimensionFormat::OmitDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer,
            "user—Claude Code—"
            "beam.smp \xE2\x97\x82 claude",
            "tab title: lead+osc+chain, no dims");
    }

    // 7. Lead + chain, empty OSC: no doubled separator where OSC would sit.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("src", "", "vim", 80, 24, Ubiquitty::PAL::Process::DimensionFormat::IncludeDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer,
            "src—vim—80\xC3\x97"
            "24",
            "lead+chain, empty osc, dims");
    }

    // 8. Lead only (no osc, no chain), dims dropped.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("src", "", "", 80, 24, Ubiquitty::PAL::Process::DimensionFormat::OmitDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer, "src", "lead-only, no dims");
    }

    // 9. Empty lead: title starts at the OSC segment, no leading separator.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("", "hi", "vim", 80, 24, Ubiquitty::PAL::Process::DimensionFormat::IncludeDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer,
            "hi—vim—80\xC3\x97"
            "24",
            "empty lead");
    }

    // 10. Dimensions included but zero → dropped.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("src", "hi", "vim", 0, 0, Ubiquitty::PAL::Process::DimensionFormat::IncludeDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer, "src—hi—vim", "zero dims dropped");
    }

    // 11. Everything empty → empty title.
    {
        buffer = Ubiquitty::PAL::Process::formatTitle("", "", "", 0, 0, Ubiquitty::PAL::Process::DimensionFormat::IncludeDimensions, Ubiquitty::PAL::Process::TitleSeparator::Bare);
        checkString(buffer, "", "all empty");
    }

    // 12. shouldRewalkForegroundChain: a foreground transient in its own job
    // pgid (session leader backgrounded — a bare `eval "$(mise activate zsh)"`
    // as opposed to a `$(...)` substitution sharing the shell's pgid) must
    // still trigger a re-walk once it exits and the count settles to 1, even
    // with the group/name otherwise unchanged.
    {
        using Ubiquitty::PAL::Process::shouldRewalkForegroundChain;
        // Steady prompt: same group/name, count 1 — no re-walk.
        check(!shouldRewalkForegroundChain(100, 100, true, false, 1, false), "steady prompt does not re-walk");
        check(shouldRewalkForegroundChain(100, 100, true, false, 1, true), "a changed process tree re-walks under a steady foreground");
        // Group changed (a new job took over) — re-walk.
        check(shouldRewalkForegroundChain(200, 100, true, false, 1, false), "group change triggers re-walk");
        // Same group exec'd a new image (name changed) — re-walk.
        check(shouldRewalkForegroundChain(100, 100, true, true, 1, false), "name change triggers re-walk");
        // Transient still up (count > 1), group/name otherwise unchanged —
        // re-walk regardless of whether it shares the group or runs in its
        // own (the case a session-leader-foreground check misses, latching
        // the transient's chain forever).
        check(shouldRewalkForegroundChain(100, 100, true, false, 2, false), "count > 1 triggers re-walk");
    }
}

TEST(remoteConnectionSurvivesATransportHelperLeaf)
{
    using namespace Ubiquitty::PAL::Process;
    Record records[] = {
        makeRec(100, 1, "zsh", false, true),
        makeRec(200, 100, "ssh", true, false),
        makeRec(300, 200, "sh", true, false),
        makeRec(400, 300, "sleep", true, false),
    };
    records[1].commandLine = "ssh -N example.invalid";
    records[2].commandLine = "sh -c sleep";
    records[3].commandLine = "sleep 30";
    check(remoteTargetForChain(records).name == "example.invalid", "SSH destination survives its proxy helper");
    records[1].commandLine = "ubiquitty-host --attach";
    check(remoteTargetForChain(records).isRemote(), "host attach has no local directory");
    records[1].commandLine = "ubiquitty-host --daemon";
    check(!remoteTargetForChain(records).isRemote(), "host daemon is local");
}

TEST(aConnectionIsNamedByItsOwnerNotItsJumpHelper)
{
    using namespace Ubiquitty::PAL::Process;
    // `ssh -J`, or a ProxyJump in ssh_config, runs `ssh -W` beneath the
    // client, in its foreground group, so the deepest process naming a host
    // is the jump.
    Record jump[] = {
        makeRec(100, 1, "-zsh", false, true),
        makeRec(200, 100, "ssh", true, false),
        makeRec(300, 200, "ssh", true, false),
    };
    jump[1].commandLine = "ssh dest.example";
    jump[2].commandLine = "ssh -W [192.0.2.4]:22 jump.example";
    const std::span<const Record> viaJump(jump, 3);
    check(remoteTargetForChain(viaJump).name == "dest.example", "the client nearest the shell names the destination, not the jump");
    checkString(formatChain(viaJump), "ssh dest.example", "the chain names the connection, not its helper");
    checkString(formatChain(viaJump, ChainArguments::IncludeArguments), "ssh dest.example", "and so does its argv spelling");

    // An older ProxyCommand runs `nc` on the jump host through another `ssh`.
    jump[2].commandLine = "ssh -q jump.example nc dest.example 22";
    check(remoteTargetForChain(viaJump).name == "dest.example", "a ProxyCommand's ssh is transport too");

    // A helper that names no host is never followed down into either.
    jump[2].name = "nc";
    jump[2].commandLine = "nc -X 5 -x proxy.example:1080 dest.example 22";
    checkString(formatChain(viaJump), "ssh dest.example", "a connection's owner is not followed into its transport");

    // A client a local program started keeps its parent beside it.
    Record push[] = {
        makeRec(100, 1, "-zsh", false, true),
        makeRec(200, 100, "git", true, false),
        makeRec(300, 200, "ssh", true, false),
    };
    push[1].commandLine = "git push";
    push[2].commandLine = "ssh user@git.example git-receive-pack 'repo.git'";
    const std::span<const Record> fromGit(push, 3);
    check(remoteTargetForChain(fromGit).name == "git.example", "a lone client owns its own connection");
    checkString(formatChain(fromGit), "ssh git.example ◂ git", "and still names the program that started it");
}

// A program whose file name holds a space is named whole, as its foreground
// name is, and its arguments are those after its argv[0], however many
// spaces that holds; the same word in its arguments is an argument.
TEST(aProgramWhoseNameHoldsASpaceIsNamedWholeInTheChain)
{
    using namespace Ubiquitty::PAL::Process;
    Record records[] = {
        makeRec(100, 1, "-zsh", false, true),
        makeRec(200, 100, "my prog", true, false),
    };
    const std::span<const Record> chain(records, 2);
    records[1].commandLine = "/Users/someone/My Tools/my prog 30 my prog";
    checkString(formatChain(chain), "my prog", "the chain names the program whole");
    checkString(formatChain(chain, ChainArguments::IncludeArguments), "my prog 30 my prog", "its arguments follow its argv[0]");
    records[1].commandLine = "my prog";
    checkString(formatChain(chain, ChainArguments::IncludeArguments), "my prog", "an argv[0] with no path and no arguments");
    // A rewritten argv[0] is named by its first word, and the rest is its
    // arguments, as before.
    records[1].name = "sshd:";
    records[1].commandLine = "sshd: someone@ttys000";
    checkString(formatChain(chain, ChainArguments::IncludeArguments), "sshd: someone@ttys000", "a rewritten argv[0]");
}

UBIQUITTY_TEST_MAIN()
