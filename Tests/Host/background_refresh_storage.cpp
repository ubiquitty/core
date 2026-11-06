// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Session/BackgroundEvents.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/BoundedFile.h"
#include "PAL/PAL.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <cerrno>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace JSON = Ubiquitty::Codec::JSON;
namespace PAL = Ubiquitty::PAL;
namespace Session = Ubiquitty::Session;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

struct PassiveCaller {
    Daemon& daemon;
    std::size_t index;
    UbiquittyTest::ScreenClient client { 1, 1 };
    int nextID = 1;

    explicit PassiveCaller(Daemon& host)
        : daemon(host)
        , index(Screen::attachControlOnly(host))
    {
        Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Control });
        Screen::setTokenID(daemon, index, "background-storage-reader");
        Screen::markInteractiveUser(daemon, index);
        Screen::greet(daemon, index, client);
    }

    Value refresh(Value parameters = Value::object())
    {
        Value request = Value::object();
        request.set("jsonrpc", Value::string("2.0"));
        request.set("id", Value::number(nextID++));
        request.set("method", Value::string("host.refresh"));
        request.set("params", std::move(parameters));
        CHECK(client.call(JSON::serialize(request)));
        Screen::deliver(daemon, index, client);
        Screen::deliver(daemon, index, client);
        const auto answers = client.takeResults();
        CHECK_EQ(answers.size(), 1u);
        if (answers.size() != 1)
            return {};
        bool isParsed = false;
        Value answer = JSON::parse(answers.front(), isParsed);
        CHECK(isParsed);
        CHECK_EQ(answer.numberOr("id"), nextID - 1);
        if (const Value* result = answer.find("result")) {
            const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("host.refresh");
            CHECK(method);
            if (method)
                CHECK(!Ubiquitty::Protocol::Control::validate(*result, Ubiquitty::Protocol::Control::parseRegistryJSON(method->resultSchema)));
        }
        return answer;
    }

    void checkPassive() const
    {
        const auto* connection = Screen::connection(daemon, index);
        REQUIRE(connection);
        CHECK(connection->state() == Sync::ConnectionState::Attached);
        CHECK(connection->grantedRole() == Sync::Role::Viewer);
        CHECK(connection->boundPanes().empty());
    }
};

std::string emptyJournal()
{
    return JSON::serialize(Session::BackgroundEvents { 41 }.persistentState());
}

void checkRefused(const Value& answer)
{
    CHECK(!answer.find("result"));
    const Value* error = answer.find("error");
    REQUIRE(error);
    CHECK_EQ(error->numberOr("code"), -32603);
    CHECK_STR_EQ(error->stringOr("message"), "the host could not persist its background inbox");
}

void checkBytes(const std::string& path, std::string_view expected)
{
    const auto bytes = PAL::storageReadBounded(path.c_str(), expected.size());
    REQUIRE(bytes);
    CHECK(std::ranges::equal(std::as_bytes(bytes->view()), std::as_bytes(std::span(expected))));
}

void checkUnchanged(const std::string& path, const struct stat& original)
{
    struct stat retained {};
    REQUIRE(::lstat(path.c_str(), &retained) == 0);
    CHECK(retained.st_dev == original.st_dev);
    CHECK(retained.st_ino == original.st_ino);
    CHECK(retained.st_mode == original.st_mode);
    CHECK(retained.st_size == original.st_size);
    CHECK(retained.st_uid == original.st_uid);
}

void exerciseRefusedJournal(const std::string& path)
{
    Daemon daemon;
    Screen::loadBackgroundEventFile(daemon, path);
    auto& session = Screen::makeSession(daemon, 1, 1);
    session.effects.push(Ubiquitty::Host::HostNotification { "New", "Must not replace refused storage", {} });
    PassiveCaller caller(daemon);
    checkRefused(caller.refresh());
    checkRefused(caller.refresh());
    caller.checkPassive();
}

void checkMalformedJournal(std::string_view contents)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string path = directory.path() + "/events";
    REQUIRE(PAL::storageWriteAtomic(path.c_str(), std::as_bytes(std::span(contents))));
    struct stat original {};
    REQUIRE(::lstat(path.c_str(), &original) == 0);
    exerciseRefusedJournal(path);
    checkUnchanged(path, original);
    checkBytes(path, contents);
}

void checkRetainedEvent(const Value& answer)
{
    const Value* result = answer.find("result");
    REQUIRE(result);
    CHECK(!answer.find("error"));
    const auto epoch = Session::backgroundSequence(result->stringOr("epoch"));
    CHECK(epoch && *epoch);
    CHECK_STR_EQ(result->stringOr("cursor"), "1");
    const Value* events = result->find("events");
    REQUIRE(events && events->isArray() && events->items().size() == 1);
    const Value& event = events->items().front();
    CHECK_STR_EQ(event.stringOr("sequence"), "1");
    CHECK_STR_EQ(event.stringOr("title"), "Retained");
    CHECK_STR_EQ(event.stringOr("body"), "After refusal");
    CHECK_STR_EQ(event.stringOr("paneID"), Session::backgroundPaneID(1, 1));
    CHECK_EQ(event.numberOr("sessionID"), 1);
}

void checkStoredReplay(const std::string& path, const Value& accepted)
{
    const Value* result = accepted.find("result");
    REQUIRE(result);
    struct stat status {};
    REQUIRE(::lstat(path.c_str(), &status) == 0);
    CHECK(S_ISREG(status.st_mode));
    CHECK((status.st_mode & 0777) == 0600);
    CHECK(status.st_uid == ::geteuid());
    const auto bytes = PAL::storageReadBounded(path.c_str(), Session::BackgroundEvents::maxFileBytes);
    REQUIRE(bytes);
    bool isParsed = false;
    const Value stored = JSON::parse(std::string(bytes->view().begin(), bytes->view().end()), isParsed);
    REQUIRE(isParsed);
    Session::BackgroundEvents decoded { 99 };
    REQUIRE(decoded.restore(stored));
    CHECK_STR_EQ(stored.stringOr("epoch"), result->stringOr("epoch"));
    CHECK_STR_EQ(stored.stringOr("latest"), result->stringOr("cursor"));

    Daemon restarted;
    Screen::loadBackgroundEventFile(restarted, path);
    PassiveCaller reader(restarted);
    const Value replay = reader.refresh();
    checkRetainedEvent(replay);
    const Value* replayed = replay.find("result");
    REQUIRE(replayed && replayed->find("events") && result->find("events"));
    CHECK_STR_EQ(replayed->stringOr("epoch"), result->stringOr("epoch"));
    CHECK_STR_EQ(JSON::serialize(*replayed->find("events")), JSON::serialize(*result->find("events")));
    Value parameters = Value::object();
    parameters.set("epoch", Value::string(result->stringOr("epoch")));
    parameters.set("after", Value::string(result->stringOr("cursor")));
    const Value caughtUp = reader.refresh(std::move(parameters));
    const Value* settled = caughtUp.find("result");
    REQUIRE(settled && settled->find("events"));
    CHECK(settled->find("events")->items().empty());
    CHECK_STR_EQ(settled->stringOr("cursor"), "1");
    reader.checkPassive();
}

} // namespace

TEST(malformedJournalIsPreservedAndRefreshStaysUnavailable)
{
    checkMalformedJournal("not a journal");
}

TEST(inconsistentJournalIsPreservedAndRefreshStaysUnavailable)
{
    checkMalformedJournal(R"({"version":1,"epoch":"41","latest":"1","droppedThrough":"0","events":[]})");
}

TEST(readableJournalPermissionsAreRefusedEvenWithValidContents)
{
    for (const mode_t mode : { 0640, 0604 }) {
        Ubiquitty::Test::TempDir directory;
        REQUIRE(directory.valid());
        const std::string path = directory.path() + "/events";
        const std::string contents = emptyJournal();
        REQUIRE(PAL::storageWriteAtomic(path.c_str(), std::as_bytes(std::span(contents))));
        REQUIRE(::chmod(path.c_str(), mode) == 0);
        struct stat original {};
        REQUIRE(::lstat(path.c_str(), &original) == 0);
        exerciseRefusedJournal(path);
        checkUnchanged(path, original);
        checkBytes(path, contents);
    }
}

TEST(symlinkJournalIsRefusedWithoutReplacingItOrItsValidTarget)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string target = directory.path() + "/target";
    const std::string link = directory.path() + "/events";
    const std::string contents = emptyJournal();
    REQUIRE(PAL::storageWriteAtomic(target.c_str(), std::as_bytes(std::span(contents))));
    REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);
    struct stat originalLink {}, originalTarget {};
    REQUIRE(::lstat(link.c_str(), &originalLink) == 0);
    REQUIRE(::lstat(target.c_str(), &originalTarget) == 0);
    exerciseRefusedJournal(link);
    checkUnchanged(link, originalLink);
    checkUnchanged(target, originalTarget);
    checkBytes(target, contents);
}

TEST(directoryIsNotReplacedWithARegularJournal)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string path = directory.path() + "/events";
    REQUIRE(::mkdir(path.c_str(), 0700) == 0);
    struct stat original {};
    REQUIRE(::lstat(path.c_str(), &original) == 0);
    exerciseRefusedJournal(path);
    checkUnchanged(path, original);
}

TEST(fifoJournalIsRefusedWithoutWaitingForAWriter)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string path = directory.path() + "/events";
    REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
    struct stat original {};
    REQUIRE(::lstat(path.c_str(), &original) == 0);
    std::fflush(nullptr);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (!child) {
        // The alarm bounds a blocking open regression, not normal read speed.
        // No writer is created: an unavailable journal must not await a peer.
        ::alarm(10);
        const int previousFailures = failures;
        exerciseRefusedJournal(path);
        ::alarm(0);
        std::fflush(nullptr);
        ::_exit(failures == previousFailures ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == child);
    REQUIRE_MSG(WIFEXITED(status), "journal startup blocked or terminated while refusing a FIFO");
    CHECK_EQ(WEXITSTATUS(status), 0);
    checkUnchanged(path, original);
}

TEST(oversizedOtherwiseValidJournalIsRefusedAndPreserved)
{
    std::string contents = emptyJournal();
    contents.resize(Session::BackgroundEvents::maxFileBytes + 1, ' ');
    checkMalformedJournal(contents);
}

TEST(missingParentRefusesTheCursorUntilDurableRetry)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string parent = directory.path() + "/missing";
    const std::string path = parent + "/events";
    Daemon daemon;
    Screen::loadBackgroundEventFile(daemon, path);
    auto& session = Screen::makeSession(daemon, 1, 1);
    session.effects.push(Ubiquitty::Host::HostNotification { "Retained", "After refusal", {} });
    PassiveCaller caller(daemon);
    checkRefused(caller.refresh());
    checkRefused(caller.refresh());
    struct stat absent {};
    CHECK(::lstat(parent.c_str(), &absent) == -1 && errno == ENOENT);
    REQUIRE(::mkdir(parent.c_str(), 0700) == 0);
    const Value accepted = caller.refresh();
    checkRetainedEvent(accepted);
    checkStoredReplay(path, accepted);
    caller.checkPassive();
}

TEST(writableParentRefusesReplacementWithoutLosingTheQueuedEvent)
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string path = directory.path() + "/events";
    const std::string contents = emptyJournal();
    REQUIRE(PAL::storageWriteAtomic(path.c_str(), std::as_bytes(std::span(contents))));
    struct stat original {};
    REQUIRE(::lstat(path.c_str(), &original) == 0);
    Daemon daemon;
    Screen::loadBackgroundEventFile(daemon, path);
    auto& session = Screen::makeSession(daemon, 1, 1);
    session.effects.push(Ubiquitty::Host::HostNotification { "Retained", "After refusal", {} });
    PassiveCaller caller(daemon);
    REQUIRE(::chmod(directory.path().c_str(), 0770) == 0);
    checkRefused(caller.refresh());
    checkUnchanged(path, original);
    checkBytes(path, contents);
    REQUIRE(::chmod(directory.path().c_str(), 0700) == 0);
    const Value accepted = caller.refresh();
    checkRetainedEvent(accepted);
    const Value* result = accepted.find("result");
    REQUIRE(result);
    CHECK_STR_EQ(result->stringOr("epoch"), "41");
    checkStoredReplay(path, accepted);
    caller.checkPassive();
}

UBIQUITTY_TEST_MAIN()
