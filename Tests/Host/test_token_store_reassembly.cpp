// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_token_store_reassembly.cpp — TokenStore::loadTokens reassembles
// each logical line from fixed-size reads, so a record longer than the read
// buffer authenticates instead of being dropped.
//
// The label is the last, uncapped field; a long one pushes the line past the
// 1024-byte read chunk. loadTokens must return that record whole (valid hash +
// full label), never a fragment, and still skip blanks / comments / malformed
// digests. Exit 0 on success.
//

#include "Assertions.h"
#include "Helpers/TempDir.h"

#include "Platform/Host/ManagementMetadata.h"
#include "Platform/Host/TokenStore.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

using Ubiquitty::Host::loadTokens;

static std::string writeTemp(const std::string& contents)
{
    char path[] = "/tmp/ubiquitty-tokenstore-XXXXXX";
    int fileDescriptor = ::mkstemp(path);
    std::FILE* file = ::fdopen(fileDescriptor, "w");
    std::fwrite(contents.data(), 1, contents.size(), file);
    std::fclose(file);
    return std::string(path);
}

// A path in the temp directory that no file occupies yet: the mode a file is
// CREATED with is only observable when the create is the test's own.
static std::string temporaryPath()
{
    char path[] = "/tmp/ubiquitty-tokenmode-XXXXXX";
    const int fileDescriptor = ::mkstemp(path);
    ::close(fileDescriptor);
    ::unlink(path);
    return std::string(path);
}

int main()
{
    {
        const auto path = temporaryPath();
        {
            Ubiquitty::Host::TokenStoreLock unconfigured("");
            CHECK(!unconfigured.isLocked());
            Ubiquitty::Host::TokenStoreLock first(path);
            CHECK(first.isLocked());
            Ubiquitty::Host::TokenStoreLock competing(path);
            CHECK(!competing.isLocked());
        }
        {
            Ubiquitty::Host::TokenStoreLock released(path);
            CHECK(released.isLocked());
        }
        ::unlink((path + ".lock").c_str());
        const auto destination = temporaryPath();
        CHECK(::symlink(destination.c_str(), (path + ".lock").c_str()) == 0);
        Ubiquitty::Host::TokenStoreLock symbolic(path);
        CHECK(!symbolic.isLocked());
        ::unlink((path + ".lock").c_str());
    }
    {
        const auto path = temporaryPath();
        Ubiquitty::Host::TokenStoreLock lock(path);
        CHECK(lock.isLocked());
        {
            Ubiquitty::Host::ManagementMetadataStore metadata(path);
            CHECK(metadata.isValid());
            CHECK(metadata.created("token:example", 100, "Local owner"));
            CHECK(metadata.used("token:example", 200, "Remote client"));
            CHECK(!metadata.used("token:example", 201, "forged\nactor"));
        }
        {
            Ubiquitty::Host::ManagementMetadataStore metadata(path);
            const auto record = metadata.metadata("token:example");
            CHECK(record.createdUnix == 100 && record.createdBy == "Local owner");
            CHECK(record.lastUsedUnix == 200 && record.lastUsedBy == "Remote client");
            CHECK(metadata.metadata("token:unknown").createdUnix == 0);
        }
        CHECK(loadTokens(path).empty());
        ::unlink((path + ".metadata").c_str());
        ::unlink((path + ".lock").c_str());
    }
    const std::string h1(64, 'a');
    const std::string h2(64, 'b');
    const std::string h3(64, 'c');
    const std::string longLabel(5000, 'x'); // well past the 1024-byte read chunk

    // Over-length record between two short ones, plus a comment and a blank line.
    {
        std::string body;
        body += "# a comment line\n";
        body += h1 + " 1 0 first\n";
        body += "\n"; // blank
        body += h2 + " 3 0 " + longLabel + "\n"; // spans several read chunks
        body += h3 + " 1 0 third\n";
        const std::string path = writeTemp(body);
        const auto records = loadTokens(path);
        ::unlink(path.c_str());

        CHECK(records.size() == 3); // comment + blank skipped, all three kept
        CHECK(records[0].hashHex == h1 && records[0].label == "first");
        // The long record is read WHOLE: valid hash + full label, not a fragment.
        CHECK(records[1].hashHex == h2);
        CHECK(records[1].label == longLabel);
        CHECK(records[2].hashHex == h3 && records[2].label == "third");
    }

    // A final line with no trailing newline still parses.
    {
        const std::string path = writeTemp(h1 + " 1 0 no-newline");
        const auto records = loadTokens(path);
        ::unlink(path.c_str());
        CHECK(records.size() == 1);
        CHECK(records[0].hashHex == h1 && records[0].label == "no-newline");
    }

    // A final over-length line without a trailing newline reassembles too.
    {
        const std::string path = writeTemp(h2 + " 1 0 " + longLabel);
        const auto records = loadTokens(path);
        ::unlink(path.c_str());
        CHECK(records.size() == 1);
        CHECK(records[0].hashHex == h2 && records[0].label == longLabel);
    }

    // Consecutive over-length lines reassemble independently (no carry-over).
    {
        const std::string body = h1 + " 1 0 " + longLabel + "\n" + h2 + " 1 0 " + longLabel + "\n";
        const std::string path = writeTemp(body);
        const auto records = loadTokens(path);
        ::unlink(path.c_str());
        CHECK(records.size() == 2);
        CHECK(records[0].hashHex == h1 && records[0].label == longLabel);
        CHECK(records[1].hashHex == h2 && records[1].label == longLabel);
    }

    // A pathological line past the 64 KiB ceiling is dropped whole (no fragment,
    // no unbounded growth); a following valid record still loads.
    {
        const std::string body = h1 + " 1 0 " + std::string(70 * 1024, 'y') + "\n" + h3 + " 1 0 sane\n";
        const std::string path = writeTemp(body);
        const auto records = loadTokens(path);
        ::unlink(path.c_str());
        CHECK(records.size() == 1);
        CHECK(records[0].hashHex == h3 && records[0].label == "sane");
    }

    // A malformed digest (wrong length) is skipped even when over-length.
    {
        const std::string body = "deadbeef 1 0 " + longLabel + "\n" + h3 + " 1 0 ok\n";
        const std::string path = writeTemp(body);
        const auto records = loadTokens(path);
        ::unlink(path.c_str());
        CHECK(records.size() == 1); // short-hash line dropped, valid one kept
        CHECK(records[0].hashHex == h3 && records[0].label == "ok");
    }

    // An authorization reads the store whole or not at all: a line the reader
    // skipped may be the revocation it was meant to see.
    {
        using Ubiquitty::Host::readTokenStore;
        using Ubiquitty::Host::verifyToken;
        const std::string written = "# a comment line\n\n" + h1 + " 1 0 first\n" + h2 + " 3 1700000000 " + longLabel + "\n" + h3 + " 255 0 \n";
        const std::string path = writeTemp(written);
        const auto whole = readTokenStore(path);
        ::unlink(path.c_str());
        CHECK(whole.isWhole);
        CHECK(whole.records.size() == 3);

        const auto absent = readTokenStore(temporaryPath());
        CHECK(absent.isWhole && absent.records.empty());

        const std::string keptToken = "kept-token-kept-token-kept-token";
        const std::string kept = Ubiquitty::Crypto::sha256Hex(keptToken) + " 1 0 kept\n";
        const std::string sound = writeTemp(kept);
        CHECK(verifyToken(sound, keptToken, 0).ok); // the control: alone, the record authenticates
        ::unlink(sound.c_str());
        for (const std::string& line : { h1 + " 1 never label", h1 + " 256 0 label", h1 + " 1x 0 label", h1 + " 1", std::string("deadbeef 1 0 label"),
                 std::string(64, 'A') + " 1 0 label", h1 + " 1 18446744073709551616 label", h1 + " -1 0 label" }) {
            const std::string corrupted = writeTemp(kept + line + "\n");
            const auto store = readTokenStore(corrupted);
            CHECK(!store.isWhole);
            // The record beside the bad line authenticates nothing either.
            CHECK(!verifyToken(corrupted, keptToken, 0).ok);
            ::unlink(corrupted.c_str());
        }

        const std::string overlong = writeTemp(h1 + " 1 0 " + std::string(70 * 1024, 'y') + "\n" + h3 + " 1 0 sane\n");
        CHECK(!readTokenStore(overlong).isWhole);
        ::unlink(overlong.c_str());

        // A NUL cannot hide a suffix or turn an incomplete physical line
        // into a valid record. Earlier valid records grant nothing either.
        std::vector<std::string> binary;
        binary.push_back(std::string(1, '\0') + kept);
        binary.push_back(kept.substr(0, kept.size() - 1) + std::string("\0hidden\n", 8));
        binary.push_back(kept + std::string("\0hidden\n", 8));
        for (const std::size_t offset : std::array<std::size_t, 2> { 1023, 1024 }) {
            std::string bytes = kept + "#";
            bytes.append(offset - bytes.size(), 'x');
            bytes.push_back('\0');
            bytes += "hidden\n";
            binary.push_back(std::move(bytes));
        }
        for (const auto& bytes : binary) {
            const std::string corrupted = writeTemp(bytes);
            CHECK(!readTokenStore(corrupted).isWhole);
            CHECK(!verifyToken(corrupted, keptToken, 0).ok);
            ::unlink(corrupted.c_str());
        }

        const std::string unterminated = writeTemp(kept.substr(0, kept.size() - 1));
        CHECK(readTokenStore(unterminated).isWhole);
        CHECK(verifyToken(unterminated, keptToken, 0).ok);
        ::unlink(unterminated.c_str());

        const std::string carriageReturns = writeTemp(kept.substr(0, kept.size() - 1) + "\r\n");
        CHECK(readTokenStore(carriageReturns).isWhole);
        CHECK(verifyToken(carriageReturns, keptToken, 0).ok);
        ::unlink(carriageReturns.c_str());

        // A store this user cannot open is not an empty one.
        const std::string unreadable = writeTemp(h3 + " 1 0 kept\n");
        CHECK(::chmod(unreadable.c_str(), 0) == 0);
        if (::geteuid() != 0)
            CHECK(!readTokenStore(unreadable).isWhole);
        ::unlink(unreadable.c_str());

        const std::string fifo = temporaryPath();
        CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
        CHECK(!readTokenStore(fifo).isWhole);
        CHECK(!verifyToken(fifo, keptToken, 0).ok);
        ::unlink(fifo.c_str());

        const std::string destination = writeTemp(kept);
        const std::string symbolic = temporaryPath();
        CHECK(::symlink(destination.c_str(), symbolic.c_str()) == 0);
        CHECK(!readTokenStore(symbolic).isWhole);
        CHECK(!verifyToken(symbolic, keptToken, 0).ok);
        CHECK(verifyToken(destination, keptToken, 0).ok);
        ::unlink(symbolic.c_str());
        ::unlink(destination.c_str());
        CHECK(!readTokenStore("/dev/null").isWhole);
    }

    // The store holds token digests, and the mode that matters is the one the
    // file is CREATED with: a create-then-chmod leaves a window a co-located
    // user can win. Under a permissive umask, a file that is 0600 on arrival
    // can only have been created that way.
    {
        const mode_t previousMask = ::umask(0);
        const std::string path = temporaryPath();
        FILE* file = Ubiquitty::Host::openOwnerOnly(path, "a");
        CHECK(file != nullptr);
        struct stat created {};
        CHECK(::fstat(::fileno(file), &created) == 0);
        std::fclose(file);
        ::unlink(path.c_str());
        ::umask(previousMask);
        // Exactly owner read/write: no group, no world, whatever the umask
        // would have allowed.
        CHECK((created.st_mode & 0777) == 0600);
    }

    // A rewrite creates its temporary file with owner-only access too.
    {
        const mode_t previousMask = ::umask(0);
        const std::string path = temporaryPath();
        std::vector<Ubiquitty::Host::TokenRecord> records;
        Ubiquitty::Host::parseTokenLine(h3 + " 1 0 kept", records);
        CHECK(records.size() == 1);
        CHECK(Ubiquitty::Host::writeTokens(path, records) == Ubiquitty::Host::TokenWriteOutcome::Durable);
        struct stat written {};
        CHECK(::stat(path.c_str(), &written) == 0);
        ::unlink(path.c_str());
        ::umask(previousMask);
        CHECK((written.st_mode & 0777) == 0600);
    }

    // A stale symlink at the predictable temporary path cannot redirect a
    // rewrite into another owner file, change its mode, or become the store.
    {
        Ubiquitty::Test::TempDir directory;
        CHECK(directory.valid());
        if (!directory.valid())
            return 1;
        const std::string path = directory.path() + "/tokens";
        const std::string sentinelPath = directory.path() + "/sentinel";
        const std::string sentinel = "sentinel must survive\n";
        FILE* sentinelFile = Ubiquitty::Host::openOwnerOnly(sentinelPath, "w");
        CHECK(sentinelFile != nullptr);
        if (sentinelFile) {
            CHECK(std::fwrite(sentinel.data(), 1, sentinel.size(), sentinelFile) == sentinel.size());
            CHECK(std::fclose(sentinelFile) == 0);
        }
        CHECK(::chmod(sentinelPath.c_str(), 0640) == 0);
        CHECK(::symlink(sentinelPath.c_str(), (path + ".tmp").c_str()) == 0);
        std::vector<Ubiquitty::Host::TokenRecord> records;
        Ubiquitty::Host::parseTokenLine(h3 + " 1 0 kept", records);
        CHECK(Ubiquitty::Host::writeTokens(path, records) == Ubiquitty::Host::TokenWriteOutcome::Durable);

        sentinelFile = std::fopen(sentinelPath.c_str(), "r");
        CHECK(sentinelFile != nullptr);
        if (sentinelFile) {
            char bytes[64];
            const auto length = std::fread(bytes, 1, sizeof(bytes), sentinelFile);
            CHECK(std::string(bytes, length) == sentinel);
            CHECK(std::ferror(sentinelFile) == 0);
            CHECK(std::fclose(sentinelFile) == 0);
        }
        struct stat information {};
        CHECK(::stat(sentinelPath.c_str(), &information) == 0);
        CHECK((information.st_mode & 0777) == 0640);
        CHECK(::lstat((path + ".tmp").c_str(), &information) == 0);
        CHECK(S_ISLNK(information.st_mode));
        CHECK(::lstat(path.c_str(), &information) == 0);
        CHECK(S_ISREG(information.st_mode));
        CHECK((information.st_mode & 0777) == 0600);
        const auto reloaded = loadTokens(path);
        CHECK(reloaded.size() == 1);
        if (reloaded.size() == 1)
            CHECK(reloaded[0].hashHex == h3 && reloaded[0].label == "kept");
        CHECK(::unlink(path.c_str()) == 0);
        CHECK(::unlink(sentinelPath.c_str()) == 0);
        CHECK(::unlink((path + ".tmp").c_str()) == 0);
        std::error_code error;
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);
    }

    // A revoke is a rewrite that drops the record: afterwards the store loads
    // exactly the kept set, so the revoked digest authenticates nowhere. The
    // rewrite reports durable, and a second rewrite over it cannot resurrect
    // the dropped record.
    {
        const std::string path = temporaryPath();
        std::vector<Ubiquitty::Host::TokenRecord> both;
        Ubiquitty::Host::parseTokenLine(h1 + " 1 0 revoked", both);
        Ubiquitty::Host::parseTokenLine(h3 + " 1 0 kept", both);
        CHECK(both.size() == 2);
        CHECK(Ubiquitty::Host::writeTokens(path, both) == Ubiquitty::Host::TokenWriteOutcome::Durable);
        CHECK(loadTokens(path).size() == 2);

        std::vector<Ubiquitty::Host::TokenRecord> kept;
        Ubiquitty::Host::parseTokenLine(h3 + " 1 0 kept", kept);
        CHECK(Ubiquitty::Host::writeTokens(path, kept) == Ubiquitty::Host::TokenWriteOutcome::Durable);
        const auto reloaded = loadTokens(path);
        CHECK(reloaded.size() == 1);
        CHECK(reloaded[0].hashHex == h3 && reloaded[0].label == "kept");
        ::unlink(path.c_str());
    }

    // A rewrite that cannot land reports IOError rather than half-applying: no
    // store file appears, and no temp file is left behind for a later write
    // to mistake for state.
    {
        Ubiquitty::Test::TempDir directory;
        CHECK(directory.valid());
        if (!directory.valid())
            return 1;
        std::vector<Ubiquitty::Host::TokenRecord> records;
        Ubiquitty::Host::parseTokenLine(h3 + " 1 0 kept", records);
        const std::string missingDir = directory.path() + "/missing/tokens";
        CHECK(Ubiquitty::Host::writeTokens(missingDir, records) == Ubiquitty::Host::TokenWriteOutcome::IOError);
        CHECK(::access(missingDir.c_str(), F_OK) != 0);
        std::error_code error;
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);

        const std::string occupied = directory.path() + "/occupied";
        CHECK(::mkdir(occupied.c_str(), 0700) == 0);
        CHECK(Ubiquitty::Host::writeTokens(occupied, records) == Ubiquitty::Host::TokenWriteOutcome::IOError);
        CHECK(std::filesystem::is_directory(occupied, error) && !error);
        CHECK(std::filesystem::is_empty(occupied, error) && !error);
        CHECK(::rmdir(occupied.c_str()) == 0);
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);
    }

    // Parent resolution covers every path shape, so the directory sync lands
    // on the directory the rename commits in.
    {
        using Ubiquitty::Host::tokenParentDirectory;
        CHECK(tokenParentDirectory("tokens") == ".");
        CHECK(tokenParentDirectory("/tokens") == "/");
        CHECK(tokenParentDirectory("/a/b/tokens") == "/a/b");
        CHECK(tokenParentDirectory("a/tokens") == "a");
    }

    // Injected sync failures map to their outcomes: EIO is IOError, EINVAL is
    // unsupported durability — the distinction `true` must never blur.
    {
        using Ubiquitty::Host::TokenStoreSync;
        using Ubiquitty::Host::TokenWriteOutcome;
        Ubiquitty::Test::TempDir directory;
        CHECK(directory.valid());
        if (!directory.valid())
            return 1;
        std::error_code error;
        std::vector<Ubiquitty::Host::TokenRecord> records;
        Ubiquitty::Host::parseTokenLine(h3 + " 1 0 kept", records);

        TokenStoreSync failingFile;
        failingFile.syncFile = [](int) { errno = EIO; return -1; };
        failingFile.syncParent = [](const char*) { return 0; };
        const std::string failingPath = directory.path() + "/tokens";
        CHECK(Ubiquitty::Host::writeTokens(failingPath, records, &failingFile) == TokenWriteOutcome::IOError);
        CHECK(::access(failingPath.c_str(), F_OK) != 0);
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);

        std::vector<Ubiquitty::Host::TokenRecord> previous;
        Ubiquitty::Host::parseTokenLine(h1 + " 1 0 original", previous);
        CHECK(Ubiquitty::Host::writeTokens(failingPath, previous) == TokenWriteOutcome::Durable);
        CHECK(Ubiquitty::Host::writeTokens(failingPath, records, &failingFile) == TokenWriteOutcome::IOError);
        const auto preserved = loadTokens(failingPath);
        CHECK(preserved.size() == 1);
        if (preserved.size() == 1)
            CHECK(preserved[0].hashHex == h1 && preserved[0].label == "original");
        CHECK(::unlink(failingPath.c_str()) == 0);
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);

        TokenStoreSync unsupportedFile;
        unsupportedFile.syncFile = [](int descriptor) {
            struct stat information {};
            CHECK(::fstat(descriptor, &information) == 0);
            CHECK((information.st_mode & 0777) == 0600);
            const int flags = ::fcntl(descriptor, F_GETFD);
            CHECK(flags >= 0 && (flags & FD_CLOEXEC));
            errno = EINVAL;
            return -1;
        };
        unsupportedFile.syncParent = [](const char*) { return 0; };
        const std::string unsupportedPath = directory.path() + "/tokens";
        CHECK(Ubiquitty::Host::writeTokens(unsupportedPath, records, &unsupportedFile) == TokenWriteOutcome::UnsupportedDurability);
        // Unsupported still lands the bytes; only the proof is missing.
        CHECK(loadTokens(unsupportedPath).size() == 1);
        ::unlink(unsupportedPath.c_str());
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);

        TokenStoreSync failingParent;
        failingParent.syncFile = [](int) { return 0; };
        failingParent.syncParent = [](const char*) { errno = EIO; return -1; };
        const std::string parentPath = directory.path() + "/tokens";
        // The rewrite landed (only the proof is missing), so it reports
        // unproven rather than failed — and rolling it back would revalidate
        // the revoked record, which is the worst outcome.
        CHECK(Ubiquitty::Host::writeTokens(parentPath, records, &failingParent) == TokenWriteOutcome::UnsupportedDurability);
        CHECK(loadTokens(parentPath).size() == 1);
        ::unlink(parentPath.c_str());
        CHECK(std::filesystem::is_empty(directory.path(), error) && !error);
    }

    if (failures == 0)
        std::printf("token_store_reassembly: all passed\n");
    return failures ? 1 : 0;
}
