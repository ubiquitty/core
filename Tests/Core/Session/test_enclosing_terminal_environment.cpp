// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_enclosing_terminal_environment.cpp — the variables that describe the
// terminal or multiplexer a launcher was started in
// (Core/Session/EnclosingTerminalEnvironment.h) are recognized by their whole
// name, and nothing else in an environment is.

#include "Harness.h"

#include "Core/Session/EnclosingTerminalEnvironment.h"

#include <string>
#include <string_view>

using Ubiquitty::Session::enclosingTerminalEnvironmentNames;
using Ubiquitty::Session::enclosingTerminalEnvironmentPrefixes;
using Ubiquitty::Session::isEnclosingTerminalEnvironmentEntry;

TEST(everyEnclosingTerminalVariableIsRecognized)
{
    for (const std::string_view name : enclosingTerminalEnvironmentNames) {
        const std::string entry = std::string(name) + "=value";
        CHECK_MSG(isEnclosingTerminalEnvironmentEntry(entry), entry.c_str());
        const std::string emptyValue = std::string(name) + "=";
        CHECK_MSG(isEnclosingTerminalEnvironmentEntry(emptyValue), emptyValue.c_str());
    }
    for (const std::string_view prefix : enclosingTerminalEnvironmentPrefixes) {
        const std::string entry = std::string(prefix) + "ANYTHING=value";
        CHECK_MSG(isEnclosingTerminalEnvironmentEntry(entry), entry.c_str());
    }
    for (const char* entry : {
             "TMUX=/private/tmp/tmux-501/default,1234,0",
             "TMUX_PANE=%3",
             "STY=4242.ttys001.host",
             "ZELLIJ=0",
             "ZELLIJ_SESSION_NAME=work",
             "ZELLIJ_PANE_ID=1",
             "TERMINFO=/Applications/kitty.app/Contents/Resources/kitty/terminfo",
             "ITERM_SESSION_ID=w0t0p0:ABC",
             "LC_TERMINAL=iTerm2",
             "KITTY_WINDOW_ID=1",
             "KITTY_PID=99",
             "WEZTERM_PANE=0",
             "WEZTERM_UNIX_SOCKET=/tmp/wezterm.sock",
             "GHOSTTY_RESOURCES_DIR=/Applications/Ghostty.app/Contents/Resources/ghostty",
             "GHOSTTY_SHELL_FEATURES=cursor,title",
             "ALACRITTY_WINDOW_ID=1",
             "ALACRITTY_SOCKET=/tmp/Alacritty.sock",
             "VTE_VERSION=7600",
             "WT_SESSION=0d6c",
             "KONSOLE_VERSION=240802",
             "KONSOLE_DBUS_SESSION=/Sessions/1",
         })
        CHECK_MSG(isEnclosingTerminalEnvironmentEntry(entry), entry);
}

// The negative half: the session's own identity, Ubiquitty's context, the
// person's ssh session, and settings that merely share a terminal's name stay.
TEST(everythingElseInTheEnvironmentIsKept)
{
    for (const char* entry : {
             "TERM=xterm-256color",
             "COLORTERM=truecolor",
             "TERM_PROGRAM=Ubiquitty",
             "TERM_PROGRAM_VERSION=1.0",
             "UBIQUITTY_PANE=pane:4",
             "UBIQUITTY_SOCKET=/tmp/control.sock",
             "SSH_CONNECTION=192.0.2.1 51000 198.51.100.2 22",
             "SSH_AUTH_SOCK=/tmp/agent.sock",
             "SSH_TTY=/dev/ttys004",
             "HOME=/Users/someone",
             "PATH=/usr/bin:/bin",
             "LANG=en_GB.UTF-8",
             "LC_CTYPE=UTF-8",
             "TERMINFO_DIRS=/usr/share/terminfo",
             "TMUX_PLUGIN_MANAGER_PATH=/Users/someone/.tmux/plugins",
             "TMUX_TMPDIR=/tmp",
             "KITTY_CONFIG_DIRECTORY=/Users/someone/.config/kitty",
             "TMUXINATOR_CONFIG=/Users/someone/.tmuxinator",
             "ZELLIJX=1",
             "WEZTERM_=1",
             "STYLE=dark",
         })
        CHECK_MSG(!isEnclosingTerminalEnvironmentEntry(entry), entry);

    // A name without a value is not an entry.
    CHECK(!isEnclosingTerminalEnvironmentEntry("TMUX"));
    CHECK(!isEnclosingTerminalEnvironmentEntry(""));
    CHECK(!isEnclosingTerminalEnvironmentEntry("=TMUX"));
}

UBIQUITTY_TEST_MAIN()
