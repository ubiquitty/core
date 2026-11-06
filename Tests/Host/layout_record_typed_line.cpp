// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/layout_record_typed_line.cpp — a line typed to a recorded
// workspace reaches the record with the directory the shell moved to, even
// when a write was already owed for something else when it was typed.
//
// A shell that reports no directory by OSC 7 changes it silently, so the host
// owes the record a write after every line, and the write asks the shell's
// process where it is. A write already owed may fall due before the shell has
// read the line. The line must then be owed a write of its own, a whole
// debounce interval after it, rather than ride the earlier one.
//
// The shell is stopped across the earlier write, so that write is certain to
// come before the shell acts on the line; the check is on what is owed once
// the shell has moved, which does not depend on timing.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Tree = Ubiquitty::Session;

// A fact the rest of the test stands on: checked, and the test ends where it
// does not hold.
#define PREMISE(expression)                                   \
    do {                                                      \
        const bool isPremise = static_cast<bool>(expression); \
        CHECK_MSG(isPremise, #expression);                    \
        if (!isPremise)                                       \
            return 1;                                         \
    } while (0)

namespace {

/// How long a record write trails what owes it, as the daemon counts it.
constexpr std::uint64_t debounceMilliseconds = 1000;

/// The daemon's clock.
std::uint64_t monotonicMilliseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1'000'000ull;
}

/// Waits up to ten seconds for a condition.
template<typename Condition>
bool waitFor(Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

/// Makes the write the daemon owes once its time has come, as the loop does.
void writeWhenDue(Daemon& d)
{
    while (monotonicMilliseconds() < Screen::layoutRecordWriteDueMilliseconds(d))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Screen::writeLayoutRecords(d);
}

} // namespace

int main()
{
    std::array<char, 64> pattern {};
    const std::string_view prefix = "/tmp/ubq-layout-line.XXXXXX";
    std::ranges::copy(prefix, pattern.begin());
    PREMISE(::mkdtemp(pattern.data()) != nullptr);
    std::error_code error;
    const std::string root = std::filesystem::canonical(pattern.data(), error).string();
    PREMISE(!error);
    const std::string before = root + "/before";
    const std::string after = root + "/after";
    PREMISE(::mkdir(before.c_str(), 0700) == 0 && ::mkdir(after.c_str(), 0700) == 0);
    const std::string recordPath = root + "/host.layouts";

    {
        Daemon d;
        Screen::setRestoreDefaults(d);
        Screen::setLayoutRecordPath(d, recordPath);
        // A shell that reports no directory: it reads a first line, then a
        // directory it moves to, and stays there.
        const std::array<std::string, 3> command { "/bin/sh", "-c", "cd '" + before + "' && read first && read line && cd \"$line\" && exec sleep 60" };
        auto* session = Screen::spawnSession(d, 80, 24, command);
        PREMISE(session != nullptr);
        Screen::nameWorkspace(d, *session, "work");

        const auto recordedDirectory = [&]() -> std::optional<std::string> {
            std::ifstream file(recordPath, std::ios::binary);
            const std::vector<char> bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            const Tree::HostLayoutFile read = Tree::readHostLayoutFile(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
            if (read.records.size() != 1 || read.records[0].arrangement.windows.empty() || read.records[0].arrangement.windows[0].tabs.empty())
                return std::nullopt;
            return read.records[0].arrangement.windows[0].tabs[0].root.directory;
        };
        const auto shellDirectory = [&]() -> std::string {
            const auto record = Screen::captureLayoutRecord(d, *session);
            if (!record || record->arrangement.windows.empty() || record->arrangement.windows[0].tabs.empty())
                return {};
            return record->arrangement.windows[0].tabs[0].root.directory;
        };
        PREMISE(waitFor([&] { return shellDirectory() == before; }));
        PREMISE(!Screen::isLayoutRecordWriteOwed(d));

        // With nothing owed, a line owes one write an interval after it, and
        // that write serves it: nothing more is owed once it is made.
        {
            const std::uint64_t typedAt = monotonicMilliseconds();
            Screen::typeInput(d, *session, "\r");
            CHECK_MSG(Screen::isLayoutRecordWriteOwed(d), "a line typed to a recorded workspace owed the record no write");
            CHECK_MSG(Screen::layoutRecordWriteDueMilliseconds(d) >= typedAt + debounceMilliseconds, "a line's write is due before an interval has passed since it");
            writeWhenDue(d);
            CHECK_MSG(!Screen::isLayoutRecordWriteOwed(d), "a line its write served owes another");
            CHECK_MSG(recordedDirectory() == before, "the record does not hold the shell's directory");
        }

        // A rename owes a write; a `cd` is typed as it falls due, and the
        // shell has not read the line when the write is made.
        {
            Screen::oweLayoutRecordWrite(d, *session);
            PREMISE(Screen::isLayoutRecordWriteOwed(d));
            const auto group = session->stream->process().foregroundProcessGroup();
            PREMISE(group.has_value() && ::kill(-static_cast<pid_t>(*group), SIGSTOP) == 0);
            while (monotonicMilliseconds() < Screen::layoutRecordWriteDueMilliseconds(d))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const std::uint64_t typedAt = monotonicMilliseconds();
            Screen::typeInput(d, *session, after + "\r");
            Screen::writeLayoutRecords(d);
            CHECK_MSG(recordedDirectory() == before, "premise: the earlier write came before the shell moved");
            PREMISE(::kill(-static_cast<pid_t>(*group), SIGCONT) == 0);
            PREMISE(waitFor([&] { return shellDirectory() == after; }));

            CHECK_MSG(Screen::isLayoutRecordWriteOwed(d), "a line typed while a write was owed rode that write, which came before the shell moved, and owes none of its own");
            CHECK_MSG(Screen::layoutRecordWriteDueMilliseconds(d) >= typedAt + debounceMilliseconds, "the line's own write is due before an interval has passed since it");
            writeWhenDue(d);
            CHECK_MSG(recordedDirectory() == after, "the record keeps the directory the shell left");
            CHECK_MSG(!Screen::isLayoutRecordWriteOwed(d), "a line its own write served owes another");
        }
    }
    std::filesystem::remove_all(root, error);
    return UBIQUITTY_TEST_SUMMARY();
}
