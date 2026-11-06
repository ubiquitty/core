// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/capture_flood_busy.cpp — a client that floods reads and reads
// nothing back costs the daemon its call limits, not its memory.
//
// A screen plane answers at most `maxOutstandingCalls` calls at once and holds
// at most `maxQueuedBulkBytes` for its client (Docs/sync.md §7.9). Past
// either, a call is answered busy before any text is read, and the connection
// stays. The §10 test, over Sync calls:
//
//   1. 1,000 concurrent `pane.capture` calls on a 200x60 pane with 10,000
//      lines of history draw 32 answers and 968 busy ones;
//   2. the bytes the connection holds stay under the queued-bulk limit;
//   3. the peak RSS of the 1,000-call run is within 8 MiB of a 10-call run's;
//   4. the connection stays attached, and every answer is the pane's text.
//
// Then the same over whole-buffer pane.capture calls:
//
//   1. 1,000 reads of a 200x60 pane with 2,000 lines of history, sent while the
//      client reads nothing, draw exactly the limit's worth of answers and 968
//      busy ones;
//   2. the bytes the connection holds stay under the queued-bulk limit;
//   3. 10,000 more reads leave the daemon's peak RSS within 8 MiB of where the
//      thousand left it — what is held is bounded by the limits, not the count;
//   4. once the client reads, every answer arrives and the client is still
//      attached.
//
// Friend seam (Tests/Host/ScreenPlaneAccess.h), same QUIC-OFF host build as
// two_client_pacing. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// The daemon's peak resident set, in bytes.
///
/// - Returns: The high-water mark `getrusage` reports.
std::uint64_t peakResidentBytes()
{
    rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    // Darwin reports bytes, Linux kilobytes.
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

/// Sends whole-buffer captures without reading any answers.
void wholeBufferReads(Daemon& daemon, std::size_t index, UbiquittyTest::ScreenClient& client, std::uint32_t first, int count)
{
    for (int read = 0; read < count; ++read)
        CHECK(client.call(R"({"jsonrpc":"2.0","id":)" + std::to_string(first + static_cast<std::uint32_t>(read))
            + R"(,"method":"pane.capture","params":{"pane":1,"source":"rows","start":0,"end":2147483647}})"));
    Screen::receive(daemon, index, client.takeStream(Screen::now()));
    Screen::service(daemon);
}

struct Answers {
    int answered = 0;
    int busy = 0;
};

Answers countAnswers(UbiquittyTest::ScreenClient& client)
{
    Answers answers;
    for (const auto& text : client.takeResults()) {
        bool isParsed = false;
        const auto result = Ubiquitty::Codec::JSON::parse(text, isParsed);
        const auto* error = result.find("error");
        if (isParsed && error && error->numberOr("code") == -32005) {
            ++answers.busy;
        } else {
            const auto* body = result.find("result");
            CHECK(isParsed && body && body->boolOr("complete") && body->stringOr("source") == "rows");
            CHECK(body && body->stringOr("text").find("1999 hhh") != std::string::npos);
            ++answers.answered;
        }
    }
    return answers;
}

} // namespace

/// A `pane.capture` request for the client's first pane.
///
/// - Parameter id: The request's id.
/// - Returns: Its text.
std::string captureRequest(int id)
{
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":"pane.capture","params":{"pane":1}})";
}

/// What a run of concurrent `pane.capture` calls drew.
struct CaptureRun {
    int answered = 0;
    int busy = 0;
    /// Answers whose text is not the pane's last screen row's.
    int wrong = 0;
    std::size_t held = 0;
    std::uint64_t peakResident = 0;
    bool isAttached = false;
    /// Whether the daemon stopped reading the client once the answers it
    /// owed passed the interactive bound, and read it again once they left.
    bool wasInputHeld = false;
    bool isInputHeldAfterReading = true;
};

/// Sends `calls` concurrent `pane.capture` calls from a client that reads
/// nothing until every call has been served, then reads the answers.
///
/// - Parameter calls: How many.
/// - Returns: What they drew.
CaptureRun runCaptures(int calls)
{
    Daemon d;
    auto& session = Screen::makeSession(d, 60, 200, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    const std::string line(190, 'h');
    for (int row = 0; row < 10'000; ++row)
        Ubiquitty::processBytes(session.term.get(), std::to_string(row) + " " + line + "\r\n");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Sync::Scope::Observe, Sync::Scope::RegionRead });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(60, 200, 0);
    Screen::greet(d, index, client);
    CHECK_MSG(client.pane(), "the client was not sent its checkpoint");
    for (int id = 1; id <= calls; ++id)
        CHECK(client.call(captureRequest(id)));
    Screen::receive(d, index, client.takeStream(Screen::now()));
    Screen::service(d);
    CaptureRun run;
    run.held = Screen::connection(d, index)->queuedBytes();
    run.peakResident = peakResidentBytes();
    run.wasInputHeld = Screen::isInputHeld(d, index);
    Screen::deliver(d, index, client);
    run.isInputHeldAfterReading = Screen::isInputHeld(d, index);
    for (const std::string& result : client.takeResults()) {
        bool ok = false;
        const Ubiquitty::Codec::JSON::Value parsed = Ubiquitty::Codec::JSON::parse(result, ok);
        const Ubiquitty::Codec::JSON::Value* error = ok ? parsed.find("error") : nullptr;
        if (error && error->numberOr("code") == -32005) {
            ++run.busy;
            continue;
        }
        ++run.answered;
        const Ubiquitty::Codec::JSON::Value* answer = ok ? parsed.find("result") : nullptr;
        if (!answer || answer->numberOr("pane") != 1 || answer->numberOr("rows") != 60 || answer->numberOr("columns") != 200
            || !answer->boolOr("complete") || answer->stringOr("text").find("9999 hhh") == std::string::npos)
            ++run.wrong;
    }
    run.isAttached = Screen::connection(d, index)->state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached && !Screen::didOverflow(d, index);
    // The daemon serves pane.search too, over the whole history the client
    // keeps none of.
    if (calls <= 10) {
        CHECK(client.call(R"({"jsonrpc":"2.0","id":99999,"method":"pane.search","params":{"pane":1,"pattern":"^9900 h"}})"));
        Screen::receive(d, index, client.takeStream(Screen::now()));
        Screen::deliver(d, index, client);
        const std::vector<std::string> results = client.takeResults();
        bool ok = false;
        const Ubiquitty::Codec::JSON::Value parsed = results.size() == 1 ? Ubiquitty::Codec::JSON::parse(results.front(), ok) : Ubiquitty::Codec::JSON::Value {};
        const Ubiquitty::Codec::JSON::Value* answer = ok ? parsed.find("result") : nullptr;
        const Ubiquitty::Codec::JSON::Value* matches = answer ? answer->find("matches") : nullptr;
        CHECK_MSG(matches && matches->items().size() == 1, ("pane.search did not find the one history line it names: " + (results.empty() ? std::string("no result") : results.front())).c_str());
        // A control-socket method the registry gives no Sync name is not
        // served over a Call.
        CHECK(client.call(R"({"jsonrpc":"2.0","id":99998,"method":"send-text","params":{"pane":1,"text":"x"}})"));
        Screen::receive(d, index, client.takeStream(Screen::now()));
        Screen::deliver(d, index, client);
        const std::vector<std::string> refused = client.takeResults();
        CHECK_MSG(refused.size() == 1 && refused.front().find("-32601") != std::string::npos, ("send-text over a Sync Call was not refused as not found: " + (refused.empty() ? std::string("no result") : refused.front())).c_str());
    }
    return run;
}

/// Runs `runCaptures` in a process of its own and returns what it drew.
///
/// The peak resident set is a process's high-water mark, and the two runs it
/// is compared across each build a daemon with ten thousand rows of
/// scrollback. In one process the second daemon's scrollback is placed by the
/// allocator wherever it has room: in the blocks the first daemon freed, or,
/// when the thread has moved to another processor's magazine, in a region
/// mapped anew, which raises the peak by megabytes with the same bytes in
/// use. A process each gives both runs the same empty heap, so the difference
/// between their peaks is what the calls cost.
///
/// - Parameter calls: How many calls.
/// - Returns: What they drew, or `nullopt` when the child could not be run
///   or did not report.
std::optional<CaptureRun> runCapturesInAProcessOfItsOwn(int calls)
{
    static_assert(std::is_trivially_copyable_v<CaptureRun>);
    std::array<int, 2> ends {};
    if (::pipe(ends.data()) != 0)
        return std::nullopt;
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return std::nullopt;
    }
    if (child == 0) {
        ::close(ends[0]);
        const CaptureRun run = runCaptures(calls);
        const bool didReport = failures == 0 && ::write(ends[1], &run, sizeof(run)) == static_cast<ssize_t>(sizeof(run));
        std::fflush(nullptr);
        ::_exit(didReport ? 0 : 1);
    }
    ::close(ends[1]);
    CaptureRun run;
    std::size_t taken = 0;
    while (taken < sizeof(run)) {
        const ssize_t count = ::read(ends[0], reinterpret_cast<char*>(&run) + taken, sizeof(run) - taken);
        if (count <= 0)
            break;
        taken += static_cast<std::size_t>(count);
    }
    ::close(ends[0]);
    int status = 0;
    const bool didExit = ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return didExit && taken == sizeof(run) ? std::optional { run } : std::nullopt;
}

int main()
{
    const std::optional<CaptureRun> tenInAProcess = runCapturesInAProcessOfItsOwn(10);
    const std::optional<CaptureRun> thousandInAProcess = runCapturesInAProcessOfItsOwn(1'000);
    CHECK_MSG(tenInAProcess && thousandInAProcess, "a run of captures failed an assertion of its own, or its process did not report");
    if (!tenInAProcess || !thousandInAProcess)
        return UBIQUITTY_TEST_SUMMARY();
    const CaptureRun tenCaptures = *tenInAProcess;
    CHECK_MSG(tenCaptures.answered == 10 && tenCaptures.busy == 0 && tenCaptures.wrong == 0, ("ten captures drew " + std::to_string(tenCaptures.answered) + " answers").c_str());
    const CaptureRun thousand = *thousandInAProcess;
    CHECK_MSG(thousand.busy == 968, ("a thousand captures drew " + std::to_string(thousand.busy) + " busy answers").c_str());
    CHECK_MSG(thousand.answered == 32 && thousand.wrong == 0, ("a thousand captures drew " + std::to_string(thousand.answered) + " answers, " + std::to_string(thousand.wrong) + " not the pane's text").c_str());
    CHECK_MSG(thousand.held <= std::size_t { 16 } << 20, ("the connection held " + std::to_string(thousand.held) + " bytes").c_str());
    // Each run's peak is its own process's, so the thousand's may be the lower.
    const std::uint64_t capturesGrowth = thousand.peakResident > tenCaptures.peakResident ? thousand.peakResident - tenCaptures.peakResident : 0;
    CHECK_MSG(capturesGrowth < (8u << 20), ("a thousand captures grew the peak RSS by " + std::to_string(capturesGrowth >> 10) + " KiB").c_str());
    CHECK_MSG(thousand.isAttached, "the capturing client was dropped");
    CHECK_MSG(!thousand.wasInputHeld, "a thousand calls' answers held the client's input");

    // Twenty thousand calls' busy answers pass the 1 MiB interactive bound, so
    // the daemon stops reading the client until they have left.
    const CaptureRun twentyThousand = runCaptures(20'000);
    CHECK_MSG(twentyThousand.wasInputHeld, "the daemon kept reading a caller that read nothing past the interactive bound");
    CHECK_MSG(!twentyThousand.isInputHeldAfterReading, "the daemon did not read the client again once it had read its answers");
    CHECK_MSG(twentyThousand.answered == 32 && twentyThousand.busy == 19'968 && twentyThousand.isAttached, ("twenty thousand captures drew " + std::to_string(twentyThousand.answered) + " answers").c_str());

    Daemon d;
    auto& session = Screen::makeSession(d, 60, 200, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    std::string line(190, 'h');
    for (int row = 0; row < 2'000; ++row)
        Ubiquitty::processBytes(session.term.get(), std::to_string(row) + " " + line + "\r\n");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const std::size_t index = Screen::attach(d, session.id);
    Screen::grantScope(d, index, { Sync::Scope::Observe, Sync::Scope::RegionRead });
    Screen::markInteractiveUser(d, index);
    UbiquittyTest::ScreenClient client(60, 200, 2'100);
    Screen::greet(d, index, client);
    CHECK_MSG(client.pane(), "the client was not sent its checkpoint");
    const Ubiquitty::Protocol::Sync::Limits limits = Screen::connection(d, index)->limits();

    // Ten reads, read back.
    wholeBufferReads(d, index, client, 1, 10);
    Screen::deliver(d, index, client);
    const Answers ten = countAnswers(client);
    CHECK_MSG(ten.answered == 10 && ten.busy == 0, ("ten reads drew " + std::to_string(ten.answered) + " answers").c_str());

    // A thousand, while the client reads nothing.
    wholeBufferReads(d, index, client, 100, 1'000);
    const std::size_t held = Screen::connection(d, index)->queuedBytes();
    CHECK_MSG(held <= limits.maxQueuedBulkBytes, ("the connection holds " + std::to_string(held) + " bytes").c_str());
    const std::uint64_t residentAfterThousand = peakResidentBytes();
    // Ten thousand more.
    wholeBufferReads(d, index, client, 2'000, 10'000);
    const std::uint64_t residentAfterFlood = peakResidentBytes();
    CHECK_MSG(residentAfterFlood < residentAfterThousand + (8u << 20),
        ("ten thousand more reads grew the peak RSS by " + std::to_string((residentAfterFlood - residentAfterThousand) >> 10) + " KiB").c_str());
    CHECK_MSG(!Screen::didOverflow(d, index), "the flooding client was dropped");

    Screen::deliver(d, index, client);
    const Answers flood = countAnswers(client);
    CHECK_MSG(flood.answered == limits.maxOutstandingCalls, ("the flood drew " + std::to_string(flood.answered) + " answers").c_str());
    CHECK_MSG(flood.busy == 11'000 - limits.maxOutstandingCalls, ("the flood drew " + std::to_string(flood.busy) + " busy answers").c_str());
    CHECK_MSG(Screen::connection(d, index)->state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached, "the connection closed");

    if (failures == 0)
        std::printf("capture_flood_busy: 1,000 pane.capture calls drew %d answers and %d busy ones, held %zu bytes, peak RSS +%llu KiB over ten\n",
            thousand.answered, thousand.busy, thousand.held, static_cast<unsigned long long>(capturesGrowth >> 10));
    if (failures == 0)
        std::printf("capture_flood_busy: 11,000 whole-buffer reads from a client that read nothing drew %d answers and %d busy ones; "
                    "the connection held %zu bytes at most and the peak RSS grew %llu KiB\n",
            flood.answered, flood.busy, held, static_cast<unsigned long long>((residentAfterFlood - residentAfterThousand) >> 10));
    return failures == 0 ? 0 : 1;
}
