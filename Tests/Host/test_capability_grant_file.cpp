// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/test_capability_grant_file.cpp — the capability grant file that
// several daemon instances share.
//
// Every intent edits the file as it is now, under its lock, and no instance
// writes its cache back: a grant one instance withdrew stays withdrawn whatever
// another records afterwards. A cached Always grant authorizes only after its
// instance confirms the file is the one it last read whole, so a withdrawal
// ends it at that instance's next check. A file that cannot be read whole
// grants nothing, and an edit that could not commit changes nothing.
//
// Each instance here is what a daemon holds: a grant store and the file
// binding it, over one shared path.
//

#include "Harness.h"

#include "Platform/Host/CapabilityGrantFile.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

using Ubiquitty::Capability;
using Ubiquitty::CapabilityGrantKey;
using Ubiquitty::CapabilityGrantScope;
using Ubiquitty::CapabilityGrantStore;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::CapabilityGrantFile;
using Ubiquitty::Host::CapabilityGrantOperation;
using Ubiquitty::Host::ConsentEdit;
using Ubiquitty::Host::grantFileHeader;
using Ubiquitty::Host::maxAlwaysAllowedCapabilityGrants;
using Ubiquitty::Host::maxCapabilityGrantLineBytes;
using Ubiquitty::Host::parseCapabilityGrantLine;
using Ubiquitty::Host::readCapabilityGrantFile;
using Ubiquitty::Host::TokenStoreLock;
using Ubiquitty::Host::TokenStoreSync;

namespace {

/// A private directory, removed with everything in it.
struct ScratchDirectory {
    ScratchDirectory()
    {
        std::string pattern = "/tmp/ubiquitty-capability-grants-XXXXXX";
        if (::mkdtemp(pattern.data()))
            path = std::move(pattern);
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        if (!path.empty())
            std::filesystem::remove_all(path, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] std::string grants() const { return path + "/capability_grants"; }

    std::string path;
};

/// What one daemon instance holds: the file binding and the store it keeps.
struct Instance {
    explicit Instance(const std::string& path)
    {
        file.bind(path, store);
        (void)file.reload(store);
    }

    ConsentEdit edit(CapabilityGrantOperation operation, const CapabilityGrantKey& key, const TokenStoreSync* sync = nullptr)
    {
        return file.edit(store, operation, key, sync);
    }
    [[nodiscard]] bool isCached(const CapabilityGrantKey& key) const { return store.isGranted(key.capability, key.origin, key.identity); }
    [[nodiscard]] bool authorizes(const CapabilityGrantKey& key) { return store.checkAndConsumeIfGranted(key.capability, key.origin, key.identity); }

    CapabilityGrantFile file;
    CapabilityGrantStore store;
};

CapabilityGrantKey grantFor(Capability capability, CapabilityOrigin origin, char digest)
{
    return { capability, origin, "token:" + std::string(64, digest) };
}

// Three principals, each with one grant.
CapabilityGrantKey withdrawnGrant() { return grantFor(Capability::ClipboardWrite, CapabilityOrigin::Remote, 'a'); }
CapabilityGrantKey approvedGrant() { return grantFor(Capability::DesktopNotification, CapabilityOrigin::Remote, 'b'); }
CapabilityGrantKey unrelatedGrant() { return grantFor(Capability::SetHyperlink, CapabilityOrigin::Agent, 'c'); }

std::string grantLine(const CapabilityGrantKey& key)
{
    return std::to_string(std::to_underlying(key.capability)) + "\t" + std::to_string(std::to_underlying(key.origin)) + "\t" + key.identity + "\n";
}

std::string header() { return grantFileHeader("capability-grants"); }

std::string slurp(const std::string& path)
{
    std::string contents;
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!file)
        return contents;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file.get())) > 0)
        contents.append(buffer, got);
    return contents;
}

// Rewrites the file in place, as a hand edit would.
bool overwrite(const std::string& path, const std::string& contents)
{
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "wb"), std::fclose);
    if (!file)
        return false;
    const bool didWrite = std::fwrite(contents.data(), 1, contents.size(), file.get()) == contents.size();
    return std::fclose(file.release()) == 0 && didWrite;
}

std::optional<ino_t> inodeOf(const std::string& path)
{
    struct stat information {};
    if (::lstat(path.c_str(), &information) != 0)
        return std::nullopt;
    return information.st_ino;
}

// The writer's temporaries left in a directory.
std::vector<std::string> temporaries(const std::string& directory)
{
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        auto name = entry.path().filename().string();
        if (name.starts_with("capability_grants.tmp"))
            names.push_back(std::move(name));
    }
    return names;
}

} // namespace

TEST(anAbsentFileHoldsNothingAndTheFirstGrantCreatesItPrivate)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto absent = readCapabilityGrantFile(scratch.grants());
    CHECK(absent.isWhole && !absent.readError && !absent.hasUnreadableLayout && absent.grants.empty());

    Instance first(scratch.grants());
    const auto withdrawn = withdrawnGrant();
    CHECK(!first.isCached(withdrawn));
    CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
    CHECK(slurp(scratch.grants()) == header() + grantLine(withdrawn));
    struct stat information {};
    CHECK(::stat(scratch.grants().c_str(), &information) == 0 && (information.st_mode & 0777) == 0600);
    CHECK(first.isCached(withdrawn) && first.authorizes(withdrawn));

    CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Unchanged);
    CHECK(slurp(scratch.grants()) == header() + grantLine(withdrawn));
    CHECK(temporaries(scratch.path).empty());
}

// The defect this file exists to prevent: an instance whose cache predates
// another's withdrawal records an unrelated approval, and the withdrawn grant
// must not come back with it.
TEST(anOlderCacheNeverRestoresAGrantAnotherInstanceWithdrew)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto withdrawn = withdrawnGrant();
    const auto approved = approvedGrant();
    const auto unrelated = unrelatedGrant();
    const CapabilityGrantKey withdrawnSecond { Capability::SetWorkingDirectory, CapabilityOrigin::Agent, withdrawn.identity };

    Instance first(scratch.grants());
    CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
    CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawnSecond) == ConsentEdit::Changed);
    CHECK(first.edit(CapabilityGrantOperation::Grant, unrelated) == ConsentEdit::Changed);
    Instance second(scratch.grants());
    CHECK(first.isCached(withdrawn) && second.isCached(withdrawn));

    CHECK(second.edit(CapabilityGrantOperation::WithdrawIdentity, withdrawn) == ConsentEdit::Changed);
    CHECK(!second.isCached(withdrawn) && !second.isCached(withdrawnSecond) && second.isCached(unrelated));
    CHECK_MSG(first.isCached(withdrawn), "the first instance has not checked since, so its cache still lists the grant");

    CHECK(first.edit(CapabilityGrantOperation::Grant, approved) == ConsentEdit::Changed);
    CHECK(slurp(scratch.grants()) == header() + grantLine(unrelated) + grantLine(approved));
    CHECK(!first.isCached(withdrawn) && !first.isCached(withdrawnSecond));
    CHECK(first.isCached(approved) && first.isCached(unrelated));

    const auto reloaded = readCapabilityGrantFile(scratch.grants());
    CHECK(reloaded.isWhole && (reloaded.grants == std::vector<CapabilityGrantKey> { unrelated, approved }));
    Instance reader(scratch.grants());
    CHECK(!reader.isCached(withdrawn) && !reader.isCached(withdrawnSecond));
    CHECK(reader.isCached(approved) && reader.isCached(unrelated));
}

TEST(aWithdrawalEndsAnotherInstancesGrantAtItsNextCheck)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto withdrawn = withdrawnGrant();
    const auto unrelated = unrelatedGrant();
    const auto sessionScoped = approvedGrant();

    Instance first(scratch.grants());
    CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
    CHECK(first.edit(CapabilityGrantOperation::Grant, unrelated) == ConsentEdit::Changed);
    Instance second(scratch.grants());
    second.store.grant(sessionScoped.capability, sessionScoped.origin, sessionScoped.identity, CapabilityGrantScope::Session, 7);
    CHECK(second.authorizes(withdrawn) && second.authorizes(unrelated));

    CHECK(first.edit(CapabilityGrantOperation::Withdraw, withdrawn) == ConsentEdit::Changed);
    CHECK(second.isCached(withdrawn));
    CHECK_MSG(!second.authorizes(withdrawn), "a grant withdrawn elsewhere ends at the next check");
    CHECK(!second.isCached(withdrawn));
    CHECK(second.authorizes(unrelated));
    CHECK_MSG(second.authorizes(sessionScoped) && second.store.sessionGrantCount(7) == 1, "confirming Always grants keeps the session's own grants");

    // Grants another instance added are learnt when the file is next confirmed.
    const auto approved = grantFor(Capability::FileReadAccess, CapabilityOrigin::Remote, 'd');
    CHECK(first.edit(CapabilityGrantOperation::Grant, approved) == ConsentEdit::Changed);
    CHECK(!second.isCached(approved));
    second.store.confirmStandingGrants();
    CHECK(second.isCached(approved));

    // A cache that did not come from the file, such as one handed over, is
    // confirmed before it authorizes.
    const auto neverPersisted = grantFor(Capability::SetWorkingDirectory, CapabilityOrigin::Agent, 'e');
    const std::vector<CapabilityGrantKey> handedOver { withdrawn, neverPersisted, unrelated };
    second.store.adoptAlwaysGrants(handedOver);
    second.file.invalidate();
    CHECK(!second.authorizes(neverPersisted) && !second.authorizes(withdrawn));
    CHECK(second.authorizes(unrelated));
}

TEST(onlyAnAlwaysGrantIsConfirmedBeforeItAuthorizes)
{
    struct CountingSource {
        int confirmations = 0;
        std::vector<CapabilityGrantKey> persisted;
    } source;
    CapabilityGrantStore store;
    const auto confirmCounting = [](void* context, CapabilityGrantStore& confirmed) {
        auto& counting = *static_cast<CountingSource*>(context);
        ++counting.confirmations;
        confirmed.adoptAlwaysGrants(counting.persisted);
    };
    store.setStandingGrantSource({ .context = &source, .confirm = confirmCounting });
    const auto always = withdrawnGrant();
    const auto session = approvedGrant();
    const auto once = unrelatedGrant();
    store.grant(always.capability, always.origin, always.identity, CapabilityGrantScope::Always);
    store.grant(session.capability, session.origin, session.identity, CapabilityGrantScope::Session, 3);
    store.grant(once.capability, once.origin, once.identity, CapabilityGrantScope::OneShot);
    source.persisted = { always };

    CHECK(store.checkAndConsumeIfGranted(session.capability, session.origin, session.identity));
    CHECK(store.checkAndConsumeIfGranted(once.capability, once.origin, once.identity));
    CHECK(!store.checkAndConsumeIfGranted(once.capability, once.origin, once.identity));
    CHECK(!store.checkAndConsumeIfGranted(Capability::ClipboardRead, CapabilityOrigin::Remote, always.identity));
    CHECK_EQ(source.confirmations, 0);

    CHECK(store.checkAndConsumeIfGranted(always.capability, always.origin, always.identity));
    CHECK_EQ(source.confirmations, 1);
    source.persisted.clear();
    CHECK(!store.checkAndConsumeIfGranted(always.capability, always.origin, always.identity));
    CHECK_EQ(source.confirmations, 2);
    CHECK(store.checkAndConsumeIfGranted(session.capability, session.origin, session.identity));

    // The persisted set names a key that was a Session grant: it is Always now.
    source.persisted = { session };
    store.confirmStandingGrants();
    CHECK(store.sessionGrantCount(3) == 0 && store.isGranted(session.capability, session.origin, session.identity));
}

// Each malformed shape, refused whole: a confirmed grant stops authorizing at
// the next check, a fresh reader holds nothing, every edit leaves the bytes,
// and repairing the file restores the grant at the next confirmation.
TEST(aFileThatCannotBeReadWholeGrantsNothing)
{
    const auto withdrawn = withdrawnGrant();
    const auto unrelated = unrelatedGrant();
    const std::string valid = header() + grantLine(withdrawn);
    const auto beyondNone = std::to_string(std::to_underlying(Capability::None) + 1);
    const auto beyondRemote = std::to_string(std::to_underlying(CapabilityOrigin::Remote) + 1);
    std::string tooMany = header();
    for (std::size_t index = 0; index <= maxAlwaysAllowedCapabilityGrants; ++index)
        tooMany += grantLine({ Capability::SetHyperlink, CapabilityOrigin::Agent, "token:grant-" + std::to_string(index) });
    // Each differs from `valid` in size, so an edit in place is seen however
    // coarse the filesystem's timestamps.
    struct Shape {
        std::string contents;
        bool isLayout;
    };
    const std::vector<Shape> shapes {
        { grantLine(withdrawn), true },
        { "# ubiquitty capability-grants v2\n" + grantLine(withdrawn) + grantLine(unrelated), true },
        { grantFileHeader("consent") + grantLine(withdrawn), true },
        { valid + "not a grant\n", true },
        { valid + beyondNone + "\t0\t" + unrelated.identity + "\n", true },
        { valid + "0\t" + beyondRemote + "\t" + unrelated.identity + "\n", true },
        { valid + "+0\t0\t" + unrelated.identity + "\n", true },
        { valid + " 0\t0\t" + unrelated.identity + "\n", true },
        { valid + "0\t0\t/usr/bin/agent\n", true },
        { valid + "0\t0\ttoken:\n", true },
        { valid + "0\t0\t" + unrelated.identity + "\tmore\n", true },
        { valid + std::string("0\t0\ttoken:\0hidden\n", 18), true },
        { tooMany, false },
        { valid + "0\t0\ttoken:" + std::string(maxCapabilityGrantLineBytes, 'f') + "\n", false },
    };
    for (const auto& shape : shapes) {
        ScratchDirectory scratch;
        REQUIRE(!scratch.path.empty());
        REQUIRE(overwrite(scratch.grants(), valid));
        Instance first(scratch.grants());
        CHECK(first.authorizes(withdrawn));

        REQUIRE(overwrite(scratch.grants(), shape.contents));
        const auto read = readCapabilityGrantFile(scratch.grants());
        CHECK(!read.isWhole && read.grants.empty() && read.hasUnreadableLayout == shape.isLayout);
        CHECK_MSG(!first.authorizes(withdrawn), "a file that cannot be read whole grants nothing at the next check");
        Instance reader(scratch.grants());
        CHECK(!reader.isCached(withdrawn));
        const auto refusal = shape.isLayout ? ConsentEdit::UnreadableLayout : ConsentEdit::IOError;
        CHECK(reader.edit(CapabilityGrantOperation::Grant, unrelated) == refusal);
        CHECK(reader.edit(CapabilityGrantOperation::Withdraw, withdrawn) == refusal);
        CHECK(reader.edit(CapabilityGrantOperation::WithdrawIdentity, withdrawn) == refusal);
        CHECK(slurp(scratch.grants()) == shape.contents);
        CHECK(temporaries(scratch.path).empty());

        REQUIRE(overwrite(scratch.grants(), valid));
        first.store.confirmStandingGrants();
        CHECK(first.authorizes(withdrawn));
    }
}

TEST(aFileReachedThroughALinkOrNotRegularGrantsNothing)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto withdrawn = withdrawnGrant();
    const std::string target = scratch.path + "/elsewhere";
    const std::string valid = header() + grantLine(withdrawn);
    REQUIRE(overwrite(target, valid));
    REQUIRE(::symlink(target.c_str(), scratch.grants().c_str()) == 0);
    {
        const auto read = readCapabilityGrantFile(scratch.grants());
        CHECK(!read.isWhole && read.readError && read.grants.empty());
        Instance linked(scratch.grants());
        CHECK(!linked.isCached(withdrawn));
        CHECK(linked.edit(CapabilityGrantOperation::Grant, approvedGrant()) == ConsentEdit::IOError);
        CHECK(slurp(target) == valid);
        struct stat information {};
        CHECK(::lstat(scratch.grants().c_str(), &information) == 0 && S_ISLNK(information.st_mode));
    }
    REQUIRE(::unlink(scratch.grants().c_str()) == 0);
    REQUIRE(::mkfifo(scratch.grants().c_str(), 0600) == 0);
    {
        const auto read = readCapabilityGrantFile(scratch.grants());
        CHECK(!read.isWhole && read.readError && read.grants.empty());
        Instance fifo(scratch.grants());
        CHECK(fifo.edit(CapabilityGrantOperation::Withdraw, withdrawn) == ConsentEdit::IOError);
        struct stat information {};
        CHECK(::lstat(scratch.grants().c_str(), &information) == 0 && S_ISFIFO(information.st_mode));
    }
}

TEST(aContendedEditChangesNothingAndARetryCommits)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto withdrawn = withdrawnGrant();
    const auto approved = approvedGrant();
    const auto unrelated = unrelatedGrant();
    Instance first(scratch.grants());
    REQUIRE(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
    REQUIRE(first.edit(CapabilityGrantOperation::Grant, unrelated) == ConsentEdit::Changed);
    const auto contents = slurp(scratch.grants());
    const auto inode = inodeOf(scratch.grants());
    {
        TokenStoreLock held(scratch.grants());
        REQUIRE(held.isLocked());
        CHECK(first.edit(CapabilityGrantOperation::Grant, approved) == ConsentEdit::IOError);
        CHECK(first.edit(CapabilityGrantOperation::Withdraw, withdrawn) == ConsentEdit::IOError);
        CHECK(first.edit(CapabilityGrantOperation::WithdrawIdentity, withdrawn) == ConsentEdit::IOError);
        CHECK(slurp(scratch.grants()) == contents && inodeOf(scratch.grants()) == inode);
        CHECK(!first.isCached(approved));
        CHECK_MSG(first.isCached(withdrawn) && first.authorizes(withdrawn), "a withdrawal that did not commit leaves the grant standing");
    }
    CHECK(first.edit(CapabilityGrantOperation::WithdrawIdentity, withdrawn) == ConsentEdit::Changed);
    CHECK(!first.isCached(withdrawn) && first.isCached(unrelated));
    CHECK(slurp(scratch.grants()) == header() + grantLine(unrelated));
}

TEST(aFailedWriteLeavesTheFileAndNoTemporary)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto withdrawn = withdrawnGrant();
    const auto approved = approvedGrant();
    Instance first(scratch.grants());
    REQUIRE(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
    const auto contents = slurp(scratch.grants());
    const TokenStoreSync failing { .syncFile = [](int) { errno = EIO; return -1; }, .syncParent = [](const char*) { return 0; } };

    CHECK(first.edit(CapabilityGrantOperation::Withdraw, withdrawn, &failing) == ConsentEdit::IOError);
    CHECK(first.edit(CapabilityGrantOperation::Grant, approved, &failing) == ConsentEdit::IOError);
    CHECK(slurp(scratch.grants()) == contents);
    CHECK(temporaries(scratch.path).empty());
    CHECK_MSG(first.isCached(withdrawn) && first.authorizes(withdrawn), "a withdrawal that did not commit leaves the grant standing");
    CHECK(!first.isCached(approved));

    // A filesystem that cannot prove durability still takes the bytes.
    const TokenStoreSync unproven { .syncFile = [](int) { errno = EINVAL; return -1; }, .syncParent = [](const char*) { return 0; } };
    CHECK(first.edit(CapabilityGrantOperation::Grant, approved, &unproven) == ConsentEdit::Changed);
    CHECK(first.edit(CapabilityGrantOperation::Withdraw, withdrawn) == ConsentEdit::Changed);
    CHECK(slurp(scratch.grants()) == header() + grantLine(approved));
    CHECK(!first.isCached(withdrawn) && first.isCached(approved));
}

TEST(aGrantAtTheBoundEvictsTheOldest)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    std::vector<CapabilityGrantKey> held;
    std::string contents = header();
    for (std::size_t index = 0; index < maxAlwaysAllowedCapabilityGrants; ++index) {
        held.push_back({ Capability::SetHyperlink, CapabilityOrigin::Agent, "token:grant-" + std::to_string(index) });
        contents += grantLine(held.back());
    }
    REQUIRE(overwrite(scratch.grants(), contents));
    Instance first(scratch.grants());
    CHECK(first.isCached(held.front()) && first.isCached(held.back()));

    const auto approved = approvedGrant();
    CHECK(first.edit(CapabilityGrantOperation::Grant, approved) == ConsentEdit::Changed);
    const auto read = readCapabilityGrantFile(scratch.grants());
    REQUIRE(read.isWhole && read.grants.size() == maxAlwaysAllowedCapabilityGrants);
    CHECK(read.grants.front() == held[1] && read.grants.back() == approved);
    CHECK(!first.isCached(held.front()) && first.isCached(held[1]) && first.isCached(approved));
    CHECK(first.edit(CapabilityGrantOperation::Grant, approved) == ConsentEdit::Unchanged);
    CHECK(readCapabilityGrantFile(scratch.grants()).grants == read.grants);
}

TEST(linesAreReadAsTheWriterWritesThem)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const auto withdrawn = withdrawnGrant();
    const auto unrelated = unrelatedGrant();
    const std::vector<CapabilityGrantKey> both { withdrawn, unrelated };

    // An empty file holds nothing, and the first edit gives it the header.
    REQUIRE(overwrite(scratch.grants(), ""));
    const auto empty = readCapabilityGrantFile(scratch.grants());
    CHECK(empty.isWhole && empty.grants.empty());
    {
        Instance first(scratch.grants());
        CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
        CHECK(slurp(scratch.grants()) == header() + grantLine(withdrawn));
    }

    auto withoutLineEnd = grantLine(unrelated);
    withoutLineEnd.pop_back();
    auto crlf = [](std::string line) {
        line.insert(line.size() - 1, "\r");
        return line;
    };
    for (const auto& contents : { header() + grantLine(withdrawn) + withoutLineEnd, crlf(header()) + crlf(grantLine(withdrawn)) + crlf(grantLine(unrelated)),
             header() + "\n" + grantLine(withdrawn) + "\n" + grantLine(unrelated), header() + grantLine(withdrawn) + grantLine(withdrawn) + grantLine(unrelated) }) {
        REQUIRE(overwrite(scratch.grants(), contents));
        const auto read = readCapabilityGrantFile(scratch.grants());
        CHECK(read.isWhole && read.grants == both);
    }

    // A chunk boundary inside a line does not split it.
    const CapabilityGrantKey longIdentity { Capability::SetHyperlink, CapabilityOrigin::Agent, "token:" + std::string(3000, 'f') };
    REQUIRE(overwrite(scratch.grants(), header() + grantLine(longIdentity) + grantLine(withdrawn)));
    const auto spanning = readCapabilityGrantFile(scratch.grants());
    CHECK(spanning.isWhole && (spanning.grants == std::vector<CapabilityGrantKey> { longIdentity, withdrawn }));
}

TEST(theWriterRefusesWhatTheReaderWould)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    Instance first(scratch.grants());
    const auto withdrawn = withdrawnGrant();
    REQUIRE(first.edit(CapabilityGrantOperation::Grant, withdrawn) == ConsentEdit::Changed);
    const auto contents = slurp(scratch.grants());
    const auto beyondNone = static_cast<Capability>(std::to_underlying(Capability::None) + 1);
    const auto beyondRemote = static_cast<CapabilityOrigin>(std::to_underlying(CapabilityOrigin::Remote) + 1);
    const std::vector<CapabilityGrantKey> refused {
        { Capability::SetHyperlink, CapabilityOrigin::Agent, "/usr/bin/agent" },
        { Capability::SetHyperlink, CapabilityOrigin::Agent, "token:" },
        { Capability::SetHyperlink, CapabilityOrigin::Agent, "token:a\tb" },
        { Capability::SetHyperlink, CapabilityOrigin::Agent, "token:a\nb" },
        { Capability::SetHyperlink, CapabilityOrigin::Agent, "token:a\r" },
        { Capability::SetHyperlink, CapabilityOrigin::Agent, std::string("token:a\0b", 9) },
        { Capability::SetHyperlink, CapabilityOrigin::Agent, "token:" + std::string(maxCapabilityGrantLineBytes, 'f') },
        { beyondNone, CapabilityOrigin::Agent, "token:a" },
        { Capability::SetHyperlink, beyondRemote, "token:a" },
    };
    for (const auto& key : refused) {
        CHECK(first.edit(CapabilityGrantOperation::Grant, key) == ConsentEdit::IOError);
        CHECK(!first.isCached(key));
    }
    CHECK(slurp(scratch.grants()) == contents);
    CHECK(first.edit(CapabilityGrantOperation::WithdrawIdentity, { Capability::None, CapabilityOrigin::Local, "" }) == ConsentEdit::IOError);
    CHECK(first.isCached(withdrawn));

    CHECK(parseCapabilityGrantLine(grantLine(withdrawn).substr(0, grantLine(withdrawn).size() - 1)) == withdrawn);
    for (const std::string_view line : { "1\t1", "1\t\ttoken:a", "\t1\ttoken:a", "1 \t1\ttoken:a", "0x1\t1\ttoken:a", "-1\t1\ttoken:a" })
        CHECK(!parseCapabilityGrantLine(line));
}

// A stale temporary at a fixed, predictable name cannot redirect the replacement.
TEST(aStaleFixedTemporaryCannotRedirectAnEdit)
{
    ScratchDirectory scratch;
    REQUIRE(!scratch.path.empty());
    const std::string target = scratch.path + "/unrelated";
    const std::string targetBytes = "unrelated file\n";
    REQUIRE(overwrite(target, targetBytes));
    const std::string fixed = scratch.grants() + ".tmp";
    REQUIRE(::symlink(target.c_str(), fixed.c_str()) == 0);
    Instance first(scratch.grants());
    CHECK(first.edit(CapabilityGrantOperation::Grant, withdrawnGrant()) == ConsentEdit::Changed);
    CHECK(first.edit(CapabilityGrantOperation::Withdraw, withdrawnGrant()) == ConsentEdit::Changed);
    CHECK(slurp(target) == targetBytes);
    struct stat information {};
    CHECK(::lstat(fixed.c_str(), &information) == 0 && S_ISLNK(information.st_mode));
    CHECK(slurp(scratch.grants()) == header());
}

UBIQUITTY_TEST_MAIN()
