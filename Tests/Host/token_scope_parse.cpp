// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/token_scope_parse.cpp — `ubiquitty-host token issue --scope ...`
// fails closed on a misspelled / empty scope.
//
// A typo in --scope must not silently fall back to the default observe|drive
// scope (an over-grant). Drives the REAL ubiquitty-host binary:
//   - a misspelled single scope ("controll") exits non-zero, mints no token,
//   - a mixed valid/invalid list ("control,bogus") exits non-zero,
//   - a trailing bare "--scope" exits non-zero,
//   - a VALID scope ("control") exits 0 and writes exactly one store record,
//   - --scope OMITTED keeps the observe|drive default (exits 0).
//
// Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Protocol/Sync/NativeAttachment.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {

// Run `<hostBin> token issue <extra args...>` under a private UBIQUITTY_HOME,
// capturing stdout to /dev/null'd or counted. Returns the child's exit code, or
// -1 on a spawn failure. `mintedToken` (when non-null) receives true if the
// child printed a non-empty token line to stdout.
int runIssue(const char* hostBin, const std::string& macHome, const char* const* extra, std::size_t extraCount, bool* mintedToken)
{
    int pipeFileDescriptors[2] = { -1, -1 };
    if (::pipe(pipeFileDescriptors) != 0)
        return -1;
    pid_t processID = ::fork();
    if (processID < 0) {
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        return -1;
    }
    if (processID == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::dup2(pipeFileDescriptors[1], STDOUT_FILENO);
        ::close(pipeFileDescriptors[0]);
        ::close(pipeFileDescriptors[1]);
        // argv: hostBin token issue <extra...>
        const std::size_t argvCount = 3 + extraCount + 1;
        char** args = static_cast<char**>(std::calloc(argvCount, sizeof(char*)));
        args[0] = const_cast<char*>(hostBin);
        args[1] = const_cast<char*>("token");
        args[2] = const_cast<char*>("issue");
        for (std::size_t i = 0; i < extraCount; ++i)
            args[3 + i] = const_cast<char*>(extra[i]);
        args[argvCount - 1] = nullptr;
        ::execv(hostBin, args);
        ::_exit(127);
    }
    ::close(pipeFileDescriptors[1]);
    std::string out;
    char buffer[512];
    for (;;) {
        ssize_t n = ::read(pipeFileDescriptors[0], buffer, sizeof(buffer));
        if (n > 0)
            out.append(buffer, static_cast<std::size_t>(n));
        else
            break;
    }
    ::close(pipeFileDescriptors[0]);
    int status = 0;
    if (::waitpid(processID, &status, 0) != processID || !WIFEXITED(status))
        return -1;
    if (mintedToken) {
        // A minted token is a 64-hex-char line on stdout; any non-whitespace
        // output counts as "something was emitted".
        *mintedToken = out.find_first_not_of(" \t\r\n") != std::string::npos;
    }
    return WEXITSTATUS(status);
}

// Count the lines in the token store (0 if absent). UBIQUITTY_HOME is the
// control directory, so the store is <home>/tokens.
long tokenStoreLines(const std::string& macHome)
{
    const std::string path = macHome + "/tokens";
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return 0;
    long lines = 0;
    int ch;
    bool sawAny = false;
    while ((ch = std::fgetc(file)) != EOF) {
        sawAny = true;
        if (ch == '\n')
            ++lines;
    }
    if (sawAny && lines == 0)
        lines = 1; // a final line without a trailing newline
    std::fclose(file);
    return lines;
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "token_scope_parse: FAIL — %s\n", msg);
    return false;
}

} // namespace

// Whether the record labelled `label` carries the FileRead scope bit.
//
// Reads the store, because the store is what a later daemon run enforces from.
// A record is `<hash> <scopeBits> <expiry> <label>` with the scope as a DECIMAL
// bitmask -- so a substring search for the word "file" finds nothing there and
// would report "not granted" for every record including the granted one. That
// version of this helper passed while `all` silently granted the bit.
static bool storeGrantsFileScopeForLabel(const std::string& home, const char* label)
{
    const std::string path = home + "/tokens";
    std::FILE* handle = std::fopen(path.c_str(), "r");
    if (handle == nullptr)
        return false;
    char line[1024];
    bool found = false;
    while (std::fgets(line, sizeof(line), handle) != nullptr) {
        char hash[256] = { 0 };
        char recordLabel[256] = { 0 };
        unsigned long scopeBits = 0;
        long long expiry = 0;
        if (std::sscanf(line, "%255s %lu %lld %255s", hash, &scopeBits, &expiry, recordLabel) != 4)
            continue;
        if (std::string_view(recordLabel) != std::string_view(label))
            continue;
        found = (scopeBits & static_cast<unsigned long>(Ubiquitty::Protocol::Sync::Scope::FileRead)) != 0;
        break;
    }
    std::fclose(handle);
    return found;
}

#include <string_view>

// The expiry, in seconds since the epoch, of the record labelled `label`; -1
// when the store holds no such record.
static long long storeExpiryForLabel(const std::string& home, const char* label)
{
    const std::string path = home + "/tokens";
    std::FILE* handle = std::fopen(path.c_str(), "r");
    if (handle == nullptr)
        return -1;
    char line[1024];
    long long found = -1;
    while (std::fgets(line, sizeof(line), handle) != nullptr) {
        char hash[256] = { 0 };
        char recordLabel[256] = { 0 };
        unsigned long scopeBits = 0;
        long long expiry = 0;
        if (std::sscanf(line, "%255s %lu %lld %255s", hash, &scopeBits, &expiry, recordLabel) == 4 && std::string_view(recordLabel) == std::string_view(label)) {
            found = expiry;
            break;
        }
    }
    std::fclose(handle);
    return found;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    char homeTemplate[] = "/tmp/ubiquitty-scope.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = home;

    // --- A misspelled single scope must FAIL and mint nothing. ---
    {
        const char* a[] = { "--scope", "controll", "--label", "typo" };
        bool minted = true;
        const int rc = runIssue(argv[1], macHome, a, 4, &minted);
        if (rc == 0)
            return fail("misspelled --scope should exit non-zero") ? 1 : 1;
        if (minted)
            return fail("misspelled --scope should not print a token") ? 1 : 1;
        if (tokenStoreLines(macHome) != 0)
            return fail("misspelled --scope must not write a store record") ? 1 : 1;
    }

    // --- A mixed valid/invalid list must FAIL (one bad entry poisons it). ---
    {
        const char* a[] = { "--scope", "control,bogus", "--label", "mixed" };
        bool minted = true;
        const int rc = runIssue(argv[1], macHome, a, 4, &minted);
        if (rc == 0)
            return fail("mixed valid/invalid --scope should exit non-zero") ? 1 : 1;
        if (tokenStoreLines(macHome) != 0)
            return fail("mixed valid/invalid --scope must not write a record") ? 1 : 1;
    }

    // --- A trailing bare --scope (no value) must FAIL. ---
    {
        const char* a[] = { "--scope" };
        const int rc = runIssue(argv[1], macHome, a, 1, nullptr);
        if (rc == 0)
            return fail("trailing bare --scope should exit non-zero") ? 1 : 1;
        if (tokenStoreLines(macHome) != 0)
            return fail("trailing bare --scope must not write a record") ? 1 : 1;
    }

    // --- A VALID single scope succeeds and writes exactly one record. ---
    {
        const char* a[] = { "--scope", "control", "--label", "ci" };
        bool minted = false;
        const int rc = runIssue(argv[1], macHome, a, 4, &minted);
        if (rc != 0)
            return fail("valid --scope control should exit 0") ? 1 : 1;
        if (!minted)
            return fail("valid --scope control should print a token") ? 1 : 1;
        if (tokenStoreLines(macHome) != 1)
            return fail("valid --scope control should write one record") ? 1 : 1;
    }

    // --- --scope OMITTED keeps the observe|drive default and succeeds. ---
    {
        const char* a[] = { "--label", "default" };
        bool minted = false;
        const int rc = runIssue(argv[1], macHome, a, 2, &minted);
        if (rc != 0)
            return fail("omitted --scope should exit 0 (default)") ? 1 : 1;
        if (!minted)
            return fail("omitted --scope should print a token") ? 1 : 1;
        if (tokenStoreLines(macHome) != 2)
            return fail("omitted --scope should append a second record") ? 1 : 1;
    }

    // --- `file` is grantable, by that name. ---
    //
    // Scope::FileRead is documented as "default-off — a token needs it granted
    // explicitly", and scopeText() prints it as `file` in `token list`. A
    // parser with no case for it would refuse the one name the listing shows:
    // a scope a reader could see and not type, and a capability nothing could
    // grant.
    {
        const char* a[] = { "--scope", "file", "--label", "fileread" };
        bool didMint = false;
        const int rc = runIssue(argv[1], macHome, a, 4, &didMint);
        if (rc != 0)
            return fail("--scope file should exit 0") ? 1 : 1;
        if (!didMint)
            return fail("--scope file should print a token") ? 1 : 1;
        if (tokenStoreLines(macHome) != 3)
            return fail("--scope file should append a third record") ? 1 : 1;
    }

    // --- and `all` leaves it out. ---
    //
    // FILE_REQUEST reads files off the host's disk. A name meaning "everything"
    // that grants it as a side effect is the opposite of the explicit grant the
    // protocol asks for, so the two are checked against each other rather than
    // each on its own: `all` mints, and what it didMint must not carry the bit.
    {
        const char* a[] = { "--scope", "all", "--label", "everything" };
        bool didMint = false;
        const int rc = runIssue(argv[1], macHome, a, 4, &didMint);
        if (rc != 0)
            return fail("--scope all should exit 0") ? 1 : 1;
        if (!didMint)
            return fail("--scope all should print a token") ? 1 : 1;
        if (tokenStoreLines(macHome) != 4)
            return fail("--scope all should append a fourth record") ? 1 : 1;
        if (storeGrantsFileScopeForLabel(macHome, "everything"))
            return fail("--scope all must not grant file: FILE_REQUEST is by name only") ? 1 : 1;
        if (!storeGrantsFileScopeForLabel(macHome, "fileread"))
            return fail("--scope file must grant it, or the check above proves nothing") ? 1 : 1;
    }

    // --- --expires reads its unit, and refuses what it cannot read. ---
    //
    // `1h` was read as far as it parsed, a one-second token that is dead by the
    // time it is used. Each unit lands within a minute of its length; a
    // spelling with no reading, or a missing value, mints nothing.
    {
        const struct {
            const char* lifetime;
            const char* label;
            long long seconds;
        } lifetimes[] = { { "90", "plain", 90 }, { "45s", "seconds", 45 }, { "30m", "minutes", 1800 }, { "1h", "hour", 3600 }, { "7d", "week", 7 * 86400 } };
        long records = tokenStoreLines(macHome);
        for (const auto& entry : lifetimes) {
            const char* a[] = { "--expires", entry.lifetime, "--label", entry.label };
            const long long before = static_cast<long long>(::time(nullptr));
            if (runIssue(argv[1], macHome, a, 4, nullptr) != 0)
                return fail("a lifetime with a known unit should exit 0") ? 1 : 1;
            const long long expiry = storeExpiryForLabel(macHome, entry.label);
            if (expiry < before + entry.seconds || expiry > before + entry.seconds + 60) {
                std::fprintf(stderr, "token_scope_parse: --expires %s stored expiry %lld, want about %lld\n", entry.lifetime, expiry, before + entry.seconds);
                return fail("--expires stored the wrong expiry") ? 1 : 1;
            }
            ++records;
        }
        for (const char* refused : { "1x", "h", "", "-5", "1h30m", "1 h", "99999999999d" }) {
            const char* a[] = { "--expires", refused, "--label", "refused" };
            bool minted = true;
            if (runIssue(argv[1], macHome, a, 4, &minted) == 0 || minted) {
                std::fprintf(stderr, "token_scope_parse: --expires \"%s\" was accepted\n", refused);
                return fail("an unreadable --expires must exit non-zero and mint nothing") ? 1 : 1;
            }
        }
        const char* bare[] = { "--label", "bare", "--expires" };
        if (runIssue(argv[1], macHome, bare, 3, nullptr) == 0)
            return fail("a trailing bare --expires must exit non-zero") ? 1 : 1;
        if (tokenStoreLines(macHome) != records)
            return fail("a refused --expires must not write a record") ? 1 : 1;
    }

    std::printf("token_scope_parse: misspelled/mixed/bare --scope fail closed; "
                "valid scope mints one record; omitted --scope keeps the default; "
                "`file` is grantable by name and `all` does not include it; "
                "--expires reads s/m/h/d and refuses anything else\n");
    return 0;
}
