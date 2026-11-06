// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The ubiquitty-host program: a portable-POSIX binary (macOS + Linux) that
// relocates the terminal Core into a persistent daemon. Modes:
//
//   --daemon          Long-lived server owning one session (forkpty'd shell into a
//                     Core Terminal), listening on host.sock; keeps the session
//                     alive across client disconnects (tmux model), torn down only
//                     when the shell exits. Normally launched via --ensure-daemon.
//
//   --attach          Connect to host.sock and BRIDGE it to stdin/stdout (a raw
//                     byte relay). Run over ssh, this streams the session's Sync
//                     frames to the local client and back. When this process dies
//                     the daemon + session keep running; a later --attach reattaches.
//
//   --ensure-daemon   Start the daemon detached (double-fork + setsid) iff none is
//                     live, then exit 0. Client flow:
//                         ssh example.com 'ubiquitty-host --ensure-daemon &&
//                                  exec ubiquitty-host --attach'
//
// No Cocoa / Metal / Swift: portable Core + Source/Core/Protocol/Sync +
// the shared PTY transport (PAL/Stream.h) + sockets only. Builds with clang on macOS and gcc on
// Linux.
//

#include "Core/Clipboard/ToolInvocation.h"
#include "Core/Crypto/SHA256.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Session/HostInstall.h"
#include "Core/Session/RemoteBootstrap.h"
#include "PAL/Log.h"
#include "PAL/PAL.h"
#include "PAL/Socket.h"
#include "Platform/Host/AttachRelay.h"
#include "Platform/Host/ClipboardTool.h"
#include "Platform/Host/ConsentStore.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HandoverStartup.h"
#include "Platform/Host/HostHandover.h"
#include "Platform/Host/HostPaths.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/LocalControlClient.h"
#include "Platform/Host/ManagementMetadata.h"
#include "Platform/Host/ResumeRecordTool.h"
#include "Platform/Host/ShellIntegrationFiles.h"
#include "Platform/Host/TokenStore.h"
#include "Platform/POSIX/LocalHostInstall.h"
#ifdef UBIQUITTY_QUIC
#include "Platform/Host/HostQUIC.h"
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <span>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

using namespace Ubiquitty::Host;

// Connect a fresh SOCK_STREAM to host.sock. Returns a valid fd owner, or empty
// (with errno set) if nothing is listening / the path is missing.
Ubiquitty::PAL::UnixFileDescriptor connectToDaemon(const char* sockPath)
{
    Ubiquitty::PAL::UnixFileDescriptor fileDescriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fileDescriptor)
        return {};
    struct sockaddr_un addr;
    addr = {};
    addr.sun_family = AF_UNIX;
    if (std::strlen(sockPath) >= sizeof(addr.sun_path))
        return {};
    std::strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
    if (::connect(fileDescriptor.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
        return {};
    return fileDescriptor;
}

// True when a daemon is already accepting on host.sock.
bool daemonAlive(const char* sockPath)
{
    Ubiquitty::PAL::UnixFileDescriptor fileDescriptor = connectToDaemon(sockPath);
    return static_cast<bool>(fileDescriptor);
}

/// What a connect to a daemon socket says about the daemon.
enum class DaemonReach : std::uint8_t {
    /// A daemon accepted the connection.
    Listening,
    /// Nothing listens there: no socket file, or one no process holds open.
    Gone,
    /// The connect failed for another reason, which says nothing about the
    /// daemon: a refused permission, a full backlog, a path too long.
    Unknown,
};

/// Connects to a daemon socket and reads the answer as a fact about the daemon.
///
/// - Parameter sockPath: The socket's path.
/// - Returns: What the connect says.
DaemonReach reachDaemon(const char* sockPath)
{
    if (std::strlen(sockPath) >= sizeof(sockaddr_un::sun_path))
        return DaemonReach::Unknown;
    if (connectToDaemon(sockPath))
        return DaemonReach::Listening;
    return errno == ENOENT || errno == ECONNREFUSED ? DaemonReach::Gone : DaemonReach::Unknown;
}

// --daemon

/// Tells the launcher waiting on a readiness pipe that the socket accepts,
/// then lets the pipe go. A daemon that never reaches this closes the pipe by
/// exiting, which the launcher reads as an end without a ready byte.
///
/// - Parameter readyFileDescriptor: The pipe's write end, or -1 for none.
void reportListening(int readyFileDescriptor)
{
    if (readyFileDescriptor < 0)
        return;
    const char ready = 'R';
    while (::write(readyFileDescriptor, &ready, 1) < 0 && errno == EINTR) {
    }
    ::close(readyFileDescriptor);
}

int runDaemon(const char* selfPath, int columns, int rows, int quicPort, const char* socketName, int readyFileDescriptor = -1, std::optional<std::uint32_t> sessionFileCreationMask = std::nullopt)
{
    // An idle stop asked before the daemon handles one is ignored rather
    // than ending it, until Daemon::start installs the handler.
    ::signal(SIGUSR2, SIG_IGN);
    if (!ensureControlDirectory()) {
        std::fprintf(stderr, "ubiquitty-host: cannot create control directory\n");
        return 1;
    }
    char sock[1024], processID[1024];
    if (socketPathForName(socketName, sock) == 0 || processIDFilePathForSocketName(socketName, processID) == 0) {
        std::fprintf(stderr, "ubiquitty-host: cannot resolve control paths\n");
        return 1;
    }

    Daemon d;
    // Pinned before anything can replace the file this daemon runs from: a
    // successor is the binary later found at this path.
    Ubiquitty::Host::HandoverStartup::initialize(d, selfPath);
    // The host binary is the one reader of the process-wide preferences; the
    // daemon only ever sees the copy it is handed.
    d.setConfiguration(Ubiquitty::Preferences::values());
    if (sessionFileCreationMask)
        d.setSessionFileCreationMask(*sessionFileCreationMask);
    // A daemon the app starts inherits the app's bundled directory; one started
    // on its own writes the scripts it carries into its control directory.
    if (const char* inherited = ::getenv("UBIQUITTY_INTEGRATION_DIR"); !inherited || !inherited[0]) {
        std::array<char, 1024> directory {};
        std::string installed;
        if (Ubiquitty::Host::controlDirectory(directory))
            installed = Ubiquitty::Host::installShellIntegration(directory.data());
        if (installed.empty())
            std::fprintf(stderr, "ubiquitty-host: shell integration scripts could not be written; shells start without UBIQUITTY_INTEGRATION_DIR\n");
        d.setShellIntegrationDirectory(std::move(installed));
    }
    // The same login-shell resolver every frontend uses (no host-local copy).
    const auto loginShell = Ubiquitty::Runtime::LaunchCommand::loginShell();
    if (!d.start(sock, processID, loginShell.executablePath().c_str(), columns, rows)) {
        const Ubiquitty::Host::BindFailure failure = d.lastBindFailure();
        if (failure == Ubiquitty::Host::BindFailure::PathTooLong) {
            std::fprintf(stderr,
                "ubiquitty-host: daemon start failed: %s — \"%s\" is %zu bytes, "
                "the maximum is %zu (set UBIQUITTY_HOME to a shorter path)\n",
                Ubiquitty::Host::describeBindFailure(failure), sock, std::strlen(sock),
                Ubiquitty::Host::maximumSocketPathLength());
        } else {
            std::fprintf(stderr, "ubiquitty-host: daemon start failed: %s\n",
                Ubiquitty::Host::describeBindFailure(failure));
        }
        return 1;
    }
    // An older daemon of this install hands its sessions over before any
    // client is accepted, so a client's first resume finds the tables it
    // carried. One that refuses keeps serving beside this one.
    const auto handover = Ubiquitty::Host::HandoverStartup::takeOverSiblings(d, Ubiquitty::Host::HostHandover::transactionTimeoutMilliseconds);
    if (handover.movedSessions || handover.refusedPredecessors)
        Ubiquitty::PAL::logLine(stderr, "ubiquitty-host: took over {} sessions; {} older hosts kept theirs", handover.movedSessions, handover.refusedPredecessors);
    // The socket listens: a connect succeeds from here, and the run loop below
    // accepts it. The launcher waits for exactly this, not for a fixed time.
    reportListening(readyFileDescriptor);
    // The daemon's log opens with the build it is, which process, and when: a
    // report read later names the version and the commit without anyone asking
    // the binary, and the lines below it belong to this start.
    Ubiquitty::PAL::logLine(stderr, "ubiquitty-host: daemon {} serving protocol {}, process {}, started {:%FT%TZ}",
        Ubiquitty::Protocol::Sync::appVersionWithIdentity(), Ubiquitty::Protocol::Sync::wireVersion, ::getpid(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    // Optional QUIC data-plane alongside the unix socket. Non-fatal if it fails:
    // the daemon stays reachable over host.sock (the ssh-stdio path).
    // A predecessor's QUIC socket is served whatever port this start named:
    // its clients hold that port and certificate pin.
    if ((quicPort >= 0 || d.hasInheritedQUICSocket()) && !d.startQUIC(static_cast<std::uint16_t>(std::max(quicPort, 0))))
        std::fprintf(stderr,
            "ubiquitty-host: QUIC listener did not start "
            "(continuing on the unix socket only)\n");
    return d.run();
}

// Double-fork + setsid so the daemon outlives the ssh/login session that
// spawned --ensure-daemon, redirect std fds away from the (closing) tty,
// then exec ourselves with --daemon. The daemon proper is the orphaned
// grandchild, so the caller cannot wait on it: it is handed the read end of a
// pipe whose write end only the daemon holds. The daemon writes one byte once
// its socket listens; it exiting first closes the pipe empty.
//
// Returns the read end, or an empty descriptor when no daemon was started.
Ubiquitty::PAL::UnixFileDescriptor spawnDetachedDaemon(const char* selfPath, int columns, int rows, int quicPort, const char* socketName)
{
    std::array<char, 1024> logPath;
    logfilePathForSocketName(socketName, logPath);

    std::array<int, 2> readyPipe { -1, -1 };
    if (::pipe(readyPipe.data()) != 0)
        return {};
    Ubiquitty::PAL::UnixFileDescriptor readyRead(readyPipe[0]);
    (void)::fcntl(readyRead.get(), F_SETFD, FD_CLOEXEC);
    pid_t p1 = ::fork();
    if (p1 < 0) {
        ::close(readyPipe[1]);
        return {};
    }
    if (p1 > 0) {
        // Parent: reap the intermediate child (it exits immediately). Only the
        // daemon keeps the write end, so its exit is the pipe's end.
        ::close(readyPipe[1]);
        int st = 0;
        ::waitpid(p1, &st, 0);
        return readyRead;
    }
    readyRead.reset();
    // Above the standard descriptors, which are redirected below: a launcher
    // started with one closed would otherwise have been handed it for the pipe.
    if (readyPipe[1] <= STDERR_FILENO) {
        const int moved = ::fcntl(readyPipe[1], F_DUPFD, STDERR_FILENO + 1);
        ::close(readyPipe[1]);
        readyPipe[1] = moved;
    }

    // Intermediate child: detach from the controlling tty.
    if (::setsid() < 0)
        _exit(127);

    // Second fork so the daemon is not a session leader and can never
    // reacquire a controlling terminal.
    pid_t p2 = ::fork();
    if (p2 < 0)
        _exit(127);
    if (p2 > 0)
        _exit(0); // intermediate child exits → daemon is orphaned

    // Grandchild: the daemon. Move off any inherited currentDirectory that might be
    // unmountable; redirect std fds so a dead ssh pipe can't EPIPE us.
    if (::chdir("/") != 0) { /* non-fatal */
    }
    // The daemon's own files are owner-only; the shells it starts are the
    // person's, and get back the mask the launcher ran under.
    const mode_t sessionFileCreationMask = ::umask(077);

    Ubiquitty::PAL::UnixFileDescriptor devnull(::open("/dev/null", O_RDWR));
    const bool openedDevnull = static_cast<bool>(devnull);
    if (devnull) {
        ::dup2(devnull.get(), STDIN_FILENO);
        if (devnull.get() <= STDERR_FILENO)
            (void)devnull.release();
    }
    // stdout/stderr → this daemon's own host-<id>.log so a crashing daemon
    // leaves a trace, and no other daemon's lines land among its own.
    Ubiquitty::PAL::UnixFileDescriptor logFileDescriptor(::open(logPath.data(), O_WRONLY | O_CREAT | O_APPEND, 0600));
    if (logFileDescriptor) {
        ::dup2(logFileDescriptor.get(), STDOUT_FILENO);
        ::dup2(logFileDescriptor.get(), STDERR_FILENO);
        if (logFileDescriptor.get() <= STDERR_FILENO)
            (void)logFileDescriptor.release();
    } else if (openedDevnull) {
        Ubiquitty::PAL::UnixFileDescriptor nullOutput(::open("/dev/null", O_WRONLY));
        if (nullOutput) {
            ::dup2(nullOutput.get(), STDOUT_FILENO);
            ::dup2(nullOutput.get(), STDERR_FILENO);
            if (nullOutput.get() <= STDERR_FILENO)
                (void)nullOutput.release();
        }
    }

    // Exec a clean --daemon image. argv[0] = selfPath so /proc and ps
    // show the daemon. On exec failure, run the loop in-process instead.
    char columnsArgument[16], rowsArgument[16], quicArgument[16], readyArgument[16], maskArgument[16];
    std::snprintf(columnsArgument, sizeof(columnsArgument), "%d", columns);
    std::snprintf(rowsArgument, sizeof(rowsArgument), "%d", rows);
    std::snprintf(quicArgument, sizeof(quicArgument), "%d", quicPort);
    std::snprintf(readyArgument, sizeof(readyArgument), "%d", readyPipe[1]);
    std::snprintf(maskArgument, sizeof(maskArgument), "%04o", static_cast<unsigned>(sessionFileCreationMask & 0777));
    char* argv[16];
    int n = 0;
    argv[n++] = const_cast<char*>(selfPath);
    argv[n++] = const_cast<char*>("--daemon");
    argv[n++] = const_cast<char*>("--columns");
    argv[n++] = columnsArgument;
    argv[n++] = const_cast<char*>("--rows");
    argv[n++] = rowsArgument;
    // Always passed, -1 included: a --daemon given no --quic-port opens the
    // default listener, so leaving a disabled port out would enable it.
    argv[n++] = const_cast<char*>("--quic-port");
    argv[n++] = quicArgument;
    // Bind exactly the socket the ensure step probed (not a fresh hash of
    // selfPath): the on-disk binary could change between probe and exec, and the
    // detached daemon must own the socket the caller is about to --attach to.
    if (socketName && socketName[0] != '\0') {
        argv[n++] = const_cast<char*>("--socket");
        argv[n++] = const_cast<char*>(socketName);
    }
    argv[n++] = const_cast<char*>("--ready-fd");
    argv[n++] = readyArgument;
    argv[n++] = const_cast<char*>("--session-umask");
    argv[n++] = maskArgument;
    argv[n] = nullptr;
    // An idle stop asked of the daemon before it handles one is ignored
    // rather than ending it (Daemon::start installs the handler).
    ::signal(SIGUSR2, SIG_IGN);
    ::execv(selfPath, argv);
    // Fallback: exec failed (selfPath not executable from here) — just
    // run the daemon directly in this process.
    _exit(runDaemon(selfPath, columns, rows, quicPort, socketName, readyPipe[1], sessionFileCreationMask));
}

// --ensure-daemon

int runEnsureDaemon(const char* selfPath, int columns, int rows, int quicPort, const char* socketName)
{
    if (!ensureControlDirectory()) {
        std::fprintf(stderr, "ubiquitty-host: cannot create control directory\n");
        return 1;
    }
    char sock[1024];
    if (socketPathForName(socketName, sock) == 0)
        return 1;

    // Refused here rather than in the spawned daemon: this process cannot wait
    // on a double-forked child, so a child that dies instantly is
    // indistinguishable at the poll below from one still coming up, and the
    // wait would report a timeout for a path that can never bind.
    if (std::strlen(sock) > Ubiquitty::Host::maximumSocketPathLength()) {
        std::fprintf(stderr,
            "ubiquitty-host: cannot start daemon: %s — \"%s\" is %zu bytes, the "
            "maximum is %zu (set UBIQUITTY_HOME to a shorter path)\n",
            Ubiquitty::Host::describeBindFailure(Ubiquitty::Host::BindFailure::PathTooLong),
            sock, std::strlen(sock), Ubiquitty::Host::maximumSocketPathLength());
        return 1;
    }

    if (daemonAlive(sock)) {
        // The existing process owns its sessions and carrier configuration.
        // Ensuring reachability never authorizes killing it or a PID-file peer.
        // QUIC bootstrap asks this owner and may decline; stdio remains usable.
        return 0;
    }

    Ubiquitty::PAL::UnixFileDescriptor ready = spawnDetachedDaemon(selfPath, columns, rows, quicPort, socketName);
    if (!ready) {
        std::fprintf(stderr, "ubiquitty-host: failed to spawn daemon\n");
        return 1;
    }

    // Wait for the detached daemon to say its socket listens, so the
    // immediate `exec ubiquitty-host --attach` in the same command line does
    // not race ahead of it. A condition, not a guess at how long a start
    // takes: a loaded machine takes as long as it takes, and a daemon that
    // dies ends the wait at once. The ceiling only bounds a daemon that hangs
    // before listening.
    constexpr int hungStartMilliseconds = 120'000;
    pollfd readable { ready.get(), POLLIN, 0 };
    int polled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hungStartMilliseconds);
    do {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        polled = ::poll(&readable, 1, static_cast<int>(std::max<long long>(0, remaining)));
    } while (polled < 0 && errno == EINTR);
    char byte = 0;
    ssize_t got = 0;
    if (polled > 0) {
        do {
            got = ::read(ready.get(), &byte, 1);
        } while (got < 0 && errno == EINTR);
    }
    // A daemon that lost the bind to another started beside it ends its pipe
    // empty, and the other one serves.
    if ((got == 1 && byte == 'R') || daemonAlive(sock))
        return 0;
    std::array<char, 1024> logPath;
    logfilePathForSocketName(socketName, logPath);
    if (polled == 0)
        std::fprintf(stderr, "ubiquitty-host: daemon did not listen within %d s (see %s)\n", hungStartMilliseconds / 1000, logPath.data());
    else
        std::fprintf(stderr, "ubiquitty-host: daemon exited before it listened (see %s)\n", logPath.data());
    return 1;
}

/// Starts an installed successor and reports holders that could not hand over.
int runTakeOver(const char* selfPath, const char* socketName)
{
    if (!ensureControlDirectory()) {
        std::fprintf(stderr, "ubiquitty-host: cannot create control directory\n");
        return 1;
    }
    const auto outcome = Ubiquitty::Session::takeOverLocalHosts(selfPath, Ubiquitty::Session::LocalHostTakeoverMode::Start);
    if (!outcome.isHostAnswering && runEnsureDaemon(selfPath, 80, 24, 0, socketName) != 0)
        return 1;
    if (outcome.olderHostCount) {
        std::array<char, 1024> logPath;
        logfilePathForSocketName(socketName, logPath);
        std::fprintf(stderr, "The new host is installed, but %zu older daemon(s) still hold sessions. They remain running. See %s for the handover refusal; retry the update after resolving it.\n",
            outcome.olderHostCount, logPath.data());
        return Ubiquitty::Session::hostHandoverPendingStatus;
    }
    return 0;
}

// --attach

// Relay raw bytes between this process's stdio and the daemon socket
// until either side closes. stdin/stdout is the ssh channel; the socket
// is the daemon. The client end speaks the Sync protocol — this relay is
// transport, not protocol-aware (it never parses frames).
int runAttach(const char* selfPath, const char* socketName, Ubiquitty::Host::AttachedDaemon attached)
{
    char sock[1024];
    if (socketPathForName(socketName, sock) == 0)
        return 1;
    Ubiquitty::PAL::UnixFileDescriptor socketFileDescriptor = connectToDaemon(sock);
    if (!socketFileDescriptor) {
        std::fprintf(stderr,
            "ubiquitty-host: no daemon to attach to "
            "(run --ensure-daemon first)\n");
        return 1;
    }
    ::signal(SIGPIPE, SIG_IGN);

    // A bridge sshd started relays a client on another machine: mark the
    // connection before the client's first byte, so the daemon gates that
    // client's sessions as a remote client's while it keeps the owner's
    // authority. Only a mark the daemon confirms it applied lets the bridge
    // relay; a person on another machine is never attached as one here.
    if (Ubiquitty::Host::isStartedBySSH(std::getenv("SSH_CONNECTION"), std::getenv("SSH_CLIENT"))) {
        const auto outcome = Ubiquitty::Host::markRelayedFromSSH(socketFileDescriptor.get());
        if (outcome != Ubiquitty::Host::RelayMarkOutcome::Marked) {
            // A pin may name this binary's own leaf. Only then, and only on
            // the way out, is the binary read to tell.
            const auto daemon = attached == Ubiquitty::Host::AttachedDaemon::Named
                ? Ubiquitty::Host::attachedDaemonOf(socketName, Ubiquitty::Host::socketLeafForBinary(selfPath))
                : attached;
            std::fprintf(stderr, "ubiquitty-host: %s\n", Ubiquitty::Host::relayMarkRefusal(outcome, selfPath, daemon).c_str());
            return 1;
        }
    }

    // Non-blocking everything so a stalled peer on one side never wedges
    // the other.
    auto setNb = [](int fileDescriptor) {
        int fl = ::fcntl(fileDescriptor, F_GETFL, 0);
        if (fl != -1)
            ::fcntl(fileDescriptor, F_SETFL, fl | O_NONBLOCK);
    };
    setNb(STDIN_FILENO);
    setNb(STDOUT_FILENO);
    setNb(socketFileDescriptor.get());

    return Ubiquitty::Host::relayAttach(socketFileDescriptor.get(), STDIN_FILENO, STDOUT_FILENO);
}

// --probe-daemon

/// Says by its status alone whether a daemon listens on a socket, for a pinned
/// bootstrap to read before it announces the Sync stream.
///
/// - Parameter socketName: The socket's basename.
/// - Returns: 0 when a daemon accepts there, `Session::daemonProbeGoneStatus`
///   when nothing listens, 1 when the connect says nothing either way.
int runProbeDaemon(const char* socketName)
{
    char sock[1024];
    if (socketPathForName(socketName, sock) == 0)
        return 1;
    switch (reachDaemon(sock)) {
    case DaemonReach::Listening:
        return 0;
    case DaemonReach::Gone:
        return Ubiquitty::Session::daemonProbeGoneStatus;
    case DaemonReach::Unknown:
        break;
    }
    return 1;
}

// --quic-bootstrap

// Ensure a daemon, ask its QUIC listener for a one-time token, and print the dial info
// for the ssh caller: "quic_port=<N> quic_token=<hex>". The ssh channel (already
// authed) is what carries the token to the remote QUIC dialer. A pinned daemon
// that nothing listens for prints `Session::instanceGoneSentinel` instead.
int runQUICBootstrap(const char* selfPath, const char* pinnedSocket)
{
    // A directory-selected daemon already owns live sessions. A failed probe
    // must not replace it or start a different daemon under its identity.
    const std::string leaf = pinnedSocket ? std::string(pinnedSocket) : Ubiquitty::Host::socketLeafForBinary(selfPath);
    if (!pinnedSocket && runEnsureDaemon(selfPath, 80, 24, /*quicPort=*/0, leaf.c_str()) != 0) {
        return 1;
    }
    char sock[1024];
    if (socketPathForName(leaf.c_str(), sock) == 0)
        return 1;
    std::uint16_t port = 0;
    std::string token, certSHA256, error;
    if (!Ubiquitty::Protocol::Sync::requestQUICToken(sock, port, token, certSHA256, error)) {
        if (pinnedSocket && reachDaemon(sock) == DaemonReach::Gone)
            std::printf("%s %s\n", Ubiquitty::Session::instanceGoneSentinel, leaf.c_str());
        std::fprintf(stderr, "ubiquitty-host: quic bootstrap failed: %s\n", error.c_str());
        return 1;
    }
    static const char digits[] = "0123456789abcdef";
    auto toHex = [](const std::string& bytes) {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) {
            hex.push_back(digits[c >> 4]);
            hex.push_back(digits[c & 0x0F]);
        }
        return hex;
    };
    // The cert fingerprint travels the trusted (ssh/local) channel so the dialer
    // pins it and an active MITM presenting a different cert is refused. The
    // format is shared with the parser (Protocol::Sync::parseQUICBootstrapLine) so producer
    // and consumer never drift.
    std::printf(Ubiquitty::Protocol::Sync::quicBootstrapLineFormat, port, toHex(token).c_str(), toHex(certSHA256).c_str());
    return 0;
}

/// Prints how the host is run.
/// - Parameters:
///   - stream: Where the usage goes: stdout when it was asked for, stderr with a refusal.
///   - argv0: The name the host was invoked by.
void usage(std::FILE* stream, const char* argv0)
{
    std::fprintf(stream,
        "usage: %s --daemon | --attach | --ensure-daemon [--columns N --rows N] [--quic-port N] [--socket NAME]\n"
        "  --quic-port: all-interface UDP listener; 0 = ephemeral (default when built with QUIC), -1 = disabled\n"
        "       %s --quic-bootstrap [--socket NAME]  (ensures the daemon and prints its QUIC bootstrap line)\n"
        "       %s --probe-daemon [--socket NAME]  (exits 0 when a daemon listens, %d when nothing does)\n"
        "       %s --take-over  (hands older daemons' sessions over; exits %d if any remain on an older daemon)\n"
        "       %s --version | --quic-version | --quic-listen | --help\n"
        "       %s token <issue|list|revoke>  (agent capability tokens; `token --help` gives their arguments)\n"
        "       %s resume-record <write|read|purge> PATH  (an editor client's resume record)\n"
        "       %s clipboard-env [--socket ABSOLUTE_PATH]  (prints the assignments that give an existing shell its session's clipboard tools)\n",
        argv0, argv0, argv0, Ubiquitty::Session::daemonProbeGoneStatus, argv0, Ubiquitty::Session::hostHandoverPendingStatus, argv0, argv0, argv0, argv0);
}

/// Parses a terminal dimension the daemon's grid can hold.
///
/// - Parameters:
///   - text: The argument, all of it a decimal number.
///   - bound: The grid's most in that dimension.
/// - Returns: The number, or nothing for a word that is not one from 1 to
///   `bound`.
std::optional<int> parseDimension(std::string_view text, int bound)
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size() || value < 1 || value > bound)
        return std::nullopt;
    return value;
}

} // namespace

// Parse a comma-separated scope list ("observe,drive,control,region,new" or
// "all") into Scope bits. Every name `scopeText` emits parses back, which is
// what `token list` showing a scope a reader cannot then type would break. An
// unrecognized entry sets `sawUnknown` so the
// caller can FAIL CLOSED — a misspelled scope must never silently fall back to
// a broader default.
struct ParsedScopeList {
    Ubiquitty::Protocol::Sync::ScopeSet scope {};
    bool sawUnknown = false; // any comma entry didn't match a known name
};
static ParsedScopeList parseScopeList(const char* list)
{
    using namespace Ubiquitty::Protocol::Sync;
    ParsedScopeList result;
    const char* p = list;
    while (p && *p) {
        const char* comma = std::strchr(p, ',');
        std::string name(p, comma ? static_cast<std::size_t>(comma - p) : std::strlen(p));
        if (name == "observe" || name == "obs")
            result.scope |= Scope::Observe;
        else if (name == "drive")
            result.scope |= Scope::Drive;
        else if (name == "control" || name == "ctl")
            result.scope |= Scope::Control;
        else if (name == "region" || name == "read")
            result.scope |= Scope::RegionRead;
        else if (name == "new" || name == "new-session")
            result.scope |= Scope::NewSession;
        else if (name == "file")
            result.scope |= Scope::FileRead;
        else if (name == "manage")
            result.scope |= Scope::Manage;
        // `all` is every scope a session needs, and FileRead is not one. file.read
        // reads files off the host's disk, and the protocol calls the scope
        // "default-off — a token needs it granted explicitly": a name that
        // grants it as a side effect of meaning "everything" is the opposite of
        // explicit. Ask for `file` by name.
        else if (name == "all")
            result.scope |= Scope::Observe | Scope::Drive | Scope::Control | Scope::RegionRead | Scope::NewSession;
        else if (!name.empty())
            result.sawUnknown = true; // an empty entry (e.g. "drive,") is tolerated; a name isn't
        p = comma ? comma + 1 : nullptr;
    }
    return result;
}

// Parses `token issue --expires`: a whole number of seconds, or of minutes,
// hours or days with a trailing `m`, `h` or `d` (`s` names seconds). Anything
// else is refused, never read as far as it parses: read that way, `1h` is a
// one-second token. Zero is a token that never expires; a lifetime past
// 36500 days is refused, so the expiry it adds to the clock cannot wrap.
static std::optional<std::uint64_t> parseTokenLifetime(std::string_view text)
{
    constexpr std::uint64_t longestSeconds = 36500ull * 86400ull;
    std::uint64_t count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc {} || end == text.data())
        return std::nullopt;
    const std::string_view unit(end, static_cast<std::size_t>(text.data() + text.size() - end));
    std::uint64_t secondsPerUnit = 0;
    if (unit.empty() || unit == "s")
        secondsPerUnit = 1;
    else if (unit == "m")
        secondsPerUnit = 60;
    else if (unit == "h")
        secondsPerUnit = 3600;
    else if (unit == "d")
        secondsPerUnit = 86400;
    else
        return std::nullopt;
    if (count > longestSeconds / secondsPerUnit)
        return std::nullopt;
    return count * secondsPerUnit;
}

struct CredentialReloadReport {
    std::size_t acknowledgedOwners = 0;
    std::size_t unconfirmedCandidates = 0;
    bool didFinishScan = false;
};
static CredentialReloadReport reloadDaemonCredentials();
static void reportCredentialReload(const CredentialReloadReport&);

/// Prints the `token` subcommands and their arguments.
/// - Parameters:
///   - stream: Where the usage goes: stdout when it was asked for, stderr with a refusal.
///   - program: The name the host was invoked by.
static void printTokenUsage(std::FILE* stream, const char* program)
{
    std::fprintf(stream,
        "usage: %s token <issue|list|revoke>\n"
        "  issue  [--scope observe,drive,control,region,new,file,manage|all] [--expires N|Nm|Nh|Nd] [--label TEXT] [--allow-always]\n"
        "         (all excludes file and manage: each is granted by name only;\n"
        "          --allow-always also records the standing attach consent a human gives by answering \"always\")\n"
        "  list\n"
        "  revoke <token-id>\n",
        program);
}

// `ubiquitty-host token issue [--scope ...] [--expires LIFETIME] [--label TEXT] [--allow-always]`:
// mint a capability token. The plaintext is printed once (stdout); only its
// SHA-256 + scope + expiry + label persist to ~/.ubiquitty/tokens (0600). Issuance
// is human-only by construction — there's no wire frame for it.
//
// --allow-always records the standing attach consent for the token as well,
// the grant a human gives by answering "always" at the prompt, keyed on the
// token's digest. It is the answer for a host with no human to prompt: the
// operator issuing the token is present, and that is the moment to say what
// may attach. An agent nobody authorised stays denied there.
static int runTokenIssue(int argc, char** argv)
{
    Ubiquitty::Protocol::Sync::ScopeSet scope {};
    std::uint64_t expirySeconds = 0; // 0 = never
    std::string label = "agent";
    bool scopeExplicit = false; // --scope was supplied (with or without a value)
    bool allowAlways = false; // --allow-always: record the standing attach consent too
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scope") == 0) {
            scopeExplicit = true;
            if (i + 1 >= argc) {
                // Trailing bare `--scope` with no value: fail closed, never
                // fall through to the default scope.
                std::fprintf(stderr, "token issue: --scope requires a value\n");
                return 2;
            }
            const ParsedScopeList parsed = parseScopeList(argv[++i]);
            if (parsed.sawUnknown) {
                std::fprintf(stderr,
                    "token issue: unknown scope in \"%s\"; "
                    "valid: observe,drive,control,region,new,file,manage,all (all excludes file and manage)\n",
                    argv[i]);
                return 2;
            }
            scope = parsed.scope;
        } else if (std::strcmp(argv[i], "--expires") == 0) {
            const auto lifetime = i + 1 < argc ? parseTokenLifetime(argv[i + 1]) : std::nullopt;
            if (!lifetime) {
                std::fprintf(stderr, "token issue: --expires takes a lifetime such as 90, 30m, 12h or 7d (seconds, minutes, hours or days), not \"%s\"\n", i + 1 < argc ? argv[i + 1] : "");
                return 2;
            }
            expirySeconds = *lifetime;
            ++i;
        } else if (std::strcmp(argv[i], "--label") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "token issue: --label requires a value; nothing was issued\n");
                return 2;
            }
            label = argv[++i];
        } else if (std::strcmp(argv[i], "--allow-always") == 0)
            allowAlways = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printTokenUsage(stdout, argv[0]);
            return 0;
        } else {
            // Issuing mints a live credential, so an argument this parser does
            // not know is a request it cannot honour, never one to skip.
            std::fprintf(stderr, "token issue: unknown argument \"%s\"; nothing was issued\n", argv[i]);
            printTokenUsage(stderr, argv[0]);
            return 2;
        }
    }
    if (scopeExplicit && scope.isEmpty()) {
        // An explicit list that parsed to nothing (e.g. "--scope ,") is a
        // request the operator made and we couldn't honor — fail closed
        // rather than mint a broader default.
        std::fprintf(stderr, "token issue: --scope parsed to an empty scope set\n");
        return 2;
    }
    if (scope.isEmpty())
        scope = Ubiquitty::Protocol::Sync::Scope::Observe | Ubiquitty::Protocol::Sync::Scope::Drive; // default only when --scope is omitted

    // 32 bytes of entropy → a 64-char hex secret (the token).
    unsigned char raw[32];
    Ubiquitty::PAL::UnixFileDescriptor randomFileDescriptor(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    bool gotEntropy = randomFileDescriptor && ::read(randomFileDescriptor.get(), raw, sizeof(raw)) == static_cast<ssize_t>(sizeof(raw));
    if (!gotEntropy) {
        std::fprintf(stderr, "token issue: could not read /dev/urandom\n");
        return 1;
    }
    static const char* hexDigits = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    for (unsigned char b : raw) {
        token.push_back(hexDigits[(b >> 4) & 0xF]);
        token.push_back(hexDigits[b & 0xF]);
    }
    const std::string hash = Ubiquitty::Crypto::sha256Hex(token);
    const std::uint64_t expiryUnix = expirySeconds ? static_cast<std::uint64_t>(::time(nullptr)) + expirySeconds : 0;

    // Labels are single-line (the store is one record per line).
    for (char& ch : label)
        if (ch == '\n' || ch == '\r')
            ch = ' ';

    Ubiquitty::Host::ensureControlDirectory();
    char directory[1024];
    Ubiquitty::Host::controlDirectory(directory);
    const std::string path = std::string(directory) + "/tokens";
    const std::string consentPath = std::string(directory) + "/consent";
    Ubiquitty::Host::TokenStoreLock tokenLock(path);
    if (!tokenLock.isLocked()) {
        std::fprintf(stderr, "token issue: token store is busy or inaccessible; retry\n");
        return 1;
    }
    if (allowAlways) {
        // Refused before the token exists: a consent list this build cannot
        // read is one it must not append to, and a token issued without the
        // grant it was asked for is not what the operator asked for.
        const Ubiquitty::Host::ConsentList consent = Ubiquitty::Host::readConsentList(consentPath);
        if (consent.hasUnreadableLayout) {
            std::fprintf(stderr, "token issue: %s names a layout this host does not read; nothing issued\n", consentPath.c_str());
            return 1;
        }
        if (!consent.isWhole) {
            std::fprintf(stderr, "token issue: could not read %s completely; nothing issued\n", consentPath.c_str());
            return 1;
        }
    }
    auto store = Ubiquitty::Host::readTokenStore(path);
    if (!store.isWhole) {
        std::fprintf(stderr, "token issue: could not read %s completely; nothing issued\n", path.c_str());
        return 1;
    }
    auto records = std::move(store.records);
    records.push_back({ hash, scope, expiryUnix, label });
    const auto writeOutcome = Ubiquitty::Host::writeTokens(path, records);
    if (writeOutcome == Ubiquitty::Host::TokenWriteOutcome::IOError) {
        std::fprintf(stderr, "token issue: could not commit token store\n");
        return 1;
    }
    if (writeOutcome == Ubiquitty::Host::TokenWriteOutcome::UnsupportedDurability) {
        std::fprintf(stderr, "token issue: token stored but crash durability could not be proven\n");
    }

    CredentialReloadReport reload;
    if (allowAlways) {
        const Ubiquitty::Host::ConsentEdit edit = Ubiquitty::Host::addStandingConsent(consentPath, Ubiquitty::Host::tokenGrantKey(hash));
        if (edit != Ubiquitty::Host::ConsentEdit::Changed) {
            // The record is taken back so that exit 1 means nothing was
            // issued: a token that exists without the grant it was minted
            // for would be usable, and unlike what was asked for.
            auto rollback = Ubiquitty::Host::TokenWriteOutcome::IOError;
            const auto rollbackStore = Ubiquitty::Host::readTokenStore(path);
            if (rollbackStore.isWhole) {
                std::vector<Ubiquitty::Host::TokenRecord> kept;
                for (const auto& record : rollbackStore.records)
                    if (record.hashHex != hash)
                        kept.push_back(record);
                rollback = Ubiquitty::Host::writeTokens(path, kept);
            }
            std::fprintf(stderr, "token issue: could not record the standing consent in %s%s\n", consentPath.c_str(),
                rollback == Ubiquitty::Host::TokenWriteOutcome::IOError ? " — and the token record could not be taken back; revoke it" : "; nothing issued");
            return 1;
        }
        // A running daemon holds the list in memory. An authenticated local
        // request reloads it without trusting a stale process-ID file.
        reload = reloadDaemonCredentials();
    }

    Ubiquitty::Host::ManagementMetadataStore metadata(path);
    const auto issuedAt = static_cast<std::uint64_t>(::time(nullptr));
    const auto issuer = "Local owner (uid " + std::to_string(::geteuid()) + ")";
    if (!metadata.created("token:" + hash, issuedAt, issuer)
        || (allowAlways && !metadata.created("grant:attach:" + Ubiquitty::Host::tokenGrantKey(hash), issuedAt, issuer))) {
        std::fprintf(stderr, "token issue: created, but activity history could not be recorded\n");
    }

    std::printf("%s\n", token.c_str()); // the secret — printed once, on stdout
    std::fprintf(stderr, "issued token \"%s\" (scope 0x%02x%s) — store it now; it cannot be recovered.\n", label.c_str(), static_cast<unsigned>(scope.toRaw()), expiryUnix ? ", expiring" : ", no expiry");
    if (allowAlways) {
        std::fprintf(stderr, "recorded standing attach consent for \"%s\"\n", label.c_str());
        reportCredentialReload(reload);
    }
    return 0;
}

// Render a scope bitmask as a comma list (inverse of parseScopeList), into the
// caller's buffer. The decode rule lives in Core (Protocol::Sync::scopeMaskToString) so the
// daemon's `token list` and the app's Agents pane name the scopes identically;
// this only copies the result into the allocation-free buffer the callers expect.
static const char* scopeToText(Ubiquitty::Protocol::Sync::ScopeSet scope, std::span<char> output)
{
    const std::string text = Ubiquitty::Protocol::Sync::scopeMaskToString(scope);
    std::snprintf(output.data(), output.size(), "%s", text.c_str());
    return output.data();
}

static std::string tokensFilePath()
{
    char directory[1024];
    if (Ubiquitty::Host::controlDirectory(directory) == 0)
        return std::string();
    return std::string(directory) + "/tokens";
}

// The standing agent-attach consent list beside the token store.
static std::string consentFilePath()
{
    std::array<char, 1024> directory {};
    if (Ubiquitty::Host::controlDirectory(directory) == 0)
        return std::string();
    return std::string(directory.data()) + "/consent";
}

// `ubiquitty-host token list`: the issued tokens (digest prefix + scope + expiry +
// label). The secret never persisted, so the digest prefix is the handle revoke
// takes.
static int runTokenList()
{
    const std::string path = tokensFilePath();
    const std::vector<Ubiquitty::Host::TokenRecord> records = Ubiquitty::Host::loadTokens(path);
    if (records.empty()) {
        std::printf("no tokens issued\n");
        return 0;
    }
    const std::uint64_t now = static_cast<std::uint64_t>(::time(nullptr));
    char scopes[128];
    std::printf("%-16s  %-26s  %-9s  %s\n", "TOKEN-ID", "SCOPE", "EXPIRY", "LABEL");
    for (const auto& record : records) {
        const char* expiry = Ubiquitty::Protocol::Sync::tokenExpiryName(Ubiquitty::Protocol::Sync::tokenExpiry(record, now));
        std::printf("%-16.16s  %-26s  %-9s  %s\n", record.hashHex.c_str(), scopeToText(record.scope, scopes), expiry, record.label.c_str());
    }
    return 0;
}

static std::uint64_t reloadClockMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool waitForLocalReload(int descriptor, short events, std::uint64_t deadlineMilliseconds)
{
    for (;;) {
        const auto now = reloadClockMilliseconds();
        if (now >= deadlineMilliseconds)
            return false;
        pollfd item { descriptor, events, 0 };
        const int result = ::poll(&item, 1, static_cast<int>(deadlineMilliseconds - now));
        if (result > 0)
            return !(item.revents & POLLNVAL) && (item.revents & events);
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
}

static bool requestLocalReload(const std::string& directory, const struct stat& expectedDirectory,
    const char* path, const struct stat& expectedSocket, uid_t owner, std::uint64_t correlation, std::uint64_t deadlineMilliseconds)
{
    using namespace Ubiquitty::Host;
    LocalReloadExchange exchange(correlation, deadlineMilliseconds);
    sockaddr_un address {};
    const auto length = std::strlen(path);
    if (path[0] != '/' || length >= sizeof(address.sun_path) || reloadClockMilliseconds() >= deadlineMilliseconds)
        return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, length + 1);
    Ubiquitty::PAL::UnixFileDescriptor channel(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!channel || ::fcntl(channel.get(), F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(channel.get(), F_SETFL, O_NONBLOCK) != 0)
        return false;
#if defined(MSG_NOSIGNAL)
    constexpr int sendFlags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
    const int suppress = 1;
    if (::setsockopt(channel.get(), SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress)) != 0)
        return false;
    constexpr int sendFlags = 0;
#else
    return false; // No global signal policy is changed for this one request.
    constexpr int sendFlags = 0;
#endif
    if (::connect(channel.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if ((errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK)
            || !waitForLocalReload(channel.get(), POLLOUT, deadlineMilliseconds))
            return false;
        int error = 0;
        socklen_t errorLength = sizeof(error);
        if (::getsockopt(channel.get(), SOL_SOCKET, SO_ERROR, &error, &errorLength) != 0 || errorLength != sizeof(error) || error)
            return false;
    }
    // Re-examine the path after connecting: a renamed directory or replaced
    // socket must not turn a checked candidate into another endpoint.
    struct stat currentDirectory {};
    struct stat currentSocket {};
    const bool hasPrivateOwnerDirectory = ::lstat(directory.c_str(), &currentDirectory) == 0
        && S_ISDIR(currentDirectory.st_mode) && currentDirectory.st_uid == owner && !(currentDirectory.st_mode & 077)
        && currentDirectory.st_dev == expectedDirectory.st_dev && currentDirectory.st_ino == expectedDirectory.st_ino;
    const bool hasPrivateOwnerSocket = ::lstat(path, &currentSocket) == 0
        && S_ISSOCK(currentSocket.st_mode) && currentSocket.st_uid == owner && !(currentSocket.st_mode & 077)
        && currentSocket.st_dev == expectedSocket.st_dev && currentSocket.st_ino == expectedSocket.st_ino;
    exchange.authorize({ .hasPrivateOwnerDirectory = hasPrivateOwnerDirectory, .hasPrivateOwnerSocket = hasPrivateOwnerSocket, .hasSameUserPeer = Ubiquitty::PAL::peerIsSameUser(channel) },
        reloadClockMilliseconds());
    while (exchange.state(reloadClockMilliseconds()) == LocalReloadExchange::State::Sending) {
        const auto bytes = exchange.outgoing(reloadClockMilliseconds());
        if (bytes.empty())
            return false;
        const auto sent = ::send(channel.get(), bytes.data(), bytes.size(), sendFlags);
        if (sent > 0) {
            exchange.didSend(static_cast<std::size_t>(sent), reloadClockMilliseconds());
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitForLocalReload(channel.get(), POLLOUT, deadlineMilliseconds))
            continue;
        return false;
    }
    // One extra byte makes an overlong reply fail before it can count as an ACK.
    std::array<std::uint8_t, localControlReplyBytes + 1> buffer {};
    while (exchange.state(reloadClockMilliseconds()) == LocalReloadExchange::State::AwaitingReply) {
        const auto received = ::recv(channel.get(), buffer.data(), buffer.size(), 0);
        if (received > 0) {
            exchange.receive(std::span { buffer }.first(static_cast<std::size_t>(received)), reloadClockMilliseconds());
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitForLocalReload(channel.get(), POLLIN, deadlineMilliseconds))
            continue;
        return false;
    }
    return exchange.state(reloadClockMilliseconds()) == LocalReloadExchange::State::Reloaded;
}

// The token edit is already committed. Notify only through authenticated local
// sockets; a stale, unresponsive or old-protocol endpoint grants no success.
static CredentialReloadReport reloadDaemonCredentials()
{
    constexpr std::size_t maxDirectoryEntries = 4096;
    constexpr std::size_t maxSocketCandidates = 64;
    constexpr std::uint64_t totalMilliseconds = 2000;
    constexpr std::uint64_t perOwnerMilliseconds = 250;
    const auto deadline = reloadClockMilliseconds() + totalMilliseconds;
    CredentialReloadReport report;
    std::array<char, 1024> directoryBuffer {};
    if (!Ubiquitty::Host::controlDirectory(directoryBuffer) || directoryBuffer.front() != '/')
        return report;
    std::string directory(directoryBuffer.data());
    while (directory.size() > 1 && directory.back() == '/')
        directory.pop_back();
    Ubiquitty::PAL::UnixFileDescriptor directoryDescriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!directoryDescriptor) {
        report.didFinishScan = errno == ENOENT;
        return report;
    }
    const uid_t owner = ::geteuid();
    struct stat directoryInformation {};
    if (::fstat(directoryDescriptor.get(), &directoryInformation) != 0 || !S_ISDIR(directoryInformation.st_mode)
        || directoryInformation.st_uid != owner || (directoryInformation.st_mode & 077))
        return report;
    DIR* rawEntries = ::fdopendir(directoryDescriptor.get());
    if (!rawEntries)
        return report;
    (void)directoryDescriptor.release(); // fdopendir owns it only after success.
    const auto closeDirectory = [](DIR* directory) { (void)::closedir(directory); };
    const std::unique_ptr<DIR, decltype(closeDirectory)> entries(rawEntries, closeDirectory);
    std::size_t candidates = 0;
    for (std::size_t scanned = 0; scanned < maxDirectoryEntries; ++scanned) {
        if (reloadClockMilliseconds() >= deadline || candidates == maxSocketCandidates)
            break;
        errno = 0;
        const auto* entry = ::readdir(entries.get());
        if (!entry) {
            report.didFinishScan = errno == 0;
            break;
        }
        // The shared path builder accepts exactly host-<16 lower hex>.sock;
        // regular PID files, legacy names, separators and aliases never qualify.
        std::array<char, 1024> path {};
        if (!Ubiquitty::Host::socketPathForName(entry->d_name, path))
            continue;
        ++candidates;
        struct stat socketInformation {};
        if (::fstatat(::dirfd(entries.get()), entry->d_name, &socketInformation, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISSOCK(socketInformation.st_mode) || socketInformation.st_uid != owner || (socketInformation.st_mode & 077)) {
            ++report.unconfirmedCandidates;
            continue;
        }
        const auto ownerDeadline = std::min(deadline, reloadClockMilliseconds() + perOwnerMilliseconds);
        if (requestLocalReload(directory, directoryInformation, path.data(), socketInformation, owner, candidates, ownerDeadline))
            ++report.acknowledgedOwners;
        else
            ++report.unconfirmedCandidates;
    }
    return report;
}

static void reportCredentialReload(const CredentialReloadReport& report)
{
    std::fprintf(stderr, "credential reload acknowledged by %zu daemon(s); %zu socket candidate(s) unconfirmed%s\n",
        report.acknowledgedOwners, report.unconfirmedCandidates, report.didFinishScan ? "" : "; socket scan incomplete");
    if (report.unconfirmedCandidates || !report.didFinishScan)
        std::fprintf(stderr, "warning: not every owner confirmed the credential update. An unconfirmed path may be stale, or a daemon that takes no reload request from this program; one that follows the store applies the edit before its next operation, and one built without that check keeps its prior grants until it restarts. No process was signalled.\n");
}

// `ubiquitty-host token revoke <token-id>`: drop the matching record (by full
// digest or an unambiguous prefix) from the store and ask its local owners to reload so
// any live connection using it is torn down. Future HELLOs with it are denied.
static int runTokenRevoke(int argc, char** argv)
{
    if (argc == 4 && (std::strcmp(argv[3], "--help") == 0 || std::strcmp(argv[3], "-h") == 0)) {
        std::printf("usage: %s token revoke <token-id>\n", argv[0]);
        return 0;
    }
    // Revoking is a change, so a word this does not read refuses it rather
    // than being skipped: `revoke a1 b2` must not revoke `a1` alone.
    if (argc != 4 || argv[3][0] == '\0') {
        std::fprintf(stderr, "%susage: %s token revoke <token-id>\n", argc > 4 ? "token revoke: takes one token ID; nothing was revoked\n" : "", argv[0]);
        return 2;
    }
    const std::string prefix = argv[3];
    const std::string path = tokensFilePath();
    Ubiquitty::Host::TokenStoreLock tokenLock(path);
    if (!tokenLock.isLocked()) {
        std::fprintf(stderr, "token revoke: token store is busy or inaccessible; retry\n");
        return 1;
    }
    const auto store = Ubiquitty::Host::readTokenStore(path);
    if (!store.isWhole) {
        std::fprintf(stderr, "token revoke: could not read %s completely; nothing revoked\n", path.c_str());
        return 1;
    }
    const auto& records = store.records;

    std::vector<Ubiquitty::Host::TokenRecord> kept;
    int matched = 0;
    std::string matchedLabel;
    std::string matchedDigest;
    for (const auto& record : records) {
        if (record.hashHex.starts_with(prefix)) {
            ++matched;
            matchedLabel = record.label;
            matchedDigest = record.hashHex;
        } else {
            kept.push_back(record);
        }
    }
    if (matched == 0) {
        std::fprintf(stderr, "token revoke: no token matches \"%s\"\n", prefix.c_str());
        return 1;
    }
    if (matched > 1) {
        std::fprintf(stderr, "token revoke: \"%s\" matches %d tokens — use a longer prefix\n", prefix.c_str(), matched);
        return 1;
    }
    const Ubiquitty::Host::TokenWriteOutcome outcome = Ubiquitty::Host::writeTokens(path, kept);
    if (outcome == Ubiquitty::Host::TokenWriteOutcome::IOError) {
        std::fprintf(stderr, "token revoke: could not rewrite %s\n", path.c_str());
        return 1;
    }
    if (outcome == Ubiquitty::Host::TokenWriteOutcome::UnsupportedDurability) {
        // The bytes landed and live sessions are torn down below, but this
        // filesystem cannot prove they survive a crash — so a crash could
        // resurrect the revoked token. That is reported, loudly, rather than
        // believed or refused: refusing would make revocation unusable where
        // it is weakest, and silence would spend the one warning that matters.
        std::fprintf(stderr,
            "token revoke: warning: %s cannot prove stable bytes (filesystem without fsync); "
            "the revocation holds now but may not survive a crash\n",
            path.c_str());
    }
    // The token's standing consent goes with it. A stale line would be inert
    // — the digest no longer verifies — but the daemon keeps a bounded list
    // and evicts its oldest entry past the cap, so dead lines could push a
    // live grant out.
    const std::string consentPath = consentFilePath();
    const Ubiquitty::Host::ConsentEdit consent = Ubiquitty::Host::removeStandingConsent(consentPath, Ubiquitty::Host::tokenGrantKey(matchedDigest));
    if (consent == Ubiquitty::Host::ConsentEdit::IOError || consent == Ubiquitty::Host::ConsentEdit::UnreadableLayout)
        std::fprintf(stderr, "token revoke: warning: the token is revoked, but its standing consent in %s could not be withdrawn\n", consentPath.c_str());
    const auto reload = reloadDaemonCredentials();
    std::fprintf(stderr, "revoked token \"%s\" (%s)%s\n", prefix.c_str(), matchedLabel.c_str(),
        consent == Ubiquitty::Host::ConsentEdit::Changed ? " and withdrew its standing consent" : "");
    reportCredentialReload(reload);
    return 0;
}

static int runTokenCommand(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "";
    if (std::strcmp(sub, "issue") == 0)
        return runTokenIssue(argc, argv);
    if (std::strcmp(sub, "list") == 0) {
        if (argc > 3) {
            std::fprintf(stderr, "token list: takes no arguments, not \"%s\"\n", argv[3]);
            return 2;
        }
        return runTokenList();
    }
    if (std::strcmp(sub, "revoke") == 0)
        return runTokenRevoke(argc, argv);
    if (std::strcmp(sub, "--help") == 0 || std::strcmp(sub, "-h") == 0) {
        printTokenUsage(stdout, argv[0]);
        return 0;
    }
    printTokenUsage(stderr, argv[0]);
    return 2;
}

int main(int argc, char** argv)
{
    if (Ubiquitty::Clipboard::toolFromArgumentZero(argv[0]) != Ubiquitty::Clipboard::Tool::Unknown) {
        return Ubiquitty::Host::runClipboardTool({ argv, static_cast<std::size_t>(argc) });
    }
    if (argc >= 2 && std::strcmp(argv[1], "clipboard-env") == 0) {
        const char* endpoint = std::getenv("UBIQUITTY_CLIPBOARD_SOCKET");
        if (argc == 4 && std::strcmp(argv[2], "--socket") == 0) {
            endpoint = argv[3];
        } else if (argc != 2) {
            std::fprintf(stderr, "usage: ubiquitty-host clipboard-env [--socket ABSOLUTE_PATH]\n");
            return 2;
        }
        return Ubiquitty::Host::runClipboardEnvironment(endpoint ? endpoint : "");
    }
    // Install the process-wide platform-service context before anything
    // runs. The headless host supplies only the required monotonic clock; the
    // image decoder / DECPS synth / selection-color hooks stay absent (optional).
    Ubiquitty::PAL::Context palContext;
    palContext.monotonicClock = &Ubiquitty::PAL::monotonicTicks;
    Ubiquitty::PAL::installContext(palContext);
    if (argc >= 2 && std::strcmp(argv[1], "token") == 0)
        return runTokenCommand(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "resume-record") == 0)
        return Ubiquitty::Host::runResumeRecordTool({ argv + 2, static_cast<std::size_t>(argc - 2) });

    const char* mode = nullptr;
    int columns = 80, rows = 24;
    // QUIC listens by default when built with QUIC (0 = OS-ephemeral port); -1
    // disables. --quic-port N overrides. A build with QUIC off can't listen.
#ifdef UBIQUITTY_QUIC
    int quicPort = 0;
#else
    int quicPort = -1;
#endif
    const char* explicitSocket = nullptr; // --socket NAME; else derived from argv[0]
    int readyFileDescriptor = -1; // --ready-fd N: the launcher's readiness pipe
    // --session-umask OCTAL: the mask the launcher ran under, for the shells of
    // a daemon that runs under its own owner-only mask.
    std::optional<std::uint32_t> sessionFileCreationMask;
    // A mode starts a daemon or reaches one, so the loop refuses whatever it
    // does not read, and a value it cannot, before anything starts: a typo
    // must not bring up a daemon with a setting left out.
    const auto refuse = [&](const std::string& why) {
        std::fprintf(stderr, "ubiquitty-host: %s; nothing was started\n", why.c_str());
        usage(stderr, argv[0]);
        return 2;
    };
    // --help, --version, --quic-version or --quic-listen: what the host answers
    // instead of running a mode, the first given.
    const char* answer = nullptr;
    // The options that take a value: each is refused at the end of the line.
    constexpr std::array<std::string_view, 6> valued { "--columns", "--rows", "--quic-port", "--socket", "--ready-fd", "--session-umask" };
    for (int i = 1; i < argc; ++i) {
        if (std::ranges::find(valued, std::string_view(argv[i])) != valued.end() && i + 1 >= argc)
            return refuse(std::string(argv[i]) + " needs a value");
        if (std::strcmp(argv[i], "--daemon") == 0 || std::strcmp(argv[i], "--attach") == 0 || std::strcmp(argv[i], "--ensure-daemon") == 0 || std::strcmp(argv[i], "--quic-bootstrap") == 0
            || std::strcmp(argv[i], "--probe-daemon") == 0 || std::strcmp(argv[i], "--take-over") == 0) {
            if (mode && std::strcmp(mode, argv[i]) != 0)
                return refuse(std::string(mode) + " and " + argv[i] + " are two modes; give one");
            mode = argv[i];
        } else if (std::strcmp(argv[i], "--columns") == 0 || std::strcmp(argv[i], "--rows") == 0) {
            const bool isColumns = std::strcmp(argv[i], "--columns") == 0;
            const int bound = isColumns ? Ubiquitty::maxColumns : Ubiquitty::maxRows;
            const std::optional<int> dimension = parseDimension(argv[i + 1], bound);
            if (!dimension)
                return refuse(std::string(argv[i]) + " takes a number from 1 to " + std::to_string(bound) + ", not \"" + argv[i + 1] + "\"");
            (isColumns ? columns : rows) = *dimension;
            ++i;
        } else if (std::strcmp(argv[i], "--quic-port") == 0) {
            // -1 disables the listener and 0 picks an ephemeral port; anything
            // else must be a port, never a value atoi reads as one of those.
            const std::string_view text = argv[++i];
            int port = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
            if (error != std::errc {} || end != text.data() + text.size() || port < -1 || port > 65535) {
                std::fprintf(stderr, "ubiquitty-host: --quic-port takes -1 (disabled), 0 (ephemeral) or a port up to 65535, not \"%s\"\n", argv[i]);
                return 2;
            }
            quicPort = port;
        } else if (std::strcmp(argv[i], "--socket") == 0) {
            explicitSocket = argv[++i]; // pin a specific (versioned) socket leaf
        } else if (std::strcmp(argv[i], "--ready-fd") == 0) {
            // Only a descriptor above the standard three: the launcher puts
            // it there, and anything else would write a byte into a stream.
            readyFileDescriptor = std::atoi(argv[++i]);
            if (readyFileDescriptor <= STDERR_FILENO)
                readyFileDescriptor = -1;
        } else if (std::strcmp(argv[i], "--session-umask") == 0) {
            const std::string_view text = argv[++i];
            std::uint32_t mask = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), mask, 8);
            if (text.empty() || error != std::errc {} || end != text.data() + text.size() || mask > 0777) {
                std::fprintf(stderr, "ubiquitty-host: --session-umask takes an octal mask up to 0777, not \"%s\"\n", argv[i]);
                return 2;
            }
            sessionFileCreationMask = mask;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "--version") == 0
            || std::strcmp(argv[i], "--quic-version") == 0 || std::strcmp(argv[i], "--quic-listen") == 0) {
            if (!answer)
                answer = argv[i];
        } else {
            return refuse("unknown argument \"" + std::string(argv[i]) + "\"");
        }
    }
    // An answer that needs no mode is given once the whole line has been
    // read, so a word after it is refused as well: --quic-listen writes the
    // listener's certificate into the control directory.
    if (answer) {
        if (std::strcmp(answer, "--version") == 0) {
            // The version first, so a script reads it as the first word, then
            // the build identity in parentheses.
            std::printf("%s\n", Ubiquitty::Protocol::Sync::appVersionWithIdentity().c_str());
            return 0;
        }
        if (std::strcmp(answer, "--quic-version") == 0) {
            // Report the linked QUIC stack (diagnostic + linkage proof).
#ifdef UBIQUITTY_QUIC
            std::printf("%s\n", Ubiquitty::Host::quicStackVersion().c_str());
#else
            std::printf("QUIC not built (configure with -DUBIQUITTY_QUIC=ON)\n");
#endif
            return 0;
        }
        if (std::strcmp(answer, "--quic-listen") == 0) {
            // Bring up the QUIC listener and report its bound port, then exit —
            // verifies cert generation + the AWS-LC context + the UDP bind.
#ifdef UBIQUITTY_QUIC
            Ubiquitty::Host::QUICListener listener;
            if (listener.start(0)) {
                std::printf("QUIC listening on UDP port %u\n", listener.boundPort());
                return 0;
            }
            std::fprintf(stderr, "QUIC listen failed: %s\n", listener.lastError().c_str());
            return 1;
#else
            std::fprintf(stderr, "QUIC not built (configure with -DUBIQUITTY_QUIC=ON)\n");
            return 1;
#endif
        }
        usage(stdout, argv[0]);
        return 0;
    }
    if (!mode) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (std::strcmp(mode, "--take-over") == 0 && argc != 2)
        return refuse("--take-over takes no other arguments");

    // Resolve the REAL running binary once: a PATH launch leaves argv[0]
    // unresolvable, so hashing it (for the socket leaf) and execing it both fail.
    // Using the resolved self-exe makes the leaf a stable per-binary function and
    // makes the daemon relaunch reliable. Empty ⇒ resolution failed ⇒ fail closed.
    const std::string selfExecutablePath = Ubiquitty::Host::resolveSelfExecutablePath();
    const char* selfPath = !selfExecutablePath.empty() ? selfExecutablePath.c_str() : argv[0];

    if (std::strcmp(mode, "--quic-bootstrap") == 0) {
        return runQUICBootstrap(selfPath, explicitSocket);
    }

    // The socket leaf is a function of the binary's content (HostSocketName.h):
    // a rebuilt binary owns a NEW socket, so its daemon runs ALONGSIDE the old
    // one (which keeps its sessions and self-exits when idle) instead of one
    // killing the other. --socket pins a specific leaf for a reattach.
    const std::string socketName = (explicitSocket != nullptr && explicitSocket[0] != '\0') ? std::string(explicitSocket) : Ubiquitty::Host::socketLeafForBinary(selfPath);

    // Fail closed on an uncomputable per-binary identity — never derive or
    // bind a shared well-known leaf (cross-binary bleed). An explicit --socket is
    // exempt (it is non-empty by construction above). Guard before any socket use
    // so an empty leaf can't degrade into binding the control dir itself.
    if (socketName.empty()) {
        std::fprintf(stderr,
            "ubiquitty-host: cannot resolve this binary's identity "
            "(self-exe unresolved or unreadable); refusing to run without a "
            "per-binary socket. Pass --socket to override.\n");
        return 2;
    }

    if (std::strcmp(mode, "--daemon") == 0)
        return runDaemon(selfPath, columns, rows, quicPort, socketName.c_str(), readyFileDescriptor, sessionFileCreationMask);
    if (std::strcmp(mode, "--ensure-daemon") == 0)
        return runEnsureDaemon(selfPath, columns, rows, quicPort, socketName.c_str());
    if (std::strcmp(mode, "--take-over") == 0)
        return runTakeOver(selfPath, socketName.c_str());
    if (std::strcmp(mode, "--attach") == 0)
        return runAttach(selfPath, socketName.c_str(),
            explicitSocket != nullptr && explicitSocket[0] != '\0' ? Ubiquitty::Host::AttachedDaemon::Named : Ubiquitty::Host::AttachedDaemon::OwnBinary);
    if (std::strcmp(mode, "--probe-daemon") == 0)
        return runProbeDaemon(socketName.c_str());
    usage(stderr, argv[0]);
    return 2;
}
