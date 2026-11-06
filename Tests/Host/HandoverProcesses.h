// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HandoverProcesses — real daemons, the binary at an install path replaced in
// place, and people attached through that binary's own `--attach` relay: the
// fixture for the stanzas that run a whole-host handover between processes
// (handover_input_once, handover_refusal_keeps_serving).

#include "Assertions.h"
#include "Host/HostConnection.h"

#include "Core/Codec/JSON.h"
#include "Platform/Host/HostSocketName.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace UbiquittyTest::HandoverProcesses {

namespace Sync = Ubiquitty::Protocol::Sync;
using Ubiquitty::HostTest::Connection;

/// The processes a stanza started, each an exact unreaped direct child.
/// - Returns: The registry, shared by every helper here.
inline std::vector<pid_t>& children()
{
    static std::vector<pid_t> started;
    return started;
}

/// Ends every child still running and reaps it; nothing else is signalled.
inline void endChildren()
{
    for (const pid_t child : std::vector(children())) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
    }
    children().clear();
}

/// A private directory holding the install, its control directory and logs.
struct Install {
    std::string root;
    std::string home;
    std::string binary;

    /// Copies `built` to `<root>/bin/ubiquitty-host` in a new private root.
    /// - Parameters:
    ///   - built: The built host.
    ///   - name: Names the private root.
    /// - Returns: Whether the install is in place.
    bool create(const std::string& built, std::string_view name)
    {
        // Under /tmp, not $TMPDIR: a Mac's per-user temporary directory is long
        // enough that the daemon's socket path inside this home would pass the
        // 103 bytes a Unix socket's path may hold.
        std::string pattern = "/tmp/ubq-" + std::string(name) + ".XXXXXX";
        if (!::mkdtemp(pattern.data()))
            return false;
        root = pattern;
        home = root + "/.ubiquitty";
        binary = root + "/bin/ubiquitty-host";
        return ::mkdir(home.c_str(), 0700) == 0 && ::mkdir((root + "/bin").c_str(), 0755) == 0 && copy(built, binary);
    }

    /// The socket the binary now at the install path owns.
    [[nodiscard]] std::string socketLeaf() const { return Ubiquitty::Host::socketLeafForBinary(binary.c_str()); }

    /// Replaces the binary at the install path with one of other content.
    /// - Returns: Whether the rename put it in place.
    bool replace() const
    {
        const std::string staged = binary + ".next";
        return copy(binary, staged, "\nhandover successor\n") && ::rename(staged.c_str(), binary.c_str()) == 0;
    }

    /// Removes the install, unless a failure keeps it for the reader.
    void finish() const
    {
        if (failures) {
            std::fprintf(stderr, "kept for the reader: %s\n", root.c_str());
            return;
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    /// Copies a file, with bytes appended, as an executable.
    /// - Parameters:
    ///   - from: The source.
    ///   - to: The destination, replaced.
    ///   - suffix: Bytes appended so the copy is another binary.
    /// - Returns: Whether the copy is complete and executable.
    static bool copy(const std::string& from, const std::string& to, std::string_view suffix = {})
    {
        {
            std::ifstream source(from, std::ios::binary);
            std::ofstream target(to, std::ios::binary | std::ios::trunc);
            if (!source || !target)
                return false;
            target << source.rdbuf();
            target.write(suffix.data(), static_cast<std::streamsize>(suffix.size()));
            if (!target)
                return false;
        }
        return ::chmod(to.c_str(), 0755) == 0;
    }
};

/// Starts `binary --daemon` under the install's home, its stderr to `log`
/// when one is named.
/// - Parameters:
///   - install: The install whose binary runs.
///   - log: Where its stderr goes; empty keeps the stanza's.
/// - Returns: The daemon's process, a direct child.
inline pid_t startDaemon(const Install& install, const std::string& log = {})
{
    const pid_t daemon = ::fork();
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", install.home.c_str(), 1);
        // A shell that reads no rc file of the person running the test, and
        // starts and keeps its history in the private root, never their home.
        ::setenv("HOME", install.root.c_str(), 1);
        ::setenv("HISTFILE", "/dev/null", 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        if (!log.empty()) {
            const int file = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (file >= 0)
                ::dup2(file, STDERR_FILENO);
        }
        ::execl(install.binary.c_str(), install.binary.c_str(), "--daemon", "--quic-port", "-1", "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (daemon > 0)
        children().push_back(daemon);
    return daemon;
}

/// Waits for one direct child, for a bounded time.
/// - Parameters:
///   - child: The direct child.
///   - milliseconds: The longest to wait.
/// - Returns: Its exit status, or nullopt while it still runs.
inline std::optional<int> waitForExit(pid_t child, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        int status = 0;
        const pid_t reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == child) {
            std::erase(children(), child);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        if ((reaped < 0 && errno != EINTR) || std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        ::usleep(10'000);
    }
}

/// Polls a file until it holds `needle`, for a bounded time.
/// - Parameters:
///   - path: The file.
///   - needle: The text to find.
///   - milliseconds: The longest to wait.
/// - Returns: Whether the file held it in time.
inline bool waitForFileText(const std::string& path, std::string_view needle, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream file(path, std::ios::binary);
        const std::string text { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        if (text.find(needle) != std::string::npos)
            return true;
        ::usleep(50'000);
    }
    return false;
}

/// One person attached through the install's own relay.
struct Person {
    std::optional<Connection> connection;
    Sync::ID128 instance;
    std::string workspace;
    std::uint32_t sessionID = 0;
    Sync::ID128 paneID;
    Sync::ID128 attachmentID;
    Sync::ID128 resumeSecret;
    int nextCall = 1;

    /// Attaches to a new workspace, or resumes the attachment it holds.
    /// - Parameters:
    ///   - install: The install whose binary relays.
    ///   - resume: The resume, or nothing for a fresh attach.
    /// - Returns: Whether the host welcomed it on the binary's own socket and bound its pane.
    bool attach(const Install& install, std::optional<Sync::ResumeRequest> resume = std::nullopt)
    {
        const std::uint64_t nextInputSeq = connection ? connection->nextInputSeq : 1;
        connection.emplace();
        connection->nextInputSeq = nextInputSeq;
        const auto bridge = Ubiquitty::HostTest::connectDaemonAsLocalBridge(install.binary, install.home);
        if (bridge.bridgeProcessID > 0)
            children().push_back(bridge.bridgeProcessID);
        connection->fileDescriptor = bridge.fileDescriptor;
        if (connection->fileDescriptor < 0)
            return false;
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = instance;
        greeting.clientKind = Sync::ClientKind::Human;
        greeting.requestedRole = Sync::Role::Owner;
        greeting.request = resume ? Sync::AdmissionRequest { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .isAttachOnly = true }
                                  : Sync::AdmissionRequest { .target = Sync::AdmissionTarget::Session, .workspaceName = workspace };
        greeting.resume = std::move(resume);
        greeting.facts.viewportRows = 24;
        greeting.facts.viewportColumns = 80;
        greeting.historyCacheLines = Sync::defaultHistoryCacheLines;
        greeting.backfillDepthLines = Sync::defaultBackfillDepthLines;
        std::string body;
        if (!Ubiquitty::HostTest::sendGreeting(*connection, std::move(greeting))
            || !Ubiquitty::HostTest::waitFor(*connection, Sync::FrameKind::Welcome, body, 5000) || !connection->welcome
            || connection->admission != Ubiquitty::HostTest::FixtureAdmission::Granted
            || connection->welcome->selection.socketName != install.socketLeaf() || !Ubiquitty::HostTest::waitForPane(*connection, 5000))
            return false;
        if (!sessionID) {
            sessionID = connection->welcome->selection.sessionID;
            paneID = connection->panes.front().paneID;
            attachmentID = connection->welcome->attachmentID;
        }
        resumeSecret = connection->welcome->resumeSecret;
        return true;
    }

    /// Reads the pane's retained text, scrollback included, through `pane.capture`.
    /// - Returns: The text, or nothing when the host did not answer it inline.
    std::optional<std::string> capture()
    {
        const std::string id = std::format("capture-{}", nextCall++);
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Call { .text = std::format(R"({{"jsonrpc":"2.0","id":"{}","method":"pane.capture","params":{{"pane":{},"source":"tail","lines":1000}}}})", id, connection->paneHandle) });
        if (!Ubiquitty::HostTest::sendFrame(*connection, Sync::FrameKind::Call, { reinterpret_cast<const char*>(body.data()), body.size() }))
            return std::nullopt;
        std::string frame;
        if (!Ubiquitty::HostTest::waitFor(*connection, Sync::FrameKind::Result, frame, 5000))
            return std::nullopt;
        const auto result = Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size() });
        if (!result || result->transfer)
            return std::nullopt;
        bool isParsed = false;
        const auto parsed = Ubiquitty::Codec::JSON::parse(result->text, isParsed);
        const auto* answer = isParsed ? parsed.find("result") : nullptr;
        const auto* text = answer ? answer->find("text") : nullptr;
        if (!text || !text->isString())
            return std::nullopt;
        return text->asString();
    }

    /// Waits until the pane's shell reads what is typed.
    ///
    /// A shell can discard input typed while it starts, so a probe is typed
    /// again until its output appears, for a bounded time.
    /// - Parameter milliseconds: The longest to wait.
    /// - Returns: Whether the shell answered a probe in time.
    bool waitForShell(int milliseconds = 30'000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!Ubiquitty::HostTest::sendInput(*connection, "echo shell-re\"\"ady\r"))
                return false;
            if (waitForText("shell-ready", 2'000))
                return true;
        }
        return false;
    }

    /// Polls the pane's text until it holds `needle`, for a bounded time.
    /// - Parameters:
    ///   - needle: The text to find.
    ///   - milliseconds: The longest to wait.
    /// - Returns: Whether the pane showed it in time.
    bool waitForText(std::string_view needle, int milliseconds = 10'000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto text = capture();
            if (text && text->find(needle) != std::string::npos)
                return true;
            if (!text && connection->hasFailed)
                return false;
            ::usleep(50'000);
        }
        return false;
    }
};

} // namespace UbiquittyTest::HandoverProcesses
