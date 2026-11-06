// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pane_status_facts.cpp — every status a program can send reaches the
// roll-up through the two records a host sends for a pane.
//
// Each sequence is fed as a program would send it, read into the pane's
// records, carried across their wire form, and read back as the leaf a listing
// shows. A vocabulary a record forgot, or one its codec drops, is a failing
// line here rather than a mark that never appears on a remote pane's tab.

#include "Core/App/ProgramActivity.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Parser.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Ubiquitty;
using App::StatusKind;
using App::TerminalHolder;

namespace {

void feed(Terminal& terminal, std::string_view bytes) { processBytes(&terminal, bytes); }

/// A pane's status as a client reads it: the host's two records, encoded,
/// decoded, and handed to the one reader.
App::RolledStatus statusOverTheWire(const Terminal& terminal, App::TerminalHolder holder)
{
    using namespace Protocol::Sync;
    PaneHostFacts host;
    host.isAtPrompt = holder == TerminalHolder::Prompt;
    PaneFacts facts = paneFactsOf(terminal, host);
    facts.revision = 1;
    std::vector<std::uint8_t> factsBody;
    encodePaneFacts(factsBody, facts);
    const std::optional<PaneFacts> readFacts = decodePaneFacts(factsBody);
    CHECK(readFacts && *readFacts == facts);
    if (!readFacts)
        return {};

    StatusRecords records = statusRecordsOf(terminal);
    records.revision = 1;
    StatusAssembly assembly;
    StatusAssembly::Outcome outcome = StatusAssembly::Outcome::Partial;
    for (std::size_t next = 0; outcome == StatusAssembly::Outcome::Partial;) {
        std::vector<std::uint8_t> body;
        next = encodeStatusFragment(body, records, next);
        std::optional<StatusFragment> fragment = decodeStatusFragment(body);
        CHECK(fragment);
        if (!fragment)
            return {};
        outcome = assembly.accept(std::move(*fragment));
    }
    CHECK(outcome == StatusAssembly::Outcome::Whole);
    if (outcome != StatusAssembly::Outcome::Whole)
        return {};
    const StatusRecords readRecords = assembly.take();
    CHECK(readRecords == records);
    return App::paneStatus(App::paneStatusFacts(*readFacts, readRecords));
}

StatusKind kindOf(const Terminal& terminal, App::TerminalHolder holder)
{
    // The local reader and the wire agree on every kind.
    const StatusKind local = App::paneStatus(App::paneStatusFacts(&terminal, holder)).kind;
    CHECK(statusOverTheWire(terminal, holder).kind == local);
    return local;
}

} // namespace

TEST(everyStatusAProgramSendsReachesTheLeaf)
{
    Terminal terminal;
    hardReset(terminal);
    CHECK(kindOf(terminal, TerminalHolder::Prompt) == StatusKind::Idle);
    CHECK(kindOf(terminal, TerminalHolder::Program) == StatusKind::Running);
    CHECK(App::paneStatus(App::paneStatusFacts(nullptr, TerminalHolder::Prompt)).kind == StatusKind::Idle);

    // An agent's status is an OSC 7501 record.
    feed(terminal, "\033]7501;id=agent:app=codex:state=working\033\\");
    CHECK(kindOf(terminal, TerminalHolder::Program) == StatusKind::Working);
    feed(terminal, "\033]7501;id=agent:app=codex:state=blocked:kind=permission\033\\");
    const App::RolledStatus blocked = statusOverTheWire(terminal, TerminalHolder::Program);
    CHECK(blocked.kind == StatusKind::Blocked && blocked.blockKind == OSC::ProgramBlockKind::Permission);
    feed(terminal, "\033]7501;id=agent:state=clear\033\\");
    CHECK(kindOf(terminal, TerminalHolder::Program) == StatusKind::Running);

    // OSC 9;4 progress.
    feed(terminal, "\033]9;4;1;40\007");
    const App::RolledStatus progressing = statusOverTheWire(terminal, TerminalHolder::Program);
    CHECK(progressing.kind == StatusKind::Working && progressing.progress.state == OSC::ProgressNormal && progressing.progress.value == 40);
    feed(terminal, "\033]9;4;2\007");
    CHECK(kindOf(terminal, TerminalHolder::Program) == StatusKind::Failed);
    feed(terminal, "\033]9;4;0\007");

    // OSC 133;D exit status, counted while the prompt shows it.
    feed(terminal, "\033]133;A\007\033]133;B\007\033]133;C\007\033]133;D;3\007");
    CHECK(kindOf(terminal, TerminalHolder::Prompt) == StatusKind::Failed);
    CHECK(kindOf(terminal, TerminalHolder::Program) == StatusKind::Running);
    feed(terminal, "\033]133;A\007\033]133;B\007\033]133;C\007\033]133;D;0\007");
    CHECK(kindOf(terminal, TerminalHolder::Prompt) == StatusKind::Idle);

    // A bell and a notification, until seen.
    feed(terminal, "\007");
    CHECK(kindOf(terminal, TerminalHolder::Prompt) == StatusKind::Attention);
    CHECK(markAttentionSeen(terminal));
    feed(terminal, "\033]777;notify;Deploy;went out\007");
    CHECK(kindOf(terminal, TerminalHolder::Prompt) == StatusKind::Attention);
    CHECK(markAttentionSeen(terminal));
    CHECK(kindOf(terminal, TerminalHolder::Prompt) == StatusKind::Idle);

    // The colours and the badge ride along without changing the kind.
    feed(terminal, "\033]21337;indicator=#102030\007\033]6;1;bg;red;brightness;200\007\033]1337;SetBadgeFormat=cHJvZA==\007");
    const App::RolledStatus marked = statusOverTheWire(terminal, TerminalHolder::Prompt);
    CHECK(marked.kind == StatusKind::Idle);
    CHECK(marked.indicatorColor && marked.indicatorColor->red == 0x10 && marked.indicatorColor->blue == 0x30);
    CHECK(marked.tabColor && marked.tabColor->red == 200);
    CHECK(marked.badgeCount == 1);
}

// A pane's title inputs are read from the same two records, so a title
// composed for a remote pane starts from what one composed locally does.
TEST(aPanesTitleInputsAreReadFromItsRecords)
{
    using namespace Protocol::Sync;
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 30, 100);
    feed(terminal, "\033]2;make test\007");
    PaneHostFacts host;
    host.homeDirectory = "/home/user";
    host.processChain = "make ◂ -zsh";
    host.processChainWithArguments = "make test ◂ -zsh";
    host.shellCommandName = "zsh";
    host.terminalName = "pts/3";
    host.hostName = "build-host";
    host.end = { .didEnd = true, .hasStatus = true, .wasSignalled = false, .code = 2 };
    App::PaneTitleFacts title = App::paneTitleFacts(paneFactsOf(terminal, host), statusRecordsOf(terminal));
    CHECK(title.programTitle == "make test");
    CHECK(title.homeDirectory == "/home/user" && title.processChain == "make ◂ -zsh" && title.processChainWithArguments == "make test ◂ -zsh");
    CHECK(title.shellCommandName == "zsh" && title.ttyName == "pts/3" && title.hostName == "build-host");
    CHECK(title.columns == 100 && title.rows == 30);
    CHECK(title.end.didEnd && title.end.hasStatus && !title.end.wasSignalled && title.end.code == 2);
    // Nothing a layout decides is filled in.
    CHECK(title.paneLabel.empty() && title.tabTitle.empty() && title.windowTitle.empty());
    CHECK(title.programStatus.empty());

    // The status label is the indicator's word until a program posts a
    // record, and the record's from then on.
    feed(terminal, "\033]21337;indicator=#102030;status=busy\007");
    title = App::paneTitleFacts(paneFactsOf(terminal, host), statusRecordsOf(terminal));
    CHECK(title.programStatus == "busy");
    feed(terminal, "\033]7501;id=agent:app=codex:state=working\033\\");
    title = App::paneTitleFacts(paneFactsOf(terminal, host), statusRecordsOf(terminal));
    CHECK(title.programStatus == App::programActivityLabel(terminal.programStatus));
    CHECK(title.programStatus.find("Working") != std::string::npos);
}

UBIQUITTY_TEST_MAIN()
