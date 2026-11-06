// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/LaunchCommand.h"

#include "PAL/PAL.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

LaunchCommand LaunchCommand::loginShell(bool interactive)
{
    std::string shell;
    if (const char* env = std::getenv("SHELL"); env && env[0] != '\0')
        shell = env;
    if (shell.empty()) {
        char buffer[1024];
        const int n = PAL::systemLoginShellPath(buffer, static_cast<int>(sizeof(buffer)));
        if (n > 0)
            shell.assign(buffer, static_cast<std::size_t>(n));
    }
    if (shell.empty())
        shell = "/bin/zsh";

    std::vector<std::string> arguments { std::move(shell) };
    if (interactive)
        arguments.emplace_back("-i");
    return LaunchCommand(std::move(arguments));
}

namespace {

// A PAL string seam into a std::string; empty when the seam has no answer.
std::string palString(int (*query)(char*, int))
{
    std::array<char, 1024> buffer {};
    const int n = query(buffer.data(), static_cast<int>(buffer.size()));
    return n > 0 ? std::string(buffer.data(), static_cast<std::size_t>(n)) : std::string();
}

// The trailing component of a path, which is what a shell reads as its own
// name once the leading dash marks it a login shell.
std::string_view fileName(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

LaunchCommand LaunchCommand::loginSession(const std::string& initialDirectory)
{
    const PAL::LoginSessionKind kind = PAL::loginSessionKind();

    LaunchCommand fallback = loginShell();
    fallback.setInitialDirectory(initialDirectory);
    // The plain shell still opens a session where the platform records the
    // pseudo-terminal: the record is written after the fork, so it is the same
    // command either way and only the marking differs.
    if (kind == PAL::LoginSessionKind::ByRecordingThePTY)
        fallback.m_sessionRecord = PAL::SessionRecord::AsLogin;

    // Only the platforms that register by exec take a wrapped argument vector.
    if (kind != PAL::LoginSessionKind::ThroughLoginProgram)
        return fallback;
    const std::string program = palString(&PAL::loginSessionProgram);
    if (program.empty())
        return fallback;
    const std::string user = palString(&PAL::systemLoginUserName);
    if (user.empty())
        return fallback;

    std::vector<std::string> arguments;
    if (initialDirectory.empty()) {
        // The login program's own behaviour is to land in the home directory,
        // which is where a window with nothing to inherit belongs anyway.
        arguments = { program, "-pf", user };
    } else {
        // -l keeps the directory the spawn chdir'd to instead of moving to the
        // home directory. It also stops the program from execing the login
        // shell itself, so the shell is re-execed here with a leading dash in
        // its own name — without that the inherited directory costs the user
        // their login shell's startup files.
        const std::string shell = fallback.executablePath();
        std::string reexec = "exec -la ";
        reexec += fileName(shell);
        reexec += ' ';
        reexec += shell;
        arguments = { program, "-pfl", user, "/bin/bash", "-c", std::move(reexec) };
    }

    LaunchCommand command(std::move(arguments));
    command.setInitialDirectory(initialDirectory);
    return command;
}

LaunchCommand LaunchCommand::fromArguments(std::vector<std::string> arguments) { return LaunchCommand(std::move(arguments)); }

std::string LaunchCommand::displayName() const
{
    std::string joined;
    for (std::size_t i = 0; i < m_arguments.size(); ++i) {
        if (i != 0)
            joined.push_back(' ');
        joined += m_arguments[i];
    }
    return joined;
}

std::string LaunchCommand::displayShellName() const
{
    if (m_arguments.empty())
        return {};
    const std::string& path = m_arguments.front();
    const std::size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    // A login shell's argv[0] is "-zsh"; the leading '-' is not part of the name.
    if (!name.empty() && name.front() == '-')
        name.erase(name.begin());
    return name;
}

std::string LaunchCommand::titleSeedName() const
{
    std::string name = displayShellName();
    if (name.empty() || m_argument0Style != PAL::Argument0Style::LoginShell)
        return name;
    name.insert(name.begin(), '-');
    return name;
}

std::string LaunchCommand::startDirectoryFor(const std::string& inheritedDirectory, const std::string& home)
{
    // Trailing slashes are the one spelling difference worth folding: a
    // sampled directory and a configured home reach here from different
    // sources, and "/Users/you/" names the same place as "/Users/you".
    const auto trimmed = [](std::string_view path) {
        while (path.size() > 1 && path.back() == '/')
            path.remove_suffix(1);
        return path;
    };
    const std::string_view directory = trimmed(inheritedDirectory);
    if (directory.empty() || directory == trimmed(home))
        return {};
    return std::string(directory);
}

LaunchCommand LaunchCommandDraft::build() const
{
    // An empty draft is a window nobody named a program for — an open-with
    // that carried no arguments, a picker that resolved nothing. The login
    // shell is what that window should hold, and answering it here keeps
    // every caller from repeating the same fallback.
    LaunchCommand command = m_arguments.empty()
        ? LaunchCommand::loginShell()
        : LaunchCommand::fromArguments(m_arguments);
    if (!m_initialDirectory.empty())
        command.setInitialDirectory(m_initialDirectory);
    command.setCharacterLocalePolicy(m_characterLocalePolicy);
    // Only a command the caller named can end with its child. An empty draft
    // fell back to the login shell above, and a login shell that exits leaves
    // its window up whatever the caller asked for.
    if (!m_arguments.empty()) {
        command.setArgument0Style(PAL::Argument0Style::ProgramPath);
        command.setSessionLifetime(m_sessionLifetime);
        if (m_tmuxControlHost) {
            command.setTmuxControlHost(*m_tmuxControlHost);
        }
    }
    return command;
}

} // namespace Ubiquitty::Runtime
