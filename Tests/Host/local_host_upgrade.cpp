// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/local_host_upgrade.cpp — this machine's host upgraded the way the
// macOS app upgrades it after an update (Platform/POSIX/LocalHostInstall.h,
// Docs/sync.md §6.13), between real processes, real PTYs and real relays.
//
//   1. A holder installed at the control directory's `ubiquitty-host`, as an
//      SSH bootstrap installs one, refuses an unsigned bundled host started
//      elsewhere and keeps serving. A team-signed release successor may take
//      over from that location.
//   2. The bundled host installed over that path, as a hard link, and started
//      as the app starts it (`upgradeLocalHost`): the holder hands over every
//      session and exits 0, and the successor's log counts the session. A
//      person whose window saved a reference to the holder's workspace (its
//      instance, workspace and session, and no attachment) reattaches through
//      a relay pinned to the holder's socket, and is answered with the
//      workspace as it runs now: the same ids, its scrollback, its shell. A
//      second client saved with the same reference lands on the same running
//      session. A reference to an instance that never held it is refused.
//   3. A holder started from an app bundle, as earlier apps started theirs,
//      hands over to the host of the bundle an update put in its place,
//      whether that host is linked into the control directory or copied.
//   4. An install over a directory, or into a directory that is not there,
//      changes nothing. A bundled host others can write, or one on another
//      filesystem, is copied owner-only; an equal copy gives way to a link
//      once one can be made. A newer installed host is never replaced, nor
//      one too slow to say its version; one that says none is. Two installs
//      at once leave one install.
//   5. A copy a holder installed there refuses still has the bundled host
//      started, the holder keeps serving and the person is told; once the
//      install is linked, the idle bundled host stops and starts again and
//      the holder hands over. A bundled host that runs a session is neither
//      asked to stop nor waited for.
//   6. A host started from a copy trusts the app whose helper holds the same
//      signed code, where code is signed, and no app elsewhere.
//   7. An idle stop asked of a host from the moment its socket answers never
//      ends it by the signal, and one asked while a client is still greeting
//      waits for it.
//   8. On macOS, where UBQ_TEAM_SIGN_IDENTITY names a code-signing identity of
//      the trusted team (`Host::trustedSigningTeam`): a host binary the team
//      signed takes over a holder the team signed though it lies elsewhere,
//      and one signed ad hoc beside it is refused. UBQ_TEAM_SIGN_KEYCHAIN,
//      when set, names the keychain that holds the identity. Elsewhere it
//      says it was not run.
//
// The holders of steps 1 to 7 are not the team's, so they admit by place.
//
// argv[1] = the built ubiquitty-host. With argv[2] = an older release's
// ubiquitty-host and argv[3] = that release's ubq, step 2's holder runs the
// older release and its session is made by the older ubq; this stanza's
// client, which speaks only this build's wire, meets the session on the
// successor. `local_host_upgrade --greet-control <socket>` greets a host
// directly, as the app does, and prints its answer: step 6 runs this binary
// as the app. Exit 0 on success.
//

#include "Host/DetachedDaemons.h"
#include "Host/HandoverProcesses.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Session/DeployPolicy.h"
#include "Core/Session/HostInstall.h"
#include "Core/Session/SessionShare.h"
#include "Core/Transport/SSHHost.h"
#include "Platform/Host/SignaturePolicy.h"
#include "Platform/Host/TrustedLocalBinaries.h"
#include "Platform/POSIX/LocalHostInstall.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace UbiquittyTest::HandoverProcesses;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Session = Ubiquitty::Session;
using Ubiquitty::HostTest::Connection;

namespace {

/// A private root: a control directory, the host installed in it, and an
/// app bundle beside it.
struct Machine {
    std::string root;
    std::string home;
    std::string installPath;
    std::string bundleRoot;
    std::string bundledHost;
    std::string interface;

    bool create(std::string_view name)
    {
        // Under /tmp, not $TMPDIR: a Mac's per-user temporary directory is
        // long enough to pass the 103 bytes a socket path may hold.
        std::string pattern = "/tmp/ubq-" + std::string(name) + ".XXXXXX";
        if (!::mkdtemp(pattern.data()))
            return false;
        root = pattern;
        home = root + "/.ubiquitty";
        installPath = home + "/ubiquitty-host";
        bundleRoot = root + "/Applications/Ubiquitty.app";
        bundledHost = bundleRoot + "/Contents/Helpers/ubiquitty-host";
        interface = bundleRoot + "/Contents/MacOS/Ubiquitty";
        std::error_code error;
        std::filesystem::create_directories(bundleRoot + "/Contents/Helpers", error);
        std::filesystem::create_directories(bundleRoot + "/Contents/MacOS", error);
        return !error && ::mkdir(home.c_str(), 0700) == 0 && static_cast<bool>(std::ofstream(interface) << "the app\n");
    }

    /// Runs this process's upgrade calls, and the hosts they start, in this
    /// machine's control directory, with shells that read none of the
    /// person's files.
    void adopt() const
    {
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        ::setenv("HOME", root.c_str(), 1);
        ::setenv("HISTFILE", "/dev/null", 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
    }

    /// A holder running the file at `path`.
    [[nodiscard]] Install holderAt(const std::string& path) const { return Install { .root = root, .home = home, .binary = path }; }

    /// Ends the hosts this machine started detached, on whichever path its
    /// case leaves by, then keeps the machine for the reader of a failure or
    /// removes it.
    void finish() const
    {
        // An upgrade's successor, a bundled host started where it lies and an
        // SSH bootstrap's host are each no child of this process.
        CHECK_MSG(Ubiquitty::HostTest::endDetachedDaemons(home), "every host the machine started detached has ended");
        if (failures) {
            std::fprintf(stderr, "local_host_upgrade: kept for the reader: %s\n", root.c_str());
            return;
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

std::string logPathFor(const std::string& home, const std::string& socketLeaf)
{
    const std::string stem = socketLeaf.substr(0, socketLeaf.size() - std::string_view(".sock").size());
    return home + "/" + stem + ".log";
}

std::string fileText(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

bool isSameFile(const std::string& first, const std::string& second)
{
    struct stat one {}, other {};
    return ::stat(first.c_str(), &one) == 0 && ::stat(second.c_str(), &other) == 0 && one.st_dev == other.st_dev && one.st_ino == other.st_ino;
}

/// The process ID a detached host wrote beside its socket.
std::optional<pid_t> detachedHost(const std::string& home, const std::string& socketLeaf)
{
    const std::string stem = socketLeaf.substr(0, socketLeaf.size() - std::string_view(".sock").size());
    const std::string text = fileText(home + "/" + stem + ".pid");
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (value <= 0 || end == text.c_str())
        return std::nullopt;
    return static_cast<pid_t>(value);
}

/// Runs a program to its end and returns what it printed.
std::optional<std::string> runAndRead(const std::vector<std::string>& arguments)
{
    int ends[2] = { -1, -1 };
    if (arguments.empty() || ::pipe(ends) != 0)
        return std::nullopt;
    const pid_t child = ::fork();
    if (child == 0) {
        ::dup2(ends[1], STDOUT_FILENO);
        ::close(ends[0]);
        ::close(ends[1]);
        std::vector<char*> argv;
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    ::close(ends[1]);
    if (child < 0) {
        ::close(ends[0]);
        return std::nullopt;
    }
    std::string output;
    std::array<char, 4096> chunk {};
    for (;;) {
        const ssize_t count = ::read(ends[0], chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        output.append(chunk.data(), static_cast<std::size_t>(count));
    }
    ::close(ends[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "local_host_upgrade: %s exited with %d and printed %s\n", arguments.front().c_str(), status, output.c_str());
        return std::nullopt;
    }
    return output;
}

/// Opens `binary --attach --socket leaf`, the relay a pinned reattach runs.
int connectPinnedRelay(const std::string& binary, const std::string& home, const std::string& leaf)
{
    int ends[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
        return -1;
    const pid_t relay = ::fork();
    if (relay == 0) {
        ::dup2(ends[1], STDIN_FILENO);
        ::dup2(ends[1], STDOUT_FILENO);
        ::close(ends[0]);
        ::close(ends[1]);
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        ::execl(binary.c_str(), binary.c_str(), "--attach", "--socket", leaf.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(ends[1]);
    if (relay < 0) {
        ::close(ends[0]);
        return -1;
    }
    children().push_back(relay);
    return ends[0];
}

/// Greets as a person through a relay pinned to `leaf`, asking for `request`.
///
/// - Returns: The Welcome, with the connection left open in `person`, or
///   nothing when the host refused or said nothing.
std::optional<Sync::Welcome> greetPinned(Person& person, const std::string& binary, const std::string& home, const std::string& leaf,
    Sync::AdmissionRequest request, std::optional<Sync::ResumeRequest> resume = std::nullopt)
{
    person.connection.emplace();
    person.connection->fileDescriptor = connectPinnedRelay(binary, home, leaf);
    if (person.connection->fileDescriptor < 0)
        return std::nullopt;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = person.instance;
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request = std::move(request);
    greeting.resume = std::move(resume);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    greeting.historyCacheLines = Sync::defaultHistoryCacheLines;
    greeting.backfillDepthLines = Sync::defaultBackfillDepthLines;
    std::string body;
    if (!Ubiquitty::HostTest::sendGreeting(*person.connection, std::move(greeting))
        || !Ubiquitty::HostTest::waitFor(*person.connection, Sync::FrameKind::Welcome, body, 10'000) || !person.connection->welcome
        || person.connection->admission != Ubiquitty::HostTest::FixtureAdmission::Granted) {
        // The host's own words, for the reader of a refusal.
        for (const auto& frame : person.connection->frames) {
            if (frame.kind != Sync::FrameKind::Bye)
                continue;
            const auto bye = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(frame.body.data()), frame.body.size() });
            if (bye)
                std::printf("local_host_upgrade: the host refused with reason %u: %s\n", static_cast<unsigned>(bye->reason), bye->detail.c_str());
        }
        if (person.connection->hasFailed)
            std::printf("local_host_upgrade: the connection's frames did not read as a host's answer\n");
        return std::nullopt;
    }
    return person.connection->welcome;
}

/// A person's greeting for an unbound control connection.
Sync::Greet controlGreeting(Sync::ID128 instance)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = instance;
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    return greeting;
}

/// The process the kernel names as serving a socket this user's process
/// listens on, or nothing.
std::optional<pid_t> listeningProcess(const std::string& path)
{
    const int channel = Ubiquitty::HostTest::connectDaemon(path);
    if (channel < 0)
        return std::nullopt;
    std::optional<pid_t> process;
#if defined(__APPLE__)
    uid_t user = 0;
    gid_t group = 0;
    pid_t peer = 0;
    socklen_t length = sizeof(peer);
    if (::getpeereid(channel, &user, &group) == 0 && user == ::geteuid() && ::getsockopt(channel, SOL_LOCAL, LOCAL_PEERPID, &peer, &length) == 0 && peer > 0)
        process = peer;
#elif defined(__linux__)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(channel, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 && credentials.uid == ::geteuid() && credentials.pid > 0)
        process = credentials.pid;
#endif
    ::close(channel);
    return process;
}

/// As the app: greets the host at `socket` directly for a control
/// connection and prints `welcome`, or `refused <reason>`.
int greetAsTheApp(const std::string& socket)
{
    Connection connection;
    connection.fileDescriptor = Ubiquitty::HostTest::connectDaemon(socket);
    std::string body;
    if (connection.fileDescriptor < 0 || !Ubiquitty::HostTest::sendGreeting(connection, controlGreeting({ .low = 0x617070, .high = 6 })))
        return 1;
    for (int attempt = 0; attempt < 100 && !connection.welcome && connection.admission != Ubiquitty::HostTest::FixtureAdmission::Closed; ++attempt)
        (void)Ubiquitty::HostTest::waitFor(connection, Sync::FrameKind::Welcome, body, 100);
    if (connection.welcome) {
        std::printf("welcome\n");
        return 0;
    }
    for (const auto& frame : connection.frames) {
        if (frame.kind != Sync::FrameKind::Bye)
            continue;
        if (const auto bye = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(frame.body.data()), frame.body.size() })) {
            std::printf("refused %u\n", static_cast<unsigned>(bye->reason));
            return 0;
        }
    }
    return 1;
}

/// The session a holder's own `ubq new` made, read from its `session:N` line.
std::optional<std::uint32_t> sessionMadeBy(const std::string& output)
{
    const auto at = output.find("session:");
    if (at == std::string::npos)
        return std::nullopt;
    const unsigned long value = std::strtoul(output.c_str() + at + std::string_view("session:").size(), nullptr, 10);
    if (!value || value > UINT32_MAX)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

/// 1 and 2: a holder at the control directory's install path.
void upgradesAHostInstalledInTheControlDirectory(const std::string& built, const std::string& olderHost, const std::string& olderAgent)
{
    Machine machine;
    if (!machine.create("lhu")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    machine.adopt();
    const bool isCrossVersion = !olderHost.empty();
    CHECK_MSG(Install::copy(isCrossVersion ? olderHost : built, machine.installPath, isCrossVersion ? std::string_view {} : "\nan older host\n"),
        "premise: the holder is installed where an SSH bootstrap installs one");
    CHECK_MSG(Install::copy(built, machine.bundledHost, isCrossVersion ? std::string_view {} : "\nthe app's bundled host\n"),
        "premise: the app bundles a newer host");
    const Install holder = machine.holderAt(machine.installPath);
    const std::string holderLeaf = holder.socketLeaf();
    const std::string bundledLeaf = Ubiquitty::Host::socketLeafForBinary(machine.bundledHost.c_str());
    CHECK_MSG(!holderLeaf.empty() && holderLeaf != bundledLeaf, "premise: the two builds own two sockets");
    const pid_t holderProcess = startDaemon(holder);
    if (holderProcess <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(machine.home + "/" + holderLeaf)) {
        CHECK_MSG(false, "the holder listens");
        machine.finish();
        return;
    }

    // A session on the holder, with a line in its scrollback.
    Person person;
    person.instance = { .low = 0x6c6f63616c, .high = 1 };
    person.workspace = "upgraded";
    std::uint32_t sessionID = 0;
    std::uint32_t workspaceID = 0;
    std::uint64_t holderInstance = 0;
    // What the session printed before the upgrade, which only its output
    // holds: the older release's ubq types a sum, the person a quoted word.
    const std::string_view mark = isCrossVersion ? "6345" : "before-upgrade";
    if (isCrossVersion) {
        // The older release's ubq runs nothing unattended without a rule
        // allowing it; this stanza is the person who writes one, in the
        // holder's private control directory.
        CHECK_MSG(static_cast<bool>(std::ofstream(machine.home + "/agent-policy.conf") << "default allow\n"), "premise: the older ubq is allowed to run");
        // And it presents a token its host issued, as an agent does.
        const auto token = runAndRead({ olderHost, "token", "issue", "--scope", "all", "--label", "local-host-upgrade" });
        CHECK_MSG(token && !token->empty(), "premise: the older host issues its ubq a token");
        if (token) {
            std::string secret = *token;
            while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
                secret.pop_back();
            ::setenv("UBIQUITTY_AGENT_TOKEN", secret.c_str(), 1);
        }
        const auto made = runAndRead({ olderAgent, "new" });
        const auto session = made ? sessionMadeBy(*made) : std::nullopt;
        CHECK_MSG(session.has_value(), "the older release's ubq makes a session on its own host");
        if (!session) {
            machine.finish();
            return;
        }
        sessionID = *session;
        // A policy holds a line with a shell metacharacter for confirmation
        // even where it allows the rest, so the line has none.
        CHECK_MSG(runAndRead({ olderAgent, "send", "-s", std::to_string(sessionID), "--enter", "expr 4000 + 2345" }).has_value(),
            "the older release's ubq types into its session");
        // Its output, before anything is replaced.
        bool isShown = false;
        for (int attempt = 0; attempt < 100 && !isShown; ++attempt) {
            const auto read = runAndRead({ olderAgent, "read", "-s", std::to_string(sessionID), "--scrollback", "200" });
            isShown = read && read->find(mark) != std::string::npos;
            if (!isShown)
                ::usleep(100'000);
        }
        CHECK_MSG(isShown, "the older release's shell answers");
    } else {
        if (!person.attach(holder) || !person.waitForShell()) {
            CHECK_MSG(false, "a person attaches to the holder");
            machine.finish();
            return;
        }
        sessionID = person.sessionID;
        workspaceID = person.connection->welcome->selection.workspaceID;
        holderInstance = person.connection->welcome->selection.daemonInstanceID;
        CHECK_MSG(workspaceID && holderInstance, "premise: the session is in a workspace of the holder's");
        CHECK_MSG(Ubiquitty::HostTest::sendInput(*person.connection, "echo before-up\"\"grade\r") && person.waitForText("before-upgrade"),
            "the holder's shell answers");
    }

    // A signed release successor may be admitted where it lies; an unsigned
    // successor is admitted only after installation at the trusted path.
    bool didTakeOverBeforeInstall = false;
    {
        const auto ready = Session::readyLocalDaemon(machine.bundledHost);
        CHECK_MSG(!ready.socketPath.empty(), "premise: the bundled host starts beside the holder");
        didTakeOverBeforeInstall = isCrossVersion && waitForExit(holderProcess, 1000) == std::optional { 0 };
        if (didTakeOverBeforeInstall)
            CHECK_MSG(waitForFileText(logPathFor(machine.home, bundledLeaf), "took over 1 sessions; 0 older hosts kept theirs", 5000),
                "the signed bundled host takes the holder's session directly");
        else {
            CHECK_MSG(!waitForExit(holderProcess, 1000), "a holder installed elsewhere keeps running");
            CHECK_MSG(waitForFileText(logPathFor(machine.home, bundledLeaf), "1 older hosts kept theirs", 5000),
                "the bundled host started where it lies says the holder kept its sessions");
        }
        if (!isCrossVersion)
            CHECK_MSG(Ubiquitty::HostTest::sendInput(*person.connection, "echo still-on-the-ho\"\"lder\r") && person.waitForText("still-on-the-holder"),
                "the holder still serves its session");
    }
    const auto refused = didTakeOverBeforeInstall ? std::nullopt : detachedHost(machine.home, bundledLeaf);
    if (!didTakeOverBeforeInstall)
        CHECK_MSG(refused.has_value(), "premise: the refused bundled host still runs");

    // Installing the trusted host starts it at the control path if it did
    // not already take over as a signed release successor.
    const auto outcome = Session::upgradeLocalHost(machine.bundledHost, Session::LocalHostBuild::Release);
    CHECK_MSG(outcome.install.kind == Session::LocalHostInstallKind::Linked, "the bundled host is linked over the install");
    CHECK_MSG(isSameFile(machine.installPath, machine.bundledHost), "the install and the bundled host are one file");
    CHECK_MSG(Ubiquitty::Host::interfacePathForHost(machine.installPath) == machine.interface,
        "a host started from the install trusts the app whose helper it is");
    CHECK_MSG(outcome.isBundledHostAnswering, "the bundled host answers after installation");
    if (!didTakeOverBeforeInstall) {
        CHECK_MSG(outcome.didStartSuccessor, "the bundled host is started again to take the holder over");
        CHECK_MSG(waitForFileText(logPathFor(machine.home, bundledLeaf), "asked to stop while it holds nothing", 5000),
            "the refused bundled host stopped as it held nothing");
        CHECK_MSG(refused && ::kill(*refused, 0) != 0, "the refused bundled host is gone");
    }
    const auto holderExit = didTakeOverBeforeInstall ? std::optional { 0 } : waitForExit(holderProcess, 30'000);
    CHECK_MSG(holderExit == std::optional { 0 }, "the holder hands over and exits cleanly");
    CHECK_MSG(waitForFileText(logPathFor(machine.home, bundledLeaf), "took over 1 sessions; 0 older hosts kept theirs", 5000),
        "the successor took the holder's session and no host kept any");
    CHECK_MSG(outcome.olderHostCount == 0, "no host of another build serves afterwards");
    if (!isCrossVersion) {
        std::string body;
        CHECK_MSG(Ubiquitty::HostTest::waitFor(*person.connection, Sync::FrameKind::Bye, body, 5000), "the person is told the holder is handing over");
        ::close(person.connection->fileDescriptor);
        person.connection->fileDescriptor = -1;
        // The person whose attachment the holder issued resumes it, pinned
        // to the holder's socket and instance: a resume, not an answer from
        // the workspace.
        Person resumed;
        resumed.instance = person.instance;
        const auto welcome = greetPinned(resumed, machine.installPath, machine.home, holderLeaf,
            { .target = Sync::AdmissionTarget::Workspace, .sessionID = sessionID, .workspaceID = workspaceID, .expectedDaemonInstanceID = holderInstance, .isAttachOnly = true },
            Sync::ResumeRequest { .attachmentID = person.attachmentID, .resumeSecret = person.resumeSecret, .panes = {} });
        CHECK_MSG(welcome && welcome->attachmentID == person.attachmentID, "the holder's attachment resumes on the successor");
        CHECK_MSG(welcome && !welcome->selection.restoredFromDaemonInstanceID, "a resume is not answered as a saved reference");
        CHECK_MSG(welcome && welcome->predecessorHostInstanceID.has_value(), "the successor names its predecessor");
        if (resumed.connection && resumed.connection->fileDescriptor >= 0)
            ::close(resumed.connection->fileDescriptor);
    }
    const auto again = Session::installBundledHost(machine.bundledHost, machine.installPath, Sync::appVersion());
    CHECK_MSG(again.kind == Session::LocalHostInstallKind::Current, "the next install finds the bundled host installed");

    // The session runs on the successor, with its scrollback and its shell.
    Person relaunched;
    relaunched.instance = { .low = 0x72656c61756e6368, .high = 2 };
    if (isCrossVersion) {
        const auto welcome = greetPinned(relaunched, machine.installPath, machine.home, holderLeaf,
            { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .isAttachOnly = true });
        CHECK_MSG(welcome && welcome->selection.sessionID == sessionID, "the older release's session is on the successor under its id");
    } else {
        // A window saved on the holder: its instance, workspace and session,
        // and no attachment, reattached through a relay pinned to the
        // holder's socket, which the successor serves now.
        const auto welcome = greetPinned(relaunched, machine.installPath, machine.home, holderLeaf,
            { .target = Sync::AdmissionTarget::Workspace, .sessionID = sessionID, .workspaceID = workspaceID, .expectedDaemonInstanceID = holderInstance, .isAttachOnly = true });
        CHECK_MSG(welcome.has_value(), "a saved reference to the holder's workspace is admitted by its successor");
        if (welcome) {
            CHECK_MSG(welcome->selection.workspaceID == workspaceID && welcome->selection.sessionID == sessionID,
                "the reference is answered with the same workspace and session");
            CHECK_MSG(welcome->selection.restoredFromDaemonInstanceID == holderInstance, "the answer names the instance the reference named");
            CHECK_MSG(welcome->selection.daemonInstanceID && welcome->selection.daemonInstanceID != holderInstance,
                "the answer comes from the successor's instance");
            CHECK_MSG(welcome->selection.socketName == bundledLeaf, "the successor names its own socket");
        }
    }
    if (relaunched.connection && relaunched.connection->fileDescriptor >= 0 && Ubiquitty::HostTest::waitForPane(*relaunched.connection, 5000)) {
        CHECK_MSG(relaunched.waitForText(mark), "the session keeps its scrollback");
        CHECK_MSG(Ubiquitty::HostTest::sendInput(*relaunched.connection, "echo after-up\"\"grade\r") && relaunched.waitForText("after-upgrade"),
            "the inherited shell takes input on the successor");
    } else {
        CHECK_MSG(false, "the reattached session's pane is bound");
    }
    if (!isCrossVersion) {
        // Another app instance that saved the same reference lands on the
        // same live workspace and the session it runs, while the first still
        // shows it: the handover's sessions answer every such reference, and
        // nothing is started for the second.
        Person another;
        another.instance = { .low = 0x616e6f74686572, .high = 9 };
        const auto welcome = greetPinned(another, machine.installPath, machine.home, holderLeaf,
            { .target = Sync::AdmissionTarget::Workspace, .sessionID = sessionID, .workspaceID = workspaceID, .expectedDaemonInstanceID = holderInstance, .isAttachOnly = true });
        CHECK_MSG(welcome && welcome->selection.workspaceID == workspaceID && welcome->selection.sessionID == sessionID,
            "a second reference to the handed-over workspace lands on its running session");
        CHECK_MSG(welcome && welcome->selection.restoredFromDaemonInstanceID == holderInstance, "and is answered as the reference it is");
        if (another.connection && another.connection->fileDescriptor >= 0 && Ubiquitty::HostTest::waitForPane(*another.connection, 5000))
            CHECK_MSG(another.waitForText("after-upgrade"), "the second client shows the shell the first typed into");
        else
            CHECK_MSG(false, "the second client's pane is bound");
        if (another.connection && another.connection->fileDescriptor >= 0)
            ::close(another.connection->fileDescriptor);
        // A reference to an instance that never held the workspace is
        // refused, and nothing is created for it.
        Person stranger;
        stranger.instance = { .low = 0x737472616e676572, .high = 3 };
        const auto refused = greetPinned(stranger, machine.installPath, machine.home, bundledLeaf,
            { .target = Sync::AdmissionTarget::Workspace, .sessionID = sessionID, .workspaceID = workspaceID, .expectedDaemonInstanceID = holderInstance ^ 0x5a5a, .isAttachOnly = true });
        CHECK_MSG(!refused, "a reference to another instance is refused");
        if (stranger.connection && stranger.connection->fileDescriptor >= 0)
            ::close(stranger.connection->fileDescriptor);
    }
    if (relaunched.connection && relaunched.connection->fileDescriptor >= 0) {
        std::string body;
        CHECK_MSG(Ubiquitty::HostTest::sendInput(*relaunched.connection, "exit\r")
                && Ubiquitty::HostTest::waitFor(*relaunched.connection, Sync::FrameKind::PaneUnbind, body, 10'000),
            "the inherited shell ends on the successor");
        ::close(relaunched.connection->fileDescriptor);
    }
    machine.finish();
}

/// How an updated bundle's host reaches the control directory.
enum class InstallWay : std::uint8_t {
    /// As the bundled file itself.
    Linked,
    /// As a copy, because others can write the bundled file.
    Copied,
};

/// 3: a holder started from an app bundle that an update has replaced.
void upgradesAHostStartedFromAnEarlierBundle(const std::string& built, InstallWay way)
{
    Machine machine;
    if (!machine.create(way == InstallWay::Linked ? "lhb" : "lhd")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    machine.adopt();
    CHECK_MSG(Install::copy(built, machine.bundledHost, "\nthe earlier bundle's host\n"), "premise: the earlier app bundles a host");
    const Install holder = machine.holderAt(machine.bundledHost);
    const std::string holderLeaf = holder.socketLeaf();
    const pid_t holderProcess = startDaemon(holder);
    Person person;
    person.instance = { .low = 0x62756e646c65, .high = 4 };
    person.workspace = "bundled";
    if (holderProcess <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(machine.home + "/" + holderLeaf) || !person.attach(holder)
        || !person.waitForShell()) {
        CHECK_MSG(false, "a person attaches to the earlier bundle's host");
        machine.finish();
        return;
    }
    CHECK_MSG(Ubiquitty::HostTest::sendInput(*person.connection, "echo before-bun\"\"dle-update\r") && person.waitForText("before-bundle-update"),
        "the earlier bundle's shell answers");

    // The update swaps the whole bundle, as an updater does.
    const std::string staged = machine.root + "/Applications/Ubiquitty.app.update";
    const std::string retired = machine.root + "/Applications/Ubiquitty.app.retired";
    std::error_code error;
    std::filesystem::create_directories(staged + "/Contents/Helpers", error);
    std::filesystem::create_directories(staged + "/Contents/MacOS", error);
    CHECK_MSG(!error && static_cast<bool>(std::ofstream(staged + "/Contents/MacOS/Ubiquitty") << "the updated app\n")
            && Install::copy(built, staged + "/Contents/Helpers/ubiquitty-host", "\nthe updated bundle's host\n")
            && ::rename(machine.bundleRoot.c_str(), retired.c_str()) == 0 && ::rename(staged.c_str(), machine.bundleRoot.c_str()) == 0,
        "premise: the update puts a new bundle in the earlier one's place");
    const std::string successorLeaf = Ubiquitty::Host::socketLeafForBinary(machine.bundledHost.c_str());
    CHECK_MSG(successorLeaf != holderLeaf, "premise: the update bundles another build");
    if (way == InstallWay::Copied)
        CHECK_MSG(::chmod(machine.bundledHost.c_str(), 0775) == 0, "premise: the updated bundle's host cannot be linked");

    const auto outcome = Session::upgradeLocalHost(machine.bundledHost, Session::LocalHostBuild::Release);
    if (way == InstallWay::Linked) {
        CHECK_MSG(outcome.install.kind == Session::LocalHostInstallKind::Linked && isSameFile(machine.installPath, machine.bundledHost),
            "the updated bundle's host is linked into the control directory");
        CHECK_MSG(Ubiquitty::Host::interfacePathForHost(machine.installPath) == machine.interface, "the install names the updated app");
    } else {
        CHECK_MSG(outcome.install.kind == Session::LocalHostInstallKind::Copied && !isSameFile(machine.installPath, machine.bundledHost),
            "the updated bundle's host is copied into the control directory");
    }
    CHECK_MSG(outcome.didStartSuccessor && outcome.isBundledHostAnswering, "the updated bundle's host is started, wherever its install is");
    CHECK_MSG(waitForExit(holderProcess, 30'000) == std::optional { 0 }, "the earlier bundle's host hands over and exits cleanly");
    CHECK_MSG(outcome.olderHostCount == 0, "no older host serves afterwards");
    std::string body;
    CHECK_MSG(Ubiquitty::HostTest::waitFor(*person.connection, Sync::FrameKind::Bye, body, 5000), "the person is told the holder is handing over");
    ::close(person.connection->fileDescriptor);
    const Sync::ResumeRequest resume { .attachmentID = person.attachmentID, .resumeSecret = person.resumeSecret, .panes = {} };
    const Install successor = machine.holderAt(machine.bundledHost);
    CHECK_MSG(person.attach(successor, resume) && person.waitForText("before-bundle-update"), "the person resumes the session on the updated bundle's host");
    CHECK_MSG(Ubiquitty::HostTest::sendInput(*person.connection, "exit\r")
            && Ubiquitty::HostTest::waitFor(*person.connection, Sync::FrameKind::PaneUnbind, body, 10'000),
        "the inherited shell ends on the successor");
    ::close(person.connection->fileDescriptor);
    machine.finish();
}

/// 4: installs that change nothing, a copy, a newer install, and two at once.
void refusesAnUpdateHostWhoseBinaryStatesAnotherVersion(const std::string& built)
{
    Machine machine;
    if (!machine.create("lhu")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    machine.adopt();
    CHECK_MSG(Install::copy(built, machine.bundledHost), "premise: the update archive carries a host");
    const auto outcome = Session::upgradeLocalHostForUpdate(machine.bundledHost, Session::LocalHostBuild::Release, "0.0.0");
    CHECK_MSG(outcome.install.kind == Session::LocalHostInstallKind::Refused, "a host stating another version cannot be installed for the update");
    CHECK_MSG(::access(machine.installPath.c_str(), F_OK) != 0, "the mismatched host changed no install");
    const auto admitted = Session::upgradeLocalHostForUpdate(machine.bundledHost, Session::LocalHostBuild::Release, std::string(Sync::appVersion()));
    CHECK_MSG((admitted.isInstalled || admitted.install.kind == Session::LocalHostInstallKind::Copied) && admitted.olderHostCount == 0,
        "the host stating the update's version is installed before takeover");
    machine.finish();
}

void installsOnlyWhatItCan(const std::string& built)
{
    Machine machine;
    if (!machine.create("lhi")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    const std::string version(Sync::appVersion());
    CHECK_MSG(Install::copy(built, machine.bundledHost, "\nbundled\n"), "premise: the app bundles a host");
    const std::string directory = machine.home + "/occupied";
    CHECK_MSG(::mkdir(directory.c_str(), 0700) == 0, "premise: a directory at the install path");
    const auto overDirectory = Session::installBundledHost(machine.bundledHost, directory, version);
    CHECK_MSG(overDirectory.kind == Session::LocalHostInstallKind::Refused && !overDirectory.reason.empty(), "nothing replaces a directory");
    const auto intoNowhere = Session::installBundledHost(machine.bundledHost, machine.root + "/missing/ubiquitty-host", version);
    CHECK_MSG(intoNowhere.kind == Session::LocalHostInstallKind::Refused, "nothing is installed into a directory that is not there");
    CHECK_MSG(Session::installBundledHost(machine.bundledHost, "ubiquitty-host", version).kind == Session::LocalHostInstallKind::Refused,
        "nothing is installed at a relative path");

    // A bundled file others can write is copied, owner-only, rather than
    // made the install's own; the copy names the app, which a host started
    // from it trusts while the app's helper holds the same code (step 6).
    CHECK_MSG(::chmod(machine.bundledHost.c_str(), 0775) == 0, "premise: the bundled host is writable by its group");
    const auto copied = Session::installBundledHost(machine.bundledHost, machine.installPath, version);
    CHECK_MSG(copied.kind == Session::LocalHostInstallKind::Copied && !copied.reason.empty(), "a bundled host others can write is copied");
    CHECK_MSG(!isSameFile(machine.installPath, machine.bundledHost) && fileText(machine.bundledHost) == fileText(machine.installPath),
        "the copy holds the bundled host's bytes in a file of its own");
    struct stat installed {};
    CHECK_MSG(::stat(machine.installPath.c_str(), &installed) == 0 && (installed.st_mode & 0777) == 0700, "the copy is its owner's alone");
    std::error_code linkError;
    CHECK_MSG(std::filesystem::read_symlink(machine.installPath + ".interface", linkError) == machine.interface && !linkError, "the copy names the app");
    CHECK_MSG(Session::installBundledHost(machine.bundledHost, machine.installPath, version).kind == Session::LocalHostInstallKind::Copied,
        "the copy stays while the bundled host cannot be linked");
    // Once it can be, the copy gives way to the bundled file itself.
    CHECK_MSG(::chmod(machine.bundledHost.c_str(), 0755) == 0, "premise: the bundled host is its owner's to write");
    CHECK_MSG(Session::installBundledHost(machine.bundledHost, machine.installPath, version).kind == Session::LocalHostInstallKind::Linked
            && isSameFile(machine.installPath, machine.bundledHost),
        "an equal copy gives way to a link once the bundled host can be linked");

    // Another filesystem, where one is at hand.
    struct stat here {};
    std::string elsewhere;
    for (const char* candidate : { "/dev/shm", "/var/tmp" }) {
        struct stat there {};
        if (::stat(machine.root.c_str(), &here) == 0 && ::stat(candidate, &there) == 0 && S_ISDIR(there.st_mode) && there.st_dev != here.st_dev
            && ::access(candidate, W_OK) == 0) {
            std::string pattern = std::string(candidate) + "/ubq-lhx.XXXXXX";
            if (::mkdtemp(pattern.data())) {
                elsewhere = pattern;
                break;
            }
        }
    }
    if (elsewhere.empty()) {
        std::printf("local_host_upgrade: no writable directory on another filesystem; the copy across filesystems is not run here\n");
    } else {
        const std::string bundle = elsewhere + "/ubiquitty-host";
        CHECK_MSG(Install::copy(built, bundle, "\nacross filesystems\n"), "premise: a bundle on another filesystem");
        CHECK_MSG(Session::installBundledHost(bundle, machine.installPath, version).kind == Session::LocalHostInstallKind::Copied,
            "a bundle on another filesystem is copied");
        CHECK_MSG(fileText(bundle) == fileText(machine.installPath), "the copy holds the bundled host's bytes");
        std::error_code ignored;
        std::filesystem::remove_all(elsewhere, ignored);
    }

    // A host from a newer build is never replaced by an older app's.
    const std::string newerInstall = machine.home + "/newer-host";
    CHECK_MSG(static_cast<bool>(std::ofstream(newerInstall) << "#!/bin/sh\necho '99.0.0 (a newer build)'\n") && ::chmod(newerInstall.c_str(), 0755) == 0,
        "premise: a newer host is installed");
    const std::string newerBytes = fileText(newerInstall);
    const auto newer = Session::installBundledHost(machine.bundledHost, newerInstall, version);
    CHECK_MSG(newer.kind == Session::LocalHostInstallKind::NewerInstalled && newer.reason == "99.0.0", "a newer installed host is left as it is");
    CHECK_MSG(fileText(newerInstall) == newerBytes, "the newer host's file is unchanged");
    Session::LocalHostUpgradeFacts facts;
    facts.isNewerInstalled = true;
    CHECK_MSG(Session::localHostUpgradeTitle(facts) == "This Mac Runs a Newer Ubiquitty Host", "the person is told a newer host stays");
    // An older one is replaced.
    CHECK_MSG(static_cast<bool>(std::ofstream(newerInstall) << "#!/bin/sh\necho '0.0.1 (an older build)'\n"), "premise: an older host is installed");
    CHECK_MSG(Session::installBundledHost(machine.bundledHost, newerInstall, version).kind == Session::LocalHostInstallKind::Linked,
        "an older installed host is replaced");
    // One too slow to say its version is not known to be older, and stays;
    // one that says none, or says one and fails, is replaced.
    const std::string slowInstall = machine.home + "/slow-host";
    CHECK_MSG(static_cast<bool>(std::ofstream(slowInstall) << "#!/bin/sh\nexec sleep 30\n") && ::chmod(slowInstall.c_str(), 0755) == 0,
        "premise: an installed host that does not answer");
    const std::string slowBytes = fileText(slowInstall);
    const auto slow = Session::installBundledHost(machine.bundledHost, slowInstall, version);
    CHECK_MSG(slow.kind == Session::LocalHostInstallKind::Refused && fileText(slowInstall) == slowBytes, "a host too slow to say its version is left as it is");
    constexpr std::array<std::string_view, 2> muteScripts { "#!/bin/sh\necho not-a-version\n", "#!/bin/sh\necho '99.0.0 (a newer build)'\nexit 3\n" };
    for (const std::string_view script : muteScripts) {
        const std::string mute = machine.home + "/mute-host";
        CHECK_MSG(static_cast<bool>(std::ofstream(mute) << script) && ::chmod(mute.c_str(), 0755) == 0, "premise: an installed host that says no version");
        CHECK_MSG(Session::installBundledHost(machine.bundledHost, mute, version).kind == Session::LocalHostInstallKind::Linked,
            "a host that says no version is replaced");
        (void)::unlink(mute.c_str());
    }

    // Two installs at once, as two upgrades in one app could start them,
    // each stage under a name of its own and leave one install.
    const std::string together = machine.home + "/together-host";
    std::array<Session::LocalHostInstallKind, 2> kinds {};
    {
        std::thread first([&] { kinds[0] = Session::installBundledHost(machine.bundledHost, together, version).kind; });
        std::thread second([&] { kinds[1] = Session::installBundledHost(machine.bundledHost, together, version).kind; });
        first.join();
        second.join();
    }
    const auto isInstalled = [](Session::LocalHostInstallKind kind) { return kind == Session::LocalHostInstallKind::Linked || kind == Session::LocalHostInstallKind::Current; };
    CHECK_MSG(isInstalled(kinds[0]) && isInstalled(kinds[1]) && isSameFile(together, machine.bundledHost), "two installs at once leave the bundled host installed");

    // Nothing is left staged beside the installs.
    bool isStageLeft = false;
    std::error_code listing;
    for (const auto& entry : std::filesystem::directory_iterator(machine.home, listing))
        isStageLeft |= entry.path().filename().string().starts_with(".ubiquitty-host.");
    CHECK_MSG(!listing, "the control directory lists");
    CHECK_MSG(!isStageLeft, "no stage is left beside the install");
    machine.finish();
}

/// 5: an install a holder would not admit still has the bundled host
/// started; once the install is linked, the idle bundled host starts again
/// and takes the holder over. One that runs a session is not asked to stop.
void startsTheBundledHostForEveryOlderHost(const std::string& built)
{
    Machine machine;
    if (!machine.create("lhc")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    machine.adopt();
    CHECK_MSG(Install::copy(built, machine.installPath, "\nan older host\n") && Install::copy(built, machine.bundledHost, "\nthe app's bundled host\n"),
        "premise: an older host is installed and the app bundles a newer one");
    const Install holder = machine.holderAt(machine.installPath);
    const std::string bundledLeaf = Ubiquitty::Host::socketLeafForBinary(machine.bundledHost.c_str());
    const std::string bundledLog = logPathFor(machine.home, bundledLeaf);
    const pid_t holderProcess = startDaemon(holder);
    Person person;
    person.instance = { .low = 0x636f7079, .high = 5 };
    person.workspace = "copied";
    if (holderProcess <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(machine.home + "/" + holder.socketLeaf()) || !person.attach(holder)) {
        CHECK_MSG(false, "a person attaches to the older host");
        machine.finish();
        return;
    }
    CHECK_MSG(::chmod(machine.bundledHost.c_str(), 0775) == 0, "premise: the bundled host cannot be linked");
    const auto copied = Session::upgradeLocalHost(machine.bundledHost, Session::LocalHostBuild::Release);
    CHECK_MSG(copied.install.kind == Session::LocalHostInstallKind::Copied && !copied.isInstalled && !copied.install.isOnAnotherVolume,
        "the bundled host is copied, not installed as itself");
    CHECK_MSG(copied.didStartSuccessor && copied.isBundledHostAnswering, "the bundled host is started to ask the older host");
    CHECK_MSG(waitForFileText(bundledLog, "1 older hosts kept theirs", 5000), "the older host, installed where the copy is, refuses it");
    CHECK_MSG(copied.olderHostCount == 1 && copied.refusingHostCount == 1 && !waitForExit(holderProcess, 500),
        "the older host keeps serving, and is counted as refusing");
    Session::LocalHostUpgradeFacts facts;
    facts.olderHostCount = copied.olderHostCount;
    facts.isMoveAdvised = copied.refusingHostCount && copied.install.isOnAnotherVolume;
    CHECK_MSG(Session::localHostUpgradeTitle(facts) == "An Older Ubiquitty Host Keeps Its Sessions"
            && Session::localHostUpgradeSentence(facts).find("Applications folder") == std::string::npos,
        "the person is told what was left undone, and not to move an app that is where it can be linked from");
    const auto refused = detachedHost(machine.home, bundledLeaf);
    CHECK_MSG(refused.has_value(), "premise: the refused bundled host still runs, holding nothing");

    CHECK_MSG(::chmod(machine.bundledHost.c_str(), 0755) == 0, "premise: the bundled host can be linked");
    const auto linked = Session::upgradeLocalHost(machine.bundledHost, Session::LocalHostBuild::Release);
    CHECK_MSG(linked.install.kind == Session::LocalHostInstallKind::Linked && linked.didStartSuccessor, "the next upgrade links the bundled host and starts it again");
    CHECK_MSG(waitForFileText(bundledLog, "asked to stop while it holds nothing", 5000) && refused && ::kill(*refused, 0) != 0,
        "the idle bundled host stopped to ask again");
    CHECK_MSG(waitForExit(holderProcess, 30'000) == std::optional { 0 } && linked.olderHostCount == 0, "the older host hands over and exits");
    std::string body;
    CHECK_MSG(Ubiquitty::HostTest::waitFor(*person.connection, Sync::FrameKind::Bye, body, 5000), "the person is told the holder is handing over");
    ::close(person.connection->fileDescriptor);
    // The bundled host runs a session now, and an upgrade asks nothing of it.
    const auto settled = Session::upgradeLocalHost(machine.bundledHost, Session::LocalHostBuild::Release);
    CHECK_MSG(settled.install.kind == Session::LocalHostInstallKind::Current && !settled.didStartSuccessor && settled.isBundledHostAnswering
            && settled.olderHostCount == 0,
        "an upgrade with nothing left to take starts nothing");

    // Another older host beside it: the bundled host, which runs a session,
    // is neither asked to stop nor waited for.
    const Install second = machine.holderAt(machine.home + "/second-host");
    CHECK_MSG(Install::copy(built, second.binary, "\na second older host\n"), "premise: a second older host is installed");
    const pid_t secondProcess = startDaemon(second);
    CHECK_MSG(secondProcess > 0 && Ubiquitty::HostTest::waitForDaemonReady(machine.home + "/" + second.socketLeaf()), "premise: the second older host listens");
    const auto asked = std::chrono::steady_clock::now();
    const auto busy = Session::upgradeLocalHost(machine.bundledHost, Session::LocalHostBuild::Release);
    const auto took = std::chrono::steady_clock::now() - asked;
    CHECK_MSG(!busy.didStartSuccessor && busy.isBundledHostAnswering && busy.olderHostCount == 1 && busy.refusingHostCount == 1,
        "a bundled host that runs a session keeps serving, and the second older host is counted");
    CHECK_MSG(took < std::chrono::milliseconds(2500), "the upgrade does not wait on a host that runs a session");
    CHECK_MSG(fileText(bundledLog).find("it keeps serving") == std::string::npos, "the bundled host is not asked to stop");
    if (secondProcess > 0) {
        ::kill(secondProcess, SIGTERM);
        (void)waitForExit(secondProcess, 5000);
    }
    if (const auto successor = detachedHost(machine.home, bundledLeaf)) {
        // Holding a session, it refuses to stop when idle stops are asked.
        CHECK_MSG(::kill(*successor, SIGUSR2) == 0 && waitForFileText(bundledLog, "holds 1 sessions; it keeps serving", 5000),
            "a host that holds a session keeps serving when asked to stop idle");
    }
    machine.finish();
}

/// 6: a host started from a copy trusts the app whose helper holds the
/// copy's signed code, as the app greeting it directly finds.
void trustsTheAppAHostWasCopiedFrom(const std::string& built, const std::string& self)
{
    Machine machine;
    if (!machine.create("lht")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    machine.adopt();
    CHECK_MSG(Install::copy(built, machine.bundledHost, "\nthe app's bundled host\n") && ::chmod(machine.bundledHost.c_str(), 0775) == 0,
        "premise: the app bundles a host that cannot be linked");
    // This binary is the app: it greets the host directly, as the app does.
    CHECK_MSG(Install::copy(self, machine.interface), "premise: the app's executable");
    const auto installed = Session::installBundledHost(machine.bundledHost, machine.installPath, Sync::appVersion());
    CHECK_MSG(installed.kind == Session::LocalHostInstallKind::Copied, "the bundled host is copied");
    CHECK_MSG(runAndRead({ machine.installPath, "--ensure-daemon", "--quic-port", "-1" }).has_value(), "a host starts from the copy, as an SSH bootstrap starts it");
    const std::string leaf = Ubiquitty::Host::socketLeafForBinary(machine.installPath.c_str());
    const auto answer = runAndRead({ machine.interface, "--greet-control", machine.home + "/" + leaf });
#if defined(__APPLE__)
    CHECK_MSG(Ubiquitty::Host::hasSameCode(machine.installPath, machine.bundledHost), "premise: the copy holds the bundled host's signed code");
    CHECK_MSG(answer && answer->starts_with("welcome"), "the host started from the copy admits the app whose helper holds its code");
#else
    CHECK_MSG(answer && answer->starts_with("refused"), "where code is not signed, a host started from a copy trusts no app");
#endif
    // An app whose helper holds other code is not the copy's.
    CHECK_MSG(Install::copy(built, machine.bundledHost, "\nanother build's host\n"), "premise: the app's helper is replaced by another build");
    CHECK_MSG(Ubiquitty::Host::interfacePathForHost(machine.installPath).empty(), "a copy names no app whose helper holds other code");
    machine.finish();
}

#if defined(__APPLE__)
/// Signs a host binary in place under its identifier, ad hoc for "-".
///
/// - Parameters:
///   - path: The binary, with no bytes appended, which codesign refuses.
///   - identity: The code-signing identity, or "-".
///   - option: A signing option to state, telling two signatures apart.
/// - Returns: Whether codesign signed it.
bool signHost(const std::string& path, const std::string& identity, const std::string& option)
{
    std::vector<std::string> arguments { "/usr/bin/codesign", "--force", "--sign", identity, "--identifier", std::string(Ubiquitty::Host::hostSigningIdentifier) };
    // The machines this test adopts replace HOME, which hides the person's
    // keychain search list from codesign; a keychain named by path is read
    // whatever HOME says.
    if (const char* keychain = ::getenv("UBQ_TEAM_SIGN_KEYCHAIN"); keychain && keychain[0] && identity != "-") {
        arguments.emplace_back("--keychain");
        arguments.emplace_back(keychain);
    }
    if (!option.empty()) {
        arguments.emplace_back("--options");
        arguments.push_back(option);
    }
    arguments.push_back(path);
    return runAndRead(arguments).has_value();
}

/// A configured development identity may sign code without the release team's
/// Apple-issued certificate. Only the latter can exercise the team route.
bool hasTrustedTeamSignature(const std::string& path)
{
    const std::string requirement = "=" + Ubiquitty::Host::trustedHostSignatureRequirement();
    const pid_t child = ::fork();
    if (child == 0) {
        std::array<char*, 7> arguments {
            const_cast<char*>("/usr/bin/codesign"), const_cast<char*>("--verify"), const_cast<char*>("--strict"),
            const_cast<char*>("-R"), const_cast<char*>(requirement.c_str()),
            const_cast<char*>(path.c_str()), nullptr
        };
        ::execv(arguments[0], arguments.data());
        _exit(127);
    }
    if (child < 0)
        return false;
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

/// 8, macOS: a host binary the trusted team signed succeeds a holder the
/// team signed wherever it lies, and an ad hoc one beside it does not.
void admitsATeamSignedSuccessorWhereverItLies(const std::string& built)
{
#if defined(__APPLE__)
    const char* identity = ::getenv("UBQ_TEAM_SIGN_IDENTITY");
    if (!identity || !identity[0]) {
        std::printf("local_host_upgrade: UBQ_TEAM_SIGN_IDENTITY names no identity of the team; a successor by signature is not run here\n");
        return;
    }
    Machine machine;
    if (!machine.create("lhs8")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    // Two signatures of one build, told apart by an option, so the two own
    // two sockets. They are made before the machine takes HOME over, because
    // codesign finds the team's identity through the person's own keychains.
    const bool didSignHolder = Install::copy(built, machine.installPath) && signHost(machine.installPath, identity, {});
    const bool didSignBundled = Install::copy(built, machine.bundledHost) && signHost(machine.bundledHost, identity, "runtime");
    CHECK_MSG(didSignHolder, "premise: the holder is signed with the configured identity");
    CHECK_MSG(didSignBundled, "premise: the bundled host is signed with the configured identity");
    if (!didSignHolder || !didSignBundled) {
        machine.finish();
        return;
    }
    if (!hasTrustedTeamSignature(machine.installPath) || !hasTrustedTeamSignature(machine.bundledHost)) {
        std::printf("local_host_upgrade: configured signing identity lacks the trusted team's Apple-issued certificate; team-route case not run\n");
        machine.finish();
        return;
    }
    machine.adopt();
    const Install holder = machine.holderAt(machine.installPath);
    const std::string bundledLeaf = Ubiquitty::Host::socketLeafForBinary(machine.bundledHost.c_str());
    CHECK_MSG(holder.socketLeaf() != bundledLeaf && !isSameFile(machine.installPath, machine.bundledHost), "premise: two files, two sockets");
    const pid_t holderProcess = startDaemon(holder);
    Person person;
    person.instance = { .low = 0x7465616d, .high = 8 };
    person.workspace = "signed";
    if (holderProcess <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(machine.home + "/" + holder.socketLeaf()) || !person.attach(holder)
        || !person.waitForShell()) {
        CHECK_MSG(false, "a person attaches to the holder the team signed");
        machine.finish();
        return;
    }
    // Not installed over the holder: the bundled host started where it lies.
    const auto ready = Session::readyLocalDaemon(machine.bundledHost);
    CHECK_MSG(!ready.socketPath.empty(), "the bundled host the team signed starts where it lies");
    CHECK_MSG(waitForExit(holderProcess, 30'000) == std::optional { 0 }, "the holder hands over to a host binary the team signed, though it lies elsewhere");
    CHECK_MSG(waitForFileText(logPathFor(machine.home, bundledLeaf), "took over 1 sessions; 0 older hosts kept theirs", 5000),
        "the successor took the holder's session");
    std::string body;
    CHECK_MSG(Ubiquitty::HostTest::waitFor(*person.connection, Sync::FrameKind::Bye, body, 5000), "the person is told the holder is handing over");
    ::close(person.connection->fileDescriptor);
    // An ad hoc host beside it asks for the sessions and is refused: the
    // team's holder admits no binary the team did not sign, but by place.
    const std::string adHoc = machine.root + "/ad-hoc/ubiquitty-host";
    std::error_code error;
    std::filesystem::create_directories(machine.root + "/ad-hoc", error);
    CHECK_MSG(!error && Install::copy(built, adHoc) && signHost(adHoc, "-", {}), "premise: a host signed ad hoc");
    const std::string adHocLeaf = Ubiquitty::Host::socketLeafForBinary(adHoc.c_str());
    CHECK_MSG(!Session::readyLocalDaemon(adHoc).socketPath.empty(), "the ad hoc host starts beside the successor");
    CHECK_MSG(waitForFileText(logPathFor(machine.home, adHocLeaf), "older hosts kept theirs", 5000)
            && fileText(logPathFor(machine.home, adHocLeaf)).find(" 0 older hosts kept theirs") == std::string::npos,
        "the team's host refuses a successor signed ad hoc");
    const auto successor = detachedHost(machine.home, bundledLeaf);
    CHECK_MSG(successor && ::kill(*successor, 0) == 0, "and keeps serving the session it took");
    machine.finish();
#else
    (void)built;
#endif
}

/// 7: an idle stop asked of a host from the moment its socket answers, and
/// while a client is still greeting.
void answersAnIdleStopOnlyWhenItCan(const std::string& built)
{
    Machine machine;
    if (!machine.create("lhs")) {
        CHECK_MSG(false, "a private machine");
        return;
    }
    machine.adopt();
    CHECK_MSG(Install::copy(built, machine.bundledHost, "\nthe app's bundled host\n"), "premise: the app bundles a host");
    const std::string leaf = Ubiquitty::Host::socketLeafForBinary(machine.bundledHost.c_str());
    const std::string socket = machine.home + "/" + leaf;
    // Each start is asked to stop from the moment its socket answers until
    // its launcher has returned, as an upgrade may ask: it says it listens,
    // then stops as asked, never by the signal. Its log says both. The
    // asker's own connections, which never greet, hold the stop until it
    // stops asking; a start that stops before its launcher looks a last
    // time leaves the launcher with no answer, which is its due.
    constexpr int starts = 8;
    for (int start = 0; start < starts; ++start) {
        std::atomic<bool> isAsking { true };
        std::thread asker([&] {
            while (isAsking.load()) {
                if (const auto process = listeningProcess(socket))
                    ::kill(*process, SIGUSR2);
            }
        });
        (void)Session::readyLocalDaemon(machine.bundledHost);
        isAsking = false;
        asker.join();
        if (const auto process = listeningProcess(socket))
            ::kill(*process, SIGUSR2);
        bool isGone = false;
        for (int attempt = 0; attempt < 500 && !isGone; ++attempt) {
            const int channel = Ubiquitty::HostTest::connectDaemon(socket);
            isGone = channel < 0;
            if (channel >= 0)
                ::close(channel);
            ::usleep(10'000);
        }
        CHECK_MSG(isGone, "a host asked to stop as its socket first answers stops");
    }
    const std::string log = fileText(logPathFor(machine.home, leaf));
    const auto occurrences = [&log](std::string_view words) {
        int count = 0;
        for (auto at = log.find(words); at != std::string::npos; at = log.find(words, at + 1))
            ++count;
        return count;
    };
    CHECK_MSG(occurrences("serving protocol") == starts, "every start listened before it stopped");
    CHECK_MSG(occurrences("asked to stop while it holds nothing; it stops") == starts, "every start stopped as asked, none by the signal");

    // A client that has connected and not yet greeted holds the stop.
    const pid_t host = startDaemon(machine.holderAt(machine.bundledHost));
    if (host <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(socket)) {
        CHECK_MSG(false, "premise: a host listens");
        machine.finish();
        return;
    }
    Connection greeting;
    greeting.fileDescriptor = Ubiquitty::HostTest::connectDaemon(socket);
    CHECK_MSG(greeting.fileDescriptor >= 0, "premise: a client connects");
    // Accepted before it is asked.
    ::usleep(300'000);
    CHECK_MSG(::kill(host, SIGUSR2) == 0 && !waitForExit(host, 500), "a host asked to stop while a client is greeting keeps serving");
    std::string body;
    CHECK_MSG(Ubiquitty::HostTest::sendGreeting(greeting, controlGreeting({ .low = 0x6772656574, .high = 7 }))
            && (Ubiquitty::HostTest::waitFor(greeting, Sync::FrameKind::Welcome, body, 5000) || greeting.admission == Ubiquitty::HostTest::FixtureAdmission::Closed),
        "the greeting client is answered");
    ::close(greeting.fileDescriptor);
    CHECK_MSG(waitForExit(host, 5000) == std::optional { 0 }, "once no client is greeting, a host that holds nothing stops as asked");
    machine.finish();
}

enum class RemoteHolderPlacement {
    Installed,
    Elsewhere,
};

void remoteUpdateKeepsLiveSessions(const std::string& built, RemoteHolderPlacement placement)
{
    Machine machine;
    if (!machine.create("remote-up")) {
        CHECK_MSG(false, "a private remote machine");
        return;
    }
    machine.adopt();
    const std::string holderPath = placement == RemoteHolderPlacement::Elsewhere ? machine.root + "/other-host" : machine.installPath;
    CHECK_MSG(Install::copy(built, holderPath, "\nremote predecessor\n"), "premise: a distinct installed predecessor");
    const auto holder = machine.holderAt(holderPath);
    const auto leaf = holder.socketLeaf();
    const pid_t process = startDaemon(holder);
    Person first;
    first.instance = { .low = 0x72656d6f7465, .high = 1 };
    first.workspace = "remote-upgrade";
    if (process <= 0 || !Ubiquitty::HostTest::waitForDaemonReady(machine.home + "/" + leaf) || !first.attach(holder) || !first.waitForShell()) {
        CHECK_MSG(false, "premise: the predecessor runs a shell");
        machine.finish();
        return;
    }
    const auto workspace = first.connection->welcome->selection.workspaceID;
    const auto instance = first.connection->welcome->selection.daemonInstanceID;
    const Sync::AdmissionRequest request { .target = Sync::AdmissionTarget::Workspace, .sessionID = first.sessionID, .workspaceID = workspace, .expectedDaemonInstanceID = instance, .isAttachOnly = true };
    Person second;
    second.instance = { .low = 0x72656d6f7465, .high = 2 };
    CHECK_MSG(greetPinned(second, holderPath, machine.home, leaf, request).has_value(), "a second client attaches to the same workspace");
    CHECK_MSG(Ubiquitty::HostTest::sendInput(*first.connection,
                  "UBQ_UPGRADE_PID=$$; echo before-remote-up\"\"grade; i=0; while [ $i -lt 40 ]; do echo history-$i; i=$((i+1)); done\r")
            && first.waitForText("history-39"),
        "the shell keeps process state and fills scrollback before the update");

    const auto script = std::string("export SSH_CONNECTION=\"192.0.2.1 42000 192.0.2.2 22\"; export SSH_CLIENT=\"192.0.2.1 42000 22\"; ")
        + Session::hostInstallCommand(Ubiquitty::Crypto::sha256Hex(fileText(built)), Session::HostInstallMode::Now);
    // The install reads its upload from standard input, as the SSH command does.
    const auto installed = runAndRead({ "/bin/sh", "-c", "(" + script + ") < " + Ubiquitty::Transport::shellSingleQuote(built) });
    CHECK_MSG(installed.has_value(), "the remote installer returns an outcome");
    const auto outcome = Session::readHostInstallOutput(installed.value_or(""), installed.value_or(""));
    if (placement == RemoteHolderPlacement::Elsewhere) {
        CHECK_MSG(outcome.kind == Session::HostInstallOutcomeKind::HandoverPending, "a refused takeover is pending, not successful");
        const auto retry = runAndRead({ "/bin/sh", "-c", "(" + script + ") < " + Ubiquitty::Transport::shellSingleQuote(built) });
        CHECK_MSG(retry && Session::readHostInstallOutput(*retry, *retry).kind == Session::HostInstallOutcomeKind::HandoverPending,
            "retrying a refused update remains pending without stopping a session");
        CHECK_MSG(!waitForExit(process, 100), "a refusing predecessor keeps running");
        CHECK_MSG(Ubiquitty::HostTest::sendInput(*first.connection, "[ \"$UBQ_UPGRADE_PID\" = \"$$\" ] && echo still-same-sh\"\"ell\r")
                && first.waitForText("still-same-shell"),
            "a refused update preserves the original shell and its PTY");
    } else {
        CHECK_MSG(outcome.kind == Session::HostInstallOutcomeKind::Installed, "the remote update completes live handover");
        CHECK_MSG(waitForExit(process, 30'000) == std::optional { 0 }, "the predecessor exits cleanly after handover");
        for (Person* client : { &first, &second }) {
            const auto attachment = client->connection->welcome->attachmentID;
            const auto secret = client->connection->welcome->resumeSecret;
            const auto nextInput = client->connection->nextInputSeq;
            ::close(client->connection->fileDescriptor);
            client->connection->fileDescriptor = -1;
            const auto welcome = greetPinned(*client, machine.installPath, machine.home, leaf, request,
                Sync::ResumeRequest { .attachmentID = attachment, .resumeSecret = secret, .panes = {} });
            CHECK_MSG(welcome && welcome->attachmentID == attachment && welcome->selection.workspaceID == workspace
                    && welcome->selection.sessionID == first.sessionID,
                "each client resumes its attachment and original workspace on the successor");
            client->connection->nextInputSeq = nextInput;
            CHECK_MSG(Ubiquitty::HostTest::waitForPane(*client->connection, 5000), "the resumed client receives its pane");
            CHECK_MSG(!client->connection->panes.empty() && client->connection->panes.front().paneID == first.paneID, "handover preserves pane identity");
            CHECK_MSG(client->waitForText("before-remote-upgrade"), "each resumed client retains the scrollback");
        }
        CHECK_MSG(Ubiquitty::HostTest::sendInput(*first.connection, "[ \"$UBQ_UPGRADE_PID\" = \"$$\" ] && echo same-sh\"\"ell-after-upgrade\r")
                && first.waitForText("same-shell-after-upgrade"),
            "the original shell process and its PTY run on after the remote update");
    }
    for (Person* client : { &first, &second }) {
        if (client->connection && client->connection->fileDescriptor >= 0) {
            ::close(client->connection->fileDescriptor);
            client->connection->fileDescriptor = -1;
        }
    }
    machine.finish();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--greet-control")
        return greetAsTheApp(argv[2]);
    if (argc != 2 && argc != 4) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host> [<older ubiquitty-host> <older ubq>]\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    const std::string built = argv[1];
    std::error_code selfError;
    const std::string self = std::filesystem::canonical(argv[0], selfError).string();
    upgradesAHostInstalledInTheControlDirectory(built, argc == 4 ? argv[2] : "", argc == 4 ? argv[3] : "");
    if (argc == 2) {
        remoteUpdateKeepsLiveSessions(built, RemoteHolderPlacement::Installed);
        remoteUpdateKeepsLiveSessions(built, RemoteHolderPlacement::Elsewhere);
        upgradesAHostStartedFromAnEarlierBundle(built, InstallWay::Linked);
        upgradesAHostStartedFromAnEarlierBundle(built, InstallWay::Copied);
        refusesAnUpdateHostWhoseBinaryStatesAnotherVersion(built);
        installsOnlyWhatItCan(built);
        startsTheBundledHostForEveryOlderHost(built);
        CHECK_MSG(!selfError, "premise: this binary's own path");
        if (!selfError)
            trustsTheAppAHostWasCopiedFrom(built, self);
        answersAnIdleStopOnlyWhenItCan(built);
        admitsATeamSignedSuccessorWhereverItLies(built);
    }
    endChildren();
    if (failures == 0)
        std::printf(argc == 4 ? "local_host_upgrade: an older release's host hands its live session to the signed bundled successor, "
                                "before or after that successor is installed at the control path\n"
                              : "local_host_upgrade: a host installed in the control directory refuses the bundled host started where it lies and hands "
                                "every session to it once it is linked over the install; a saved reference to its workspace reattaches on the successor; "
                                "a host started from an earlier bundle hands over to the updated one, linked or copied; installs change nothing they cannot; "
                                "a copy trusts the app whose helper holds its code; an idle stop never ends a starting host or a greeting client's; "
                                "the team-signature route runs only with an identity whose certificate meets the trusted-team requirement\n");
    return failures == 0 ? 0 : 1;
}
