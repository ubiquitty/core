// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/echo_delay_measure.cpp — the write-to-echo delay of real programs
// on a PTY, idle and under CPU load: the measurement that sets the echo timer
// `E` (Docs/sync.md §6.7). A write `s` settles once it was written at least `E`
// before a read that found the PTY empty, so `E` must exceed the time a
// program takes to echo a byte; a frame then never claims an echo it does not
// hold.
//
// Each program runs on its own PTY: the kernel's own echo under `cat`, a line
// editor (`bash` at an interactive prompt), a full-screen editor (`vim` in
// insert mode) and a pager (`less` scrolling a line). A byte is written, and
// the delay runs to the first byte read back. The loaded runs start one busy
// process per core, then two, reaped on every exit path.
//
// Without UBQ_SYNC_CORPUS_REPORT it is a smoke: the kernel echo answers a few
// bytes. With it, it prints the table recorded in
// Docs/sync-measurements.md, "Input and liveness".
//

#include "Assertions.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

std::uint64_t nowMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// One program on its own PTY, killed and reaped when it goes.
class PTYChild {
public:
    PTYChild(const std::vector<std::string>& argv, const std::vector<std::string>& environment)
    {
        winsize size {};
        size.ws_row = 24;
        size.ws_col = 80;
        int master = -1;
        const pid_t pid = forkpty(&master, nullptr, nullptr, &size);
        if (pid == 0) {
            for (const std::string& entry : environment)
                putenv(const_cast<char*>(entry.c_str()));
            std::vector<char*> arguments;
            for (const std::string& argument : argv)
                arguments.push_back(const_cast<char*>(argument.c_str()));
            arguments.push_back(nullptr);
            execvp(arguments[0], arguments.data());
            _exit(127);
        }
        m_pid = pid;
        m_master = master;
    }
    PTYChild(const PTYChild&) = delete;
    PTYChild& operator=(const PTYChild&) = delete;
    ~PTYChild()
    {
        if (m_master >= 0)
            close(m_master);
        if (m_pid > 0) {
            kill(m_pid, SIGKILL);
            int status = 0;
            waitpid(m_pid, &status, 0);
        }
    }

    [[nodiscard]] bool isRunning() const { return m_pid > 0 && m_master >= 0; }

    /// Reads until nothing arrives for `quietMicroseconds`, or `limit` passes.
    /// Returns what was read.
    std::string drain(std::uint64_t quietMicroseconds, std::uint64_t limitMicroseconds)
    {
        std::string output;
        const std::uint64_t deadline = nowMicroseconds() + limitMicroseconds;
        for (;;) {
            const std::uint64_t now = nowMicroseconds();
            if (now >= deadline)
                return output;
            const std::uint64_t wait = std::min(quietMicroseconds, deadline - now);
            pollfd descriptor { .fd = m_master, .events = POLLIN, .revents = 0 };
            const int ready = poll(&descriptor, 1, static_cast<int>((wait + 999) / 1000));
            if (ready <= 0)
                return output;
            std::array<char, 4096> buffer {};
            const ssize_t count = read(m_master, buffer.data(), buffer.size());
            if (count <= 0)
                return output;
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }

    /// Writes every byte, retrying an interrupted or partial write.
    ///
    /// - Returns: False when the PTY refused before the last byte.
    [[nodiscard]] bool write(std::string_view bytes)
    {
        while (!bytes.empty()) {
            const ssize_t count = ::write(m_master, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }

    /// Writes one byte and returns the delay to the first byte read back, or
    /// `nullopt` when nothing came within a second.
    std::optional<std::uint64_t> echoDelay(char byte)
    {
        const std::uint64_t written = nowMicroseconds();
        if (::write(m_master, &byte, 1) != 1)
            return std::nullopt;
        pollfd descriptor { .fd = m_master, .events = POLLIN, .revents = 0 };
        if (poll(&descriptor, 1, 1000) <= 0)
            return std::nullopt;
        const std::uint64_t echoed = nowMicroseconds();
        std::array<char, 4096> buffer {};
        // A hang-up is readiness too; only a byte read back is an echo.
        if (read(m_master, buffer.data(), buffer.size()) <= 0)
            return std::nullopt;
        return echoed - written;
    }

private:
    pid_t m_pid = -1;
    int m_master = -1;
};

/// One busy process per core, killed and reaped when it goes.
class LoadGenerator {
public:
    explicit LoadGenerator(unsigned processes)
    {
        for (unsigned index = 0; index < processes; ++index) {
            const pid_t pid = fork();
            if (pid == 0) {
                volatile std::uint64_t spin = 0;
                for (;;)
                    spin = spin + 1;
            }
            if (pid > 0)
                m_pids.push_back(pid);
        }
    }
    LoadGenerator(const LoadGenerator&) = delete;
    LoadGenerator& operator=(const LoadGenerator&) = delete;
    ~LoadGenerator()
    {
        for (const pid_t pid : m_pids)
            kill(pid, SIGKILL);
        for (const pid_t pid : m_pids) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }

private:
    std::vector<pid_t> m_pids;
};

struct Program {
    const char* name;
    std::vector<std::string> argv;
    /// Written once the program settles, before the measured bytes.
    std::string setup;
    /// The byte each sample writes.
    char byte;
    /// Written after each sample to undo it, so the screen never fills.
    std::string undo;
};

std::uint64_t percentile(std::vector<std::uint64_t> values, std::uint32_t perTenThousand)
{
    if (values.empty())
        return 0;
    std::ranges::sort(values);
    const std::size_t index = (values.size() * perTenThousand + 9'999) / 10'000;
    return values[std::min(values.size() - 1, index == 0 ? 0 : index - 1)];
}

std::string pagerFile()
{
    std::string path = "/tmp/ubq-echo-delay-pager-" + std::to_string(getpid()) + ".txt";
    if (FILE* file = std::fopen(path.c_str(), "w")) {
        for (int line = 0; line < 100'000; ++line)
            std::fprintf(file, "line %d of the pager's input\n", line);
        std::fclose(file);
    }
    return path;
}

std::vector<std::uint64_t> measure(const Program& program, int samples)
{
    const std::vector<std::string> environment { "TERM=xterm-256color", "PS1=$ ", "HISTFILE=/dev/null", "LESSHISTFILE=-" };
    PTYChild child(program.argv, environment);
    std::vector<std::uint64_t> delays;
    if (!child.isRunning())
        return delays;
    (void)child.drain(300'000, 5'000'000);
    if (!program.setup.empty()) {
        const bool didSetUp = child.write(program.setup);
        CHECK_MSG(didSetUp, "the program's setup reached its PTY");
        if (!didSetUp)
            return delays;
        (void)child.drain(200'000, 2'000'000);
    }
    for (int sample = 0; sample < samples; ++sample) {
        if (const std::optional<std::uint64_t> delay = child.echoDelay(program.byte))
            delays.push_back(*delay);
        (void)child.drain(5'000, 200'000);
        if (!program.undo.empty()) {
            // A sample left in place changes the screen every later one
            // echoes into, so the run ends at the first undo that fails.
            const bool didUndo = child.write(program.undo);
            CHECK_MSG(didUndo, "each sample's undo reached the PTY");
            if (!didUndo)
                break;
            (void)child.drain(5'000, 200'000);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return delays;
}

} // namespace

int main()
{
    const bool isReporting = std::getenv("UBQ_SYNC_CORPUS_REPORT") != nullptr;
    if (!isReporting) {
        const std::vector<std::uint64_t> delays = measure({ .name = "kernel echo", .argv = { "cat" }, .setup = {}, .byte = 'x', .undo = {} }, 5);
        CHECK_MSG(delays.size() == 5, "the kernel did not echo every byte written to cat's PTY");
        std::printf("echo_delay_measure: the kernel echoed %zu of 5 bytes\n", delays.size());
        return UBIQUITTY_TEST_SUMMARY();
    }

    const std::string pager = pagerFile();
    const std::vector<Program> programs {
        { .name = "kernel echo (cat)", .argv = { "cat" }, .setup = {}, .byte = 'x', .undo = {} },
        { .name = "line editor (bash)", .argv = { "bash", "--norc", "--noprofile", "-i" }, .setup = {}, .byte = 'x', .undo = "\x7f" },
        { .name = "full-screen editor (vim)", .argv = { "vim", "-u", "NONE", "-i", "NONE", "-N", "-n" }, .setup = "i", .byte = 'x', .undo = "\x7f" },
        { .name = "pager (less)", .argv = { "less", pager }, .setup = {}, .byte = 'j', .undo = {} },
    };
    constexpr int samples = 2000;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    std::printf("| program | load | samples | p50 ms | p90 ms | p99 ms | p99.9 ms | max ms |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for (const unsigned busy : { 0u, cores, 2 * cores }) {
        std::optional<LoadGenerator> load;
        if (busy > 0)
            load.emplace(busy);
        for (const Program& program : programs) {
            const std::vector<std::uint64_t> delays = measure(program, samples);
            CHECK_MSG(!delays.empty(), (std::string(program.name) + " echoed nothing").c_str());
            const auto ms = [&](std::uint32_t perTenThousand) { return static_cast<double>(percentile(delays, perTenThousand)) / 1000.0; };
            std::printf("| %s | %s | %zu | %.2f | %.2f | %.2f | %.2f | %.2f |\n", program.name,
                busy > 0 ? (std::to_string(busy) + " busy processes").c_str() : "idle", delays.size(),
                ms(5'000), ms(9'000), ms(9'900), ms(9'990), ms(10'000));
            std::fflush(stdout);
        }
    }
    std::remove(pager.c_str());
    return UBIQUITTY_TEST_SUMMARY();
}
