// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_processsubtree.cpp — pins the pure PPID-chain subtree classifier in
// Source/Core/Session/ProcessSubtree.cpp against scripted flat process lists (no libproc,
// no /proc). Compiles against bare libc++.

#include "Core/Session/ProcessSubtree.h"

#include <cstdio>

using Ubiquitty::PAL::Process::Record;
using Ubiquitty::Session::Process::gatherSubtree;
using Ubiquitty::Session::Process::processInSubtree;
using Ubiquitty::Session::Process::RawNode;

namespace {
void check(bool cond, const char* what)
{
    CHECK_MSG(cond, what);
}

RawNode node(uint32_t pid, uint32_t ppid, uint32_t pgid, uint32_t uid = 501) { return RawNode { pid, ppid, pgid, uid }; }

// Find the emitted record for `pid` in [0,count); nullptr if absent.
const Record* find(const Record* recs, int count, uint32_t pid)
{
    for (int i = 0; i < count; ++i)
        if (recs[i].processID == pid)
            return &recs[i];
    return nullptr;
}

} // namespace

TEST(processsubtree)
{

    // A representative session tree:
    //   100 = login -zsh (leader), pgid 100
    //   200 = vim, child of 100, foreground pgid 200
    //   300 = grep, child of 200 (grandchild of leader), foreground pgid 200
    //   400 = a sibling shell job, child of 100, pgid 400 (background)
    //   999 = an UNRELATED process in another session (ppid 1), pgid 999
    //     1 = init
    RawNode all[] = {
        node(1, 0, 1),
        node(100, 1, 100), // leader
        node(200, 100, 200), // vim, foreground
        node(300, 200, 200), // grep under vim, foreground
        node(400, 100, 400), // background sibling
        node(999, 1, 999), // unrelated
    };
    const int allCount = static_cast<int>(sizeof(all) / sizeof(all[0]));

    // ---- processInSubtree: direct ancestry policy ----
    {
        check(processInSubtree(all, allCount, all[1], 100), "leader itself in subtree");
        check(processInSubtree(all, allCount, all[2], 100), "direct child in subtree");
        check(processInSubtree(all, allCount, all[3], 100), "grandchild in subtree");
        check(processInSubtree(all, allCount, all[4], 100), "background sibling in subtree");
        check(!processInSubtree(all, allCount, all[5], 100), "unrelated NOT in subtree");
        check(!processInSubtree(all, allCount, all[0], 100), "init NOT in subtree");
        check(!processInSubtree(all, allCount, all[2], 0), "leader 0 admits nothing");
    }

    // ---- A chain whose parent is ABSENT from the list is rejected (no false admit) ----
    {
        RawNode orphan[] = {
            node(100, 1, 100), // leader
            node(500, 444, 500), // ppid 444 is not in the list
        };
        check(!processInSubtree(orphan, 2, orphan[1], 100), "missing-parent chain rejected");
    }

    // ---- A PPID cycle / over-deep chain terminates via maxHops ----
    {
        // Two nodes pointing at each other; neither reaches the leader. The hop
        // cap must stop the walk and reject rather than spin.
        RawNode cyc[] = {
            node(100, 1, 100), // leader
            node(600, 700, 600),
            node(700, 600, 700),
        };
        check(!processInSubtree(cyc, 3, cyc[1], 100, /*maxHops=*/8), "cyclic chain rejected within maxHops");
    }

    // ---- Ubiquitty::Session::Process::gatherSubtree: admission + foreground/leader/tty stamping ----
    {
        Record out[16];
        const int n = Ubiquitty::Session::Process::gatherSubtree(all, allCount, /*leader=*/100,
            /*foregroundGroup=*/200, "ttys004", std::span<Record>(out, 16));

        check(n == 4, "admits exactly leader+3 descendants (not init, not unrelated)");
        check(find(out, n, 100) != nullptr, "leader emitted");
        check(find(out, n, 200) != nullptr, "vim emitted");
        check(find(out, n, 300) != nullptr, "grep emitted");
        check(find(out, n, 400) != nullptr, "background sibling emitted");
        check(find(out, n, 999) == nullptr, "unrelated NOT emitted");
        check(find(out, n, 1) == nullptr, "init NOT emitted");

        const Record* leader = find(out, n, 100);
        const Record* vim = find(out, n, 200);
        const Record* grep = find(out, n, 300);
        const Record* bg = find(out, n, 400);
        check(leader && leader->isSessionLeader, "leader stamped isSessionLeader");
        check(vim && !vim->isSessionLeader, "vim not session leader");
        check(leader && !leader->isForeground, "leader pgid 100 not foreground");
        check(vim && vim->isForeground, "vim pgid 200 foreground");
        check(grep && grep->isForeground, "grep pgid 200 foreground");
        check(bg && !bg->isForeground, "background pgid 400 not foreground");

        // Field copy + tty stamp + empty strings (platform fills later).
        check(vim && vim->parentProcessID == 100, "vim parentProcessID copied");
        check(vim && vim->processGroupID == 200, "vim processGroupID copied");
        check(leader && leader->tty == "ttys004", "tty stamped");
        check(vim && vim->name.empty(), "name left empty for platform");
        check(vim && vim->commandLine.empty(), "commandLine left empty for platform");
        check(vim && vim->user.empty(), "user left empty for platform");
    }

    // ---- foregroundGroup 0 marks nothing foreground ----
    {
        Record out[16];
        const int n = Ubiquitty::Session::Process::gatherSubtree(all, allCount, 100, /*foregroundGroup=*/0, "ttys004", std::span<Record>(out, 16));
        bool anyForeground = false;
        for (int i = 0; i < n; ++i)
            anyForeground = anyForeground || out[i].isForeground;
        check(!anyForeground, "foregroundGroup 0 => no foreground");
    }

    // ---- out capacity is respected (no overflow) ----
    {
        Record out[2];
        const int n = Ubiquitty::Session::Process::gatherSubtree(all, allCount, 100, 200, "ttys004", std::span<Record>(out, 2));
        check(n == 2, "writes at most out.size() records");
    }

    // ---- degenerate inputs ----
    {
        Record out[4];
        check(Ubiquitty::Session::Process::gatherSubtree(nullptr, 0, 100, 200, "t", std::span<Record>(out, 4)) == 0, "null node list => 0");
        check(Ubiquitty::Session::Process::gatherSubtree(all, allCount, /*leader=*/0, 200, "t", std::span<Record>(out, 4)) == 0, "leader 0 => 0");
        check(Ubiquitty::Session::Process::gatherSubtree(all, allCount, 100, 200, "t", std::span<Record>(out, 0)) == 0, "empty out => 0");
    }
}

UBIQUITTY_TEST_MAIN()
