// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/token_auth.cpp — the capability-token auth gate, end to end.
//
// Spawns the real ubiquitty-host daemon with token enforcement and a
// pre-seeded hashed-token store, then drives hand-rolled agent clients to assert:
//   - a tokenless agent is rejected (Bye TokenMissing),
//   - an invalid token is rejected (Bye TokenInvalid),
//   - a valid full-scope token is accepted (Welcome),
//   - a valid token without the drive scope attaches but its input closes it
//     (Bye Malformed),
//   - a token that lapses under an established connection drops it
//     (Bye TokenExpired),
//   - a token revoked by a program the daemon does not recognise, which the
//     daemon refuses to take a reload request from, still ends the live
//     connection before its next input reaches the shell (Bye TokenRevoked),
//   - a revoke landing between two pieces of one paste ends the connection
//     before the last piece is taken (Bye TokenRevoked),
//   - standing consent withdrawn from the store by another owner stops
//     admitting the token's next attach (Bye NoConsent),
//   - a store that no longer parses whole, or cannot be read, ends every
//     live token-bound connection and admits no token.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "HostConnection.h"
#include "Platform/Host/ConsentStore.h"
#include "Platform/Host/HostSocketName.h"

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
// Scope bits.
constexpr std::uint8_t scopeObserve = 0x01;
constexpr std::uint8_t scopeDrive = 0x02;
constexpr std::uint8_t scopeNewSession = 0x10;

// Run `<hostBin> token revoke <id>` against the test's UBIQUITTY_HOME and reap it.
// Returns the exit code, or -1 if it couldn't run.
int runRevoke(const char* hostBin, const std::string& macHome, const std::string& id)
{
    pid_t processID = ::fork();
    if (processID < 0)
        return -1;
    if (processID == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::execl(hostBin, hostBin, "token", "revoke", id.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(processID, &status, 0) != processID || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

// Copies a file byte for byte with mode 0700. A copy of the host binary has
// the original's content, so it names the same socket, and its own inode, so
// the daemon does not recognise it as its own executable.
bool copyExecutable(const char* from, const std::string& to)
{
    FILE* input = std::fopen(from, "rb");
    if (!input)
        return false;
    FILE* output = std::fopen(to.c_str(), "wb");
    if (!output) {
        std::fclose(input);
        return false;
    }
    std::vector<char> buffer(1 << 16);
    bool isCopied = true;
    for (;;) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), input);
        if (count && std::fwrite(buffer.data(), 1, count, output) != count) {
            isCopied = false;
            break;
        }
        if (count < buffer.size()) {
            isCopied = !std::ferror(input);
            break;
        }
    }
    std::fclose(input);
    isCopied = std::fclose(output) == 0 && isCopied;
    return isCopied && ::chmod(to.c_str(), 0700) == 0;
}

bool fileAppears(const std::string& path, int milliseconds)
{
    for (int waited = 0; waited <= milliseconds; waited += 50) {
        struct stat information {};
        if (::stat(path.c_str(), &information) == 0)
            return true;
        ::usleep(50 * 1000);
    }
    return false;
}

using namespace Ubiquitty::HostTest;

// Sends one piece of a paste on the connection's bound pane.
bool sendPastePiece(Connection& connection, std::uint64_t inputSeq, std::uint64_t offset, Sync::PasteState state, std::string_view text)
{
    if (!waitForPane(connection))
        return false;
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Paste { .inputSeq = inputSeq, .offset = offset, .state = state, .text = { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() } });
    return sendFrame(connection, Sync::FrameKind::Paste, { reinterpret_cast<const char*>(body.data()), body.size() }, connection.paneHandle);
}

bool replaceFile(const std::string& path, const std::string& contents)
{
    const std::string staged = path + ".fixture";
    FILE* file = std::fopen(staged.c_str(), "w");
    if (!file)
        return false;
    const bool isWritten = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    return std::fclose(file) == 0 && isWritten && ::chmod(staged.c_str(), 0600) == 0 && ::rename(staged.c_str(), path.c_str()) == 0;
}

bool greetAgent(Connection& connection, std::uint32_t sessionID, std::string_view token)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = static_cast<std::uint64_t>(connection.fileDescriptor) + 1, .high = 0x746f6b656e };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .isAttachOnly = sessionID != 0 };
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    if (!token.empty())
        greeting.credential.emplace(token.begin(), token.end());
    return sendGreeting(connection, std::move(greeting));
}

bool receivesBye(Connection& connection, Sync::ByeReason reason, int milliseconds, std::string_view detail = {})
{
    std::string body;
    if (!waitFor(connection, Sync::FrameKind::Bye, body, milliseconds))
        return false;
    const auto bye = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    return bye && bye->reason == reason && (detail.empty() || bye->detail.find(detail) != std::string::npos);
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "token_auth: FAIL — %s\n", msg);
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-token.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    std::string macHome = std::string(home) + "/.ubiquitty";
    // Derive the daemon's content-hashed socket leaf (host-<hash>.sock)
    // from the binary under test rather than assuming a fixed "host.sock".
    std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    // Seed the token store: a full-scope token and an observe+new (no drive) one.
    const std::string fullToken = "fulltokenfulltokenfulltokenfulltoken";
    const std::string observeToken = "observetokenobservetokenobservetoken";
    const std::string attachOnlyToken = "attach-only-token-0123456789abcdef";
    // Valid at the handshake and lapsed a few seconds into the connection, which
    // is the only way to see expiry bind a live connection rather than a Greet.
    const std::string lapsingToken = "lapsing-token-lapsing-token-lapsing";
    const std::uint64_t lapsesAt = static_cast<std::uint64_t>(::time(nullptr)) + 3;
    // Revoked by a program the daemon will not take a reload request from.
    const std::string strandedToken = "stranded-token-stranded-token-stranded";
    // Holds standing consent until another owner withdraws it from the store.
    const std::string standingToken = "standing-token-standing-token-standing";
    // Revoked between two pieces of one paste.
    const std::string pastingToken = "pasting-token-pasting-token-pasting-tok";
    // Live while the store is corrupted, then while it cannot be read.
    const std::string corruptToken = "corrupt-token-corrupt-token-corrupt-tok";
    const std::string unreadableToken = "unreadable-token-unreadable-token-unread";
    const std::uint8_t fullScope = scopeObserve | scopeDrive | scopeNewSession;
    const std::uint8_t observeScope = scopeObserve | scopeNewSession; // no drive
    const std::uint8_t attachOnlyScope = scopeObserve; // no drive, no new-session
    {
        std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 full\n", Ubiquitty::Crypto::sha256Hex(fullToken).c_str(), static_cast<unsigned>(fullScope));
        std::fprintf(file, "%s %u 0 observe\n", Ubiquitty::Crypto::sha256Hex(observeToken).c_str(), static_cast<unsigned>(observeScope));
        std::fprintf(file, "%s %u 0 attach\n", Ubiquitty::Crypto::sha256Hex(attachOnlyToken).c_str(), static_cast<unsigned>(attachOnlyScope));
        std::fprintf(file, "%s %u %llu lapsing\n", Ubiquitty::Crypto::sha256Hex(lapsingToken).c_str(), static_cast<unsigned>(fullScope),
            static_cast<unsigned long long>(lapsesAt));
        std::fprintf(file, "%s %u 0 stranded\n", Ubiquitty::Crypto::sha256Hex(strandedToken).c_str(), static_cast<unsigned>(fullScope));
        std::fprintf(file, "%s %u 0 standing\n", Ubiquitty::Crypto::sha256Hex(standingToken).c_str(), static_cast<unsigned>(observeScope));
        std::fprintf(file, "%s %u 0 pasting\n", Ubiquitty::Crypto::sha256Hex(pastingToken).c_str(), static_cast<unsigned>(fullScope));
        std::fprintf(file, "%s %u 0 corrupt\n", Ubiquitty::Crypto::sha256Hex(corruptToken).c_str(), static_cast<unsigned>(fullScope));
        std::fclose(file);
    }
    const std::string consentPath = macHome + "/consent";
    const std::string standingKey = Ubiquitty::Host::tokenGrantKey(Ubiquitty::Crypto::sha256Hex(standingToken));
    if (Ubiquitty::Host::addStandingConsent(consentPath, standingKey) != Ubiquitty::Host::ConsentEdit::Changed) {
        std::fprintf(stderr, "token_auth: could not seed standing consent\n");
        return 1;
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        // Token enforcement is on by default — leave UBIQUITTY_REQUIRE_TOKEN
        // unset, so the tokenless/bogus rejections below are testing the default
        // and not the override (a regression to default-off would make them connect).
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    bool isDaemonReady = waitForDaemonReady(sockPath);

    int rc = 1;
    std::string body;
    do {
        if (!isDaemonReady) {
            fail("daemon never accepted a connection");
            break;
        }

        // --- A tokenless agent is rejected (require_token on). ---
        Connection tokenless;
        tokenless.fileDescriptor = connectDaemon(sockPath);
        if (tokenless.fileDescriptor < 0) {
            fail("tokenless connect");
            break;
        }
        if (!greetAgent(tokenless, 0, {}) || !receivesBye(tokenless, Sync::ByeReason::TokenMissing, 3000)) {
            fail("tokenless agent should get Bye TokenMissing");
            break;
        }

        // --- An invalid token is rejected. ---
        Connection bogus;
        bogus.fileDescriptor = connectDaemon(sockPath);
        if (bogus.fileDescriptor < 0) {
            fail("bogus connect");
            break;
        }
        if (!greetAgent(bogus, 0, "not-a-real-token") || !receivesBye(bogus, Sync::ByeReason::TokenInvalid, 3000)) {
            fail("invalid token should get Bye TokenInvalid");
            break;
        }

        // --- A full-scope token is accepted. ---
        Connection full;
        full.fileDescriptor = connectDaemon(sockPath);
        if (full.fileDescriptor < 0) {
            fail("full connect");
            break;
        }
        if (!greetAgent(full, 0, fullToken) || !waitFor(full, Sync::FrameKind::Welcome, body, 3000)) {
            fail("full-scope token should be accepted (Welcome)");
            break;
        }

        // --- An observe-only token attaches, but its input closes its
        // connection, with the reason in the Bye. ---
        Connection observe;
        observe.fileDescriptor = connectDaemon(sockPath);
        if (observe.fileDescriptor < 0) {
            fail("observe connect");
            break;
        }
        if (!greetAgent(observe, 0, observeToken) || !waitFor(observe, Sync::FrameKind::Welcome, body, 3000)) {
            fail("observe token should attach (Welcome)");
            break;
        }
        (void)waitForScreen(observe, 1000); // drain its session checkpoint
        if (!sendInput(observe, "ls\r")) { // input needs the drive scope
            fail("the host never bound the observe-only client's pane");
            break;
        }
        if (!receivesBye(observe, Sync::ByeReason::Malformed, 3000, "may not drive")) {
            fail("input without the drive scope should close the connection with a Bye saying it may not drive");
            break;
        }

        // --- A nonexistent session id must not become a new shell. ---
        // Numeric targets are attach-only: a vanished one is not permission
        // to create. A separate explicit fresh request exercises NewSession.
        Connection invent;
        invent.fileDescriptor = connectDaemon(sockPath);
        if (invent.fileDescriptor < 0) {
            fail("attach-only connect");
            break;
        }
        if (!greetAgent(invent, 999999, attachOnlyToken) || !receivesBye(invent, Sync::ByeReason::SessionGone, 3000) || invent.welcome) {
            fail("a nonexistent attach-only session must get Bye SessionGone, not a replacement shell");
            break;
        }
        Connection fresh;
        fresh.fileDescriptor = connectDaemon(sockPath);
        if (fresh.fileDescriptor < 0 || !greetAgent(fresh, 0, attachOnlyToken)
            || !receivesBye(fresh, Sync::ByeReason::OutsideScope, 3000, "; creating a session needs new.") || fresh.welcome) {
            fail("an explicit fresh session without NewSession scope must get Bye OutsideScope naming the new scope");
            break;
        }

        // --- Expiry tears down the LIVE connection the token authenticated. ---
        // The token is valid at the handshake and lapses a few seconds later. The
        // store does not change when a record lapses, so nothing but the
        // connection's own deadline notices; without it this client keeps its
        // full scope for as long as it stays connected.
        Connection lapsing;
        lapsing.fileDescriptor = connectDaemon(sockPath);
        if (lapsing.fileDescriptor < 0) {
            fail("lapsing connect");
            break;
        }
        if (!greetAgent(lapsing, 0, lapsingToken) || !waitFor(lapsing, Sync::FrameKind::Welcome, body, 3000)) {
            fail("a token valid at the handshake should be accepted (Welcome)");
            break;
        }
        if (!receivesBye(lapsing, Sync::ByeReason::TokenExpired, 8000)) {
            fail("a token that lapses under a live connection should drop it with Bye TokenExpired");
            break;
        }

        // --- A revoke the daemon never acknowledges still binds it. ---
        // The copy refuses nothing itself: it rewrites the shared store, and
        // the daemon refuses its reload request because the copy is not the
        // daemon's own executable, which is what any other build or install
        // sharing this control directory meets. The store is the authority,
        // so the next input from the revoked token must close the connection
        // and never reach the shell.
        Connection stranded;
        stranded.fileDescriptor = connectDaemon(sockPath);
        if (stranded.fileDescriptor < 0) {
            fail("stranded connect");
            break;
        }
        if (!greetAgent(stranded, 0, strandedToken) || !waitFor(stranded, Sync::FrameKind::Welcome, body, 3000)) {
            fail("the stranded token should be accepted (Welcome)");
            break;
        }
        (void)waitForScreen(stranded, 1000);
        const std::string otherBuild = std::string(home) + "/other-ubiquitty-host";
        if (!copyExecutable(argv[1], otherBuild)) {
            fail("could not copy the host binary");
            break;
        }
        if (runRevoke(otherBuild.c_str(), macHome, Ubiquitty::Crypto::sha256Hex(strandedToken)) != 0) {
            fail("token revoke from the copied binary failed");
            break;
        }
        const std::string servedMarker = std::string(home) + "/revoked-input-was-served";
        (void)sendInput(stranded, "touch " + servedMarker + "\r");
        if (!receivesBye(stranded, Sync::ByeReason::TokenRevoked, 3000)) {
            fail("a token revoked in the store should end its live connection even when the daemon refused the revoker's reload request");
            break;
        }
        if (fileAppears(servedMarker, 1500)) {
            fail("input sent after the revoke reached the shell");
            break;
        }

        // --- Standing consent withdrawn by another owner stops admitting. ---
        // The token keeps its record; only the grant that let it attach to a
        // session it did not create leaves the shared store, the way another
        // daemon's grant revocation removes it. No person is attached, so the
        // next attach must be refused rather than admitted on a grant the
        // store no longer holds.
        if (!full.welcome || !full.welcome->selection.sessionID) {
            fail("the full-scope client named no session to attach to");
            break;
        }
        const std::uint32_t sharedSession = full.welcome->selection.sessionID;
        Connection standing;
        standing.fileDescriptor = connectDaemon(sockPath);
        if (standing.fileDescriptor < 0 || !greetAgent(standing, sharedSession, standingToken)
            || !waitFor(standing, Sync::FrameKind::Welcome, body, 3000)) {
            fail("standing consent should admit the token to a session it did not create");
            break;
        }
        ::close(standing.fileDescriptor);
        standing.fileDescriptor = -1;
        if (Ubiquitty::Host::removeStandingConsent(consentPath, standingKey) != Ubiquitty::Host::ConsentEdit::Changed) {
            fail("could not withdraw the standing consent from the store");
            break;
        }
        Connection withdrawn;
        withdrawn.fileDescriptor = connectDaemon(sockPath);
        if (withdrawn.fileDescriptor < 0 || !greetAgent(withdrawn, sharedSession, standingToken)
            || !receivesBye(withdrawn, Sync::ByeReason::NoConsent, 3000) || withdrawn.welcome) {
            fail("standing consent withdrawn from the store should not admit the next attach");
            break;
        }

        // --- A revoke between two pieces of one operation ends it. ---
        // The paste's first piece is taken under a valid token; the store
        // changes before the last piece, which must close the connection
        // rather than finish the paste.
        Connection pasting;
        pasting.fileDescriptor = connectDaemon(sockPath);
        if (pasting.fileDescriptor < 0 || !greetAgent(pasting, 0, pastingToken) || !waitFor(pasting, Sync::FrameKind::Welcome, body, 3000)) {
            fail("the pasting token should be accepted (Welcome)");
            break;
        }
        (void)waitForScreen(pasting, 1000);
        const std::string firstPiece = "echo pasted-before-the-revoke";
        if (!sendPastePiece(pasting, pasting.nextInputSeq, 0, Sync::PasteState::More, firstPiece)) {
            fail("the first paste piece could not be sent");
            break;
        }
        if (runRevoke(otherBuild.c_str(), macHome, Ubiquitty::Crypto::sha256Hex(pastingToken)) != 0) {
            fail("token revoke of the pasting token failed");
            break;
        }
        (void)sendPastePiece(pasting, pasting.nextInputSeq, firstPiece.size(), Sync::PasteState::Final, "\r");
        if (!receivesBye(pasting, Sync::ByeReason::TokenRevoked, 3000)) {
            fail("a revoke between two pieces of a paste should end the connection before the last piece is taken");
            break;
        }
        ::unlink(otherBuild.c_str());

        // --- Revoke tears down the LIVE token-bound connection. ---
        // `full` is attached on fullToken; revoking it (the CLI rewrites the store
        // and requests its local reload) must drop that live connection with
        // Bye TokenRevoked, not merely deny future greetings.
        const std::string fullHash = Ubiquitty::Crypto::sha256Hex(fullToken);
        if (runRevoke(argv[1], macHome, fullHash) != 0) {
            fail("token revoke CLI failed");
            break;
        }
        if (!receivesBye(full, Sync::ByeReason::TokenRevoked, 3000)) {
            fail("revoke should drop the live token-bound agent with Bye TokenRevoked");
            break;
        }

        // --- A store that cannot be read whole grants nothing. ---
        // A line no writer produces: its expiry is not a number. The record
        // beside it may be the one a revocation removed, so the daemon must
        // not keep any token-bound authority on the strength of the rest.
        const std::string tokensPath = macHome + "/tokens";
        Connection corrupt;
        corrupt.fileDescriptor = connectDaemon(sockPath);
        if (corrupt.fileDescriptor < 0 || !greetAgent(corrupt, 0, corruptToken) || !waitFor(corrupt, Sync::FrameKind::Welcome, body, 3000)) {
            fail("the corrupt-case token should be accepted (Welcome)");
            break;
        }
        (void)waitForScreen(corrupt, 1000);
        {
            FILE* file = std::fopen(tokensPath.c_str(), "a");
            if (!file) {
                fail("could not open the token store to corrupt it");
                break;
            }
            std::fprintf(file, "%s %u never mangled\n", Ubiquitty::Crypto::sha256Hex("mangled").c_str(), static_cast<unsigned>(fullScope));
            std::fclose(file);
        }
        (void)sendInput(corrupt, "true\r");
        if (!receivesBye(corrupt, Sync::ByeReason::TokenRevoked, 3000)) {
            fail("a store that no longer parses whole should end every live token-bound connection");
            break;
        }
        Connection whileCorrupt;
        whileCorrupt.fileDescriptor = connectDaemon(sockPath);
        if (whileCorrupt.fileDescriptor < 0 || !greetAgent(whileCorrupt, 0, unreadableToken)
            || !receivesBye(whileCorrupt, Sync::ByeReason::TokenInvalid, 3000) || whileCorrupt.welcome) {
            fail("a store that does not parse whole should admit no token");
            break;
        }

        // --- A store that cannot be read at all grants nothing. ---
        if (!replaceFile(tokensPath, Ubiquitty::Crypto::sha256Hex(unreadableToken) + " " + std::to_string(fullScope) + " 0 unreadable\n")) {
            fail("could not repair the token store");
            break;
        }
        Connection unreadable;
        unreadable.fileDescriptor = connectDaemon(sockPath);
        if (unreadable.fileDescriptor < 0 || !greetAgent(unreadable, 0, unreadableToken) || !waitFor(unreadable, Sync::FrameKind::Welcome, body, 3000)) {
            fail("a token in the repaired store should be accepted (Welcome)");
            break;
        }
        (void)waitForScreen(unreadable, 1000);
        if (::chmod(tokensPath.c_str(), 0) != 0) {
            fail("could not make the token store unreadable");
            break;
        }
        (void)sendInput(unreadable, "true\r");
        const bool didEndUnreadable = receivesBye(unreadable, Sync::ByeReason::TokenRevoked, 3000);
        (void)::chmod(tokensPath.c_str(), 0600);
        if (!didEndUnreadable) {
            fail("a store that cannot be read should end every live token-bound connection");
            break;
        }

        rc = 0;
    } while (false);

    // The copied host binary is the size of the daemon; nothing else here is.
    (void)::unlink((std::string(home) + "/other-ubiquitty-host").c_str());
    if (rc == 0)
        std::printf("token_auth: tokenless + invalid rejected; full-scope accepted; "
                    "observe-only attaches but its input closes it; expiry and revoke each tear "
                    "down the live token-bound agent, a revoke the daemon was never told of included, "
                    "mid-paste included; withdrawn standing consent admits nothing; a corrupt or "
                    "unreadable store grants nothing\n");

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
