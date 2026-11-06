// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/file_transfer_confinement.cpp — file.read for click-to-open is
// confined to the session's own current-working-directory subtree, with no
// escape hatch: an absolute path outside the shell's cwd (however phrased —
// verbatim, or via a literal ".." in the middle) is denied, never served. Also
// checks the non-regular-file reject (a directory is not readable as a file).
//
// The subtree is the one the shell process is in, which is not the one its
// output claims: a session that prints OSC 7 naming another directory does not
// move the root, so a program's bytes cannot widen what a FileRead client may
// read (case 6).
//
// Drives the REAL ubiquitty-host daemon over the unix socket, like resize_clamp.cpp
// and token_auth.cpp: fork+exec the daemon, Greet with a pre-seeded
// token (observe+drive+file scope — this test's own AF_UNIX connection is a
// bare test binary, not the bundled ubiquitty-host attach bridge, so it's
// classified Agent-origin like any other unlabeled local tool; a real
// tokenScopeAllows() pass needs the token, exactly as a live agent would need
// one), drive the child shell to create real files, then send correlated Calls
// and assert complete inline/bulk Results. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "FileReadClient.h"
#include "Platform/Host/HostSocketName.h"

#include <chrono>
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Ubiquitty::Protocol::Control::FileReadStatus;

// Scope bits (Sync/Scope.h Scope).
constexpr std::uint8_t scopeObserve = 0x01;
constexpr std::uint8_t scopeDrive = 0x02;
constexpr std::uint8_t scopeNewSession = 0x10; // sessionID 0 (fresh session) needs this at admission
constexpr std::uint8_t scopeFileRead = 0x20;

constexpr auto statusOK = FileReadStatus::OK;
constexpr auto statusNotFound = FileReadStatus::NotFound;
constexpr auto statusTooLarge = FileReadStatus::TooLarge;
constexpr auto statusOutsideConfinement = FileReadStatus::OutsideConfinement;
constexpr auto statusNotRegular = FileReadStatus::NotRegular;

using namespace Ubiquitty::HostTest;

void sendFileRequest(FileReadClient& c, std::uint32_t requestID, const std::string& path)
{
    (void)c.send(requestID, path);
}

// The canonical PaneFacts reports Terminal::currentDirectory here, so a record
// naming a directory is the host saying it parsed the OSC 7 that named it.
bool parseTitlePartsCurrentDirectory(const std::string& body, std::string& outCurrentDirectory)
{
    const auto facts = Sync::decodePaneFacts({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    if (!facts)
        return false;
    outCurrentDirectory = facts->currentDirectory;
    return true;
}
bool waitForFileData(FileReadClient& client, std::uint32_t requestID, FileReadStatus& status, std::string& data, int totalMilliseconds)
{
    const auto reply = client.wait(totalMilliseconds);
    if (!reply || reply->requestID != requestID)
        return false;
    status = reply->status;
    const auto bytes = reply->data();
    data.assign(bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}
// Wait until the host has parsed the claimed directory. The read that follows
// needs it: an unparsed root and a
// refused one both answer OutsideConfinement, so a case that raced the shell's
// output would pass while proving nothing.
bool waitForAnnouncedDirectory(Connection& c, const std::string& wanted, int totalMilliseconds)
{
    const int stepMilliseconds = 100;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string body;
        if (c.paneHandle && waitFor(c, Sync::FrameKind::PaneFacts, body, stepMilliseconds, c.paneHandle)) {
            std::string announced;
            if (parseTitlePartsCurrentDirectory(body, announced) && announced == wanted)
                return true;
        }
    }
    return false;
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "file_transfer_confinement: FAIL — %s\n", msg);
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-filexfer.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string homeDirectory(home);
    const std::string macHome = homeDirectory + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    // The shell's cwd for this test — a SUBDIRECTORY of home, so a sibling of
    // home (outsideFile below) is unambiguously outside its subtree regardless
    // of how home itself resolves (e.g. a /tmp -> /private/tmp symlink).
    const std::string workdir = homeDirectory + "/workdir";
    ::mkdir(workdir.c_str(), 0700);
    const std::string insideFile = workdir + "/infile.txt";
    const std::string insideSubdirectory = workdir + "/subdir";
    ::mkdir(insideSubdirectory.c_str(), 0700);
    const std::string outsideFile = homeDirectory + "/outside-secret.txt";
    {
        FILE* file = std::fopen(outsideFile.c_str(), "w");
        if (!file)
            return 1;
        std::fputs("outside-secret", file);
        std::fclose(file);
    }

    // Seed a token store entry granting exactly what this test's client needs
    // (observe + drive, to see/steer the shell; file, for file.read) — the
    // same "token store" file format token_auth.cpp/control_token.cpp seed.
    const std::string token = "filexfertokenfilexfertokenfilexfertoken";
    const std::uint8_t scope = scopeObserve | scopeDrive | scopeNewSession | scopeFileRead;
    {
        const std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 filexfer\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scope));
        std::fclose(file);
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    bool isDaemonUp = waitForDaemonReady(sockPath);

    int rc = 1;
    do {
        if (!isDaemonUp) {
            fail("daemon did not come up");
            break;
        }

        Connection client;
        client.fileDescriptor = connectDaemon(sockPath);
        if (client.fileDescriptor < 0) {
            fail("connect");
            break;
        }
        // This is an explicit fresh shell over the fixture's own socket, not
        // retained-session continuity or a pin inferred from its Welcome.
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { .low = 1, .high = 0x66696c65 };
        greeting.clientKind = Sync::ClientKind::Agent;
        greeting.requestedRole = Sync::Role::Agent;
        greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = 0, .expectedDaemonInstanceID = 0 };
        greeting.credential.emplace(token.begin(), token.end());
        greeting.facts.viewportRows = 24;
        greeting.facts.viewportColumns = 80;
        std::string body;
        if (!sendGreeting(client, std::move(greeting)) || !waitFor(client, Sync::FrameKind::Welcome, body, 3000)
            || !client.welcome || client.hasFailed || client.welcome->admission != Sync::AttachmentAdmission::Granted
            || client.welcome->selection.target != Sync::AdmissionTarget::Session
            || !client.welcome->selection.sessionID || !waitForPane(client)) {
            fail("the credential did not admit one fresh shell and bind its pane");
            break;
        }
        FileReadClient fileReader(client);

        // Drive the shell into workdir and create infile.txt there, retrying
        // (the shell may not be at a prompt the first time) like resize_clamp's
        // measurePTYSize — polling for infile.txt to exist rather than the PTY.
        bool isInsideFileReady = false;
        for (int attempt = 0; attempt < 12 && !isInsideFileReady; ++attempt) {
            // Also emit OSC 7 (the shell-integration cwd hook Core parses into
            // Terminal::currentDirectory), as a shell-integrated real session
            // does automatically. The confinement root comes from the shell
            // process's own cwd, which this `cd` moves; the OSC is here so the
            // announced directory and the real one agree for cases 1-5, and
            // case 6 pulls them apart.
            const std::string in = "cd " + workdir
                + " && printf 'hello world' > infile.txt"
                  " && printf '\\033]7;file://localhost"
                + workdir + "\\007'\n";
            (void)sendInput(client, in);
            for (int i = 0; i < 20; ++i) {
                ::usleep(50 * 1000);
                struct stat st;
                if (::stat(insideFile.c_str(), &st) == 0) {
                    isInsideFileReady = true;
                    break;
                }
            }
        }
        if (!isInsideFileReady) {
            fail("shell never created infile.txt in workdir");
            break;
        }

        // 1) A file INSIDE the shell's cwd is served.
        sendFileRequest(fileReader, 1, insideFile);
        auto status = FileReadStatus::Denied;
        std::string data;
        if (!waitForFileData(fileReader, 1, status, data, 3000)) {
            fail("no file.read Result for in-cwd request");
            break;
        }
        if (status != statusOK || data != "hello world") {
            std::fprintf(stderr, "file_transfer_confinement: in-cwd status=%u data=%s\n", static_cast<unsigned>(status), data.c_str());
            fail("in-cwd file not served correctly");
            break;
        }

        // 2) An absolute path with a literal ".." that resolves OUTSIDE the
        // cwd subtree is denied — never served, regardless of the target
        // existing and being perfectly readable by the daemon's own uid.
        sendFileRequest(fileReader, 2, workdir + "/../outside-secret.txt");
        status = FileReadStatus::Denied;
        data.clear();
        if (!waitForFileData(fileReader, 2, status, data, 3000)) {
            fail("no file.read Result for outside-cwd request");
            break;
        }
        if (status != statusOutsideConfinement || !data.empty()) {
            std::fprintf(stderr, "file_transfer_confinement: outside-cwd status=%u bytes=%zu\n", static_cast<unsigned>(status), data.size());
            fail("a '..'-escaping path outside cwd was not denied with OutsideConfinement");
            break;
        }

        // 3) A directory (a proper descendant of cwd, so this exercises the
        // non-regular reject specifically, not the confinement one) is
        // rejected as non-regular (never opened/read).
        sendFileRequest(fileReader, 3, insideSubdirectory);
        status = FileReadStatus::Denied;
        data.clear();
        if (!waitForFileData(fileReader, 3, status, data, 3000)) {
            fail("no file.read Result for directory request");
            break;
        }
        if (status != statusNotRegular) {
            std::fprintf(stderr, "file_transfer_confinement: directory status=%u\n", static_cast<unsigned>(status));
            fail("a directory was not rejected as NotRegular");
            break;
        }

        // 4) A path that doesn't exist at all reports NotFound (sanity check
        // that NotFound and OutsideConfinement are actually distinguished).
        sendFileRequest(fileReader, 4, workdir + "/does-not-exist.txt");
        status = FileReadStatus::Denied;
        data.clear();
        if (!waitForFileData(fileReader, 4, status, data, 3000)) {
            fail("no file.read Result for missing-file request");
            break;
        }
        if (status != statusNotFound) {
            std::fprintf(stderr, "file_transfer_confinement: missing-file status=%u\n", static_cast<unsigned>(status));
            fail("a nonexistent in-cwd path was not reported NotFound");
            break;
        }

        // 5) DoS guard: a FIFO inside cwd must be rejected as NotRegular
        //    via the fstat-after-open path — and the Result must arrive.
        //    The type check lives on the opened fd (no pre-open path stat),
        //    so the daemon opens the FIFO with O_NONBLOCK and fstat-rejects it;
        //    Without O_NONBLOCK, open(fifo, O_RDONLY) would block forever and
        //    wedge the client-handling thread — this waitForFileData would then
        //    time out. (Also stands in for a same-uid regular→FIFO swap, which
        //    a pre-open path stat could race and the check on the fd cannot.)
        const std::string fifoPath = workdir + "/pipe.fifo";
        ::unlink(fifoPath.c_str());
        if (::mkfifo(fifoPath.c_str(), 0600) != 0) {
            fail("mkfifo");
            break;
        }
        sendFileRequest(fileReader, 5, fifoPath);
        status = FileReadStatus::Denied;
        data.clear();
        if (!waitForFileData(fileReader, 5, status, data, 3000)) {
            fail("no file.read Result for a FIFO request — the daemon likely BLOCKED on the "
                 "FIFO open (the FIFO DoS: the open needs O_NONBLOCK + an fstat reject)");
            break;
        }
        if (status != statusNotRegular || !data.empty()) {
            std::fprintf(stderr, "file_transfer_confinement: fifo status=%u bytes=%zu\n", static_cast<unsigned>(status), data.size());
            fail("a FIFO was not rejected as NotRegular");
            break;
        }

        // 6) The session's output does not choose the confinement root. The
        //    program prints an OSC 7 naming the PARENT directory — the sequence
        //    a shell uses to report a `cd`, here reporting a directory the shell
        //    never entered — and the file that sits there stays outside. Anything
        //    that reaches the screen can print this (a compromised host on the
        //    far end of an ssh, a file the user cats), so a root taken from it is
        //    a root the attacker picks, and `file://localhost/` would pick the
        //    whole filesystem.
        {
            // The shell's prompt announces its real directory again right
            // after; the pause lets a frame be taken while the printed one
            // stands, since frames are paced and a directory that lived for
            // less than a frame interval is never reported.
            const std::string announce = "printf '\\033]7;file://localhost" + homeDirectory + "\\007'; sleep 0.3\n";
            (void)sendInput(client, announce);
            if (!waitForAnnouncedDirectory(client, homeDirectory, 5000)) {
                fail("the host never reported the announced directory — the OSC 7 under test "
                     "never reached Terminal::currentDirectory, so the case proves nothing");
                break;
            }
            sendFileRequest(fileReader, 6, outsideFile);
            status = FileReadStatus::Denied;
            data.clear();
            if (!waitForFileData(fileReader, 6, status, data, 3000)) {
                fail("no file.read Result for the announced-root request");
                break;
            }
            if (status != statusOutsideConfinement || !data.empty()) {
                std::fprintf(stderr, "file_transfer_confinement: announced-root status=%u bytes=%zu data=%s\n",
                    static_cast<unsigned>(status), data.size(), data.c_str());
                fail("an OSC 7 printed by the session moved the confinement root: a file outside "
                     "the shell's own cwd was served because the session's output named its parent");
                break;
            }
        }

        // 7) The existing raw-file ceiling is inclusive. Its base64 Result
        // crosses the bulk lane, preserving NUL and high bytes exactly.
        const std::string largePath = workdir + "/large.bin";
        std::string wanted(Ubiquitty::Protocol::Control::maxReadFileBytes, '\0');
        wanted.front() = '\xFF';
        wanted.back() = '\x81';
        FILE* large = std::fopen(largePath.c_str(), "wb");
        if (!large) {
            fail("cannot create the task's large fixture");
            break;
        }
        const bool didWriteLarge = std::fwrite(wanted.data(), 1, wanted.size(), large) == wanted.size();
        std::fclose(large);
        if (!didWriteLarge || !fileReader.send(7, largePath)
            || !waitForFileData(fileReader, 7, status, data, 10000)
            || status != statusOK || data != wanted || fileReader.bulkResults() != 1) {
            fail("the exact raw-file limit did not return through one complete bulk Result");
            break;
        }
        large = std::fopen(largePath.c_str(), "ab");
        if (!large) {
            fail("cannot extend the task's large fixture");
            break;
        }
        std::fputc('x', large);
        std::fclose(large);
        if (!fileReader.send(8, largePath) || !waitForFileData(fileReader, 8, status, data, 3000)
            || status != statusTooLarge || !data.empty() || fileReader.bulkResults() != 1) {
            fail("one byte over the limit was not refused without another bulk transfer");
            break;
        }
        large = std::fopen(largePath.c_str(), "wb");
        if (!large) {
            fail("cannot truncate the task's fixture");
            break;
        }
        std::fclose(large);
        if (!fileReader.send(9, largePath) || !waitForFileData(fileReader, 9, status, data, 3000)
            || status != statusOK || !data.empty()) {
            fail("an empty regular file is not a successful empty Result");
            break;
        }
        // Unknown and retired kinds are rejected by the canonical ingress,
        // never retained as a second numeric carrier beside a typed Result.
        if (client.hasFailed || client.admission != FixtureAdmission::Granted) {
            fail("the typed file exchange did not remain on a valid granted canonical connection");
            break;
        }

        rc = 0;
        ::close(client.fileDescriptor);
    } while (false);

    if (rc == 0)
        std::printf("file_transfer_confinement: file.read confines to the shell process's own cwd subtree (Ok in-cwd, "
                    "OutsideConfinement past a '..' escape, NotRegular for a directory, NotFound "
                    "for a missing path, NotRegular for a FIFO without blocking the daemon, "
                    "OutsideConfinement for a root the session's own OSC 7 named), and exact "
                    "4 MiB bulk/over-limit/empty results pass without FILE_DATA\n");

    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10 * 1000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, WNOHANG);
    return rc;
}
