// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Platform/StreamDouble.h"

#include "Core/Session/SessionInfo.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>

namespace {

using Ubiquitty::PAL::Process::Record;

/// Whether a row sits in the tty's foreground process group.
enum class Foreground : bool {
    No = false,
    Yes = true
};

/// A record with the fields this function reads, and nothing else.
///
/// - Parameters:
///   - name: The executable basename.
///   - foreground: Whether it is in the tty's foreground group.
/// - Returns: The record.
Record process(const std::string& name, Foreground foreground)
{
    Record record;
    record.name = name;
    record.isForeground = foreground == Foreground::Yes;
    return record;
}

} // namespace

TEST(theProcessTreeArrivesLeafFirstAndIsHandedOverRootFirst)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(terminal);

    Ubiquitty::TestDoubles::StreamDouble stream;
    auto editor = process("vim", Foreground::Yes);
    editor.processID = 3;
    editor.parentProcessID = 2;
    auto shell = process("zsh", Foreground::Yes);
    shell.processID = 2;
    shell.parentProcessID = 1;
    auto login = process("login-zsh", Foreground::No);
    login.processID = 1;
    login.isSessionLeader = true;
    stream.facts.processes = { editor, shell, login };

    Ubiquitty::Session::SessionInfo info;
    Ubiquitty::Session::gatherSessionInfo(terminal, stream, info);

    CHECK_EQ(static_cast<int>(info.processes.size()), 3);
    CHECK_CSTR_EQ(info.processes[0].name.c_str(), "login-zsh");
    CHECK_CSTR_EQ(info.processes[1].name.c_str(), "zsh");
    CHECK_CSTR_EQ(info.processes[2].name.c_str(), "vim");

    // The shallowest foreground row follows the session leader.
    CHECK_EQ(info.foregroundIndex, 1);
}

// No foreground group is -1 rather than 0, so a caller indexing the list cannot
// read the first row as foreground when none is.
TEST(aTreeWithNoForegroundRowReportsNoIndex)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(terminal);

    Ubiquitty::TestDoubles::StreamDouble stream;
    stream.facts.processes = { process("zsh", Foreground::No), process("login-zsh", Foreground::No) };

    Ubiquitty::Session::SessionInfo info;
    Ubiquitty::Session::gatherSessionInfo(terminal, stream, info);

    CHECK_EQ(static_cast<int>(info.processes.size()), 2);
    CHECK_EQ(info.foregroundIndex, -1);
}

// The geometry and the swatch come from Core, and the swatch is a conversion
// rather than a copy: the palette is 16 bits a channel and the inspector shows
// 8, so a renderer reading these has to see the same colour the terminal does.
TEST(theGeometryAndSwatchComeFromTheTerminal)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 30, 100, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(terminal);

    Ubiquitty::TestDoubles::StreamDouble stream;
    stream.facts.currentDirectory = "/tmp/somewhere";
    stream.facts.tty = "ttys009";

    Ubiquitty::Session::SessionInfo info;
    Ubiquitty::Session::gatherSessionInfo(terminal, stream, info);

    CHECK_EQ(info.columns, terminal.columns);
    CHECK_EQ(info.rows, terminal.rows);
    CHECK_CSTR_EQ(info.directory.c_str(), "/tmp/somewhere");
    CHECK_CSTR_EQ(info.tty.c_str(), "ttys009");

    const Ubiquitty::PaletteRGB& background = terminal.palette.defaultBackground;
    CHECK_EQ(static_cast<int>(info.backgroundRed), static_cast<int>(Ubiquitty::channel16To8(background.red)));
    CHECK_EQ(static_cast<int>(info.backgroundGreen), static_cast<int>(Ubiquitty::channel16To8(background.green)));
    CHECK_EQ(static_cast<int>(info.backgroundBlue), static_cast<int>(Ubiquitty::channel16To8(background.blue)));

    // Cleared rather than left alone: the launch command line lives in the
    // frontend, and a stale value here would render as the current one.
    CHECK(info.command.empty());
    CHECK(info.shell.empty());
}

TEST(processRowsFollowAncestryRegardlessOfEnumerationOrder)
{
    Record login;
    login.processID = 40;
    login.isSessionLeader = true;
    Record shell;
    shell.processID = 10;
    shell.parentProcessID = 40;
    Record tmux;
    tmux.processID = 30;
    tmux.parentProcessID = 10;
    tmux.isForeground = true;
    Record worker;
    worker.processID = 20;
    worker.parentProcessID = 10;
    for (const auto& sample : { std::vector<Record> { login, shell, tmux, worker }, std::vector<Record> { worker, tmux, shell, login } }) {
        Ubiquitty::Session::SessionInfo info;
        Ubiquitty::Session::adoptProcesses(info, sample);
        CHECK_EQ(info.processes.size(), 4u);
        CHECK_EQ(info.processes[0].processID, 40u);
        CHECK_EQ(info.processes[1].processID, 10u);
        CHECK_EQ(info.processes[2].processID, 20u);
        CHECK_EQ(info.processes[3].processID, 30u);
        CHECK_EQ(info.foregroundIndex, 3);
    }
}

TEST(partialAndCyclicProcessSamplesKeepEveryRowOnce)
{
    Record orphan;
    orphan.processID = 1;
    orphan.parentProcessID = 99;
    Record first;
    first.processID = 2;
    first.parentProcessID = 3;
    Record second;
    second.processID = 3;
    second.parentProcessID = 2;
    Ubiquitty::Session::SessionInfo info;
    Ubiquitty::Session::adoptProcesses(info, std::vector<Record> { second, orphan, first });
    CHECK_EQ(info.processes.size(), 3u);
    CHECK_EQ(info.processes[0].processID, 1u);
    CHECK_EQ(info.processes[1].processID, 2u);
    CHECK_EQ(info.processes[2].processID, 3u);
}

TEST(processPresentationKeepsCopyIdentityAndUsesSharedNames)
{
    Record login;
    login.name = "login";
    login.commandLine = "login -pf user";
    login.user = "root";
    login.isSessionLeader = true;
    auto shown = Ubiquitty::Session::processPresentation(login);
    CHECK(shown.name == "(login)");
    CHECK(shown.rawName == "login");
    CHECK(shown.arguments == "login -pf user");
    CHECK(shown.marker.empty());
    CHECK(shown.user == "root");
    CHECK(shown.isSecondary);

    Record shell;
    shell.name = "-zsh";
    shell.isForeground = true;
    shown = Ubiquitty::Session::processPresentation(shell);
    CHECK(shown.marker == "$");
    CHECK(shown.name == "-zsh");
    CHECK(!shown.isSecondary);
    CHECK(shown.isForeground);
    shell.isForeground = false;
    CHECK(Ubiquitty::Session::processPresentation(shell).isSecondary);
    shell.isForeground = true;
    shell.user = "root";
    CHECK(Ubiquitty::Session::processPresentation(shell).marker == "#");

    Record script;
    script.processID = 123;
    script.processGroupID = 120;
    script.name = "python3";
    script.commandLine = "python3 /srv/deploy.py --verbose";
    script.isForeground = true;
    shown = Ubiquitty::Session::processPresentation(script);
    CHECK(shown.processID == 123);
    CHECK(shown.processGroupID == 120);
    CHECK(shown.rawName == "python3");
    CHECK(shown.smartName == "deploy.py");
    CHECK(shown.smartArguments == script.commandLine);
    CHECK(shown.marker == "•");
    CHECK(!shown.isSecondary);
    script.isForeground = false;
    CHECK(Ubiquitty::Session::processPresentation(script).marker.empty());
}

TEST(inspectorCaptureUsesCachedProcessesWithoutStartingAnotherWalk)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 30, 100, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(terminal);
    terminal.currentDirectory = "/reported/project";
    terminal.paletteEpoch = 42;
    Ubiquitty::TestDoubles::StreamDouble stream;
    stream.facts.currentDirectory = "/sampled/project";
    stream.facts.tty = "/dev/ttys001";
    auto login = process("login", Foreground::No);
    login.processID = 10;
    login.isSessionLeader = true;
    login.commandLine = "login -pf user";
    auto shell = process("-zsh", Foreground::Yes);
    shell.processID = 11;
    shell.parentProcessID = 10;
    shell.commandLine = "-zsh";
    const auto info = Ubiquitty::Session::captureSessionInfo(&terminal, &stream, { shell, login });
    CHECK_EQ(stream.facts.gatherProbes, 0);
    CHECK_EQ(info.processes.size(), 2u);
    CHECK_EQ(info.processes.front().processID, 10u);
    CHECK(info.command == "login -pf user");
    CHECK(info.shell == "zsh");
    CHECK(info.directory == "/reported/project");
    CHECK(info.tty == "/dev/ttys001");
    CHECK_EQ(info.paletteGeneration, 42u);
    const auto empty = Ubiquitty::Session::captureSessionInfo(&terminal, &stream, {});
    CHECK(empty.processes.empty());
    CHECK_EQ(stream.facts.gatherProbes, 0);
}

TEST(inspectorCaptureKeepsGeometryAndPaletteWithoutALocalStream)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 47, 202, Ubiquitty::Modes::ReflowMode::Fixed);
    terminal.palette.defaultBackground = { 0x1234, 0x5678, 0x9abc };
    const auto info = Ubiquitty::Session::captureSessionInfo(&terminal, nullptr, {});
    CHECK_EQ(info.columns, 202);
    CHECK_EQ(info.rows, 47);
    CHECK_EQ(info.backgroundRed, 0x12);
    CHECK_EQ(info.backgroundGreen, 0x56);
    CHECK_EQ(info.backgroundBlue, 0x9a);
    CHECK_EQ(info.colorAt(17).red, 0x1234);
    CHECK_EQ(info.colorAt(17).green, 0x5678);
    CHECK_EQ(info.colorAt(17).blue, 0x9abc);
    CHECK(Ubiquitty::Session::captureSessionInfo(nullptr, nullptr, {}).processes.empty());
}

TEST(inspectorSignalsOnlyGroupsPresentInTheOwningSessionSample)
{
    Ubiquitty::TestDoubles::StreamDouble stream;
    stream.facts.canSignal = true;
    Record selected;
    selected.processID = 42;
    selected.processGroupID = 40;
    CHECK(!Ubiquitty::Session::signalSessionProcess(&stream, { selected }, 99, 15));
    CHECK(stream.facts.signalledGroups.empty());
    CHECK(Ubiquitty::Session::signalSessionProcess(&stream, { selected }, 42, 15));
    CHECK_EQ(stream.facts.signalledGroups.size(), 1u);
    CHECK_EQ(stream.facts.signalledGroups.front().first, 40);
    CHECK_EQ(stream.facts.signalledGroups.front().second, 15);
    for (auto group : { 0u, 1u, 0xffffffffu }) {
        selected.processGroupID = group;
        CHECK(!Ubiquitty::Session::signalSessionProcess(&stream, { selected }, 42, 15));
    }
    CHECK_EQ(stream.facts.signalledGroups.size(), 1u);
    CHECK(!Ubiquitty::Session::signalSessionProcess(nullptr, { selected }, 42, 15));
    CHECK_EQ(stream.facts.gatherProbes, 0);
}

TEST(inspectorCapturesConcealmentWithoutCopyingTheMessage)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 8);
    Ubiquitty::setAnswerbackString(terminal, "private message");
    auto info = Ubiquitty::Session::captureSessionInfo(&terminal, nullptr, {});
    CHECK(info.canConfigureAnswerback);
    CHECK(info.answerbackDisplay == "private message");
    Ubiquitty::processBytes(&terminal, "\033[?101h");
    info = Ubiquitty::Session::captureSessionInfo(&terminal, nullptr, {});
    CHECK(info.answerbackDisplay == "concealed");
    Ubiquitty::TestDoubles::StreamDouble stream;
    Ubiquitty::Session::gatherSessionInfo(terminal, stream, info);
    CHECK(info.answerbackDisplay == "concealed");
    Ubiquitty::MirrorClient mirror;
    terminal.mirrorClient = &mirror;
    info = Ubiquitty::Session::captureSessionInfo(&terminal, nullptr, {});
    CHECK(!info.canConfigureAnswerback);
    CHECK(info.answerbackDisplay.empty());
    terminal.mirrorClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
