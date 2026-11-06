// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_hostinstall.cpp — the remote host install (Session/HostInstall.h) and
// the bootstrap step that finishes a deferred one (Session/RemoteBootstrap.h).
//
// The scripts are run, not read: each case gives /bin/sh a private HOME laid
// out like a host's ~/.ubiquitty, streams an upload in on standard input the
// way ssh does, and then looks at what is installed. A daemon holding a
// session is stood in for by a shell with one child, under the pid file the
// real daemon of the installed binary would write.

#include "Core/Crypto/SHA256.h"
#include "Core/Session/DaemonSocketLeaf.h"
#include "Core/Session/HostInstall.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Transport/SSHHost.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using Ubiquitty::Session::HostInstallMode;
using Ubiquitty::Session::HostInstallOutcomeKind;

namespace {

/// A host binary that answers the install's checks and the bootstrap's verbs.
std::string fakeHost(std::string_view version)
{
    return "#!/bin/sh\n"
           "case \"$1\" in\n"
           "--version) echo "
        + std::string(version)
        + " ;;\n"
          "*) exit 0 ;;\n"
          "esac\n";
}

std::string readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void writeFile(const std::string& path, const std::string& contents, mode_t mode)
{
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << contents;
    }
    ::chmod(path.c_str(), mode);
}

bool exists(const std::string& path)
{
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0;
}

// Whether a process is running: an ended one nobody has reaped is not. The
// stand-in daemon is an orphan, reaped by the machine's first process; a
// container started without one keeps it as a zombie, which still answers a
// signal.
bool isAlive(pid_t pid)
{
    if (pid <= 0 || ::kill(pid, 0) != 0)
        return false;
#if defined(__linux__)
    // "pid (comm) state ...": the state follows the last parenthesis, since a
    // command name may hold one.
    std::ifstream status("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(status, line);
    const std::size_t close = line.rfind(')');
    if (close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z')
        return false;
#endif
    return true;
}

/// A private host: HOME, its ~/.ubiquitty, and a stand-in daemon.
class FakeHost {
public:
    FakeHost()
    {
        char pattern[] = "/tmp/ubq-hostinstall-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        m_home = made ? made : "";
        m_directory = m_home + "/.ubiquitty";
        ::mkdir(m_directory.c_str(), 0700);
    }

    ~FakeHost()
    {
        stopDaemon();
        if (m_home.empty())
            return;
        // Cleanup: a home left under /tmp changes no verdict, so the removal's
        // status is not this fixture's to check.
        [[maybe_unused]] const int removalStatus = std::system(("rm -rf " + Ubiquitty::Transport::shellSingleQuote(m_home)).c_str());
    }

    const std::string& directory() const { return m_directory; }
    std::string binary() const { return m_directory + "/ubiquitty-host"; }
    int exitStatus() const { return m_exitStatus; }

    void installHost(const std::string& contents) { writeFile(binary(), contents, 0700); }

    /// Starts a stand-in daemon for the installed binary holding one session.
    ///
    /// The session is the daemon's one child, and it ends once the daemon is
    /// gone: a daemon killed before or while it forks would otherwise orphan a
    /// session that outlived the test, and the suite counts what a test leaves
    /// running.
    void startDaemonWithOneSession()
    {
        const std::string leaf = Ubiquitty::Session::socketLeafForBinary(binary().c_str());
        const std::string pidFile = m_directory + "/" + leaf.substr(0, leaf.size() - 5) + ".pid";
        const std::string command = "sh -c 'sh -c \"while kill -0 \\$PPID 2>/dev/null; do sleep 0.2; done\" & wait' >/dev/null 2>&1 & echo $! > "
            + Ubiquitty::Transport::shellSingleQuote(pidFile);
        CHECK_EQ(std::system(command.c_str()), 0);
        m_daemon = static_cast<pid_t>(std::atoi(readFile(pidFile).c_str()));
        // The daemon's session is its child; wait until the process table shows it.
        for (int attempt = 0; attempt < 200 && sessionsOfDaemon() == 0; ++attempt)
            ::usleep(10'000);
    }

    /// Ends the stand-in daemon's one session; the daemon exits after it.
    void endSession()
    {
        // pkill succeeds only when it signalled a process, and the session it
        // ends here is one the case started.
        CHECK_EQ(std::system(("pkill -P " + std::to_string(m_daemon)).c_str()), 0);
        for (int attempt = 0; attempt < 300 && isAlive(m_daemon); ++attempt)
            ::usleep(10'000);
    }

    void stopDaemon()
    {
        if (m_daemon <= 0)
            return;
        // The session may have ended already, which pkill reports as a failure;
        // teardown needs it gone, not found.
        [[maybe_unused]] const int signalStatus = std::system(("pkill -P " + std::to_string(m_daemon) + " 2>/dev/null").c_str());
        ::kill(m_daemon, SIGKILL);
        m_daemon = 0;
    }

    pid_t daemon() const { return m_daemon; }

    int sessionsOfDaemon() const
    {
        if (m_daemon <= 0)
            return 0;
        const std::string command = "ps -A -o ppid= | grep -c '^ *" + std::to_string(m_daemon) + "$'";
        std::FILE* pipe = ::popen(command.c_str(), "r");
        if (!pipe)
            return 0;
        char buffer[32] = {};
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, pipe);
        ::pclose(pipe);
        return std::atoi(std::string(buffer, read).c_str());
    }

    /// Runs one script as the remote shell would, with `upload` on its
    /// standard input.
    std::string run(const std::string& script, const std::string& upload = {}, std::string_view shell = "/bin/sh")
    {
        const std::string input = m_home + "/upload";
        writeFile(input, upload, 0600);
        const std::string options = shell == "zsh" ? " -f -c " : shell == "fish" ? " --no-config -c "
                                                                                 : " -c ";
        const std::string command = "HOME=" + Ubiquitty::Transport::shellSingleQuote(m_home) + " UBIQUITTY_HOME= "
            + Ubiquitty::Transport::shellSingleQuote(shell) + options
            + Ubiquitty::Transport::shellSingleQuote(Ubiquitty::Transport::remoteShellCommand(script))
            + " < " + Ubiquitty::Transport::shellSingleQuote(input) + " 2>&1";
        std::FILE* pipe = ::popen(command.c_str(), "r");
        if (!pipe)
            return {};
        std::string output;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0)
            output.append(buffer, read);
        const int status = ::pclose(pipe);
        m_exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return output;
    }

    std::vector<std::string> backups() const
    {
        std::vector<std::string> found;
        if (DIR* listing = ::opendir(m_directory.c_str())) {
            while (const dirent* entry = ::readdir(listing)) {
                const std::string name = entry->d_name;
                if (name.starts_with("ubiquitty-host.backup-"))
                    found.push_back(m_directory + "/" + name);
            }
            ::closedir(listing);
        }
        return found;
    }

private:
    std::string m_home;
    std::string m_directory;
    pid_t m_daemon { 0 };
    int m_exitStatus = -1;
};

Ubiquitty::Session::HostInstallOutcome install(FakeHost& host, const std::string& upload, const std::string& claimedDigest, HostInstallMode mode)
{
    const std::string output = host.run(Ubiquitty::Session::hostInstallCommand(claimedDigest, mode), upload);
    return Ubiquitty::Session::readHostInstallOutput(output, output);
}

} // namespace

TEST(aDamagedUploadIsRefusedAndTheInstalledHostStays)
{
    FakeHost host;
    const std::string old = fakeHost("old");
    host.installHost(old);
    const std::string upload = fakeHost("new");
    const std::string damaged = upload.substr(0, upload.size() / 2);
    const auto outcome = install(host, damaged, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::IfIdle);
    CHECK(outcome.kind == HostInstallOutcomeKind::Failed);
    CHECK(outcome.reason.find("arrived damaged") != std::string::npos);
    CHECK(readFile(host.binary()) == old);
    CHECK(!exists(host.directory() + "/ubiquitty-host.new"));
    CHECK(host.backups().empty());
}

TEST(anUploadThatDoesNotRunIsRefused)
{
    FakeHost host;
    const std::string old = fakeHost("old");
    host.installHost(old);
    const std::string upload = "#!/bin/sh\nexit 3\n";
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::IfIdle);
    CHECK(outcome.kind == HostInstallOutcomeKind::Failed);
    CHECK(outcome.reason.find("does not run on this machine") != std::string::npos);
    CHECK(readFile(host.binary()) == old);
    CHECK(!exists(host.directory() + "/ubiquitty-host.new"));
}

TEST(aVerifiedUploadReplacesTheHostAndKeepsOneDatedBackup)
{
    FakeHost host;
    const std::string old = fakeHost("old");
    host.installHost(old);
    // A backup from an earlier update is the one dropped.
    writeFile(host.directory() + "/ubiquitty-host.backup-20000101000000", "older", 0700);
    const std::string upload = fakeHost("new");
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::IfIdle);
    CHECK(outcome.kind == HostInstallOutcomeKind::Installed);
    CHECK(readFile(host.binary()) == upload);
    const auto backups = host.backups();
    CHECK_EQ(backups.size(), std::size_t { 1 });
    if (!backups.empty())
        CHECK(readFile(backups.front()) == old);
    CHECK(!exists(host.directory() + "/ubiquitty-host.new"));
}

TEST(aFirstInstallHasNothingToBackUp)
{
    FakeHost host;
    const std::string upload = fakeHost("new");
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::IfIdle);
    CHECK(outcome.kind == HostInstallOutcomeKind::Installed);
    CHECK(readFile(host.binary()) == upload);
    CHECK(host.backups().empty());
}

TEST(anInstallWithNoBackupFilesSurvivesEveryLoginShell)
{
    for (const std::string shell : { "/bin/sh", "zsh", "fish" }) {
        if (std::system(("command -v " + Ubiquitty::Transport::shellSingleQuote(shell) + " >/dev/null 2>&1").c_str()) != 0) {
            std::printf("remote install: %s unavailable\n", shell.c_str());
            continue;
        }
        for (const HostInstallMode mode : { HostInstallMode::IfIdle, HostInstallMode::Now, HostInstallMode::WhenSessionsEnd }) {
            FakeHost host;
            const std::string old = fakeHost("old");
            host.installHost(old);
            REQUIRE(host.backups().empty());
            const std::string upload = fakeHost("new");
            const auto script = Ubiquitty::Session::hostInstallCommand(Ubiquitty::Crypto::sha256Hex(upload), mode);
            const auto output = host.run(script, upload, shell);
            CHECK_EQ(host.exitStatus(), 0);
            CHECK(output.find("no matches found") == std::string::npos);
            const auto outcome = Ubiquitty::Session::readHostInstallOutput(output, output);
            CHECK_MSG(outcome.kind == HostInstallOutcomeKind::Installed, output.c_str());
            CHECK(readFile(host.binary()) == upload);
            CHECK_EQ(host.backups().size(), 1u);
            const auto probe = Ubiquitty::Session::parseHostProbe(host.run(Ubiquitty::Session::hostProbeCommand(), {}, shell));
            REQUIRE(probe);
            CHECK_EQ(probe->liveSessionCount, 0u);
        }
    }
}

TEST(theProbeCountsTheSessionsOfTheInstalledHostsDaemon)
{
    FakeHost host;
    CHECK(Ubiquitty::Session::parseHostProbe(host.run(Ubiquitty::Session::hostProbeCommand()))->liveSessionCount == 0);
    host.installHost(fakeHost("old"));
    host.startDaemonWithOneSession();
    CHECK_EQ(host.sessionsOfDaemon(), 1);
    const auto probe = Ubiquitty::Session::parseHostProbe(host.run(Ubiquitty::Session::hostProbeCommand()));
    CHECK(probe.has_value());
    if (probe) {
        CHECK_EQ(probe->liveSessionCount, std::size_t { 1 });
        CHECK(!probe->platform.empty());
    }
    host.endSession();
    CHECK(Ubiquitty::Session::parseHostProbe(host.run(Ubiquitty::Session::hostProbeCommand()))->liveSessionCount == 0);
}

// The update the probe found idle installs only while the host is still idle:
// a session that started in between is not ended by it, and the daemon holding
// it is left exactly as it was.
TEST(anIdleOnlyInstallNeverStopsADaemonHoldingASession)
{
    FakeHost host;
    const std::string old = fakeHost("old");
    host.installHost(old);
    host.startDaemonWithOneSession();
    const std::string upload = fakeHost("new");
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::IfIdle);
    CHECK(outcome.kind == HostInstallOutcomeKind::Failed);
    CHECK(outcome.reason.find("1 sessions started") != std::string::npos);
    CHECK(isAlive(host.daemon()));
    CHECK_EQ(host.sessionsOfDaemon(), 1);
    CHECK(readFile(host.binary()) == old);
}

TEST(updatingNowAsksForHandoverWithoutStoppingSessions)
{
    FakeHost host;
    host.installHost(fakeHost("old"));
    host.startDaemonWithOneSession();
    const std::string upload = "#!/bin/sh\ncase \"$1\" in\n--take-over) echo asked > \"$HOME/takeover\" ;;\n--version) echo new ;;\nesac\n";
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::Now);
    CHECK(outcome.kind == HostInstallOutcomeKind::Installed);
    CHECK(readFile(host.binary()) == upload);
    CHECK(readFile(host.directory() + "/../takeover") == "asked\n");
    CHECK(isAlive(host.daemon()));
    CHECK_EQ(host.sessionsOfDaemon(), 1);
}

TEST(aRefusedHandoverLeavesTheOldDaemonServing)
{
    FakeHost host;
    host.installHost(fakeHost("old"));
    host.startDaemonWithOneSession();
    const std::string upload = "#!/bin/sh\ncase \"$1\" in\n--take-over) echo \"The old host keeps its sessions: incompatible handover state\"; exit 75 ;;\n--version) echo new ;;\nesac\n";
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::Now);
    CHECK(outcome.kind == HostInstallOutcomeKind::HandoverPending);
    CHECK(outcome.reason.find("incompatible handover state") != std::string::npos);
    CHECK(readFile(host.binary()) == upload);
    CHECK(isAlive(host.daemon()));
    CHECK_EQ(host.sessionsOfDaemon(), 1);
}

TEST(aSuccessorStartupFailureNeverStopsTheOldDaemon)
{
    FakeHost host;
    host.installHost(fakeHost("old"));
    host.startDaemonWithOneSession();
    const std::string upload = "#!/bin/sh\ncase \"$1\" in\n--take-over) echo \"cannot listen\"; exit 1 ;;\n--version) echo new ;;\nesac\n";
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::Now);
    CHECK(outcome.kind == HostInstallOutcomeKind::Failed);
    CHECK(outcome.reason.find("cannot listen") != std::string::npos);
    CHECK(outcome.reason.find("was installed") != std::string::npos);
    CHECK(isAlive(host.daemon()));
    CHECK_EQ(host.sessionsOfDaemon(), 1);
}

// Waiting for the sessions installs nothing over the running host: the new
// one waits beside it, and the bootstrap moves it into place on the first
// connect after the daemon's last session has exited — not before.
TEST(updatingWhenSessionsEndSwapsOnlyAfterTheLastSessionExits)
{
    FakeHost host;
    const std::string old = fakeHost("old");
    host.installHost(old);
    host.startDaemonWithOneSession();
    const std::string upload = fakeHost("new");
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::WhenSessionsEnd);
    CHECK(outcome.kind == HostInstallOutcomeKind::Deferred);
    CHECK_EQ(outcome.waitingSessionCount, std::size_t { 1 });
    CHECK(readFile(host.binary()) == old);
    CHECK(readFile(host.directory() + "/ubiquitty-host.pending") == upload);
    CHECK(isAlive(host.daemon()));

    const std::string bootstrap = Ubiquitty::Session::remoteBootstrapCommand();
    host.run(bootstrap);
    CHECK(readFile(host.binary()) == old);
    CHECK(exists(host.directory() + "/ubiquitty-host.pending"));
    CHECK(isAlive(host.daemon()));

    host.endSession();
    CHECK(!isAlive(host.daemon()));
    host.run(bootstrap);
    CHECK(readFile(host.binary()) == upload);
    CHECK(!exists(host.directory() + "/ubiquitty-host.pending"));
    const auto backups = host.backups();
    CHECK_EQ(backups.size(), std::size_t { 1 });
    if (!backups.empty())
        CHECK(readFile(backups.front()) == old);
}

TEST(waitingForSessionsOnAnIdleHostInstallsAtOnce)
{
    FakeHost host;
    host.installHost(fakeHost("old"));
    const std::string upload = fakeHost("new");
    const auto outcome = install(host, upload, Ubiquitty::Crypto::sha256Hex(upload), HostInstallMode::WhenSessionsEnd);
    CHECK(outcome.kind == HostInstallOutcomeKind::Installed);
    CHECK(readFile(host.binary()) == upload);
    CHECK(!exists(host.directory() + "/ubiquitty-host.pending"));
}

TEST(anInstallIsOnlyBuiltForADigest)
{
    // The digest an upload is sent with is the one the install computes.
    const std::string upload = fakeHost("new");
    CHECK(Ubiquitty::Session::hostUploadDigest(upload) == Ubiquitty::Crypto::sha256Hex(upload));
    CHECK(Ubiquitty::Session::isSHA256Hex(Ubiquitty::Session::hostUploadDigest("")));
    CHECK(Ubiquitty::Session::hostInstallCommand("", HostInstallMode::IfIdle).empty());
    CHECK(Ubiquitty::Session::hostInstallCommand(std::string(64, 'A'), HostInstallMode::IfIdle).empty());
    CHECK(Ubiquitty::Session::hostInstallCommand("abc", HostInstallMode::IfIdle).empty());
    const std::string script = Ubiquitty::Session::hostInstallCommand(std::string(64, 'a'), HostInstallMode::Now);
    CHECK(!script.empty());
    // One ssh argument, wrapped in single quotes by the caller.
    CHECK(script.find('\'') == std::string::npos);
    CHECK(Ubiquitty::Session::hostProbeCommand().find('\'') == std::string::npos);
}

TEST(aProbeAnswerIsReadFromItsOwnLine)
{
    using Ubiquitty::Session::parseHostProbe;
    const auto plain = parseHostProbe("\nubiquitty-host-probe 2 Darwin arm64\n");
    CHECK(plain.has_value());
    if (plain) {
        CHECK_EQ(plain->liveSessionCount, std::size_t { 2 });
        CHECK(plain->platform == std::string("Darwin arm64"));
    }
    const auto banner = parseHostProbe("Welcome to the host\r\nubiquitty-host-probe 0 Linux x86_64\r\n");
    CHECK(banner.has_value() && banner->platform == "Linux x86_64");
    CHECK(!parseHostProbe("").has_value());
    CHECK(!parseHostProbe("say ubiquitty-host-probe 1 Linux x86_64\n").has_value());
    CHECK(!parseHostProbe("ubiquitty-host-probe x Linux\n").has_value());
    CHECK(!parseHostProbe("ubiquitty-host-probe 1\n").has_value());
    CHECK(!parseHostProbe("ubiquitty-host-probe 123456789 Linux\n").has_value());
    CHECK(!parseHostProbe("ubiquitty-host-probe 1 " + std::string(65, 'x') + "\n").has_value());
}

TEST(anInstallOutcomeIsReadFromItsSentinel)
{
    using Ubiquitty::Session::readHostInstallOutput;
    CHECK(readHostInstallOutput("\nubiquitty-deploy-ok 0.2.0.3 (1a2b3c4d, modified)\n", "tail").kind == HostInstallOutcomeKind::Installed);
    const auto deferred = readHostInstallOutput("\nubiquitty-deploy-deferred 3\n", "tail");
    CHECK(deferred.kind == HostInstallOutcomeKind::Deferred);
    CHECK_EQ(deferred.waitingSessionCount, std::size_t { 3 });
    const auto refused = readHostInstallOutput("\nubiquitty-deploy-failed: the upload arrived damaged\n", "tail");
    CHECK(refused.kind == HostInstallOutcomeKind::Failed);
    CHECK(refused.reason == std::string("the upload arrived damaged"));
    // ssh dying before the script printed anything is a failure in ssh's words.
    const auto silent = readHostInstallOutput("ssh: connect to host host port 22: Connection refused\n", "ssh: connect to host host port 22: Connection refused");
    CHECK(silent.kind == HostInstallOutcomeKind::Failed);
    CHECK(silent.reason.find("Connection refused") != std::string::npos);
}

TEST(anUpdateOffersLiveHandoverForRunningSessions)
{
    using Ubiquitty::Session::decideHostUpdateConsent;
    using Ubiquitty::Session::HostUpdateConsent;
    CHECK(decideHostUpdateConsent(0) == HostUpdateConsent::Proceed);
    CHECK(decideHostUpdateConsent(1) == HostUpdateConsent::AskFirst);
    CHECK(decideHostUpdateConsent(7) == HostUpdateConsent::AskFirst);
    CHECK(Ubiquitty::Session::hostUpdateSessionsQuestion(2, "Studio") == std::string("2 sessions are running on Studio. Update now and keep them running?"));
    CHECK(Ubiquitty::Session::hostUpdateSessionsQuestion(1, "Studio") == std::string("1 session is running on Studio. Update now and keep it running?"));
}

UBIQUITTY_TEST_MAIN()
