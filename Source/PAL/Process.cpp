// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/PAL/Process.cpp -- see Process.h.

#include "PAL/Process.h"

#include "PAL/RemoteTarget.h"

#include <array>
#include <optional>
#if defined(__GLIBC__)
#include <errno.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#endif

namespace Ubiquitty::PAL::Process {

namespace {

const Record* findByProcessID(std::span<const Record> records, uint32_t processID)
{
    if (processID == 0)
        return nullptr;
    for (const Record& record : records) {
        if (record.processID == processID)
            return &record;
    }
    return nullptr;
}

bool hasForegroundChild(std::span<const Record> records, uint32_t processID)
{
    for (const Record& record : records) {
        if (record.isForeground && record.parentProcessID == processID)
            return true;
    }
    return false;
}

std::string_view stripLeadingDash(std::string_view name)
{
    if (!name.empty() && name.front() == '-')
        name.remove_prefix(1);
    return name;
}

// Interpreters that take a script as an operand, and the options of each that
// swallow the token after them.
//
// A value looks like a script more often than not — a load path has slashes, a
// warning filter has dots — so an interpreter without its own option spelling
// gets its flag's argument named instead of its script. That is per-tool work:
// ruby's `-I` is perl's `-I` and node's `--require`, and none of them is
// python's `-X`.
//
// `awk` and `sed` are absent because their operand is program text, and a title
// naming a fragment of an inline program is worse than one naming the tool.
struct Interpreter {
    std::string_view name;
    // Single letters whose value is the next token, or attached to them.
    std::string_view valueOptions;
    // Long options whose value is the next token, space-separated, no dashes.
    std::string_view longValueOptions;
    // Options carrying program text rather than a path: seeing one means there
    // is no script here. Matched as a PREFIX, since the text attaches as often
    // as it follows — `perl -e'print "a/b"'` splits into tokens that carry a
    // slash like any path.
    std::string_view programTextOptions;
    // Whether this tool can name a module: `-m` means one here.
    bool canNameAModule;
};

constexpr Interpreter interpreters[] {
    { "python", "WX", "check-hash-based-pycs", "-c", true },
    { "python2", "WX", "", "-c", true },
    { "python3", "WX", "check-hash-based-pycs", "-c", true },
    { "ruby", "IrECFKS", "", "-e", false },
    { "perl", "ImMFCD", "", "-e -E", false },
    { "node", "r", "require loader import experimental-loader", "-e -p --eval --print", false },
    { "deno", "", "", "--eval", false },
    { "bun", "", "", "-e --eval", false },
    { "php", "dc", "", "-r", false },
    { "lua", "l", "", "-e", false },
    { "luajit", "l", "", "-e", false },
    { "sh", "", "", "-c", false },
    { "bash", "O", "rcfile init-file", "-c", false },
    { "dash", "", "", "-c", false },
    { "ash", "", "", "-c", false },
    { "zsh", "o", "", "-c", false },
    { "ksh", "", "", "-c", false },
    { "csh", "", "", "-c", false },
    { "tcsh", "", "", "-c", false },
    { "fish", "", "", "-c --command", false },
    { "nu", "", "", "-c --commands", false },
    { "elvish", "", "", "-c", false },
    { "xonsh", "", "", "-c", false },
    { "osascript", "e", "", "-e", false },
    { "swift", "", "", "-e", false },
    { "tclsh", "", "", "", false },
    { "wish", "", "", "", false },
    { "julia", "", "", "-e --eval", false },
    { "racket", "", "", "-e", false },
    { "guile", "", "", "-c", false },
    { "janet", "", "", "-e", false },
    { "groovy", "", "", "-e", false },
    { "scala", "", "", "-e", false },
    { "kotlin", "", "", "-e", false },
    { "rscript", "", "", "-e", false },
    { "ts-node", "", "", "-e", false },
    { "tsx", "", "", "-e", false },
    { "pwsh", "", "", "-Command", false },
    { "powershell", "", "", "-Command", false },
};

bool equalsIgnoringCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char here = left[index];
        const char lowered = (here >= 'A' && here <= 'Z') ? static_cast<char>(here - 'A' + 'a') : here;
        if (lowered != right[index])
            return false;
    }
    return true;
}

// Whether a space-separated list of dashless long names holds this one.
bool namesLongOption(std::string_view list, std::string_view candidate)
{
    for (std::size_t start = 0; start < list.size();) {
        std::size_t end = list.find(' ', start);
        if (end == std::string_view::npos)
            end = list.size();
        if (list.substr(start, end - start) == candidate)
            return true;
        start = end + 1;
    }
    return false;
}

// Whether a token opens program text, attached or not. `-e` and `-e'print 1'`
// are the same option, and the second is the one that leaks code into a title.
bool opensProgramText(std::string_view list, std::string_view token)
{
    for (std::size_t start = 0; start < list.size();) {
        std::size_t end = list.find(' ', start);
        if (end == std::string_view::npos)
            end = list.size();
        const std::string_view option = list.substr(start, end - start);
        if (!option.empty() && token.starts_with(option))
            return true;
        start = end + 1;
    }
    return false;
}

// The interpreter a basename names, allowing the version suffix these are
// installed and invoked under: `python3.12`, `ruby3.3`, `php8`. Stripping a
// trailing run of digits and dots leaves the name to look up, and leaves a word
// that merely ends in a digit — `log4j` — untouched.
const Interpreter* interpreterFor(std::string_view basename)
{
    std::string_view stem = basename;
    while (!stem.empty() && ((stem.back() >= '0' && stem.back() <= '9') || stem.back() == '.'))
        stem.remove_suffix(1);
    for (const Interpreter& candidate : interpreters) {
        if (equalsIgnoringCase(basename, candidate.name) || (!stem.empty() && equalsIgnoringCase(stem, candidate.name)))
            return &candidate;
    }
    return nullptr;
}

std::string_view lastPathComponent(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Whether a token names a script rather than a subcommand or a flag.
//
// A path or a filename with an extension is one; a bare word is not. That is
// what keeps `deno run serve.ts` from being called `run`, while recognizing
// `deploy.py` as the script in `python3 deploy.py` without a path prefix.
bool looksLikeAScript(std::string_view token)
{
    if (token.empty() || token.front() == '-')
        return false;
    return token.find('/') != std::string_view::npos
        || lastPathComponent(token).find('.') != std::string_view::npos;
}

std::string_view nextToken(std::string_view line, std::size_t& cursor)
{
    while (cursor < line.size() && line[cursor] == ' ')
        ++cursor;
    const std::size_t start = cursor;
    while (cursor < line.size() && line[cursor] != ' ')
        ++cursor;
    return line.substr(start, cursor - start);
}

// Where argv[0] ends in a command line that joins argv with single spaces,
// for a name holding a space. Such a name is argv[0]'s basename kept whole
// (PTYFacts.cpp), so argv[0] runs to where that name first ends a path
// component before a space or the line's end, not to the first space.
//
// - Returns: The offset past argv[0]; nothing for a name with no space, whose
//   argv[0] is the line's first token, or a line that spells no such name.
std::optional<std::size_t> spacedArgumentZeroEnd(std::string_view name, std::string_view commandLine)
{
    if (name.find(' ') == std::string_view::npos)
        return std::nullopt;
    for (std::size_t at = commandLine.find(name); at != std::string_view::npos; at = commandLine.find(name, at + 1)) {
        const std::size_t end = at + name.size();
        if ((at == 0 || commandLine[at - 1] == '/') && (end == commandLine.size() || commandLine[end] == ' '))
            return end;
    }
    return std::nullopt;
}

std::string_view displayName(const Record& record)
{
    return scriptDisplayName(record.name, record.commandLine);
}

static void appendLeafCommand(std::string& out, const Record& record, ChainArguments arguments)
{
    const std::string_view shown = displayName(record);

    // The option governs arguments; where a session leads is not one of them.
    // `ssh` alone names the program rather than the session, so a reader with
    // three windows open on three machines reads the same word three times,
    // and so does a window tag. Under OmitArguments the destination is added
    // and the flags that carried it are not.
    if (arguments == ChainArguments::OmitArguments) {
        const RemoteTarget target = remoteTargetForCommand(record.commandLine);
        if (target.isRemote() && !target.name.empty()) {
            out.append(target.via);
            out.push_back(' ');
            out.append(target.name);
            return;
        }
    }

    out.append(shown);

    const std::string_view commandLine = record.commandLine;
    const std::size_t space = spacedArgumentZeroEnd(record.name, commandLine).value_or(commandLine.find(' '));
    if (space == std::string_view::npos || space + 1 >= commandLine.size())
        return;

    // The option is absolute: arguments, or none. A length test would make it
    // mean "arguments, sometimes" — shown for a short invocation and hidden for
    // a long one, so which answer a reader gets would depend on how the program
    // was spelled rather than on what they chose.
    if (arguments == ChainArguments::IncludeArguments)
        out.append(commandLine.substr(space));
}

void appendSegment(std::string& out, std::string_view segment, Ubiquitty::PAL::Process::TitleSeparator separator)
{
    if (segment.empty())
        return;
    if (!out.empty())
        out.append(separator == Ubiquitty::PAL::Process::TitleSeparator::Bare ? "—" : " — ");
    out.append(segment);
}

} // namespace

bool isLoginShell(std::string_view name, SessionLeaderStatus leaderStatus)
{
    if (leaderStatus == SessionLeaderStatus::Leader)
        return true;
    return !name.empty() && name.front() == '-';
}

bool safeToClose(std::string_view name)
{
    std::string_view base = stripLeadingDash(name);
    if (base.empty())
        return false;
    static constexpr std::array<std::string_view, 15> allowlist {
        "login",
        "zsh",
        "bash",
        "sh",
        "dash",
        "tcsh",
        "csh",
        "ksh",
        "fish",
        "ssh",
        "slogin",
        "tmux",
        "screen",
        "mosh",
        "et",
    };
    for (std::string_view entry : allowlist) {
        if (base == entry)
            return true;
    }
    return false;
}

std::string_view marker(std::string_view name, SessionLeaderStatus leaderStatus, UserPrivilege privilege)
{
    if (isLoginShell(name, leaderStatus))
        return privilege == UserPrivilege::Root ? "#" : "$";
    if (!safeToClose(name))
        return "•";
    return {};
}

bool isForegroundControlModeTmux(std::span<const Record> records)
{
    for (const Record& record : records) {
        if (record.isForeground && record.name == "tmux" && record.commandLine.find("-CC") != std::string::npos)
            return true;
    }
    return false;
}

std::string_view hostProcessName()
{
    // Three answers to one question. glibc publishes the string as a global;
    // Apple and the BSDs supply getprogname(); musl, and the wasm build on top
    // of it, has neither. That last host also has no process table to walk, so
    // nothing can observe a child between fork and exec there — the empty name
    // is never compared against anything.
#if defined(__GLIBC__)
    return program_invocation_short_name ? std::string_view(program_invocation_short_name) : std::string_view {};
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    const char* name = getprogname();
    return name ? std::string_view(name) : std::string_view {};
#else
    return {};
#endif
}

bool shouldRewalkForegroundChain(uint32_t foregroundGroup, uint32_t chainProcessGroup, bool hasCachedChain, bool processNameChanged, int chainForegroundCount, bool treeChanged) { return foregroundGroup == 0 || foregroundGroup != chainProcessGroup || !hasCachedChain || processNameChanged || chainForegroundCount > 1 || treeChanged; }

uint32_t foregroundLeafProcessID(std::span<const Record> records)
{
    for (const Record& record : records) {
        if (!record.isForeground)
            continue;
        if (record.isSessionLeader)
            continue;
        if (!hasForegroundChild(records, record.processID))
            return record.processID;
    }
    return 0;
}

const Record* sessionLeader(std::span<const Record> records)
{
    for (const Record& record : records) {
        if (record.isSessionLeader)
            return &record;
    }
    return nullptr;
}

const Record* effectiveShellLeader(std::span<const Record> records)
{
    const Record* leader = sessionLeader(records);
    if (!leader)
        return nullptr;
    // On macOS the PTY's session leader is the login(1) wrapper; the user's
    // shell is its login-shell child. Every "is this the shell?" judgment must
    // use that child, or the wrapper leaks into titles ("login" as the shell
    // segment) and the real shell escapes chain parent-suppression.
    if (stripLeadingDash(leader->name) != "login")
        return leader;
    for (const Record& record : records) {
        if (record.parentProcessID == leader->processID && isLoginShell(record.name, SessionLeaderStatus::NonLeader))
            return &record;
    }
    return leader; // wrapper with no shell child yet (mid-spawn) — better than nullptr
}

std::string_view shellCommandDisplayName(const Record& record)
{
    return stripLeadingDash(record.name); // "-zsh" (argv0, chain's spelling) → "zsh"
}

std::string_view scriptDisplayName(std::string_view name, std::string_view commandLine)
{
    if (commandLine.empty())
        return name;

    std::size_t cursor = 0;
    std::string_view program = nextToken(commandLine, cursor);
    if (const std::optional<std::size_t> end = spacedArgumentZeroEnd(name, commandLine)) {
        program = commandLine.substr(0, *end);
        cursor = *end;
    }
    if (program.empty())
        return name;

    // argv[0]'s own basename leads. A script run by its shebang holds the
    // script there while the kernel reports the interpreter it exec'd, so this
    // is what makes `./deploy.py` read as itself rather than as `Python`.
    const std::string_view leading = lastPathComponent(program);
    if (leading.empty())
        return name;
    const Interpreter* interpreter = interpreterFor(leading);
    if (!interpreter)
        return leading;

    for (;;) {
        const std::size_t before = cursor;
        const std::string_view token = nextToken(commandLine, cursor);
        if (token.empty())
            break;
        if (cursor == before)
            break;

        if (opensProgramText(interpreter->programTextOptions, token))
            break;

        if (interpreter->canNameAModule && (token == "-m" || token == "--module")) {
            const std::string_view module = nextToken(commandLine, cursor);
            return module.empty() ? leading : module;
        }

        if (token.starts_with("--")) {
            const std::string_view body = token.substr(2);
            // `--name=value` carries its own value; only the spaced form eats
            // the token after it.
            if (body.find('=') == std::string_view::npos && namesLongOption(interpreter->longValueOptions, body))
                nextToken(commandLine, cursor);
            continue;
        }

        if (token.size() > 1 && token.front() == '-') {
            // A bundle's value attaches to the letter that takes one, so only a
            // bundle ENDING in such a letter reaches for the next token.
            const char last = token.back();
            if (interpreter->valueOptions.find(last) != std::string_view::npos)
                nextToken(commandLine, cursor);
            continue;
        }

        if (looksLikeAScript(token))
            return lastPathComponent(token);
    }
    return leading;
}

// The process that owns the connection a foreground leaf belongs to: the
// outermost process between the leaf and the shell that leads off this
// machine, or null when none does. A client's own children are its transport,
// never its destination: `ssh -J jump host` runs `ssh -W host:22 jump` beneath
// it, and a ProxyCommand runs `nc` or another `ssh`, each in the client's
// foreground group, so the deepest process naming a host names the jump and
// the client nearest the shell names where the session went.
static const Record* connectionOwner(std::span<const Record> records, const Record* leaf, const Record* shell)
{
    const Record* owner = nullptr;
    const Record* record = leaf;
    for (std::size_t remaining = records.size(); record && remaining; --remaining) {
        if (record->isSessionLeader || record == shell)
            break;
        if (remoteTargetForCommand(record->commandLine).isRemote())
            owner = record;
        record = findByProcessID(records, record->parentProcessID);
    }
    return owner;
}

std::string formatChain(std::span<const Record> records, ChainArguments arguments)
{
    if (records.empty())
        return {};

    const Record* shell = effectiveShellLeader(records);
    const Record* leaf = findByProcessID(records, foregroundLeafProcessID(records));
    // A connection is named by the client that owns it, never by its
    // transport helper.
    const Record* owner = connectionOwner(records, leaf, shell);

    // The process holding the terminal leads, without exception. What is shown beside it
    // is one related process, and the arrow points at whichever of the pair is
    // the child:
    //
    //   "mc ▸ vi"      mc holds the terminal and vi is mc's child
    //   "vi ◂ claude"  vi holds the terminal and claude is vi's parent
    //
    // Same relationship spelled from opposite ends, which is why both glyphs
    // are needed and why the leader is never the one that moves. Running vi
    // from mc leaves mc in front, because mc gives vi a pty of its own; running
    // vi from claude puts vi in front, because claude steps back.
    const Record* holder = owner ? owner : leaf;
    if (!holder) {
        // Only the shell holds the terminal: a prompt. Name the effective
        // shell, so a login(1)-wrapped session reads "-zsh" and never "login".
        //
        // Nothing is shown beside it. A backgrounded job is a child of the
        // shell that the reader pushed out of the way, and naming it would make
        // the title follow a job the reader pushed into the background.
        return shell ? std::string(displayName(*shell)) : std::string();
    }

    std::string out;
    appendLeafCommand(out, *holder, arguments);

    // A descendant first: a program that keeps the terminal while running
    // something beneath it is describing what it is doing, and that is the more
    // useful of the two. Only a lone child qualifies — several is ambiguous,
    // and the walk should not pick one and present it as the answer.
    // The DEEPEST such descendant, not the first: mc parks a subshell and runs
    // the editor beneath it, and the reader means the editor. Walking down while
    // each step has exactly one child gets there, and stops the moment a step is
    // ambiguous. Bounded so a pathological tree cannot make a title unbounded work.
    // A remote client's children are its transport, which says nothing of
    // what the reader is doing, so a connection's owner is never followed down.
    constexpr int descentLimit = 8;
    const Record* descendant = nullptr;
    const Record* from = holder;
    for (int depth = 0; depth < descentLimit && !owner; ++depth) {
        const Record* onlyChild = nullptr;
        int childCount = 0;
        for (const Record& candidate : records) {
            if (candidate.parentProcessID != from->processID || candidate.processID == from->processID)
                continue;
            if (candidate.isSessionLeader || candidate.name.empty())
                continue;
            ++childCount;
            onlyChild = &candidate;
        }
        if (childCount != 1 || !onlyChild)
            break;
        descendant = onlyChild;
        from = onlyChild;
    }
    // A step between two processes the chain would spell alike says nothing,
    // and is not taken: Neovim's interface runs its editor as a child of the
    // same name, which read "nvim ▸ nvim". The names are compared, not the
    // spellings, so the holder's arguments do not make the step informative —
    // the other end is spelled without them either way.
    const std::string_view holderName = displayName(*holder);
    if (descendant && displayName(*descendant) != holderName) {
        out.append(chainArrow);
        out.append(displayName(*descendant));
        return out;
    }

    // Otherwise the ancestor, which says where this program was started from.
    // Suppressed when it is the shell (judged against the EFFECTIVE shell
    // leader, so a login(1)-wrapped startup subshell renders "-zsh" and not
    // "-zsh ◂ -zsh") or the session leader itself (under login the leaf at a
    // prompt is the shell and its parent is the login leader, without which
    // every prompt would render "-zsh ◂ login"). Both guards are needed.
    const Record* parent = findByProcessID(records, holder->parentProcessID);
    if (parent && parent != shell && !parent->isSessionLeader && parent->processID != holder->processID && !parent->name.empty()
        && displayName(*parent) != holderName) {
        out.append(chainParentArrow);
        out.append(displayName(*parent));
    }
    return out;
}

RemoteTarget remoteTargetForChain(std::span<const Record> records)
{
    const Record* owner = connectionOwner(records, findByProcessID(records, foregroundLeafProcessID(records)), effectiveShellLeader(records));
    return owner ? remoteTargetForCommand(owner->commandLine) : RemoteTarget {};
}

std::string formatTitle(std::string_view lead, std::string_view osc, std::string_view chain, int columns, int rows, DimensionFormat dimensions, TitleSeparator separator)
{
    std::string out;
    appendSegment(out, lead, separator);
    appendSegment(out, osc, separator);
    appendSegment(out, chain, separator);

    if (dimensions == DimensionFormat::IncludeDimensions && columns > 0 && rows > 0) {
        std::string dims = std::to_string(columns);
        dims.append("×");
        dims.append(std::to_string(rows));
        appendSegment(out, dims, separator);
    }
    return out;
}

} // namespace Ubiquitty::PAL::Process
