// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_consent_store.cpp — the token CLI's edits of the standing
// consent list: `token issue --allow-always` records a grant, `token revoke`
// withdraws it, and every shape of file the daemon reads survives both.
//
// The list is the daemon's file, so the invariants are the daemon's: a
// header once and first; a list that does not open with it is refused
// whole; a key is recorded once however many times it is
// asked for; nothing changes on any refusal. Exit 0 on success.
//

#include "Assertions.h"

#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/ConsentStore.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

using Ubiquitty::Host::addStandingConsent;
using Ubiquitty::Host::ConsentEdit;
using Ubiquitty::Host::ConsentList;
using Ubiquitty::Host::grantFileHeader;
using Ubiquitty::Host::maxConsentListBytes;
using Ubiquitty::Host::maxConsentListEntries;
using Ubiquitty::Host::maxConsentListLineBytes;
using Ubiquitty::Host::readConsentList;
using Ubiquitty::Host::removeStandingConsent;
using Ubiquitty::Host::tokenGrantKey;

static std::string slurp(const std::string& path)
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

static void overwrite(const std::string& path, const std::string& contents)
{
    std::FILE* file = std::fopen(path.c_str(), "w");
    std::fwrite(contents.data(), 1, contents.size(), file);
    std::fclose(file);
}

static bool exists(const std::string& path)
{
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

int main()
{
    char directoryTemplate[] = "/tmp/ubiquitty-consentstore-XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    if (directory == nullptr) {
        std::printf("consent_store: mkdtemp failed\n");
        return 1;
    }
    const std::string path = std::string(directory) + "/consent";
    const std::string temporary = path + ".tmp";
    const std::string header = grantFileHeader("consent");
    const std::string alpha = tokenGrantKey(std::string(64, 'a'));
    const std::string beta = tokenGrantKey(std::string(64, 'b'));
    const auto refusesAppendWithoutWrite = [&](const std::string& contents) {
        overwrite(path, contents);
        std::unique_ptr<FILE, int (*)(FILE*)> original(std::fopen(path.c_str(), "r"), std::fclose);
        REQUIRE(original);
        struct stat before {};
        REQUIRE(::fstat(::fileno(original.get()), &before) == 0);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(slurp(path) == contents);
        struct stat after {};
        CHECK(::stat(path.c_str(), &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino);
        CHECK(!exists(temporary));
    };

    CHECK(tokenGrantKey("0123") == "token:0123");

    // No list yet: reading says so, and the first grant creates it — header
    // first, private to its owner from the moment it exists.
    {
        const ConsentList before = readConsentList(path);
        CHECK(!before.isPresent && before.entries.empty() && !before.hasHeader);
        CHECK(before.isWhole && !before.readError);
        CHECK(addStandingConsent(path, alpha) == ConsentEdit::Changed);
        CHECK(slurp(path) == header + alpha + "\n");
        struct stat info {};
        CHECK(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
        const ConsentList after = readConsentList(path);
        CHECK(after.isPresent && after.hasHeader && !after.hasUnreadableLayout);
        CHECK(after.isWhole && !after.readError);
        CHECK(after.entries.size() == 1 && after.entries[0] == alpha);
    }

    // The same key again changes nothing, byte for byte; a second key joins
    // it under the one header.
    {
        const std::string was = slurp(path);
        CHECK(addStandingConsent(path, alpha) == ConsentEdit::Unchanged);
        CHECK(slurp(path) == was);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::Changed);
        CHECK(slurp(path) == header + alpha + "\n" + beta + "\n");
    }

    // Withdrawal keeps the header and the other key, leaves no temporary
    // behind, and a key that is not there is not an error.
    {
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::Changed);
        CHECK(slurp(path) == header + beta + "\n");
        CHECK(!exists(temporary));
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::Unchanged);
        CHECK(slurp(path) == header + beta + "\n");
        CHECK(removeStandingConsent(path, beta) == ConsentEdit::Changed);
        CHECK(slurp(path) == header);
    }

    // A list with no header states no layout: it is refused whole, both
    // ways, with the file untouched.
    {
        const std::string headerless = alpha + "\n";
        overwrite(path, headerless);
        CHECK(readConsentList(path).hasUnreadableLayout);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::UnreadableLayout);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::UnreadableLayout);
        CHECK(slurp(path) == headerless);
    }

    // An empty file is a list with nothing in it: the append gives it the
    // header rather than leaving a record with no layout.
    {
        overwrite(path, "");
        const ConsentList empty = readConsentList(path);
        CHECK(empty.isWhole && empty.isPresent && !empty.hasHeader && empty.entries.empty());
        CHECK(addStandingConsent(path, beta) == ConsentEdit::Changed);
        CHECK(slurp(path) == header + beta + "\n");
    }

    // Another layout is refused whole, both ways, with the file untouched.
    {
        const std::string future = "# ubiquitty consent v2\n" + alpha + "\n";
        overwrite(path, future);
        CHECK(readConsentList(path).hasUnreadableLayout);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::UnreadableLayout);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::UnreadableLayout);
        CHECK(slurp(path) == future);
    }

    // A line past the ceiling marks the list unreadable rather than being
    // split into entries: neither edit touches such a file.
    {
        const std::string overlong = header + std::string(70 * 1024, 'x') + "\n";
        overwrite(path, overlong);
        CHECK(readConsentList(path).hasOverlongLine);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(slurp(path) == overlong);
    }

    // A line longer than one read chunk but under the ceiling is one entry.
    {
        const std::string longKey = tokenGrantKey(std::string(3000, 'c'));
        overwrite(path, header + longKey + "\n" + alpha + "\n");
        const ConsentList read = readConsentList(path);
        CHECK(read.isWhole);
        CHECK(read.entries.size() == 2 && read.entries[0] == longKey && read.entries[1] == alpha);
        CHECK(removeStandingConsent(path, longKey) == ConsentEdit::Changed);
        CHECK(slurp(path) == header + alpha + "\n");
    }

    // A clean EOF closes the final text line; CRLF carries the same entries.
    {
        overwrite(path, header + alpha + "\n" + beta);
        const ConsentList read = readConsentList(path);
        CHECK(read.isWhole && !read.readError);
        CHECK((read.entries == std::vector<std::string> { alpha, beta }));
        overwrite(path, "# ubiquitty consent v1\r\n" + alpha + "\r\n" + beta + "\r\n");
        const ConsentList windows = readConsentList(path);
        CHECK(windows.isWhole && !windows.readError);
        CHECK((windows.entries == std::vector<std::string> { alpha, beta }));
    }
    {
        overwrite(path, header.substr(0, header.size() - 1));
        CHECK(addStandingConsent(path, alpha) == ConsentEdit::Changed);
        const ConsentList first = readConsentList(path);
        CHECK(first.isWhole);
        CHECK((first.entries == std::vector<std::string> { alpha }));

        overwrite(path, header + alpha);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::Changed);
        const ConsentList second = readConsentList(path);
        CHECK(second.isWhole);
        CHECK((second.entries == std::vector<std::string> { alpha, beta }));
    }

    // NUL is a byte in the input, never a terminator that can hide a suffix or
    // join two physical lines. Refusal also withholds an earlier valid entry.
    {
        std::vector<std::string> malformed;
        for (const std::size_t offset : std::array<std::size_t, 4> { 0, header.size(), 1023, 1024 }) {
            std::string contents = offset < header.size() ? std::string(offset, 'x') : header;
            if (offset > contents.size())
                contents.append(offset - contents.size(), 'x');
            contents.push_back('\0');
            contents += "hidden\n" + alpha + "\n";
            malformed.push_back(std::move(contents));
        }
        malformed.push_back(header + alpha + std::string("\0hidden\n", 8));
        malformed.push_back(header + alpha + "\n" + beta + std::string("\0hidden\n", 8));
        for (const auto& contents : malformed) {
            overwrite(path, contents);
            const ConsentList read = readConsentList(path);
            CHECK(!read.isWhole && read.hasUnreadableLayout && read.entries.empty());
            CHECK(addStandingConsent(path, beta) == ConsentEdit::UnreadableLayout);
            CHECK(removeStandingConsent(path, alpha) == ConsentEdit::UnreadableLayout);
            CHECK(slurp(path) == contents);
            CHECK(!exists(temporary));
        }
    }

    // The logical-line limit counts its newline; chunks cannot turn a tail
    // into a separate entry, and a rejected line withdraws the valid prefix.
    for (const std::size_t lineBytes : std::array<std::size_t, 3> { maxConsentListLineBytes - 1, maxConsentListLineBytes, maxConsentListLineBytes + 1 }) {
        const std::string entry(lineBytes - 1, 'x');
        const std::string contents = header + alpha + "\n" + entry + "\n";
        overwrite(path, contents);
        const ConsentList read = readConsentList(path);
        if (lineBytes <= maxConsentListLineBytes) {
            CHECK(read.isWhole && !read.hasOverlongLine);
            CHECK((read.entries == std::vector<std::string> { alpha, entry }));
        } else {
            CHECK(!read.isWhole && read.hasOverlongLine && read.entries.empty());
            CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
            CHECK(removeStandingConsent(path, alpha) == ConsentEdit::IOError);
            CHECK(slurp(path) == contents);
        }
    }
    {
        const std::string entry(maxConsentListLineBytes, 'x');
        overwrite(path, header + entry);
        const ConsentList read = readConsentList(path);
        CHECK(read.isWhole);
        CHECK((read.entries == std::vector<std::string> { entry }));
        overwrite(path, header + alpha + "\n" + entry);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::Changed);
        const ConsentList kept = readConsentList(path);
        CHECK(kept.isWhole);
        CHECK((kept.entries == std::vector<std::string> { entry }));
    }

    // A file of individually bounded lines still has a total work limit.
    {
        std::string contents = header + alpha + "\n";
        while (contents.size() < maxConsentListBytes) {
            const auto lineBytes = std::min(maxConsentListLineBytes, maxConsentListBytes - contents.size());
            contents.append(lineBytes - 1, 'x');
            contents.push_back('\n');
        }
        overwrite(path, contents);
        const ConsentList atLimit = readConsentList(path);
        CHECK(atLimit.isWhole && !atLimit.entries.empty());
        refusesAppendWithoutWrite(contents);
        contents.push_back('\n');
        overwrite(path, contents);
        const ConsentList tooLarge = readConsentList(path);
        CHECK(!tooLarge.isWhole && !tooLarge.readError && tooLarge.entries.empty());
        CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::IOError);
        CHECK(slurp(path) == contents);
    }

    // Empty lines cost decoded-entry storage even when they grant nothing.
    for (const std::string& line : { std::string("\n"), std::string("x\n") }) {
        std::string contents = header;
        for (std::size_t index = 0; index < maxConsentListEntries; ++index)
            contents += line;
        overwrite(path, contents);
        const ConsentList atLimit = readConsentList(path);
        CHECK(atLimit.isWhole && atLimit.entries.size() == maxConsentListEntries);
        refusesAppendWithoutWrite(contents);
        contents += line;
        overwrite(path, contents);
        const ConsentList tooMany = readConsentList(path);
        CHECK(!tooMany.isWhole && !tooMany.readError && tooMany.entries.empty());
        CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::IOError);
        CHECK(slurp(path) == contents);
    }

    // Keeping the writer open makes the next read fail with EAGAIN rather
    // than EOF, after the stream has delivered a complete header and entry.
    [&] {
        int descriptors[2] { -1, -1 };
        REQUIRE(::pipe(descriptors) == 0);
        Ubiquitty::PAL::UnixFileDescriptor reader(descriptors[0]);
        Ubiquitty::PAL::UnixFileDescriptor writer(descriptors[1]);
        const int flags = ::fcntl(reader.get(), F_GETFL, 0);
        REQUIRE(flags != -1);
        REQUIRE(::fcntl(reader.get(), F_SETFL, flags | O_NONBLOCK) == 0);
        std::unique_ptr<FILE, int (*)(FILE*)> stream(::fdopen(reader.get(), "r"), std::fclose);
        REQUIRE(stream);
        const int borrowedDescriptor = reader.release();
        const std::string prefix = header + alpha + "\n";
        REQUIRE(::write(writer.get(), prefix.data(), prefix.size()) == static_cast<ssize_t>(prefix.size()));
        const ConsentList read = readConsentList(stream.get());
        CHECK(!read.isWhole && read.readError == EAGAIN && read.entries.empty());
        const bool isStillOpen = ::fcntl(borrowedDescriptor, F_GETFD) >= 0;
        CHECK(isStillOpen);
        if (!isStillOpen)
            (void)stream.release();
    }();

    // Failure to open an existing file is distinct from an absent store. An
    // append must not use that failure as permission to write a second header.
    if (::geteuid() != 0) {
        const std::string contents = header + alpha + "\n";
        overwrite(path, contents);
        CHECK(::chmod(path.c_str(), 0200) == 0);
        const ConsentList read = readConsentList(path);
        CHECK(!read.isWhole && read.readError == EACCES && read.entries.empty());
        CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::IOError);
        CHECK(::chmod(path.c_str(), 0600) == 0);
        CHECK(slurp(path) == contents);
    }

    // A FIFO must be refused without waiting for a writer or changing its type.
    [&] {
        REQUIRE(::unlink(path.c_str()) == 0);
        REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
        const ConsentList read = readConsentList(path);
        CHECK(!read.isWhole && read.readError && read.entries.empty());
        CHECK(addStandingConsent(path, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::IOError);
        struct stat information {};
        CHECK(::lstat(path.c_str(), &information) == 0 && S_ISFIFO(information.st_mode));
        CHECK(::unlink(path.c_str()) == 0);
    }();
    {
        const ConsentList read = readConsentList(std::string(directory));
        CHECK(!read.isWhole && read.readError && read.entries.empty());
        CHECK(addStandingConsent(directory, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(directory, alpha) == ConsentEdit::IOError);
    }
    [&] {
        const std::string contents = header + alpha + "\n";
        overwrite(path, contents);
        const std::string link = path + ".link";
        REQUIRE(::symlink(path.c_str(), link.c_str()) == 0);
        const ConsentList read = readConsentList(link);
        CHECK(!read.isWhole && read.readError && read.entries.empty());
        CHECK(addStandingConsent(link, beta) == ConsentEdit::IOError);
        CHECK(removeStandingConsent(link, alpha) == ConsentEdit::IOError);
        CHECK(slurp(path) == contents);
        struct stat information {};
        CHECK(::lstat(link.c_str(), &information) == 0 && S_ISLNK(information.st_mode));
        CHECK(::unlink(link.c_str()) == 0);
    }();

    // Contending intents fail before replacement; releasing the one shared
    // lock makes both operations available again.
    [&] {
        const std::string contents = header + alpha + "\n";
        overwrite(path, contents);
        std::unique_ptr<FILE, int (*)(FILE*)> original(std::fopen(path.c_str(), "r"), std::fclose);
        REQUIRE(original);
        struct stat before {};
        REQUIRE(::fstat(::fileno(original.get()), &before) == 0);
        {
            Ubiquitty::Host::TokenStoreLock lock(path);
            REQUIRE(lock.isLocked());
            for (const auto operation : { Ubiquitty::Host::StandingConsentOperation::Add, Ubiquitty::Host::StandingConsentOperation::Remove }) {
                const auto result = Ubiquitty::Host::editStandingConsentList(path,
                    operation == Ubiquitty::Host::StandingConsentOperation::Add ? beta : alpha, operation);
                CHECK(result.outcome == ConsentEdit::IOError && result.entries.empty() && result.withdrawnGrants.empty());
                CHECK(slurp(path) == contents);
                struct stat after {};
                CHECK(::stat(path.c_str(), &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino);
            }
        }
        CHECK(addStandingConsent(path, beta) == ConsentEdit::Changed);
        CHECK(removeStandingConsent(path, beta) == ConsentEdit::Changed);
        CHECK(slurp(path) == contents);
    }();

    // Only token entries consume the active-grant bound. Eviction names the
    // exact lost principal while preserving inert lines from the same file.
    {
        std::string contents = header + "\n/usr/bin/inert-agent\n";
        for (std::size_t index = 0; index < Ubiquitty::Host::maxStandingConsentGrants; ++index)
            contents += "token:grant-" + std::to_string(index) + "\n";
        overwrite(path, contents);
        const auto added = Ubiquitty::Host::editStandingConsentList(path, beta, Ubiquitty::Host::StandingConsentOperation::Add);
        CHECK(added.outcome == ConsentEdit::Changed);
        CHECK(added.entries.size() == Ubiquitty::Host::maxStandingConsentGrants + 2);
        CHECK(static_cast<std::size_t>(std::ranges::count_if(added.entries, [](const auto& entry) { return entry.starts_with("token:"); })) == Ubiquitty::Host::maxStandingConsentGrants);
        CHECK(added.entries.size() >= 2 && added.entries[0].empty() && added.entries[1] == "/usr/bin/inert-agent");
        CHECK(!added.entries.empty() && added.entries.back() == beta);
        CHECK((added.withdrawnGrants == std::vector<std::string> { "token:grant-0" }));
        const auto duplicate = Ubiquitty::Host::editStandingConsentList(path, beta, Ubiquitty::Host::StandingConsentOperation::Add);
        CHECK(duplicate.outcome == ConsentEdit::Unchanged && duplicate.entries == added.entries && duplicate.withdrawnGrants.empty());
        const auto removed = Ubiquitty::Host::editStandingConsentList(path, beta, Ubiquitty::Host::StandingConsentOperation::Remove);
        CHECK(removed.outcome == ConsentEdit::Changed);
        CHECK((removed.withdrawnGrants == std::vector<std::string> { beta }));
        CHECK(readConsentList(path).entries == removed.entries);
    }

    // A stale fixed temporary name is unrelated to the uniquely created
    // replacement and cannot redirect either mutation onto another file.
    [&] {
        overwrite(path, header + alpha + "\n");
        const std::string target = path + ".target";
        const std::string targetBytes = "unrelated file\n";
        overwrite(target, targetBytes);
        REQUIRE(::symlink(target.c_str(), temporary.c_str()) == 0);
        CHECK(addStandingConsent(path, beta) == ConsentEdit::Changed);
        CHECK(removeStandingConsent(path, alpha) == ConsentEdit::Changed);
        CHECK(slurp(target) == targetBytes);
        struct stat information {};
        CHECK(::lstat(temporary.c_str(), &information) == 0 && S_ISLNK(information.st_mode));
        CHECK(::unlink(temporary.c_str()) == 0);
        CHECK(::unlink(target.c_str()) == 0);
    }();

    ::unlink(path.c_str());
    ::unlink(temporary.c_str());
    ::unlink((path + ".lock").c_str());
    ::unlink((path + ".link.lock").c_str());
    ::unlink((std::string(directory) + ".lock").c_str());
    ::rmdir(directory);

    if (failures == 0)
        std::printf("consent_store: all passed\n");
    return failures ? 1 : 0;
}
