// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/PAL/RemoteTarget.cpp — see RemoteTarget.h.

#include "PAL/RemoteTarget.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace Ubiquitty::PAL::Process {

namespace {

// A tool is a ROW, never a branch. Every shape here is the same grammar with
// different spellings — some option letters take a value, some tools hide the
// connecting verb behind a subcommand — so the engine below reads the row and
// nothing about any particular tool is written into it. Adding `mosh` is a row;
// adding a fifth `if` would be the thing this exists to prevent.
struct Shape {
    // Matched against `basename(argv[0])`, case-insensitively.
    std::string_view tool;
    // Words that must follow the tool, in order, before the destination is
    // meaningful — `gcloud compute ssh`. The tool's own options are skipped
    // while looking for them. Empty when the tool connects by itself.
    std::string_view subcommands;
    RemoteKind kind;
    // Single letters that consume the token after them (or carry it attached).
    std::string_view shortValueOptions;
    // Long options that consume the token after them, space-separated and
    // written without dashes. A `--name=value` spelling needs no entry.
    std::string_view longValueOptions;
    // The letter whose value is the login, when the tool has one.
    char userOption = '\0';
    // The letter whose value is itself the destination, for a tool that names
    // it with an option rather than an operand (`minicom -D /dev/ttyUSB0`).
    char destinationOption = '\0';
    // Whether a destination is required for the claim to stand. `vagrant ssh`
    // with no name still goes to the box this directory owns; `ssh` with no
    // host goes nowhere at all.
    bool requiresDestination = true;
    // Whether `user@host` and a `scheme://` URI are meaningful here. A
    // container name is neither.
    bool acceptsUserAndScheme = false;
    // Whether the destination has to look like a device node. `screen` is a
    // multiplexer until its operand is one.
    bool requiresDeviceDestination = false;
    std::string_view destinationLongOption {};
};

// ssh's own option letters, which several of these borrow wholesale.
constexpr std::string_view sshValueOptions = "bcDEeFIiJLlmOoPpQRSWw";

constexpr std::array shapes {
    // Straight onto a machine.
    Shape { "ssh", "", RemoteKind::Host, sshValueOptions, "", 'l', '\0', true, true, false },
    Shape { "slogin", "", RemoteKind::Host, sshValueOptions, "", 'l', '\0', true, true, false },
    Shape { "autossh", "", RemoteKind::Host, "M"
                                             "bcDEeFIiJLlmOoPpQRSWw",
        "", 'l', '\0', true, true, false },
    Shape { "mosh", "", RemoteKind::Host, "p", "ssh server predict bind-server port", '\0', '\0', true, true, false },
    Shape { "et", "", RemoteKind::Host, "cjpt", "", '\0', '\0', true, true, false },
    Shape { "telnet", "", RemoteKind::Host, "belnSx", "", 'l', '\0', true, true, false },
    Shape { "rlogin", "", RemoteKind::Host, "el", "", 'l', '\0', true, true, false },

    Shape { "ubiquitty-host", "--attach", RemoteKind::Host, "", "socket", '\0', '\0', false, false, false },

    // The connecting verb is a subcommand.
    Shape { "tailscale", "ssh", RemoteKind::Host, "", "socket", '\0', '\0', true, true, false },
    Shape { "tsh", "ssh", RemoteKind::Host, "", "proxy user cluster", '\0', '\0', true, true, false },
    Shape { "gcloud", "compute ssh", RemoteKind::Host, "", "project zone", '\0', '\0', true, true, false },
    Shape { "kitten", "ssh", RemoteKind::Host, sshValueOptions, "", '\0', '\0', true, true, false },
    Shape { "wezterm", "ssh", RemoteKind::Host, "", "", '\0', '\0', true, true, false },

    // A machine this host is running.
    Shape { "vagrant", "ssh", RemoteKind::VirtualMachine, "", "command", '\0', '\0', false, false, false },
    Shape { "limactl", "shell", RemoteKind::VirtualMachine, "", "workdir", '\0', '\0', false, false, false },
    Shape { "multipass", "shell", RemoteKind::VirtualMachine, "", "", '\0', '\0', false, false, false },
    Shape { "virsh", "console", RemoteKind::VirtualMachine, "c", "connect", '\0', '\0', true, false, false },

    // A namespace on some host.
    Shape { "docker", "exec", RemoteKind::Container, "ue", "user env workdir", 'u', '\0', true, false, false },
    Shape { "docker", "attach", RemoteKind::Container, "", "detach-keys", '\0', '\0', true, false, false },
    Shape { "podman", "exec", RemoteKind::Container, "ue", "user env workdir", 'u', '\0', true, false, false },
    Shape { "podman", "attach", RemoteKind::Container, "", "detach-keys", '\0', '\0', true, false, false },
    Shape { "nerdctl", "exec", RemoteKind::Container, "ue", "user env workdir", 'u', '\0', true, false, false },
    Shape { "kubectl", "exec", RemoteKind::Container, "nc", "namespace container context", '\0', '\0', true, false, false },
    Shape { "lxc", "exec", RemoteKind::Container, "", "user env cwd", '\0', '\0', true, false, false },
    Shape { "incus", "exec", RemoteKind::Container, "", "user env cwd", '\0', '\0', true, false, false },
    Shape { "distrobox", "enter", RemoteKind::Container, "n", "name", '\0', 'n', true, false, false, "name" },
    Shape { "toolbox", "enter", RemoteKind::Container, "c", "container", '\0', 'c', true, false, false, "container" },

    // Hardware on the other end of a line.
    Shape { "screen", "", RemoteKind::Serial, "", "", '\0', '\0', true, false, true },
    Shape { "picocom", "", RemoteKind::Serial, "bfdpsr", "baud flow databits", '\0', '\0', true, false, true },
    Shape { "minicom", "", RemoteKind::Serial, "D", "device", '\0', 'D', true, false, true },
    Shape { "cu", "", RemoteKind::Serial, "l", "line", '\0', 'l', true, false, true },
};

// Commands that run another command. The destination belongs to what they
// wrap, so they are stripped and the answer comes from behind them — but they
// have options of their own, so "drop the first token" is not enough.
struct Prefix {
    std::string_view tool;
    std::string_view shortValueOptions;
    // Whether bare `NAME=value` tokens precede the command, as `env`'s do.
    bool acceptsAssignments;
};

constexpr std::array prefixes {
    Prefix { "env", "uC", true },
    Prefix { "sudo", "ugCprh", true },
    Prefix { "doas", "aCu", false },
    Prefix { "nice", "n", false },
    Prefix { "ionice", "cnp", false },
    Prefix { "time", "", false },
    Prefix { "sshpass", "pfePd", false },
    Prefix { "stdbuf", "ioe", false },
    Prefix { "command", "", false },
    Prefix { "exec", "acl", false },
    Prefix { "nohup", "", false },
};

char lowerASCII(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool equalIgnoringCase(std::string_view left, std::string_view right)
{
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(),
            [](char a, char b) { return lowerASCII(a) == lowerASCII(b); });
}

std::string_view baseName(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::vector<std::string_view> splitOnSpaces(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && line[index] == ' ')
            ++index;
        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ')
            ++index;
        if (index > start)
            out.push_back(line.substr(start, index - start));
    }
    return out;
}

bool containsWord(std::string_view haystack, std::string_view word)
{
    for (std::size_t start = 0; start < haystack.size();) {
        std::size_t end = haystack.find(' ', start);
        if (end == std::string_view::npos)
            end = haystack.size();
        if (haystack.substr(start, end - start) == word)
            return true;
        start = end + 1;
    }
    return false;
}

// How many tokens this option occupies, and whether it hands back a value.
struct OptionRead {
    std::size_t tokensUsed = 1;
    std::string_view value;
    char letter = '\0';
    std::string_view longOption;
};

// Reads one option token against a spec. The three spellings that break a naive
// parser all resolve here: an attached value (`-p2222`), a bundle whose LAST
// letter takes the next token (`-vp 2222`), and a long option written with `=`.
OptionRead readOption(std::span<const std::string_view> tokens, std::size_t index,
    std::string_view shortValueOptions, std::string_view longValueOptions)
{
    const std::string_view token = tokens[index];
    OptionRead read;
    if (token.starts_with("--")) {
        const std::string_view body = token.substr(2);
        const std::size_t equals = body.find('=');
        const std::string_view word = body.substr(0, equals);
        if (containsWord(longValueOptions, word)) {
            read.longOption = word;
            if (equals != std::string_view::npos) {
                read.value = body.substr(equals + 1);
            } else if (index + 1 < tokens.size()) {
                read.tokensUsed = 2;
                read.value = tokens[index + 1];
            }
        }
        return read;
    }
    const std::string_view letters = token.substr(1);
    for (std::size_t position = 0; position < letters.size(); ++position) {
        const char letter = letters[position];
        if (shortValueOptions.find(letter) == std::string_view::npos)
            continue;
        read.letter = letter;
        if (position + 1 < letters.size()) {
            // The rest of the token is this option's value: `-p2222`, `-oKey=1`.
            read.value = letters.substr(position + 1);
            return read;
        }
        if (index + 1 < tokens.size()) {
            read.tokensUsed = 2;
            read.value = tokens[index + 1];
        }
        return read;
    }
    return read;
}

bool looksLikeDevice(std::string_view name) { return name.starts_with("/dev/"); }

/// How a destination may be spelled, which differs by what it names.
enum class DestinationSpelling {
    /// A bare name: a container, a pod, a box, a device path.
    Plain,
    /// `[scheme://][user@]host[:port]`, as a network destination is written.
    UserAndScheme,
};

// Peels `scheme://`, then `user@`, then a URI port. The port comes off only in
// the URI form: a bare `::1` is an address, and taking everything after its
// last colon would leave a single colon behind.
void splitDestination(std::string_view raw, DestinationSpelling spelling, std::string& name, std::string& user)
{
    std::string_view rest = raw;
    bool wasURI = false;
    if (spelling == DestinationSpelling::UserAndScheme) {
        const std::size_t scheme = rest.find("://");
        if (scheme != std::string_view::npos) {
            rest = rest.substr(scheme + 3);
            wasURI = true;
        }
        const std::size_t at = rest.find_last_of('@');
        if (at != std::string_view::npos) {
            user = std::string(rest.substr(0, at));
            rest = rest.substr(at + 1);
        }
    }
    if (wasURI) {
        if (rest.starts_with('[')) {
            const std::size_t close = rest.find(']');
            if (close != std::string_view::npos) {
                name = std::string(rest.substr(1, close - 1));
                return;
            }
        }
        const std::size_t colon = rest.find_last_of(':');
        if (colon != std::string_view::npos && rest.find(':') == colon)
            rest = rest.substr(0, colon);
    }
    name = std::string(rest);
}

// Drops every wrapper in front of the real command. `env FOO=1 nice -n 5 ssh`
// has two, so this repeats until the leading token is not one.
std::size_t skipTransparentPrefixes(std::span<const std::string_view> tokens)
{
    std::size_t index = 0;
    for (;;) {
        if (index >= tokens.size())
            return index;
        const std::string_view tool = baseName(tokens[index]);
        const auto match = std::ranges::find_if(prefixes,
            [&](const Prefix& prefix) { return equalIgnoringCase(tool, prefix.tool); });
        if (match == prefixes.end())
            return index;
        ++index;
        while (index < tokens.size()) {
            const std::string_view token = tokens[index];
            if (token.starts_with('-') && token.size() > 1) {
                index += readOption(tokens, index, match->shortValueOptions, "").tokensUsed;
                continue;
            }
            if (match->acceptsAssignments && token.find('=') != std::string_view::npos) {
                ++index;
                continue;
            }
            break;
        }
    }
}

} // namespace

RemoteTarget remoteTargetForArguments(std::span<const std::string_view> arguments)
{
    const std::size_t start = skipTransparentPrefixes(arguments);
    if (start >= arguments.size())
        return {};
    const std::string_view typedTool = baseName(arguments[start]);

    for (const Shape& shape : shapes) {
        if (!equalIgnoringCase(typedTool, shape.tool))
            continue;

        std::size_t index = start + 1;
        std::string via { typedTool };
        bool didMatchSubcommands = true;
        for (const std::string_view word : splitOnSpaces(shape.subcommands)) {
            // The tool's own options come before its verb, so step over them
            // rather than demanding the verb sit at a fixed position.
            while (index < arguments.size() && arguments[index].starts_with('-')
                && arguments[index].size() > 1 && !equalIgnoringCase(arguments[index], word))
                index += readOption(arguments, index, shape.shortValueOptions,
                    shape.longValueOptions)
                             .tokensUsed;
            if (index >= arguments.size() || !equalIgnoringCase(arguments[index], word)) {
                didMatchSubcommands = false;
                break;
            }
            via += ' ';
            via += arguments[index];
            ++index;
        }
        if (!didMatchSubcommands)
            continue;

        std::string_view destination;
        std::string user;
        while (index < arguments.size()) {
            const std::string_view token = arguments[index];
            if (token == "--") {
                if (destination.empty() && index + 1 < arguments.size())
                    destination = arguments[index + 1];
                break;
            }
            if (token.starts_with('-') && token.size() > 1) {
                const OptionRead read = readOption(arguments, index, shape.shortValueOptions,
                    shape.longValueOptions);
                if (read.letter != '\0' && read.letter == shape.userOption)
                    user = std::string(read.value);
                if ((read.letter != '\0' && read.letter == shape.destinationOption)
                    || (!read.longOption.empty() && read.longOption == shape.destinationLongOption))
                    destination = read.value;
                index += read.tokensUsed;
                continue;
            }
            if (destination.empty() && (shape.destinationOption == '\0' || !shape.destinationLongOption.empty()))
                destination = token;
            break;
        }

        if (destination.empty()) {
            if (shape.requiresDestination)
                return {};
            return RemoteTarget { shape.kind, {}, {}, via };
        }
        if (shape.requiresDeviceDestination && !looksLikeDevice(destination))
            return {};

        RemoteTarget target;
        target.kind = shape.kind;
        target.via = via;
        splitDestination(destination,
            shape.acceptsUserAndScheme ? DestinationSpelling::UserAndScheme : DestinationSpelling::Plain,
            target.name, target.user);
        if (target.user.empty())
            target.user = user;
        return target;
    }
    return {};
}

RemoteTarget remoteTargetForCommand(std::string_view commandLine)
{
    const std::vector<std::string_view> tokens = splitOnSpaces(commandLine);
    return remoteTargetForArguments(tokens);
}

} // namespace Ubiquitty::PAL::Process
