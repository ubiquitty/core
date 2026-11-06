// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Host/ScreenPlaneAccess.h"

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

struct Fixture {
    Daemon daemon;
    Ubiquitty::TestDoubles::StreamDouble* stream = nullptr;
    std::remove_reference_t<decltype(Screen::makeSession(std::declval<Daemon&>(), 24, 80))>* session = nullptr;
    UbiquittyTest::ScreenClient first { 24, 80, { 1, 2 } };
    UbiquittyTest::ScreenClient second { 24, 80, { 3, 4 } };
    std::size_t firstIndex = 0;
    std::size_t secondIndex = 0;

    Fixture()
    {
        auto owned = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        stream = owned.get();
        session = &Screen::makeSession(daemon, 24, 80, std::move(owned));
        Screen::publish(daemon, *session, Ubiquitty::Runtime::FrameClass::Structural);
        firstIndex = attach(first, "first");
        secondIndex = attach(second, "second");
    }

    std::size_t attach(UbiquittyTest::ScreenClient& client, const char* token)
    {
        const auto index = Screen::attach(daemon, session->id);
        Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(daemon, index, token);
        Screen::markInteractiveUser(daemon, index);
        Screen::greet(daemon, index, client);
        return index;
    }

    void send(UbiquittyTest::ScreenClient& client, std::size_t index, std::string_view text)
    {
        CHECK(client.sendRaw({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }).has_value());
        Screen::deliver(daemon, index, client);
    }

    void drain()
    {
        stream->allowWrites();
        (void)stream->drainPendingWrite();
        session->screenSource->drainInput(Screen::now());
        (void)stream->drainPendingWrite();
    }
};

} // namespace

TEST(revocationRemovesQueuedInputWithoutDroppingAnotherAttachment)
{
    Fixture f;
    f.stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "revoked");
    f.send(f.second, f.secondIndex, "neighbor");
    REQUIRE(f.session->screenSource->hasPendingInput());
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "neighbor");
    CHECK(!f.session->screenSource->hasPendingInput());
}

TEST(revocationRemovesOnlyTheUndeliveredPTYTail)
{
    Fixture f;
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(2, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "abcdef");
    REQUIRE(f.stream->written == "ab");
    REQUIRE(f.stream->pendingWriteBytes() == 4);
    f.send(f.second, f.secondIndex, "neighbor");
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "abneighbor");
}

TEST(ordinaryTransportLossDoesNotWithdrawAdmittedInput)
{
    Fixture f;
    f.stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "retained");
    REQUIRE(f.session->screenSource->hasPendingInput());
    Screen::drop(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "retained");
}

TEST(withdrawalRepairsEveryPartiallyDeliveredPasteDelimiter)
{
    const std::string start = "\x1b[200~";
    const std::string text = "abcdef";
    const std::string end = "\x1b[201~";
    for (std::size_t accepted = 0; accepted <= start.size() + text.size() + end.size(); ++accepted) {
        Fixture f;
        Ubiquitty::processBytes(f.session->term.get(), "\x1b[?2004h");
        f.stream->queueWriteAll();
        f.stream->acceptBytesThenRefuse(accepted, Ubiquitty::PAL::StreamStatus::Again);
        REQUIRE(f.first.sendPaste(text).has_value());
        Screen::deliver(f.daemon, f.firstIndex, f.first);
        REQUIRE(f.stream->written == (start + text + end).substr(0, accepted));
        f.send(f.second, f.secondIndex, "neighbor");
        Screen::kick(f.daemon, f.firstIndex);
        f.drain();
        const std::size_t payload = accepted > start.size() ? std::min(text.size(), accepted - start.size()) : 0;
        const std::string expected = accepted ? start + text.substr(0, payload) + end : "";
        CHECK_STR_EQ(f.stream->written, expected + "neighbor");
    }
}

TEST(expiryAtTheSinkRefusesQueuedInputBeforeTheNextSweep)
{
    Fixture f;
    f.stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "expired");
    f.send(f.second, f.secondIndex, "neighbor");
    Screen::setTokenExpiry(f.daemon, f.firstIndex, 1);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "neighbor");
}

TEST(terminalRepliesSurviveOwnedTailWithdrawal)
{
    Fixture f;
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(2, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "abcdef");
    REQUIRE(f.stream->writeAll(std::span<const char>("reply", 5)) == 0);
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "abreply");
}

TEST(pasteRepairPrecedesRepliesQueuedBehindItsWithdrawnTail)
{
    Fixture f;
    Ubiquitty::processBytes(f.session->term.get(), "\x1b[?2004h");
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(3, Ubiquitty::PAL::StreamStatus::Again);
    REQUIRE(f.first.sendPaste("secret").has_value());
    Screen::deliver(f.daemon, f.firstIndex, f.first);
    REQUIRE(f.stream->writeAll(std::span<const char>("reply", 5)) == 0);
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "\x1b[200~\x1b[201~reply");
}

TEST(sharedReplayIdentityDoesNotShareWithdrawalAuthority)
{
    Fixture f;
    UbiquittyTest::ScreenClient alias { 24, 80, { 1, 2 } };
    const auto aliasIndex = f.attach(alias, "other-principal");
    f.stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "revoked");
    // Number one is already held under the shared replay identity. The next
    // number still belongs to a different server-issued attachment.
    f.send(alias, aliasIndex, "duplicate");
    f.send(alias, aliasIndex, "neighbor");
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "neighbor");
    CHECK(f.session->screenSource->inputLedger().discards({ 1, 2 }).empty());
}

TEST(expiryDuringPollCancelsTheOwnedPTYTailBeforeDrain)
{
    Fixture f;
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(2, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "abcdef");
    f.send(f.second, f.secondIndex, "neighbor");
    Screen::setTokenExpiry(f.daemon, f.firstIndex, 1);
    f.stream->allowWrites();
    Screen::drainReadyInput(f.daemon, *f.session);
    CHECK_STR_EQ(f.stream->written, "abneighbor");
}

TEST(retainedLostExpiryCancelsQueuedInputAndRefusesResume)
{
    Fixture f;
    const auto* attachment = Screen::connection(f.daemon, f.firstIndex)->attachment();
    REQUIRE(attachment);
    const auto identity = attachment->attachmentID;
    const auto authority = attachment->authority;
    const Sync::ResumeRequest resume { .attachmentID = identity, .resumeSecret = attachment->resumeSecret, .panes = {} };
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(2, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "abcdef");
    Screen::drop(f.daemon, f.firstIndex);
    Screen::expireRetainedToken(f.daemon, identity);
    f.stream->allowWrites();
    Screen::drainReadyInput(f.daemon, *f.session);
    CHECK_STR_EQ(f.stream->written, "ab");
    const auto resumed = Screen::attachments(f.daemon).resume(resume, authority, 999, Screen::now());
    CHECK(!resumed && resumed.error() == Sync::ResumeRefusal::UnknownAttachment);
}

TEST(kickingASupersededConnectionDoesNotCancelTheResumedAttachment)
{
    Fixture f;
    f.stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
    f.send(f.first, f.firstIndex, "retained");
    REQUIRE(Screen::resumeAttachmentElsewhere(f.daemon, f.firstIndex));
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "retained");
}

TEST(signalBypassBytesDoNotChangePastePrefixAccounting)
{
    Fixture f;
    Ubiquitty::processBytes(f.session->term.get(), "\x1b[?2004h");
    f.stream->facts.discipline = Ubiquitty::PAL::LineDiscipline { .isEchoEnabled = false, .isCanonical = false, .isSignalEnabled = true, .isInputDiscardedOnSignal = false, .interruptCharacter = 3, .quitCharacter = 28, .suspendCharacter = 26 };
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(3, Ubiquitty::PAL::StreamStatus::Again);
    REQUIRE(f.first.sendPaste("secret").has_value());
    Screen::deliver(f.daemon, f.firstIndex, f.first);
    f.send(f.first, f.firstIndex, std::string_view("\x03x", 2));
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "\x1b[200~\x1b[201~");
}

TEST(retiringAPartialPasteDoesNotInventAWholeEventReceipt)
{
    Fixture f;
    Ubiquitty::processBytes(f.session->term.get(), "\x1b[?2004h");
    f.stream->queueWriteAll();
    f.stream->acceptBytesThenRefuse(3, Ubiquitty::PAL::StreamStatus::Again);
    REQUIRE(f.first.sendPaste(std::string(20000, 'x')).has_value());
    Screen::deliver(f.daemon, f.firstIndex, f.first);
    REQUIRE(f.session->screenSource->hasPendingInput());
    REQUIRE(f.session->screenSource->inputLedger().lastWritten({ 1, 2 }).value_or(0) == 0);
    Screen::kick(f.daemon, f.firstIndex);
    f.drain();
    CHECK_STR_EQ(f.stream->written, "\x1b[200~\x1b[201~");
    CHECK(f.session->screenSource->inputLedger().lastWritten({ 1, 2 }).value_or(0) == 0);
    CHECK(f.session->screenSource->inputLedger().discards({ 1, 2 }).empty());
}

UBIQUITTY_TEST_MAIN()
