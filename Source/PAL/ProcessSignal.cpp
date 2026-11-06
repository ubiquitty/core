// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/ProcessSignal.h"

#include <csignal>

namespace Ubiquitty::PAL::Process::Signal {

std::span<const Entry> table()
{
    static constexpr Entry signals[] {
        { "Suspend", "TSTP", SIGTSTP },
        { "Stop", "STOP", SIGSTOP },
        { "Continue", "CONT", SIGCONT },
        { "Interrupt", "INT", SIGINT },
        { "Hangup", "HUP", SIGHUP },
        { "Terminate", "TERM", SIGTERM },
        { "Kill", "KILL", SIGKILL },
        { "Quit", "QUIT", SIGQUIT },
        { "Abort", "ABRT", SIGABRT },
        { "Alarm", "ALRM", SIGALRM },
        { "Window Size Change", "WINCH", SIGWINCH },
#ifdef SIGINFO
        { "Information Request", "INFO", SIGINFO },
#endif
        { "User Defined 1", "USR1", SIGUSR1 },
        { "User Defined 2", "USR2", SIGUSR2 },
    };
    return signals;
}

int numberForMnemonic(std::string_view wanted)
{
    for (const Entry& entry : table()) {
        if (wanted == entry.mnemonic) {
            return entry.number;
        }
    }
    return 0;
}

} // namespace Ubiquitty::PAL::Process::Signal
