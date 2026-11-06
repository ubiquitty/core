// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/App/ShellIntegrationInstall.cpp — see Core/App/ShellIntegrationInstall.h.

#include "Core/App/ShellIntegrationInstall.h"

#include <utility>

namespace Ubiquitty::App::ShellIntegrationInstall {

namespace {

/// A list of names as a sentence joins them, with a serial comma.
std::string listed(const std::vector<std::string>& names)
{
    std::string text;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index > 0)
            text += names.size() == 2 ? " and " : (index + 1 == names.size() ? ", and " : ", ");
        text += names[index];
    }
    return text;
}

} // namespace

std::string scriptName(Shell shell)
{
    switch (shell) {
    case Shell::Zsh:
        return "zsh.sh";
    case Shell::Bash:
        return "bash.sh";
    case Shell::Fish:
        return "fish.fish";
    }
    return "zsh.sh";
}

std::string managedBlock(Shell shell)
{
    const std::string script = "\"$UBIQUITTY_INTEGRATION_DIR/" + scriptName(shell) + "\"";
    std::string block(startMarker);
    block += "\n"
             "# Loads Ubiquitty's shell integration in the shells Ubiquitty starts.\n"
             "# Remove this block to undo.\n";
    if (shell == Shell::Fish) {
        block += "if set -q UBIQUITTY_INTEGRATION_DIR; and test -r " + script + "\n";
        block += "    source " + script + "\n";
        block += "end\n";
    } else {
        block += "if [[ -n \"${UBIQUITTY_INTEGRATION_DIR-}\" && -r " + script + " ]]; then\n";
        block += "    source " + script + "\n";
        block += "fi\n";
    }
    block += endMarker;
    return block;
}

StartupFiles startupFiles()
{
    return {
        { ".zshrc", Shell::Zsh, true },
        { ".bashrc", Shell::Bash, true },
        { ".bash_profile", Shell::Bash, false },
        { ".config/fish/config.fish", Shell::Fish, false },
    };
}

PlannedEdit planEdit(const StartupFile& file, std::string_view contents)
{
    const std::string block = managedBlock(file.shell);
    const std::size_t start = contents.find(startMarker);
    if (start != std::string_view::npos) {
        const std::size_t end = contents.find(endMarker, start);
        if (end == std::string_view::npos)
            return { Edit::Malformed, std::string(contents) };
        std::string replaced;
        replaced.reserve(contents.size() + block.size());
        replaced += contents.substr(0, start);
        replaced += block;
        replaced += contents.substr(end + endMarker.size());
        if (replaced == contents)
            return { Edit::AlreadyCurrent, std::move(replaced) };
        return { Edit::Upgrade, std::move(replaced) };
    }
    // A reader who followed the guide sources the script by the variable's
    // name, with or without braces around it.
    const std::string script = scriptName(file.shell);
    if (contents.find("UBIQUITTY_INTEGRATION_DIR/" + script) != std::string_view::npos
        || contents.find("UBIQUITTY_INTEGRATION_DIR}/" + script) != std::string_view::npos)
        return { Edit::ReaderWritten, std::string(contents) };
    std::string appended(contents);
    appended += (contents.empty() || contents.ends_with('\n')) ? "\n" : "\n\n";
    appended += block;
    appended += "\n";
    return { Edit::Append, std::move(appended) };
}

PlannedEdit planAbsentFile(const StartupFile& file)
{
    if (!file.isCreatedWhenAbsent)
        return { Edit::LeftAbsent, {} };
    return { Edit::Create, managedBlock(file.shell) + "\n" };
}

std::string consentDetail(const PlannedChanges& changes)
{
    if (changes.empty())
        return "None of your shell startup files needs a change. This installs the xterm-ubiquitty terminfo entry.";
    std::vector<std::string> written;
    std::vector<std::string> created;
    for (const PlannedChange& change : changes) {
        written.push_back("~/" + change.relativePath);
        if (change.isNew)
            created.push_back("~/" + change.relativePath);
    }
    std::string detail = "This adds a block to " + listed(written)
        + " that loads Ubiquitty's shell integration in the shells Ubiquitty starts: prompt marks, and the "
          "directory and host each shell is in. It also installs the xterm-ubiquitty terminfo entry.";
    if (created.size() == 1)
        detail += " " + created.front() + " does not exist yet and is created.";
    else if (created.size() > 1)
        detail += " " + listed(created) + " do not exist yet and are created.";
    return detail;
}

} // namespace Ubiquitty::App::ShellIntegrationInstall
