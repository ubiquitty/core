// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/host_cli.cpp — the ubiquitty-host command line itself: usage on
// no mode, --version, --quic-version, the token subcommand's list and revoke
// arms, --ensure-daemon's double-fork detach with its already-up early
// return, and --quic-bootstrap's line or its refusal. Everything runs the
// REAL binary under a private UBIQUITTY_HOME; every daemon the flows detach
// is ended, and waited for, before it returns.
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "DetachedDaemons.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "host_cli: FAIL — %s\n", what);
        ++failures;
    }
}

struct RunResult {
    int exitCode = -1;
    std::string output; // stdout + stderr, interleaved
};

// Runs the host binary with `arguments` under the sandbox home, capturing
// both output streams.
RunResult run(const char* hostBinary, const std::string& macHome, const std::vector<std::string>& arguments)
{
    RunResult result;
    int pipeFileDescriptors[2] = { -1, -1 };
    if (::pipe(pipeFileDescriptors) != 0)
        return result;
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        return result;
    }
    if (child == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::dup2(pipeFileDescriptors[1], STDOUT_FILENO);
        ::dup2(pipeFileDescriptors[1], STDERR_FILENO);
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(hostBinary));
        for (const std::string& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(hostBinary, argv.data());
        _exit(127);
    }
    ::close(pipeFileDescriptors[1]);
    char buffer[4096];
    ssize_t bytesRead = 0;
    while ((bytesRead = ::read(pipeFileDescriptors[0], buffer, sizeof(buffer))) > 0)
        result.output.append(buffer, static_cast<std::size_t>(bytesRead));
    ::close(pipeFileDescriptors[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

// The first field of the first token record — the digest revoke takes.
std::string firstTokenID(const std::string& macHome)
{
    std::FILE* file = std::fopen((macHome + "/tokens").c_str(), "r");
    if (!file)
        return {};
    char hash[128] = {};
    const int fieldsRead = std::fscanf(file, "%127s", hash);
    std::fclose(file);
    return fieldsRead == 1 ? std::string(hash) : std::string();
}

// The digest of the most recently issued token: the store appends.
std::string lastTokenID(const std::string& macHome)
{
    std::FILE* file = std::fopen((macHome + "/tokens").c_str(), "r");
    if (!file)
        return {};
    std::string last;
    char line[4096];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char hash[128] = {};
        if (std::sscanf(line, "%127s", hash) == 1)
            last = hash;
    }
    std::fclose(file);
    return last;
}

// The whole file, so a consent list can be compared byte for byte.
std::string slurp(const std::string& path)
{
    std::string contents;
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return contents;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        contents.append(buffer, got);
    std::fclose(file);
    return contents;
}

bool writeFixture(const std::string& path, const std::string& contents)
{
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "w"), std::fclose);
    if (!file)
        return false;
    const bool didWrite = std::fwrite(contents.data(), 1, contents.size(), file.get()) == contents.size();
    const bool didClose = std::fclose(file.release()) == 0;
    return didWrite && didClose;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    char homeTemplate[] = "/tmp/ubiquitty-hostcli.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    ::mkdir(macHome.c_str(), 0700);

    // No mode: usage, exit 2.
    RunResult result = run(hostBinary, macHome, {});
    expect(result.exitCode == 2 && contains(result.output, "usage:"), "no mode prints usage and exits 2");

    // The release identity prints and exits clean.
    result = run(hostBinary, macHome, { "--version" });
    expect(result.exitCode == 0 && !result.output.empty(), "--version prints the release identity");

    // The QUIC linkage report answers on both build shapes.
    result = run(hostBinary, macHome, { "--quic-version" });
    expect(result.exitCode == 0 && !result.output.empty(), "--quic-version reports the linked stack or its absence");

    // The token subcommand's own usage.
    result = run(hostBinary, macHome, { "token" });
    expect(result.exitCode == 2 && contains(result.output, "issue|list|revoke"), "a bare token subcommand prints its usage");

    // Asked for, the usage answers and the exit is clean.
    result = run(hostBinary, macHome, { "--help" });
    expect(result.exitCode == 0 && contains(result.output, "--ensure-daemon") && contains(result.output, "--quic-bootstrap"), "--help prints the usage, every mode included, and exits 0");
    result = run(hostBinary, macHome, { "token", "--help" });
    expect(result.exitCode == 0 && contains(result.output, "issue|list|revoke"), "token --help prints its usage and exits 0");
    result = run(hostBinary, macHome, { "token", "revoke", "--help" });
    expect(result.exitCode == 0 && contains(result.output, "token revoke <token-id>"), "token revoke --help prints its usage and revokes nothing");
    result = run(hostBinary, macHome, { "token", "list", "--bogus" });
    expect(result.exitCode == 2 && contains(result.output, "takes no arguments"), "token list refuses an argument it does not read");

    // An empty store lists as empty.
    result = run(hostBinary, macHome, { "token", "list" });
    expect(result.exitCode == 0 && contains(result.output, "no tokens issued"), "an empty store lists as empty");

    // Two issued tokens list with their labels.
    result = run(hostBinary, macHome, { "token", "issue", "--label", "alpha" });
    expect(result.exitCode == 0, "issuing alpha succeeds");
    result = run(hostBinary, macHome, { "token", "issue", "--label", "beta", "--scope", "control" });
    expect(result.exitCode == 0, "issuing beta succeeds");
    result = run(hostBinary, macHome, { "token", "list" });
    expect(result.exitCode == 0 && contains(result.output, "alpha") && contains(result.output, "beta"),
        "the list carries both labels");

    // --allow-always records the token's standing attach consent beside the
    // record: the list appears with its header first, private to its owner,
    // holding the key the daemon derives for that token; a plain issue adds
    // nothing to it; revoking the token withdraws the key and keeps the rest.
    {
        const std::string consentPath = macHome + "/consent";
        expect(slurp(consentPath).empty(), "no consent list exists before a grant is recorded");
        result = run(hostBinary, macHome, { "token", "issue", "--label", "gamma", "--scope", "control", "--allow-always" });
        expect(result.exitCode == 0 && contains(result.output, "recorded standing attach consent"), "issuing gamma with --allow-always succeeds and says so");
        const std::string gammaID = lastTokenID(macHome);
        expect(gammaID.size() == 64, "gamma's record is the newest in the store");
        const std::string consent = slurp(consentPath);
        expect(consent.rfind("# ubiquitty consent v", 0) == 0, "the consent list starts with its header");
        expect(contains(consent, ("token:" + gammaID + "\n").c_str()), "the list holds gamma's grant key");
        struct stat info {};
        expect(::stat(consentPath.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "the list is private to its owner");

        result = run(hostBinary, macHome, { "token", "issue", "--label", "delta", "--scope", "control" });
        expect(result.exitCode == 0 && !contains(result.output, "standing attach consent"), "a plain issue records no consent");
        expect(slurp(consentPath) == consent, "a plain issue leaves the list untouched");
        const std::string deltaID = lastTokenID(macHome);

        result = run(hostBinary, macHome, { "token", "issue", "--label", "epsilon", "--allow-always" });
        expect(result.exitCode == 0, "a second grant is recorded");
        const std::string epsilonID = lastTokenID(macHome);
        expect(contains(slurp(consentPath), ("token:" + epsilonID + "\n").c_str()), "the list holds epsilon's grant key too");

        result = run(hostBinary, macHome, { "token", "revoke", gammaID });
        expect(result.exitCode == 0 && contains(result.output, "withdrew its standing consent"), "revoking gamma withdraws its consent and says so");
        const std::string after = slurp(consentPath);
        expect(!contains(after, gammaID.c_str()), "gamma's key left the list");
        expect(contains(after, ("token:" + epsilonID + "\n").c_str()), "epsilon's key stayed");
        expect(after.rfind("# ubiquitty consent v", 0) == 0, "the rewritten list keeps its header first");

        result = run(hostBinary, macHome, { "token", "revoke", deltaID });
        expect(result.exitCode == 0 && !contains(result.output, "withdrew"), "revoking a token without a grant says nothing about consent");
        expect(slurp(consentPath) == after, "and leaves the list as it was");
    }

    // Revoke: no match refuses; an ambiguous (empty) prefix refuses; the full
    // digest revokes exactly one and says so.
    result = run(hostBinary, macHome, { "token", "revoke", "zzzznotatoken" });
    expect(result.exitCode == 1 && contains(result.output, "no token matches"), "an unknown prefix refuses");
    result = run(hostBinary, macHome, { "token", "revoke", "" });
    expect(result.exitCode == 2 && contains(result.output, "usage:"), "an empty prefix is the usage arm");
    // Two crafted records sharing a prefix: the ambiguous refusal needs real
    // digests that collide on the asked-for prefix, which random ones do not.
    {
        std::FILE* store = std::fopen((macHome + "/tokens").c_str(), "a");
        expect(store != nullptr, "the store opens for the crafted records");
        if (store) {
            std::fprintf(store, "%064d 1 0 twin-one\n", 1);
            std::fprintf(store, "%064d 1 0 twin-two\n", 2);
            std::fclose(store);
        }
    }
    result = run(hostBinary, macHome, { "token", "revoke", "00000000" });
    expect(result.exitCode == 1 && contains(result.output, "use a longer prefix"), "an ambiguous prefix refuses");
    const std::string tokenID = firstTokenID(macHome);
    expect(!tokenID.empty(), "the store holds a digest to revoke");
    // Revoking refuses a word it does not read rather than revoking the
    // first one alone.
    result = run(hostBinary, macHome, { "token", "revoke", tokenID, "extra" });
    expect(result.exitCode == 2 && contains(result.output, "nothing was revoked"), "token revoke refuses a second word");
    result = run(hostBinary, macHome, { "token", "list" });
    expect(contains(result.output, tokenID.substr(0, 16).c_str()), "a refused revoke left the token issued");
    result = run(hostBinary, macHome, { "token", "revoke", tokenID });
    expect(result.exitCode == 0 && contains(result.output, "revoked token"), "the full digest revokes");
    result = run(hostBinary, macHome, { "token", "list" });
    expect(result.exitCode == 0 && !contains(result.output, tokenID.substr(0, 16).c_str()), "the revoked digest left the list");

    // A token record read before malformed bytes is not a whole store. Both
    // mutation commands refuse rather than rewriting the readable prefix.
    {
        const std::string tokensPath = macHome + "/tokens";
        const std::string consentPath = macHome + "/consent";
        const std::string tokens = slurp(tokensPath);
        const std::string consent = slurp(consentPath);
        const std::string retainedID = firstTokenID(macHome);
        const bool hasRetainedToken = retainedID.size() == 64 && !tokens.empty();
        expect(hasRetainedToken, "a valid token remains for malformed-store revoke fixtures");
        if (hasRetainedToken) {
            const std::vector<std::string> malformed {
                tokens + "invalid-token-record\n",
                tokens + std::string("\0hidden-record\n", 15),
            };
            for (const auto& contents : malformed) {
                expect(writeFixture(tokensPath, contents), "the malformed token store was written");
                result = run(hostBinary, macHome, { "token", "issue", "--label", "must-not-exist" });
                expect(result.exitCode == 1, "token issue refuses a store that cannot be read whole");
                expect(slurp(tokensPath) == contents, "a refused issue preserves every token-store byte");
                expect(slurp(consentPath) == consent, "a refused issue changes no consent");

                expect(writeFixture(tokensPath, contents), "the malformed revoke fixture was restored independently");
                result = run(hostBinary, macHome, { "token", "revoke", retainedID });
                expect(result.exitCode == 1, "token revoke refuses even a valid target in an incomplete store");
                expect(slurp(tokensPath) == contents, "a refused revoke preserves every token-store byte");
                expect(slurp(consentPath) == consent, "a refused revoke changes no consent");
            }
        }
        expect(writeFixture(tokensPath, tokens), "the valid token store is restored after malformed fixtures");

        // Keeping the original token inode open distinguishes refusal before
        // any write from a token issued and then removed by a rollback.
        const std::vector<std::string> malformedConsent {
            consent + std::string("\0hidden-consent\n", 16),
            consent + std::string(70 * 1024, 'x') + "\n",
        };
        for (const auto& contents : malformedConsent) {
            expect(writeFixture(consentPath, contents), "the malformed consent store was written");
            std::unique_ptr<FILE, int (*)(FILE*)> original(std::fopen(tokensPath.c_str(), "r"), std::fclose);
            struct stat before {};
            const bool isPinned = original && ::fstat(::fileno(original.get()), &before) == 0;
            expect(isPinned, "the existing token store is pinned for the preflight assertion");
            result = run(hostBinary, macHome, { "token", "issue", "--label", "must-not-exist", "--allow-always" });
            expect(result.exitCode == 1, "allow-always refuses malformed consent before token issuance");
            expect(slurp(tokensPath) == tokens, "failed consent preflight preserves every token-store byte");
            expect(slurp(consentPath) == contents, "failed consent preflight preserves every consent-store byte");
            struct stat after {};
            expect(!isPinned || (::stat(tokensPath.c_str(), &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino),
                "failed consent preflight leaves the original token store in place");
        }
        expect(writeFixture(tokensPath, tokens), "the valid token store is restored after consent preflight fixtures");
        expect(writeFixture(consentPath, consent), "the valid consent store is restored after malformed fixtures");
    }

    // --ensure-daemon detaches a daemon through the double fork and waits for
    // its socket; a second run finds it alive and returns without spawning.
    const std::string socketLeaf = "host-00000000000000cc.sock";
    result = run(hostBinary, macHome, { "--ensure-daemon", "--socket", socketLeaf });
    expect(result.exitCode == 0, "--ensure-daemon brings the daemon up");
    result = run(hostBinary, macHome, { "--ensure-daemon", "--socket", socketLeaf });
    expect(result.exitCode == 0, "a second --ensure-daemon finds it already up");

    // --quic-bootstrap ensures the per-binary daemon and either prints the
    // bootstrap line or refuses because the build carries no QUIC.
    result = run(hostBinary, macHome, { "--quic-bootstrap" });
    expect((result.exitCode == 0 && contains(result.output, "quic_port="))
            || (result.exitCode == 1 && contains(result.output, "quic bootstrap failed")),
        "--quic-bootstrap prints the line or names its refusal");
    // Read here, before the runs below take `result` over: only a build whose
    // own bootstrap answered can tell a listener refused by --quic-port -1
    // from a build without QUIC.
    const bool hasQUIC = result.exitCode == 0 && contains(result.output, "quic_port=");

    expect(Ubiquitty::HostTest::endDetachedDaemons(macHome), "every daemon the detach flows started has ended");

    result = run(hostBinary, macHome, { "--take-over" });
    expect(result.exitCode == 0, "a first install starts its successor even when no predecessor exists");
    result = run(hostBinary, macHome, { "--take-over" });
    expect(result.exitCode == 0, "an already current host needs no takeover");
    expect(Ubiquitty::HostTest::endDetachedDaemons(macHome), "the takeover's private daemon has ended");

    // A typo starts nothing: an argument the host does not read, an option
    // missing its value, a value it cannot read, and two modes are refused,
    // and no daemon comes up in the home they named.
    char typoTemplate[] = "/tmp/ubiquitty-hostcli-typo.XXXXXX";
    if (char* typoRoot = ::mkdtemp(typoTemplate)) {
        const std::string typoHome = std::string(typoRoot) + "/.ubiquitty";
        ::mkdir(typoHome.c_str(), 0700);
        const std::vector<std::vector<std::string>> typos {
            { "--ensure-daemon", "--colums", "100" },
            { "--ensure-daemon", "--columns" },
            { "--ensure-daemon", "--columns", "abc" },
            { "--ensure-daemon", "--rows", "-5" },
            { "--ensure-daemon", "--rows", "2000" },
            { "--ensure-daemon", "token" },
            { "--ensure-daemon", "--attach" },
            { "--take-over", "--ensure-daemon" },
            { "--take-over", "--socket", "host-0000000000000001.sock" },
            { "--take-over", "--columns", "100" },
            { "--daemon", "--socket" },
            { "--quic-listen", "--bogus" },
            { "--version", "--bogus" },
        };
        for (const auto& typo : typos) {
            result = run(hostBinary, typoHome, typo);
            std::string what = "ubiquitty-host";
            for (const auto& word : typo)
                what += " " + word;
            what += " is refused before anything starts";
            expect(result.exitCode == 2 && contains(result.output, "nothing was started"), what.c_str());
        }
        bool hasProcessIDFile = false;
        if (DIR* directory = ::opendir(typoHome.c_str())) {
            while (struct dirent* entry = ::readdir(directory))
                hasProcessIDFile = hasProcessIDFile || std::string_view(entry->d_name).ends_with(".pid");
            ::closedir(directory);
        }
        expect(!hasProcessIDFile, "no refused argument started a daemon");
        struct stat certificate {};
        expect(::stat((typoHome + "/quic-cert.pem").c_str(), &certificate) != 0, "a refused --quic-listen wrote no certificate");
        expect(Ubiquitty::HostTest::endDetachedDaemons(typoHome), "nothing is left running in the home the refused arguments named");
    } else {
        expect(false, "a sandbox home for the typos is created");
    }

    // --quic-port -1 holds on the --ensure-daemon path too: the detached
    // daemon opens no QUIC listener, so asking it to bootstrap QUIC is refused
    // where the build has QUIC (`hasQUIC`). A port no listener can have is
    // refused before anything starts.
    char disabledTemplate[] = "/tmp/ubiquitty-hostcli-noquic.XXXXXX";
    if (char* disabledRoot = ::mkdtemp(disabledTemplate)) {
        const std::string disabledHome = std::string(disabledRoot) + "/.ubiquitty";
        ::mkdir(disabledHome.c_str(), 0700);
        for (const char* refused : { "70000", "-2", "12ab", "" }) {
            result = run(hostBinary, disabledHome, { "--ensure-daemon", "--quic-port", refused });
            expect(result.exitCode == 2 && contains(result.output, "--quic-port takes"), "an unreadable --quic-port is refused before anything starts");
        }
        result = run(hostBinary, disabledHome, { "--ensure-daemon", "--quic-port", "-1" });
        expect(result.exitCode == 0, "--ensure-daemon --quic-port -1 brings the daemon up");
        result = run(hostBinary, disabledHome, { "--quic-bootstrap" });
        if (hasQUIC)
            expect(result.exitCode == 1 && contains(result.output, "quic bootstrap failed"), "a daemon ensured with --quic-port -1 opens no QUIC listener");
        expect(Ubiquitty::HostTest::endDetachedDaemons(disabledHome), "the daemon ensured with --quic-port -1 has ended");
    } else {
        expect(false, "a second sandbox home is created");
    }

    if (failures == 0) {
        std::printf("host_cli: usage, help, version, token list/revoke, --allow-always consent, ensure-daemon, quic-bootstrap and --quic-port -1 answer, and a typo starts nothing\n");
        return 0;
    }
    std::fprintf(stderr, "host_cli: %d failure(s)\n", failures);
    return 1;
}
