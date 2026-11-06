// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_window_request_queue.cpp — the session's own WindowClient, for a
// frontend that installs none.
//
// `WindowClient` is called by the parser, inline, on the ingest thread, with
// the model lock held. The classic macOS frontend installs one that hops each
// call to its main queue; a Swift frontend cannot install one at all, because
// cxx-interop does not let a Swift type subclass a C++ class with virtual
// methods. So the session queues the calls and reports them the way it reports
// an OSC 50 font swap, and the frontend drains on the event.
//
// What these pin is the contract a frontend reads the queue under: the order it
// arrives in, the bound that keeps a runaway program from growing memory, the
// event that says to look, and the refusal that keeps this from displacing a
// frontend which installed its own client.

#include "Core/Runtime/TerminalEvents.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Terminal.h"
#include "PAL/TerminalClients.h"

#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Runtime::EventKind;
using Session = Ubiquitty::Runtime::TerminalSession;
using Request = Session::WindowRequest;
using Ubiquitty::OSC::FontAdoptionOutcome;

namespace {

/// Feeds a string to the session as though a program had written it.
///
/// - Parameters:
///   - session: The session to feed.
///   - bytes: What the program wrote.
void feed(Session* session, const std::string& bytes)
{
    session->feedBytes(Ubiquitty::Runtime::ByteSpan(bytes.data(), bytes.size()));
}

/// Everything the queue holds, drained.
///
/// - Parameter session: The session to drain.
/// - Returns: The requests, oldest first.
std::vector<Request> drain(Session* session)
{
    std::vector<Request> taken;
    for (;;) {
        Request request = session->takeWindowRequest();
        if (request.kind == Request::Kind::None)
            break;
        taken.push_back(std::move(request));
    }
    return taken;
}

/// A client that records, standing in for a frontend that installs its own.
struct RecordingWindowClient final : Ubiquitty::WindowClient {
    int calls = 0;
    void performWindowOperation(int, int, int) override { ++calls; }
};

class RefusingFontPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    explicit RefusingFontPolicy(Ubiquitty::CapabilityDecision decision)
        : m_decision(decision)
    {
    }

    Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        if (capability == Ubiquitty::Capability::SetFont) {
            ++m_fontChecks;
            return m_decision;
        }
        return Ubiquitty::CapabilityDecision::Allow;
    }

    int fontChecks() const { return m_fontChecks; }

private:
    Ubiquitty::CapabilityDecision m_decision;
    int m_fontChecks { 0 };
};

} // namespace

/// A program's window operations arrive in the order it wrote them.
///
/// Order is the whole reason this is a queue: iconify-then-move and
/// move-then-iconify leave the window in different places, and a single
/// pending slot would keep only the second of any pair.
TEST(theWindowQueueKeepsWhatAProgramWroteInOrder)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->installWindowRequestQueue());

    // Iconify, then raise, then lower.
    feed(session, "\x1b[2t\x1b[5t\x1b[6t");

    const std::vector<Request> taken = drain(session);
    // Counted before it is indexed: a queue that kept one of the three would
    // otherwise trap on the subscript, and a case that crashes says only that
    // it crashed.
    CHECK(taken.size() == 3);
    if (taken.size() == 3) {
        CHECK(taken[0].kind == Request::Kind::WindowOp);
        CHECK(taken[0].first == 2);
        CHECK(taken[1].first == 5);
        CHECK(taken[2].first == 6);
    }

    // Drained is drained: a frontend that asks again gets the empty answer
    // rather than the last request a second time.
    CHECK(session->takeWindowRequest().kind == Request::Kind::None);
    CHECK(!session->hasPendingWindowRequest());

    releaseTerminalSession(session);
}

/// The drain raises the event, and only when something arrived.
///
/// A frontend reads its facts when it is told something changed, and a window
/// operation changes no cell — so without the bit the request sits in the queue
/// until the program happens to print, which for a program that iconifies and
/// waits is never.
TEST(theWindowQueueRaisesItsEventOncePerArrival)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->installWindowRequestQueue());

    const auto bit = 1u << static_cast<unsigned>(EventKind::WindowRequestPending);

    // Nothing asked for anything yet.
    feed(session, "hello");
    CHECK((session->takeEventFlags() & bit) == 0);

    feed(session, "\x1b[2t");
    CHECK((session->takeEventFlags() & bit) != 0);

    // Taken once. A second read without a second request must not say a
    // request is waiting, or a frontend loops on an empty queue every drain.
    feed(session, "hello");
    CHECK((session->takeEventFlags() & bit) == 0);

    CHECK(drain(session).size() == 1);
    releaseTerminalSession(session);
}

/// A program cannot grow the queue without bound.
///
/// The far side drains on a run loop and promises nothing about when. A program
/// can write these as fast as it can write bytes, so the queue has a ceiling
/// and the overrun is dropped — a window operation nobody performed beats one
/// performed out of order, and the program learns nothing either way.
TEST(theWindowQueueRefusesToGrowPastItsBound)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->installWindowRequestQueue());

    std::string flood;
    for (int index = 0; index < 64; ++index)
        flood += "\x1b[5t";
    feed(session, flood);

    const std::vector<Request> taken = drain(session);
    CHECK(taken.size() == Session::windowRequestQueueLimit);
    // The ones kept are the FIRST ones: dropping the oldest would reorder what
    // survives against what the program wrote.
    for (const Request& request : taken)
        CHECK(request.first == 5);

    releaseTerminalSession(session);
}

/// The queue never displaces a frontend that installed its own client.
///
/// A frontend can install one and answer the parser through it. A
/// session that replaced it would leave that window silently unable to respond
/// to CSI t, which is a worse failure than the one this exists to fix: it looks
/// like the terminal ignoring the program.
TEST(theWindowQueueYieldsToAFrontendThatInstalledItsOwnClient)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);

    RecordingWindowClient client;
    Ubiquitty::Terminal* term = session->terminal();
    CHECK(term != nullptr);
    term->windowClient = &client;
    CHECK(!session->installWindowRequestQueue());

    feed(session, "\x1b[5t");
    // The frontend's client got it, and the queue stayed empty.
    CHECK(client.calls == 1);
    CHECK(!session->hasPendingWindowRequest());

    releaseTerminalSession(session);
}

/// The verbs that are not window operations arrive on the same queue.
///
/// One queue for the whole client, because a frontend answers them all from one
/// place — a window. Five queues would be five drains and five chances to
/// answer four of them.
TEST(theWindowQueueCarriesEveryVerbTheClientHas)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->installWindowRequestQueue());

    // DECCOLM to 132 columns, then OSC 1337 StealFocus.
    feed(session, "\x1b[?40h\x1b[?3h");
    feed(session, "\x1b]1337;StealFocus\x07");

    const std::vector<Request> taken = drain(session);
    bool sawColumnMode = false;
    bool sawStealFocus = false;
    for (const Request& request : taken) {
        if (request.kind == Request::Kind::ColumnMode) {
            sawColumnMode = true;
            CHECK(request.first != 0); // 132, not 80
        }
        if (request.kind == Request::Kind::StealFocus)
            sawStealFocus = true;
    }
    CHECK(sawColumnMode);
    CHECK(sawStealFocus);

    releaseTerminalSession(session);
}

TEST(theBufferedFontRequestPublishesItsFieldsGenerationAndEvent)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    Ubiquitty::Terminal* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    terminal->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    const auto bit = 1u << static_cast<unsigned>(EventKind::FontSwapRequested);

    feed(session.get(), "\x1b]50;Menlo:size=17\x07");
    CHECK_EQ(terminal->fontRequests.generation(), 1u);
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 1u);
    CHECK((session->takeEventFlags() & bit) != 0);
    CHECK((session->takeEventFlags() & bit) == 0);
    const auto absolute = session->beginFontRequest("Configured Face", 14.0);
    CHECK(absolute.receipt.isValid());
    CHECK(absolute.resolution.family == "Menlo");
    CHECK_EQ(absolute.resolution.sizePoints, 17.0);

    CHECK(session->acknowledgeFontRequest(absolute.receipt, FontAdoptionOutcome::Applied, "Menlo", 17.0));
    feed(session.get(), "\x1b]50;#+3\x07");
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 2u);
    CHECK((session->takeEventFlags() & bit) != 0);
    const auto relative = session->beginFontRequest("Configured Face", 14.0);
    CHECK(relative.resolution.family == "Menlo");
    CHECK_EQ(relative.resolution.sizePoints, 20.0);
    CHECK(session->acknowledgeFontRequest(relative.receipt, FontAdoptionOutcome::Applied, "Menlo", 20.0));

    feed(session.get(), "\x1b]50;?\x07");
    CHECK(std::string(terminal->outputBuffer.data(), static_cast<std::size_t>(terminal->outputLength)) == "\x1b]50;Menlo\x07");
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 2u);
    CHECK((session->takeEventFlags() & bit) == 0);

    feed(session.get(), "\x1b]50;\x07");
    const auto reset = session->beginFontRequest("Configured Face", 14.0);
    CHECK(reset.resolution.family == "Configured Face");
    CHECK_EQ(reset.resolution.sizePoints, 14.0);
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 3u);
    CHECK((session->takeEventFlags() & bit) != 0);
    CHECK(!session->hasPendingWindowRequest());
}

TEST(minttysEmptySizeOperandRestoresTheSizeAndKeepsTheFamily)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    Ubiquitty::Terminal* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    terminal->allowFontOperations = true;
    REQUIRE(session->publishActualFont("Original Face", 13.0));

    feed(session.get(), "\x1b]50;Monaco:size=17\x07");
    const auto named = session->beginFontRequest("Configured Face", 14.0);
    REQUIRE(named.receipt.isValid());
    CHECK(session->acknowledgeFontRequest(named.receipt, FontAdoptionOutcome::Applied, "Monaco", 17.0));

    // `OSC 7770 ;` and `OSC 7777 ;` restore the default size; the family the
    // program named stays, in the window and in the OSC 50 query.
    for (const char* reset : { "\x1b]7770;\x07", "\x1b]7777;\x07" }) {
        feed(session.get(), reset);
        const auto delivery = session->beginFontRequest("Configured Face", 14.0);
        REQUIRE(delivery.receipt.isValid());
        CHECK(delivery.resolution.family == "Monaco");
        CHECK_EQ(delivery.resolution.sizePoints, 14.0);
        CHECK(session->acknowledgeFontRequest(delivery.receipt, FontAdoptionOutcome::Applied, "Monaco", 14.0));
        terminal->outputLength = 0;
        feed(session.get(), "\x1b]50;?\x07");
        CHECK(std::string(terminal->outputBuffer.data(), static_cast<std::size_t>(terminal->outputLength)) == "\x1b]50;Monaco\x07");
        REQUIRE(session->publishActualFont("Monaco", 17.0));
    }
}

TEST(fontQueriesReportAcknowledgedFamilies)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    REQUIRE(session->publishActualFont("Original Face", 13.0));
    const auto query = [&] {
        feed(session.get(), "\033]50;?\007");
        std::string reply;
        Ubiquitty::flushOutput(*session->terminal(), [](void* context, std::span<const char> bytes) {
            static_cast<std::string*>(context)->append(bytes.data(), bytes.size());
            return true; }, &reply);
        return reply;
    };
    CHECK(query() == "\033]50;Original Face\007");
    feed(session.get(), "\033]50;Requested Face\007");
    CHECK(query() == "\033]50;Original Face\007");
    const auto first = session->beginFontRequest("Configured Face", 14.0);
    REQUIRE(first.receipt.isValid());
    const std::string actual(Ubiquitty::Terminal::maxFontLength, 'F');
    REQUIRE(session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Applied, actual, 14.0));
    CHECK(query() == "\033]50;" + actual + "\007");
    feed(session.get(), "\033]50;Unavailable Face\007");
    const auto second = session->beginFontRequest("Configured Face", 14.0);
    REQUIRE(second.receipt.isValid());
    REQUIRE(session->acknowledgeFontRequest(second.receipt, FontAdoptionOutcome::Refused, "", 0));
    CHECK(query() == "\033]50;" + actual + "\007");
}

TEST(refusedFontRequestsPreserveAcceptedStateWithAndWithoutTheBufferedClient)
{
    for (const auto decision : { Ubiquitty::CapabilityDecision::Deny, Ubiquitty::CapabilityDecision::Prompt }) {
        for (const bool hasBufferedClient : { false, true }) {
            RefusingFontPolicy policy(decision);
            const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
            REQUIRE(session != nullptr);
            session->start(24, 80);
            Ubiquitty::Terminal* terminal = session->terminal();
            REQUIRE(terminal != nullptr);
            terminal->allowFontOperations = true;
            if (!hasBufferedClient) {
                terminal->windowClient = nullptr;
            }
            feed(session.get(), "\x1b]50;Accepted Face:size=17\x07");
            CHECK((session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::FontSwapRequested))) != 0);
            terminal->capabilityPolicy = &policy;

            feed(session.get(), "\x1b]50;Denied Face:size=23\x07\x1b]50;#+3\x07\x1b]50;\x07");
            CHECK_EQ(policy.fontChecks(), 3);
            CHECK(terminal->font == "Accepted Face");
            const auto accepted = session->beginFontRequest("Configured Face", 14.0);
            CHECK(accepted.resolution.family == "Accepted Face");
            CHECK_EQ(accepted.resolution.sizePoints, 17.0);
            CHECK_EQ(terminal->fontRequests.generation(), 1u);
            CHECK_EQ(session->readSnapshot().fontRequestGeneration, 1u);
            CHECK(!terminal->fontRequestChanged);
            CHECK((session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::FontSwapRequested))) == 0);
            feed(session.get(), "\x1b]50;?\x07");
            CHECK(std::string(terminal->outputBuffer.data(), static_cast<std::size_t>(terminal->outputLength))
                == "\x1b]50;Accepted Face\x07");
        }
    }
}

TEST(theBufferedFontEventCannotPrecedeItsPublishedGeneration)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    Ubiquitty::Terminal* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    terminal->allowFontOperations = true;
    const auto bit = 1u << static_cast<unsigned>(EventKind::FontSwapRequested);

    Ubiquitty::processBytes(terminal, "\x1b]50;Menlo:size=17\x07");
    CHECK_EQ(terminal->fontRequests.generation(), 1u);
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 0u);
    CHECK((session->takeEventFlags() & bit) == 0);

    feed(session.get(), "\x1b]50;?\x07");
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 1u);
    CHECK((session->takeEventFlags() & bit) != 0);
    CHECK(!terminal->fontRequestChanged);
    CHECK((session->takeEventFlags() & bit) == 0);
}

TEST(directFontFeedsWaitForTheModelLock)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    std::promise<void> workerStarted;
    session->lockForRead();
    auto fed = std::async(std::launch::async, [&] {
        workerStarted.set_value();
        feed(session.get(), "\x1b]50;Menlo:size=17\x07");
    });
    workerStarted.get_future().wait();
    const auto statusWhileLocked = fed.wait_for(std::chrono::milliseconds(100));
    session->unlockForRead();
    fed.get();
    CHECK(statusWhileLocked == std::future_status::timeout);
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 1u);
}

TEST(directFontFeedSignalsAfterUnlockingAndPublishing)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    struct SignalState {
        Session* session;
        bool wasCalled { false };
        bool couldRead { false };
        std::uint32_t generation { 0 };
        std::uint32_t events { 0 };
    } state { session.get() };
    session->setDrainSignal(&state, [](void* context) {
        auto& state = *static_cast<SignalState*>(context);
        state.wasCalled = true;
        state.couldRead = state.session->tryLockForRead();
        if (state.couldRead) {
            state.generation = state.session->readSnapshot().fontRequestGeneration;
            state.session->unlockForRead();
            // Taking the flags polls the pane link, which reads the model
            // under its lock: a caller holding the lock would wait on itself.
            state.events = state.session->takeEventFlags();
        }
    });
    feed(session.get(), "\x1b]50;Menlo:size=17\x07");
    session->setDrainSignal(nullptr, nullptr);
    CHECK(state.wasCalled);
    CHECK(state.couldRead);
    CHECK_EQ(state.generation, 1u);
    CHECK((state.events & (1u << static_cast<unsigned>(EventKind::FontSwapRequested))) != 0);
}

TEST(bufferedFontRequestsComposeInParserOrder)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    auto applyNext = [&] {
        const auto delivery = session->beginFontRequest("Configured Face", 14.0);
        CHECK(delivery.receipt.isValid());
        CHECK(delivery.resolution.isResolved);
        CHECK(session->acknowledgeFontRequest(delivery.receipt, FontAdoptionOutcome::Applied,
            delivery.resolution.family, delivery.resolution.sizePoints));
        return delivery.resolution;
    };
    feed(session.get(), "\x1b]50;Menlo\x07\x1b]50;#20\x07");
    const auto family = applyNext();
    CHECK(family.family == "Menlo");
    CHECK_EQ(family.sizePoints, 13.0);
    const auto familyThenSize = applyNext();
    CHECK(familyThenSize.family == "Menlo");
    CHECK_EQ(familyThenSize.sizePoints, 20.0);

    CHECK(session->publishActualFont("Menlo", 13.0));
    feed(session.get(), "\x1b]50;#+2\x07\x1b]50;#+3\x07");
    CHECK_EQ(applyNext().sizePoints, 15.0);
    CHECK_EQ(applyNext().sizePoints, 18.0);

    CHECK(session->publishActualFont("Menlo", 90.0));
    feed(session.get(), "\x1b]50;#+20\x07\x1b]50;#-20\x07");
    CHECK_EQ(applyNext().sizePoints, 96.0);
    CHECK_EQ(applyNext().sizePoints, 76.0);

    feed(session.get(), "\x1b]50;\x07\x1b]50;#+3\x07");
    const auto reset = applyNext();
    CHECK(reset.family == "Configured Face");
    CHECK_EQ(reset.sizePoints, 14.0);
    CHECK_EQ(applyNext().sizePoints, 17.0);
    CHECK(!session->beginFontRequest("Configured Face", 14.0).receipt.isValid());
}

TEST(fontReceiptsCannotConsumeNewerOrAlreadyCompletedOperations)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;Menlo:size=17\x07");
    const auto first = session->beginFontRequest("Configured Face", 14.0);
    REQUIRE(first.receipt.isValid());
    CHECK(!session->beginFontRequest("Configured Face", 14.0).receipt.isValid());
    feed(session.get(), "\x1b]50;Newer Face:size=22\x07\x1b]50;#+3\x07");
    CHECK(first.resolution.family == "Menlo");
    CHECK_EQ(first.resolution.sizePoints, 17.0);
    CHECK(session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Applied, "Substituted Face", 18.0));
    CHECK(session->terminal()->font == "Newer Face");
    CHECK(!session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Applied, "Stale Face", 99.0));

    const auto second = session->beginFontRequest("Configured Face", 14.0);
    REQUIRE(second.receipt.isValid());
    CHECK(second.resolution.family == "Newer Face");
    CHECK_EQ(second.resolution.sizePoints, 22.0);
    CHECK(!session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Refused, "", 0.0));
    CHECK(session->acknowledgeFontRequest(second.receipt, FontAdoptionOutcome::Applied, "Actual Newer Face", 21.0));
    CHECK(session->terminal()->font == "Actual Newer Face");
    const auto third = session->beginFontRequest("Configured Face", 14.0);
    REQUIRE(third.receipt.isValid());
    CHECK(third.resolution.family == "Actual Newer Face");
    CHECK_EQ(third.resolution.sizePoints, 24.0);
    CHECK(session->acknowledgeFontRequest(third.receipt, FontAdoptionOutcome::Applied, "Actual Newer Face", 24.0));
    CHECK(!session->beginFontRequest("Configured Face", 14.0).receipt.isValid());
}

TEST(refusedFontAdoptionsLeaveTheNextOperationOnTheActualFont)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;Missing Face:size=30\x07\x1b]50;#+3\x07");
    const auto missing = session->beginFontRequest("Configured Face", 14.0);
    CHECK(session->terminal()->font == "Missing Face");
    CHECK(session->acknowledgeFontRequest(missing.receipt, FontAdoptionOutcome::Refused, "Original Face", 13.0));
    CHECK(session->terminal()->font == "Original Face");
    const auto relative = session->beginFontRequest("Configured Face", 14.0);
    CHECK(relative.resolution.family == "Original Face");
    CHECK_EQ(relative.resolution.sizePoints, 16.0);
    CHECK(session->acknowledgeFontRequest(relative.receipt, FontAdoptionOutcome::Applied, "Original Face", 16.0));
}

TEST(externalFontChangesInvalidateReceiptsWithoutDiscardingTheirRequests)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;#+3\x07");
    const auto beforeZoom = session->beginFontRequest("Configured Face", 14.0);
    CHECK_EQ(beforeZoom.resolution.sizePoints, 16.0);
    CHECK(session->publishActualFont("Zoomed Face", 20.0));
    CHECK(!session->acknowledgeFontRequest(beforeZoom.receipt, FontAdoptionOutcome::Applied, "Original Face", 16.0));
    CHECK(session->terminal()->font == "Zoomed Face");
    const auto afterZoom = session->beginFontRequest("Configured Face", 14.0);
    CHECK(afterZoom.resolution.family == "Zoomed Face");
    CHECK_EQ(afterZoom.resolution.sizePoints, 23.0);
    CHECK(session->acknowledgeFontRequest(afterZoom.receipt, FontAdoptionOutcome::Applied, "Zoomed Face", 23.0));
    CHECK(!session->acknowledgeFontRequest(beforeZoom.receipt, FontAdoptionOutcome::Refused, "", 0.0));

    feed(session.get(), "\x1b]50;Accepted Face\x07");
    CHECK(session->publishActualFont("Configured New Face", 18.0));
    CHECK(session->terminal()->font == "Accepted Face");
    const auto named = session->beginFontRequest("Configured New Face", 18.0);
    CHECK(named.resolution.family == "Accepted Face");
    CHECK_EQ(named.resolution.sizePoints, 18.0);
}

TEST(fontQueueOverflowRefusesBeforeChangingAcceptedState)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    constexpr std::size_t maxRequests = Ubiquitty::OSC::FontRequestQueue::maxPendingRequests;
    static_assert(maxRequests == Session::windowRequestQueueLimit);
    feed(session.get(), "\x1b]50;Accepted Face:size=14\x07");
    auto head = session->beginFontRequest("Configured Face", 14.0);
    for (std::size_t index = 1; index < maxRequests; ++index) {
        feed(session.get(), "\x1b]50;#+1\x07");
    }
    (void)session->takeEventFlags();
    feed(session.get(), "\x1b]50;Refused Face:size=30\x07");
    CHECK_EQ(session->terminal()->fontRequests.pendingCount(), maxRequests);
    CHECK_EQ(session->terminal()->fontRequests.generation(), maxRequests);
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, maxRequests);
    CHECK(session->terminal()->font == "Accepted Face");
    CHECK(!session->terminal()->fontRequestChanged);
    CHECK((session->takeEventFlags() & (1u << static_cast<unsigned>(EventKind::FontSwapRequested))) == 0);
    CHECK(session->acknowledgeFontRequest(head.receipt, FontAdoptionOutcome::Applied, "Accepted Face", 14.0));
    for (std::size_t index = 1; index < maxRequests; ++index) {
        const auto request = session->beginFontRequest("Configured Face", 14.0);
        CHECK_EQ(request.resolution.sizePoints, 14.0 + index);
        CHECK(session->acknowledgeFontRequest(request.receipt, FontAdoptionOutcome::Applied,
            request.resolution.family, request.resolution.sizePoints));
    }
    CHECK(!session->beginFontRequest("Configured Face", 14.0).receipt.isValid());
    feed(session.get(), "\x1b]50;Admitted Face:size=30\x07");
    CHECK_EQ(session->terminal()->fontRequests.generation(), maxRequests + 1);
    CHECK(session->terminal()->font == "Admitted Face");
}

TEST(invalidFontAxesAndUnsupportedClientsNeverClaimAcceptance)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;" + std::string(Ubiquitty::Terminal::maxFontLength + 1, 'x') + "\x07");
    feed(session.get(), "\x1b]50;#nan\x07\x1b]50;#+inf\x07");
    CHECK_EQ(session->terminal()->fontRequests.generation(), 0u);
    CHECK_EQ(session->terminal()->fontRequests.pendingCount(), 0u);
    CHECK(session->terminal()->font == "Original Face");
    CHECK(!session->publishActualFont("Invalid Face", std::numeric_limits<double>::quiet_NaN()));
    CHECK(session->terminal()->font == "Original Face");

    Ubiquitty::WindowClient unsupported;
    session->terminal()->windowClient = &unsupported;
    feed(session.get(), "\x1b]50;Unsupported Face:size=20\x07");
    CHECK_EQ(session->terminal()->fontRequests.generation(), 0u);
    CHECK(session->terminal()->font == "Original Face");
    session->terminal()->windowClient = nullptr;
}

TEST(fontReceiptsRejectWrongSessionsAndInvalidSuccessfulOutcomes)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> first(Session::make(), releaseTerminalSession);
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> second(Session::make(), releaseTerminalSession);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    for (Session* session : { first.get(), second.get() }) {
        session->start(24, 80);
        session->terminal()->allowFontOperations = true;
        CHECK(session->publishActualFont("Original Face", 13.0));
        feed(session, "\x1b]50;Menlo:size=17\x07");
    }
    const auto firstRequest = first->beginFontRequest("Configured Face", 14.0);
    const auto secondRequest = second->beginFontRequest("Configured Face", 14.0);
    CHECK(!second->acknowledgeFontRequest(firstRequest.receipt, FontAdoptionOutcome::Applied, "Wrong Face", 33.0));
    CHECK(!second->acknowledgeFontRequest(secondRequest.receipt, FontAdoptionOutcome::Applied, "", 17.0));
    CHECK(!second->acknowledgeFontRequest(secondRequest.receipt, FontAdoptionOutcome::Applied, "Menlo", std::numeric_limits<double>::infinity()));
    CHECK_EQ(second->terminal()->fontRequests.pendingCount(), 1u);
    CHECK(second->acknowledgeFontRequest(secondRequest.receipt, FontAdoptionOutcome::Applied, "Menlo", 17.0));
    CHECK(first->acknowledgeFontRequest(firstRequest.receipt, FontAdoptionOutcome::Refused, "Original Face", 13.0));
}

TEST(arrivalsBetweenNotificationAndLeaseNeverReplaceTheOldestRequest)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;#+2\x07");
    CHECK_EQ(session->readSnapshot().fontRequestGeneration, 1u);
    (void)session->takeEventFlags();
    feed(session.get(), "\x1b]50;#+3\x07");
    const auto first = session->beginFontRequest("Configured Face", 14.0);
    CHECK_EQ(first.resolution.sizePoints, 15.0);
    CHECK(session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Applied, "Original Face", 15.0));
    const auto second = session->beginFontRequest("Configured Face", 14.0);
    CHECK_EQ(second.resolution.sizePoints, 18.0);
    CHECK(session->acknowledgeFontRequest(second.receipt, FontAdoptionOutcome::Applied, "Original Face", 18.0));
    CHECK(!session->beginFontRequest("Configured Face", 14.0).receipt.isValid());
}

TEST(hardResetPreservesAcceptedFontOperationsAndTheirLease)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;Menlo:size=17\x07\x1b]50;#+3\x07");
    const auto first = session->beginFontRequest("Configured Face", 14.0);
    feed(session.get(), "\x1b"
                        "c");
    CHECK(session->terminal()->font == "Menlo");
    CHECK_EQ(session->terminal()->fontRequests.generation(), 2u);
    CHECK(session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Applied, "Menlo", 17.0));
    const auto second = session->beginFontRequest("Configured Face", 14.0);
    CHECK_EQ(second.resolution.sizePoints, 20.0);
}

TEST(sharedRendererPublicationDoesNotReplayOrAcknowledgeAnotherPane)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> first(Session::make(), releaseTerminalSession);
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> second(Session::make(), releaseTerminalSession);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    for (Session* session : { first.get(), second.get() }) {
        session->start(24, 80);
        session->terminal()->allowFontOperations = true;
        CHECK(session->publishActualFont("Shared Face", 13.0));
    }
    feed(first.get(), "\x1b]50;#+2\x07");
    feed(second.get(), "\x1b]50;#+3\x07");
    const auto firstRequest = first->beginFontRequest("Configured Face", 14.0);
    const auto beforeSharedAdoption = second->beginFontRequest("Configured Face", 14.0);
    CHECK_EQ(firstRequest.resolution.sizePoints, 15.0);
    CHECK_EQ(beforeSharedAdoption.resolution.sizePoints, 16.0);
    CHECK(first->acknowledgeFontRequest(firstRequest.receipt, FontAdoptionOutcome::Applied, "Shared Face", 15.0));
    CHECK(second->publishActualFont("Shared Face", 15.0));
    CHECK(!second->acknowledgeFontRequest(firstRequest.receipt, FontAdoptionOutcome::Applied, "Shared Face", 15.0));
    CHECK(!second->acknowledgeFontRequest(beforeSharedAdoption.receipt, FontAdoptionOutcome::Applied, "Shared Face", 16.0));
    const auto secondRequest = second->beginFontRequest("Configured Face", 14.0);
    CHECK_EQ(secondRequest.resolution.sizePoints, 18.0);
    CHECK(second->acknowledgeFontRequest(secondRequest.receipt, FontAdoptionOutcome::Applied, "Shared Face", 18.0));
    CHECK(first->publishActualFont("Shared Face", 18.0));
    CHECK(!first->acknowledgeFontRequest(firstRequest.receipt, FontAdoptionOutcome::Applied, "Shared Face", 15.0));
    CHECK(!first->beginFontRequest("Configured Face", 14.0).receipt.isValid());
    CHECK(!second->beginFontRequest("Configured Face", 14.0).receipt.isValid());
}

TEST(fontAcknowledgmentsAndExternalPublicationsSignalOutsideTheModelLock)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    session->terminal()->allowFontOperations = true;
    CHECK(session->publishActualFont("Original Face", 13.0));
    feed(session.get(), "\x1b]50;#+2\x07\x1b]50;#+3\x07");
    const auto first = session->beginFontRequest("Configured Face", 14.0);
    (void)session->takeEventFlags();
    struct SignalState {
        Session* session;
        int calls { 0 };
        bool couldRead { true };
        bool sawReadyRequest { true };
    } state { session.get() };
    session->setDrainSignal(&state, [](void* context) {
        auto& state = *static_cast<SignalState*>(context);
        ++state.calls;
        const bool couldRead = state.session->tryLockForRead();
        state.couldRead &= couldRead;
        if (couldRead) {
            state.sawReadyRequest &= state.session->terminal()->fontRequests.canBegin();
            state.session->unlockForRead();
        }
    });
    CHECK(session->acknowledgeFontRequest(first.receipt, FontAdoptionOutcome::Applied, "Original Face", 15.0));
    CHECK_EQ(state.calls, 1);
    const auto second = session->beginFontRequest("Configured Face", 14.0);
    // The signal fires once per service: an event before the flags are taken
    // adds its bit and fires nothing, and the take re-arms it.
    CHECK(session->publishActualFont("Zoomed Face", 20.0));
    CHECK_EQ(state.calls, 1);
    (void)session->takeEventFlags();
    CHECK(session->publishActualFont("Zoomed Face", 21.0));
    CHECK_EQ(state.calls, 2);
    CHECK(!session->acknowledgeFontRequest(second.receipt, FontAdoptionOutcome::Applied, "Original Face", 18.0));
    CHECK_EQ(state.calls, 2);
    CHECK(state.couldRead);
    CHECK(state.sawReadyRequest);
    session->setDrainSignal(nullptr, nullptr);
}

/// A run of resizes is one request: the last.
///
/// A program that writes CSI 8 t a hundred times in one burst asked for one
/// window, the last, and a frontend performing every step would lay out,
/// redraw and allocate a drawable for the ninety-nine it never asked to see;
/// past the queue's bound it would also lose the one it did ask for. The run
/// folds as it arrives, so the bound holds and the last request wins. An
/// omitted axis keeps the earlier request's, as performing both would have.
TEST(theWindowQueueFoldsARunOfResizesIntoItsLast)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->installWindowRequestQueue());

    std::string storm;
    for (int index = 1; index <= 64; ++index)
        storm += "\x1b[8;" + std::to_string(10 + index) + ";" + std::to_string(40 + index) + "t";
    feed(session, storm);
    std::vector<Request> taken = drain(session);
    CHECK(taken.size() == 1);
    if (taken.size() == 1) {
        CHECK(taken[0].kind == Request::Kind::WindowOp);
        CHECK(taken[0].first == 8);
        CHECK(taken[0].second == 74);
        CHECK(taken[0].third == 104);
    }

    // The omitted column count keeps the earlier request's 100, not the
    // window's current width; an empty parameter is omitted too.
    feed(session, "\x1b[8;30;100t\x1b[8;40t\x1b[8;;120t");
    taken = drain(session);
    CHECK(taken.size() == 1);
    if (taken.size() == 1) {
        CHECK(taken[0].second == 40);
        CHECK(taken[0].third == 120);
    }

    // Pixels fold the same way, and a move takes the later point whole.
    feed(session, "\x1b[4;600;800t\x1b[4;;1000t\x1b[3;10;20t\x1b[3;30;40t");
    taken = drain(session);
    CHECK(taken.size() == 2);
    if (taken.size() == 2) {
        CHECK(taken[0].first == 4);
        CHECK(taken[0].second == 600);
        CHECK(taken[0].third == 1000);
        CHECK(taken[1].first == 3);
        CHECK(taken[1].second == 30);
        CHECK(taken[1].third == 40);
    }

    releaseTerminalSession(session);
}

/// Only a run of one kind folds; everything between keeps its place.
///
/// The showcase's burst raises, lowers, maximizes, restores and resizes
/// twice. The two resizes are one request; the rest arrive as written,
/// because a restore folded over a maximize is a different window from the
/// one the program asked for, and so is a raise performed after a resize it
/// was written before.
TEST(theWindowQueueKeepsTheOperationsAroundAFoldedRunInOrder)
{
    Session* session = Session::make();
    CHECK(session != nullptr);
    session->start(24, 80);
    CHECK(session->installWindowRequestQueue());

    feed(session, "\x1b[5t\x1b[6t\x1b[9;1t\x1b[9;0t\x1b[8;23;78t\x1b[8;24;80t");
    const std::vector<Request> taken = drain(session);
    CHECK(taken.size() == 5);
    if (taken.size() == 5) {
        CHECK(taken[0].first == 5);
        CHECK(taken[1].first == 6);
        CHECK(taken[2].first == 9);
        CHECK(taken[2].second == 1);
        CHECK(taken[3].first == 9);
        CHECK(taken[3].second == 0);
        CHECK(taken[4].first == 8);
        CHECK(taken[4].second == 24);
        CHECK(taken[4].third == 80);
    }

    // A resize behind a raise starts a run of its own rather than reaching
    // back past it.
    feed(session, "\x1b[8;30;100t\x1b[5t\x1b[8;31;101t");
    CHECK(drain(session).size() == 3);

    releaseTerminalSession(session);
}

UBIQUITTY_TEST_MAIN()
