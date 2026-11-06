// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC shell-integration slice: FinalTerm prompt marks (OSC 133) and VS Code (OSC 633), which maps A/B/C/D
// onto the same mark machinery plus the command line (633;E) and properties (633;P). OSC 1337 SetMark
// reuses setPromptMark.

#include "Core/CellPen.h"
#include "Core/OSC/Internal.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Margins.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"
#include <algorithm>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace Ubiquitty::OSC {

// Set a prompt mark on the cursor row. Shared by OSC 133, OSC 633, and OSC 1337 SetMark.
void setPromptMark(Ubiquitty::Terminal& t, uint8_t bit)
{
    if (bit && !t.screens.isUsingAlternateScreen && t.grid) {
        int row = t.screens.active->cursorY;
        if (row >= 0 && row < maxRows)
            t.screens.active->mark[row] = static_cast<uint8_t>(t.screens.active->mark[row] | bit);
    }
}

// Remember the row of a prompt start (A), so its visible mark can be deferred
// until a command actually runs. Stored as an absolute row so a screen that
// scrolls between A and C still resolves back to the right line.
void recordPendingPromptMark(Ubiquitty::Terminal& t)
{
    t.hasMarkedPrompt = true;
    finishProgramActivity(t);
    if (!t.screens.isUsingAlternateScreen && t.grid)
        t.pendingPromptRow = Ubiquitty::Screen::scrollbackCount(&t) + t.screens.active->cursorY;
}

// Promote the prompt row recorded at A (recordPendingPromptMark) to a visible
// osc133MarkCmdExecuted, fired at command start (C). The stored absolute
// row stays valid across a scroll between A and C; addRowMark writes whether
// it is still live or has slid into history. Clears the slot so each prompt
// promotes at most once.
void promotePendingPromptMark(Ubiquitty::Terminal& t)
{
    const int32_t absoluteRow = t.pendingPromptRow;
    t.pendingPromptRow = -1;
    if (absoluteRow < 0 || t.screens.isUsingAlternateScreen || !t.grid)
        return;
    Ubiquitty::Screen::addRowMark(&t, absoluteRow, Screen::osc133MarkCmdExecuted);
}

static void freshPromptLine(Terminal& t)
{
    auto& screen = *t.screens.active;
    const int home = Screen::lineHomeColumn(t);
    if (screen.cursorX != home || screen.wrapPending) {
        VT::finishAlignedTabString(t);
        screen.cursorX = home;
        VT::linefeed(t);
    }
}

static std::string_view commandApplicationID(const Terminal::ShellCommand& command)
{
    return { command.applicationID.data(), command.applicationIDLength };
}

static bool commandRowsAreCurrent(const Terminal& t, const Terminal::ShellCommand& command)
{
    return command.rowEpoch == t.absoluteRowEpoch && command.historyEpochBase == t.historyEpochBase;
}

static void completeShellCommand(Terminal& t, int32_t code)
{
    t.lastCommandExitCode = code;
    const auto slot = t.commandCompletedGeneration % t.commandExitCodeRing.size();
    t.commandExitCodeRing[slot] = code;
    t.commandStartedAtCompletionRing[slot] = t.commandStartedGeneration;
    const std::optional<Ticks> now = terminalNow(t);
    t.commandDurationTicksRing[slot] = (t.hasCommandStartTick && now)
        ? now->since(Ticks(t.commandStartTick))
        : Screen::commandDurationUnknown;
    ++t.commandCompletedGeneration;
    setPromptMark(t, Screen::osc133MarkCmdEnd);
    if (code > 0 && !t.screens.isUsingAlternateScreen && t.grid) {
        int promptRow = -1;
        bool shouldFindPrompt = !t.shellCommandCount;
        if (t.shellCommandCount) {
            const auto& command = (*t.shellCommands)[t.shellCommandCount - 1];
            if (commandRowsAreCurrent(t, command))
                promptRow = command.promptRow;
            else
                shouldFindPrompt = command.promptRow >= 0 && !command.hasNestedCommand;
        }
        if (shouldFindPrompt) {
            const int cursorRow = Screen::scrollbackCount(&t) + t.screens.active->cursorY;
            promptRow = Screen::findPromptMarkBefore(&t, cursorRow + 1, Screen::osc133MarkCmdExecuted);
        }
        if (promptRow >= 0)
            Screen::addRowMark(&t, promptRow, Screen::osc133MarkCmdFailed);
    }
    t.hasCommandStartTick = false;
    t.pendingPromptRow = -1;
    t.isReadingCommand = false;
    t.shouldEndSemanticRoleAtLineBreak = false;
    if (t.shellCommandCount) {
        (*t.shellCommands)[--t.shellCommandCount] = {};
        if (t.shellCommandCount) {
            const auto& parent = (*t.shellCommands)[t.shellCommandCount - 1];
            t.pendingPromptRow = commandRowsAreCurrent(t, parent) ? parent.pendingPromptRow : -1;
            t.commandStartTick = parent.startTick;
            t.hasCommandStartTick = parent.hasStartTick;
            t.isReadingCommand = parent.isReading;
            t.shouldEndSemanticRoleAtLineBreak = parent.shouldEndRoleAtLineBreak;
            if (!t.screens.isUsingAlternateScreen)
                t.screens.active->pen.semanticRole = parent.role;
        }
    }
}

static bool beginShellCommand(Terminal& t, std::string_view payload)
{
    std::string_view applicationID;
    for (auto options = payload.substr(1); !options.empty();) {
        options.remove_prefix(1);
        const auto end = options.find(';');
        const auto option = options.substr(0, end);
        if (option.starts_with("aid="))
            applicationID = option.substr(4);
        options = end == std::string_view::npos ? std::string_view {} : options.substr(end);
    }
    if (applicationID.size() > Terminal::ShellCommand {}.applicationID.size())
        return false;
    for (const unsigned char byte : applicationID) {
        if (byte < 0x20 || byte == 0x7f)
            return false;
    }
    if (!t.shellCommands) {
        t.shellCommands.reset(new (std::nothrow) std::array<Terminal::ShellCommand, Terminal::shellCommandCapacity>);
        if (!t.shellCommands)
            return false;
    }
    if (payload.front() == 'N') {
        for (size_t index = t.shellCommandCount; index; --index) {
            if (commandApplicationID((*t.shellCommands)[index - 1]) != applicationID)
                continue;
            while (t.shellCommandCount >= index)
                completeShellCommand(t, -1);
            break;
        }
    }
    // A and N each begin with a fresh-line: N is "Same as A" in the
    // semantic-prompts proposal, and A's first step is "do a fresh-line".
    freshPromptLine(t);
    if (t.shellCommandCount) {
        auto& parent = (*t.shellCommands)[t.shellCommandCount - 1];
        parent.hasNestedCommand = true;
        parent.pendingPromptRow = t.pendingPromptRow;
        parent.startTick = t.commandStartTick;
        parent.hasStartTick = t.hasCommandStartTick;
        parent.isReading = t.isReadingCommand;
        parent.shouldEndRoleAtLineBreak = t.shouldEndSemanticRoleAtLineBreak;
        parent.role = t.screens.active->pen.semanticRole;
    }
    if (t.shellCommandCount == Terminal::shellCommandCapacity) {
        std::move(t.shellCommands->begin() + 1, t.shellCommands->end(), t.shellCommands->begin());
        --t.shellCommandCount;
    }
    auto& command = (*t.shellCommands)[t.shellCommandCount++];
    command = {};
    std::ranges::copy(applicationID, command.applicationID.begin());
    command.applicationIDLength = static_cast<uint16_t>(applicationID.size());
    command.rowEpoch = t.absoluteRowEpoch;
    command.historyEpochBase = t.historyEpochBase;
    t.hasCommandStartTick = false;
    recordPendingPromptMark(t);
    t.isReadingCommand = false;
    return true;
}

void handleOSC133(Ubiquitty::Terminal& t, std::string_view p)
{
    if (p.empty() || (p.size() > 1 && p[1] != ';'))
        return;
    uint8_t bit = 0;
    SemanticRole role = t.screens.active->pen.semanticRole;
    switch (p.front()) {
    case 'L':
        freshPromptLine(t);
        break;
    case 'P': {
        bool isRightPrompt = false;
        for (std::string_view options = p.substr(1); !options.empty();) {
            options.remove_prefix(1);
            const auto end = options.find(';');
            const auto option = options.substr(0, end);
            if (option.starts_with("k="))
                isRightPrompt = option == "k=r";
            options = end == std::string_view::npos ? std::string_view {} : options.substr(end);
        }
        if (!t.screens.isUsingAlternateScreen)
            t.shouldEndSemanticRoleAtLineBreak = isRightPrompt;
        role = SemanticRole::Prompt;
        bit = Screen::osc133MarkPromptStart;
        break;
    }
    case 'N':
    case 'A':
        if (t.screens.isUsingAlternateScreen || !beginShellCommand(t, p))
            return;
        role = SemanticRole::Prompt;
        bit = Screen::osc133MarkPromptStart;
        break;
    case 'I':
    case 'B':
        role = SemanticRole::Input;
        bit = Screen::osc133MarkCmdStart;
        t.isReadingCommand = true;
        break;
    case 'C':
        role = SemanticRole::Output;
        bit = Screen::osc133MarkOutputStart;
        t.isReadingCommand = false;
        if (t.shellCommandCount && !t.screens.isUsingAlternateScreen) {
            auto& command = (*t.shellCommands)[t.shellCommandCount - 1];
            command.promptRow = t.pendingPromptRow;
            command.rowEpoch = t.absoluteRowEpoch;
            command.historyEpochBase = t.historyEpochBase;
        }
        promotePendingPromptMark(t); // command is running — mark its prompt
        if (const std::optional<Ticks> now = Ubiquitty::terminalNow(t)) {
            t.commandStartTick = now->count;
            t.hasCommandStartTick = true;
        }
        ++t.commandStartedGeneration;
        break;
    case 'D': {
        role = SemanticRole::Output;
        bit = Screen::osc133MarkCmdEnd;
        t.isReadingCommand = false;
        // Optional exit status "D;n" — capture it (absent code → -1) and
        // signal completion regardless of screen, so a command that ends on
        // the alt screen still surfaces. Digits only; clamp absurd values.
        int32_t code = -1;
        if (p.size() >= 3 && p[1] == ';' && p[2] >= '0' && p[2] <= '9') {
            code = 0;
            for (const char digit : p.substr(2)) {
                if (digit < '0' || digit > '9')
                    break;
                code = code * 10 + (digit - '0');
                if (code > 65535) {
                    code = 65535;
                    break;
                }
            }
        }
        completeShellCommand(t, code);
        if (t.shellCommandCount)
            role = t.screens.active->pen.semanticRole;
        break;
    }
    default:
        break;
    }
    if (bit && !t.screens.isUsingAlternateScreen && t.grid) {
        if (p.front() != 'P' && p.front() != 'D')
            t.shouldEndSemanticRoleAtLineBreak = p.front() == 'I';
        t.screens.active->pen.semanticRole = role;
        int row = t.screens.active->cursorY;
        if (row >= 0 && row < maxRows)
            t.screens.active->mark[row] = static_cast<uint8_t>(t.screens.active->mark[row] | bit);
    }
}

// Decode VS Code's OSC 633;E command-line escaping into Terminal::commandLine.
// Per the shell-integration spec the wire escapes (a) any byte via "\xAB"
// (two hex digits, case-insensitive) and (b) a literal backslash via "\\";
// it is required to escape ';' (0x3b) and every char ≤ 0x20 (notably '\n'),
// so the decoded text can carry semicolons and newlines verbatim. `src` runs
// up to `end` (the leading "E;" already stripped, any trailing ";<nonce>"
// excluded by the caller). A malformed escape ('\' at end, "\x" with <2 hex
// digits) is passed through literally rather than dropping the command line.
// The result is NUL-terminated and truncated to the buffer;
// commandLineGeneration bumps so a consumer can detect the fresh value.
static void setCommandLineFromOSC633E(Ubiquitty::Terminal& t, std::string_view src)
{
    std::string decoded;
    while (!src.empty()) {
        if (src.front() == '\\') {
            if (src.size() >= 2 && src[1] == '\\') {
                decoded.push_back('\\');
                src.remove_prefix(2);
                continue;
            }
            if (src.size() >= 4 && (src[1] == 'x' || src[1] == 'X')) {
                const int hi = hexDigit(src[2]);
                const int lo = (hi >= 0) ? hexDigit(src[3]) : -1;
                if (hi >= 0 && lo >= 0) {
                    decoded.push_back(static_cast<char>((hi << 4) | lo));
                    src.remove_prefix(4);
                    continue;
                }
            }
        }
        decoded.push_back(src.front());
        src.remove_prefix(1);
    }
    Ubiquitty::setCommandLineString(t, decoded);
    ++t.commandLineGeneration;
}

// OSC 633;P — VS Code shell-integration property "Key=Value". The only
// property with terminal-visible effect is Cwd (the working directory, the
// bare-path analog of OSC 7's file:// URL); it routes to the same
// currentDirectory sink + shellClient hook. IsWindows / HasRichCommandDetection
// and any unknown key are accepted and ignored gracefully.
static void handleOSC633Property(Ubiquitty::Terminal& t, std::string_view p)
{
    static constexpr std::string_view cwdKey = "Cwd=";
    if (!p.starts_with(cwdKey))
        return;
    const std::string_view path = p.substr(cwdKey.size());
    // SetWorkingDirectory: OSC 633;P;Cwd= (VS Code shell integration) sets
    // the session CWD — the same daemon-read metadata sink as OSC 7 plus the
    // shellClient hook. Origin-aware; a denied origin skips both writes
    // below. An unset policy answers PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetWorkingDirectory))
        return; // host policy denied the working-directory write — skip it
    setCurrentDirectory(t, path);
    // The stored value, not the raw payload: the store filtered it.
    if (auto* shell = t.shellClient.get())
        shell->setCurrentDirectory(t.currentDirectory);
}

// OSC 633 — VS Code shell integration. Its A/B/C/D sub-commands map onto the
// FinalTerm OSC 133 prompt-mark machinery (prompt / command / output /
// command-end). 633;E carries the explicit command line (stored unescaped on
// Terminal::commandLine); 633;P;Key=Value a property (Cwd routes to currentDirectory).
// Unknown sub-commands are ignored gracefully.
void handleOSC633(Ubiquitty::Terminal& t, std::string_view p)
{
    if (p.empty() || (p.size() > 1 && p[1] != ';'))
        return;
    const auto setRole = [&](SemanticRole role) {
        if (!t.screens.isUsingAlternateScreen) {
            t.screens.active->pen.semanticRole = role;
            t.shouldEndSemanticRoleAtLineBreak = false;
        }
    };
    switch (p.front()) {
    case 'A':
        setRole(SemanticRole::Prompt);
        setPromptMark(t, Screen::osc133MarkPromptStart);
        recordPendingPromptMark(t); // defer the visible mark until C
        t.isReadingCommand = false;
        break;
    case 'B':
        setRole(SemanticRole::Input);
        setPromptMark(t, Screen::osc133MarkCmdStart);
        t.isReadingCommand = true;
        break;
    case 'C':
        setRole(SemanticRole::Output);
        setPromptMark(t, Screen::osc133MarkOutputStart);
        promotePendingPromptMark(t); // command is running — mark its prompt
        ++t.commandStartedGeneration;
        t.isReadingCommand = false;
        break;
    case 'D':
        setRole(SemanticRole::Output);
        setPromptMark(t, Screen::osc133MarkCmdEnd);
        t.isReadingCommand = false;
        break;
    case 'E':
        // 633;E;<commandline>[;<nonce>] — strip the leading "E;", drop a
        // trailing ";<nonce>" (the spec's optional spoof-guard token), then
        // unescape. A bare "E" with no ';' carries no command line — ignore it.
        if (p.size() >= 2 && p[1] == ';') {
            const std::string_view body = p.substr(2);
            // The command line escapes its own semicolons as "\x3b", so any
            // literal ';' in the body is the nonce delimiter — the last one
            // bounds the command line. VS Code emits at most one nonce.
            setCommandLineFromOSC633E(t, body.substr(0, body.rfind(';')));
        }
        break;
    case 'P':
        // 633;P;Key=Value
        if (p.size() >= 2 && p[1] == ';')
            handleOSC633Property(t, p.substr(2));
        break;
    default:
        // Any other sub-command: accepted + consumed, no dedicated storage.
        break;
    }
}

} // namespace Ubiquitty::OSC
