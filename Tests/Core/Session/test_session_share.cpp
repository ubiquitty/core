// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_session_share.cpp — what "Share This Window" hands the local daemon,
// and where it finds that daemon (Core/Session/SessionShare.h).
//
// The local adopting record carries every terminal continuation owner, not a
// display-only checkpoint. The socket is the binary's own or nothing: a share
// that fell back to a well-known name would hand a live terminal to whichever
// daemon bound it.

#include "Core/Image/TerminalImage.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/DaemonSocketLeaf.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Parser.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Ubiquitty;
namespace Local = Ubiquitty::Protocol::Local;

namespace {

/// One visible row as printable ASCII, blanks as spaces.
std::string rowText(const Terminal& terminal, int row)
{
    std::string text;
    for (int column = 0; column < terminal.columns; ++column) {
        const Cell* cell = Screen::visibleCell(&terminal, 0, row, column);
        const std::uint32_t codePoint = cell ? cell->codePoint : 0;
        text.push_back(codePoint >= 32 && codePoint < 127 ? static_cast<char>(codePoint) : ' ');
    }
    return text;
}

/// What a share hands the daemon, and what the daemon admits from it.
struct DecodedAdopt {
    /// The complete private terminal, admitted without a thin fallback.
    Protocol::Sync::HandoverTerminal continuation;
    /// The whole-terminal body a Prepare record carries.
    std::vector<std::uint8_t> payload;
};

/// Admits a share's body under the daemon's own limits.
///
/// - Parameter body: The body, as the share captured it.
/// - Returns: The body and what it decodes to, or nothing when it does not.
std::optional<DecodedAdopt> decodeAdopt(const std::vector<std::uint8_t>& body)
{
    auto terminal = Protocol::Sync::decodeHandoverTerminal(body, Session::localShareTerminalLimits(Local::maxAdoptBodyBytes));
    if (!terminal)
        return std::nullopt;
    return DecodedAdopt { .continuation = std::move(*terminal), .payload = body };
}

/// Captures what a share hands the daemon: the window's whole terminal body,
/// as the share path captures it.
///
/// - Parameters:
///   - term: The window.
///   - body: Receives the body; empty unless the result is `Encoded`.
///   - maxBodyBytes: The longest body the share may capture.
/// - Returns: How the capture ended.
Session::ShareEncoding shareFrames(Terminal& term, std::vector<std::uint8_t>& body, std::size_t maxBodyBytes = Local::maxAdoptBodyBytes)
{
    body.clear();
    const auto captured = Session::captureShareTerminal(term, maxBodyBytes);
    if (!captured)
        return captured.error();
    body.assign(captured->view().begin(), captured->view().end());
    return Session::ShareEncoding::Encoded;
}

/// Allows every effect, for a Remote window whose replies a share carries.
///
/// A terminal with no policy answers PAL's Remote default, which refuses an
/// answerback; the policy is host-owned and is not carried, so each side of a
/// share installs this one.
class AnsweringPolicy final : public CapabilityPolicy {
public:
    /// Allows the request.
    ///
    /// - Returns: Allow, whatever is asked.
    CapabilityDecision check(Capability, CapabilityOrigin, std::string_view) override { return CapabilityDecision::Allow; }
};

/// A window with history lines that share no run with their neighbours.
///
/// - Parameters:
///   - columns: Its width.
///   - lines: How many lines to print.
///   - rows: The visible height.
/// - Returns: The window.
std::unique_ptr<Terminal> windowWithHistory(int columns, int lines, int rows = 4)
{
    auto window = std::make_unique<Terminal>();
    hardReset(*window);
    resize(*window, rows, columns);
    window->scrollbackLimit = lines + 100;
    std::string line;
    for (int index = 0; index < lines; ++index) {
        line.clear();
        for (int column = 0; column + 1 < columns; ++column)
            line += "\x1b[38;5;" + std::to_string((index + column * 7) % 256) + "m" + static_cast<char>('a' + (index + column) % 26);
        line += "\r\n";
        processBytes(window.get(), line.c_str());
    }
    processBytes(window.get(), "\x1b[mvisible prompt");
    return window;
}

/// Adopts a share's payload into a fresh terminal, as the daemon does.
///
/// - Parameters:
///   - adopt: The payload.
///   - rows: The window's rows.
///   - columns: The window's columns.
/// - Returns: The terminal, or nothing when any continuation was refused.
std::unique_ptr<Terminal> adopted(const DecodedAdopt& adopt, int rows, int columns)
{
    auto terminal = Protocol::Sync::decodeHandoverTerminal(adopt.payload, Session::localShareTerminalLimits(Local::maxAdoptBodyBytes));
    if (!terminal || terminal->terminal->rows != rows || terminal->terminal->columns != columns)
        return nullptr;
    return std::move(terminal->terminal);
}

/// Whether two terminals hold the same history and show the same screen.
///
/// - Parameters:
///   - left: One terminal.
///   - right: The other.
/// - Returns: True when every history line and every visible row reads alike.
bool holdTheSame(const Terminal& left, const Terminal& right)
{
    if (!left.lineStore || !right.lineStore || left.lineStore->count() != right.lineStore->count() || left.rows != right.rows)
        return false;
    for (int index = 0; index < left.lineStore->count(); ++index) {
        if (Screen::absoluteRowText(left, index, left.columns) != Screen::absoluteRowText(right, index, right.columns))
            return false;
    }
    for (int row = 0; row < left.rows; ++row) {
        if (rowText(left, row) != rowText(right, row))
            return false;
    }
    return true;
}

} // namespace

TEST(anAdoptFrameCarriesTheGridTheGateAndTheScreen)
{
    auto window = std::make_unique<Terminal>();
    hardReset(*window);
    resize(*window, 30, 100);
    processBytes(window.get(), "shared prompt $ ls\r\nREADME\r\n");
    window->sequenceGate.oscDeny[0] = 52;
    window->sequenceGate.oscDenyCount = 1;

    std::vector<std::uint8_t> frame;
    REQUIRE(shareFrames(*window, frame) == Session::ShareEncoding::Encoded);
    const std::optional<DecodedAdopt> adopt = decodeAdopt(frame);
    REQUIRE(adopt.has_value());

    const auto& daemon = *adopt->continuation.terminal;
    CHECK_EQ(daemon.columns, 100);
    CHECK_EQ(daemon.rows, 30);
    CHECK_EQ(adopt->continuation.generation, 1u);
    CHECK_MSG(daemon.scrollbackLimit == window->scrollbackLimit, "the window's scrollback limit did not ride with the session");
    CHECK_MSG(daemon.sequenceGate.oscDenyCount == 1 && daemon.sequenceGate.oscDeny[0] == 52,
        "the window's sequence gate did not ride with the session");

    CHECK_MSG(rowText(daemon, 0).starts_with("shared prompt $ ls"), rowText(daemon, 0).c_str());
    CHECK_MSG(rowText(daemon, 1).starts_with("README"), rowText(daemon, 1).c_str());
}

TEST(aDeferredRewrapIsPaidBeforeTheScreenIsTaken)
{
    auto window = std::make_unique<Terminal>();
    hardReset(*window);
    resize(*window, 24, 120);
    for (int line = 0; line < 80; ++line) {
        std::string text = "line " + std::to_string(line) + " ";
        text.append(100, static_cast<char>('a' + line % 26));
        text += "\r\n";
        processBytes(window.get(), text);
    }
    resize(*window, 24, 73, Modes::ReflowMode::Reflow, Modes::AnchorMode::Top, Modes::ResizeUrgency::Interactive);
    REQUIRE(window->pendingHistoryRewrapColumns == 73);

    std::vector<std::uint8_t> frame;
    CHECK(shareFrames(*window, frame) == Session::ShareEncoding::NothingToShare);
    CHECK(frame.empty());
    CHECK_MSG(window->pendingHistoryRewrapColumns == 73,
        "a share completed unfinished history work instead of refusing unchanged");
}

// A share's body is admitted whole or not at all: a body cut short or one
// with a byte past its end is refused, as is one past the receiver's limit.
TEST(localShareBodiesRefuseTruncationTrailingBytesAndOversize)
{
    const auto window = windowWithHistory(10, 4);
    std::vector<std::uint8_t> body;
    REQUIRE(shareFrames(*window, body) == Session::ShareEncoding::Encoded);
    REQUIRE(decodeAdopt(body));
    REQUIRE(!body.empty());
    auto shortened = body;
    shortened.pop_back();
    CHECK(!decodeAdopt(shortened));
    auto trailing = body;
    trailing.push_back(0);
    CHECK(!decodeAdopt(trailing));
    CHECK(!Protocol::Sync::decodeHandoverTerminal(body, Session::localShareTerminalLimits(body.size() - 1)));
}

TEST(aTerminalWithNoGridEncodesNothing)
{
    auto unsized = std::make_unique<Terminal>();
    unsized->columns = 0;
    unsized->rows = 0;
    std::vector<std::uint8_t> frame { 1, 2, 3 };
    CHECK(shareFrames(*unsized, frame) == Session::ShareEncoding::NothingToShare);
    CHECK(frame.empty());
}

TEST(theDaemonSocketIsTheBinarysLeafInTheControlDirectory)
{
    const char* previous = std::getenv("UBIQUITTY_HOME");
    const std::string saved = previous ? previous : "";
    ::setenv("UBIQUITTY_HOME", "/tmp/ubq-share-home", 1);

    // Any readable file has an identity; the leaf is a function of its bytes.
    const std::string binary = "/bin/sh";
    const std::string leaf = Session::socketLeafForBinary(binary.c_str());
    REQUIRE(!leaf.empty());
    CHECK(leaf.starts_with("host-") && leaf.ends_with(".sock") && leaf.size() == 26);
    CHECK_STR_EQ(Session::localDaemonSocketPath(binary), "/tmp/ubq-share-home/" + leaf);

    if (previous)
        ::setenv("UBIQUITTY_HOME", saved.c_str(), 1);
    else
        ::unsetenv("UBIQUITTY_HOME");
}

TEST(aDaemonAlreadyAnsweringIsNotStartedAgain)
{
    const std::string socketPath = "/tmp/ubq-share-" + std::to_string(::getpid()) + ".sock";
    ::unlink(socketPath.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath.c_str());
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 4) == 0);

    // A binary that cannot run: had the answer been to start a daemon, it
    // would have been false.
    CHECK_MSG(Session::ensureLocalDaemon("/nonexistent/ubiquitty-host-share", socketPath),
        "a daemon answering on its socket was started again");

    ::close(listener);
    ::unlink(socketPath.c_str());
}

TEST(aSocketNobodyAnswersIsNotTakenForADaemon)
{
    // A daemon that died leaves its socket file: the file is not the daemon.
    const std::string socketPath = "/tmp/ubq-share-stale-" + std::to_string(::getpid()) + ".sock";
    ::unlink(socketPath.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath.c_str());
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    ::close(listener);

    // And a start that fails is a failure, not a wait.
    CHECK_MSG(!Session::ensureLocalDaemon("/usr/bin/false", socketPath),
        "a stale socket, or a start that failed, was reported as a running daemon");
    CHECK(!Session::ensureLocalDaemon("", socketPath));
    CHECK(!Session::ensureLocalDaemon("/usr/bin/false", ""));

    ::unlink(socketPath.c_str());
}

/// The daemon a quit hands shells to is the binary's own, found by the same
/// leaf a share uses, and reached by the bridge that binary serves — or there
/// is none, and the quit hands nothing over.
TEST(aQuitHandsShellsToTheBinarysOwnDaemonOrToNone)
{
    const char* previous = std::getenv("UBIQUITTY_HOME");
    const std::string saved = previous ? previous : "";
    const std::string home = "/tmp/ubq-quit-" + std::to_string(::getpid());
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);

    // A binary that starts no daemon, and one with no identity: no target.
    const std::string binary = "/usr/bin/false";
    const Session::LocalDaemonTarget absent = Session::readyLocalDaemon(binary);
    CHECK_MSG(absent.socketPath.empty() && absent.socketName.empty() && absent.bridgeCommand.empty(),
        "a daemon that never answered was named as the quit's target");
    CHECK(Session::readyLocalDaemon("/nonexistent/ubiquitty-host-share").socketPath.empty());

    // The binary's daemon answering on its own socket.
    const std::string socketPath = Session::localDaemonSocketPath(binary);
    REQUIRE(!socketPath.empty());
    ::unlink(socketPath.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath.c_str());
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 4) == 0);

    const Session::LocalDaemonTarget target = Session::readyLocalDaemon(binary);
    CHECK_STR_EQ(target.socketPath, socketPath);
    CHECK_STR_EQ(target.socketName, Session::socketLeafForBinary(binary.c_str()));
    CHECK_STR_EQ(target.bridgeCommand, Session::localBridgeCommand("'/usr/bin/false'"));

    ::close(listener);
    ::unlink(socketPath.c_str());
    ::rmdir(home.c_str());
    if (previous)
        ::setenv("UBIQUITTY_HOME", saved.c_str(), 1);
    else
        ::unsetenv("UBIQUITTY_HOME");
}

TEST(aBinaryWithNoIdentityHasNoSocket)
{
    // Fail closed: no leaf means no socket, never a shared well-known one.
    CHECK(Session::localDaemonSocketPath("").empty());
    CHECK(Session::localDaemonSocketPath("/nonexistent/ubiquitty-host-share").empty());
    CHECK(Session::socketLeafForBinary(nullptr).empty());
}

UBIQUITTY_TEST_MAIN()

TEST(adoptionCarriesTheVisibleScreenAndTheLocalHistory)
{
    auto window = std::make_unique<Terminal>();
    hardReset(*window);
    resize(*window, 4, 40);
    for (int line = 0; line < 20; ++line) {
        processBytes(window.get(), "history line\r\n");
    }
    REQUIRE(window->lineStore && window->lineStore->count() > 0);
    processBytes(window.get(), "visible prompt");
    std::vector<std::uint8_t> frame;
    REQUIRE(shareFrames(*window, frame) == Session::ShareEncoding::Encoded);
    const auto adopt = decodeAdopt(frame);
    REQUIRE(adopt.has_value());
    const auto& daemon = adopt->continuation.terminal;
    REQUIRE(daemon->lineStore);
    CHECK_EQ(daemon->lineStore->count(), window->lineStore->count());
    for (int index = 0; index < window->lineStore->count(); ++index)
        CHECK(Screen::absoluteRowText(*daemon, index, 40) == Screen::absoluteRowText(*window, index, 40));
    // A corrupt joined record never produces a partially restored terminal.
    std::vector<std::uint8_t> forged = adopt->payload;
    forged.back() ^= 0x01;
    CHECK(!Protocol::Sync::decodeHandoverTerminal(forged, Session::localShareTerminalLimits(Local::maxAdoptBodyBytes)));
    for (int row = 0; row < 4; ++row) {
        CHECK(rowText(*daemon, row) == rowText(*window, row));
    }
}

TEST(aWindowPastWhatAShareCarriesIsRefusedWhole)
{
    const std::unique_ptr<Terminal> window = windowWithHistory(40, 400);
    std::vector<std::uint8_t> frames;
    REQUIRE(shareFrames(*window, frames, Local::maxAdoptBodyBytes) == Session::ShareEncoding::Encoded);
    const auto whole = decodeAdopt(frames);
    REQUIRE(whole.has_value());
    const std::size_t length = whole->payload.size();

    REQUIRE(shareFrames(*window, frames, length) == Session::ShareEncoding::Encoded);
    const auto atTheTotal = decodeAdopt(frames);
    REQUIRE(atTheTotal.has_value());
    CHECK(atTheTotal->payload == whole->payload);
    for (const std::size_t maxBodyBytes : { length - 1, length / 2, std::size_t { 16 } }) {
        frames = { 1, 2, 3 };
        CHECK(shareFrames(*window, frames, maxBodyBytes) == Session::ShareEncoding::TooLong);
        CHECK_MSG(frames.empty(), "a share past its total wrote frames");
    }
    // The person is told the limit.
    const std::string detail = Session::shareFailureDetail(Session::ShareOutcome::TooLongToShare);
    CHECK_MSG(detail.find(std::to_string(Local::maxAdoptBodyBytes / (1024 * 1024)) + " MiB") != std::string::npos, detail.c_str());
    CHECK(!Session::shareFailureTitle(Session::ShareOutcome::TooLongToShare).empty());
    CHECK(!Session::shareFailureDetail(Session::ShareOutcome::HandoffRefused).empty());
    CHECK(!Session::shareFailureDetail(Session::ShareOutcome::DaemonDidNotStart).empty());
    // A full host is named as full, apart from the generic refusal.
    const std::string full = Session::shareFailureDetail(Session::ShareOutcome::HostFull);
    CHECK_MSG(full.find("is full") != std::string::npos, full.c_str());
    CHECK(full != Session::shareFailureDetail(Session::ShareOutcome::HandoffRefused));
    CHECK(!Session::shareFailureTitle(Session::ShareOutcome::HostFull).empty());
    CHECK(Session::shareFailureDetail(Session::ShareOutcome::Shared).empty());
}

// The complete history is not limited to a single network history body's row count.
TEST(aHistoryOfMoreLinesThanOneBodyHoldsArrivesWhole)
{
    constexpr int lines = 70'000;
    auto window = std::make_unique<Terminal>();
    hardReset(*window);
    resize(*window, 4, 20);
    window->scrollbackLimit = lines + 100;
    for (int line = 0; line < lines; ++line)
        processBytes(window.get(), (std::to_string(line) + "\r\n").c_str());
    REQUIRE(window->lineStore && window->lineStore->count() > 0xFFFF);
    std::vector<std::uint8_t> frames;
    REQUIRE(shareFrames(*window, frames) == Session::ShareEncoding::Encoded);
    const auto adopt = decodeAdopt(frames);
    REQUIRE(adopt.has_value());
    const std::unique_ptr<Terminal> daemon = adopted(*adopt, 4, 20);
    REQUIRE(daemon != nullptr);
    CHECK_EQ(daemon->lineStore->count(), window->lineStore->count());
    CHECK_MSG(holdTheSame(*daemon, *window), "a history of several bodies did not arrive as the window holds it");

    const auto roundTrip = Protocol::Sync::encodeHandoverTerminal(*daemon, 1, Session::localShareTerminalLimits(Local::maxAdoptBodyBytes));
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(roundTrip->view(), adopt->payload));
}

// A share past its total refuses whole: it never carries the newest lines and
// drops the rest, and the window's history is left as it was.
TEST(aSharePastItsTotalRefusesWithoutDroppingHistory)
{
    const std::unique_ptr<Terminal> window = windowWithHistory(40, 400);
    std::vector<std::uint8_t> frames;
    REQUIRE(shareFrames(*window, frames, Local::maxAdoptBodyBytes) == Session::ShareEncoding::Encoded);
    const auto whole = decodeAdopt(frames);
    REQUIRE(whole.has_value());
    const std::unique_ptr<Terminal> all = adopted(*whole, 4, 40);
    REQUIRE(all != nullptr);
    CHECK_MSG(holdTheSame(*all, *window), "a handover within the total left lines or added one");
    const auto fullHistory = Protocol::Sync::encodeHandoverHistory(*window, Session::localShareTerminalLimits(Local::maxAdoptBodyBytes).history);
    REQUIRE(fullHistory && fullHistory->size > whole->payload.size() / 2);

    const auto originalRows = window->lineStore->count();
    const auto originalHistory = window->lineStore->continuation();
    for (const auto maxBodyBytes : { whole->payload.size() - 1, whole->payload.size() / 2 }) {
        frames = { 1, 2, 3 };
        CHECK(shareFrames(*window, frames, maxBodyBytes) == Session::ShareEncoding::TooLong);
        CHECK(frames.empty());
        CHECK_EQ(window->lineStore->count(), originalRows);
        const auto currentHistory = window->lineStore->continuation();
        CHECK_EQ(currentHistory.front.number, originalHistory.front.number);
        CHECK(currentHistory.front.continuity == originalHistory.front.continuity);
        CHECK_EQ(currentHistory.head, originalHistory.head);
        CHECK_EQ(currentHistory.maxHead, originalHistory.maxHead);
        CHECK_EQ(currentHistory.totalAppended, originalHistory.totalAppended);
    }
    REQUIRE(shareFrames(*window, frames) == Session::ShareEncoding::Encoded);
    const auto complete = decodeAdopt(frames);
    REQUIRE(complete);
    CHECK(complete->payload == whole->payload);
}

TEST(localShareLimitsSeparateEncodedBytesFromFiniteComponentPolicy)
{
    const auto policy = Session::localShareTerminalLimits(Local::maxAdoptBodyBytes);
    for (const auto bytes : { std::size_t { 0 }, std::size_t { 1000 }, Local::maxAdoptBodyBytes, std::numeric_limits<std::size_t>::max() }) {
        const auto limits = Session::localShareTerminalLimits(bytes);
        const auto encodedBytes = std::min(bytes, Local::maxAdoptBodyBytes);
        CHECK_EQ(limits.maxRecordBytes, encodedBytes);
        CHECK_EQ(limits.maxCheckpointBytes, encodedBytes);
        CHECK_EQ(limits.maxHostStateBytes, encodedBytes);
        CHECK_EQ(limits.maxLinkBytes, encodedBytes);
        CHECK_EQ(limits.maxParserBytes, encodedBytes);
        CHECK_EQ(limits.images.maxSectionBytes, encodedBytes);
        CHECK_EQ(limits.history.maxSectionBytes, encodedBytes);
        CHECK_EQ(limits.text.maxSectionBytes, encodedBytes);
        CHECK_EQ(limits.visual.maxSectionBytes, encodedBytes);
        CHECK_EQ(limits.sounds.maxSectionBytes, encodedBytes);
        CHECK_EQ(limits.maxScreenCellBytes, policy.maxScreenCellBytes);
        CHECK_EQ(limits.images.maxPixelBytes, Image::terminalImageBytesBudgetDefault);
        CHECK_EQ(limits.history.maxCapacity, policy.history.maxCapacity);
        CHECK_EQ(limits.history.maxRows, policy.history.maxRows);
        CHECK_EQ(limits.history.maxCells, policy.history.maxCells);
        CHECK_EQ(limits.text.maxClusterEntries, ClusterPool::maxEntries);
        CHECK_EQ(limits.text.maxRunSlots, RunTextStore::maxEntries);
        CHECK_EQ(limits.text.maxCodePoints, policy.text.maxCodePoints);
        CHECK_EQ(limits.visual.maxTekPrimitives, Tek::tekMaxPrimitives);
        CHECK_EQ(limits.sounds.maxPCMBytes, policy.sounds.maxPCMBytes);
    }
    CHECK_EQ(policy.maxScreenCellBytes, std::uint64_t { 2 } * maxRows * maxColumns * sizeof(Cell));
    CHECK_EQ(policy.history.maxCapacity, Local::maxAdoptBodyBytes / sizeof(StoredLine));
    CHECK_EQ(policy.history.maxCells, Local::maxAdoptBodyBytes / sizeof(Cell));
    CHECK_EQ(policy.text.maxCodePoints, std::uint64_t { ClusterPool::maxEntries } * ClusterPool::maxCodePoints + RunTextStore::maxLiveCodePoints);
    CHECK_EQ(policy.sounds.maxPCMBytes, std::uint64_t { VT::sampleSlotCount } * VT::sampleMaxFrames * sizeof(std::int16_t) + VT::PendingSound::maxPCMBytes);
}

TEST(aShareCarriesHostParserImagesAndTextContinuationWithoutMutatingTheSource)
{
    AnsweringPolicy answering;
    auto window = windowWithHistory(40, 24);
    window->capabilityPolicy = &answering;
    window->drawsSizedText = true;
    window->drawsProportionalText = true;
    UbiquittyTest::feed(*window, "\033]66;p=1:c=12;proportional text\007\r\n"
                                 "x\xcc\x81\xcc\x82\r\n"
                                 "\033]8;id=original;https://example.test/original\033\\linked\033]8;;\033\\"
                                 "\033_Ga=T,f=32,s=1,v=1,i=7,p=9,c=2,r=3,C=1,q=2;/wAA/w==\033\\"
                                 "\033_Ga=f,f=32,s=1,v=1,i=7,z=60,q=2;AP8A/w==\033\\"
                                 "\033_Ga=a,i=7,c=3,r=2,q=2;\033\\"
                                 "\033P0;0;0;8;0;0;8;0{@~~~~\033\\");
    auto& bank = ensureSampleBank(*window);
    bank.slots[0] = { -17, 0, 19 };
    bank.tags[0] = 77;
    bank.serial = 9;
    UbiquittyTest::feed(*window, "\033[4:1:6;1;1,~");
    bank.slots[0] = { 7, 8, 9 };
    bank.serial = 10;
    window->answerback = "share continuation";
    window->sequenceGate.oscDeny[0] = 2;
    window->sequenceGate.oscDenyCount = 1;
    window->currentCapabilityOrigin = CapabilityOrigin::Remote;
    window->osc52ClipboardReadPolicy = OSC::ClipboardReadPolicy::Deny;
    window->richTextBlocks.resumeSerials(731);
    UbiquittyTest::feed(*window, "\005x");
    REQUIRE(window->parser.clusterActive && window->clusterPool.size() > 0 && window->runTextStore.liveCodePoints() > 0);
    REQUIRE(window->softFont && window->sampleBank && window->pendingSound && window->pendingSound->front());
    REQUIRE(window->images.stored[0].frameCount == 2 && window->outputLength > 0);
    const auto limits = Session::localShareTerminalLimits(Local::maxAdoptBodyBytes);
    const auto before = Protocol::Sync::encodeHandoverTerminal(*window, 1, limits);
    REQUIRE(before);

    std::vector<std::uint8_t> frames;
    REQUIRE(shareFrames(*window, frames) == Session::ShareEncoding::Encoded);
    const auto adopt = decodeAdopt(frames);
    REQUIRE(adopt);
    CHECK(std::ranges::equal(before->view(), adopt->payload));
    auto& daemon = *adopt->continuation.terminal;
    CHECK(!daemon.capabilityPolicy.installed());
    daemon.capabilityPolicy = &answering;
    CHECK_EQ(daemon.richTextBlocks.nextSerial(), 731u);
    CHECK(daemon.parser.clusterActive);
    CHECK(daemon.currentCapabilityOrigin == CapabilityOrigin::Remote);
    CHECK(daemon.osc52ClipboardReadPolicy == OSC::ClipboardReadPolicy::Deny);
    REQUIRE(daemon.softFont && daemon.sampleBank && daemon.pendingSound && daemon.pendingSound->front());
    REQUIRE(daemon.pendingSound->front()->request.sampleFrames == 3 && daemon.pendingSound->front()->request.samplePCM);
    CHECK(daemon.softFont.get() != window->softFont.get());
    CHECK(daemon.pendingSound->front()->request.samplePCM != window->pendingSound->front()->request.samplePCM);
    CHECK_EQ(daemon.pendingSound->front()->request.samplePCM[0], -17);
    CHECK_EQ(daemon.sampleBank->slots[0][0], 7);
    CHECK(daemon.images.stored[0].pixels != window->images.stored[0].pixels);
    CHECK(daemon.images.stored[0].frames[1].pixels != window->images.stored[0].frames[1].pixels);
    const auto restored = Protocol::Sync::encodeHandoverTerminal(daemon, 1, limits);
    const auto unchanged = Protocol::Sync::encodeHandoverTerminal(*window, 1, limits);
    REQUIRE(restored && unchanged);
    CHECK(std::ranges::equal(restored->view(), before->view()));
    CHECK(std::ranges::equal(unchanged->view(), before->view()));

    for (auto* terminal : { window.get(), &daemon }) {
        UbiquittyTest::feed(*terminal, "\xcc\x81\033[b\r\n\033]2;blocked\007\005"
                                       "\033]8;;https://example.test/new\033\\new\033]8;;\033\\"
                                       "\033[?1049halternate\033[?1049l"
                                       "\033]66;p=1:c=3;next run\007x");
        Image::advanceFrames(terminal, 150);
    }
    CHECK_EQ(window->outputLength, daemon.outputLength);
    REQUIRE(window->outputLength >= 0 && daemon.outputLength >= 0);
    CHECK(std::ranges::equal(std::span(window->outputBuffer).first(static_cast<std::size_t>(window->outputLength)), std::span(daemon.outputBuffer).first(static_cast<std::size_t>(daemon.outputLength))));
    const auto futureWindow = Protocol::Sync::encodeHandoverTerminal(*window, 1, limits);
    const auto futureDaemon = Protocol::Sync::encodeHandoverTerminal(daemon, 1, limits);
    REQUIRE(futureWindow && futureDaemon);
    CHECK(std::ranges::equal(futureWindow->view(), futureDaemon->view()));
}

TEST(aShareRefusesIncompleteTerminalWorkWithoutFlushingIt)
{
    for (const auto bytes : { "\033[12;", "\xf0\x9f" }) {
        auto window = windowWithHistory(20, 8);
        UbiquittyTest::feed(*window, bytes);
        const auto state = window->parser.parseState;
        const auto needed = window->parser.utf8.need;
        const auto cursorX = window->screens.active->cursorX;
        const auto cursorY = window->screens.active->cursorY;
        std::vector<std::uint8_t> frames { 1, 2, 3 };
        CHECK(shareFrames(*window, frames) == Session::ShareEncoding::NothingToShare);
        CHECK(frames.empty());
        CHECK(window->parser.parseState == state);
        CHECK_EQ(window->parser.utf8.need, needed);
        CHECK_EQ(window->screens.active->cursorX, cursorX);
        CHECK_EQ(window->screens.active->cursorY, cursorY);
    }
    auto window = windowWithHistory(20, 8);
    window->images.kittyChunkActive = true;
    std::vector<std::uint8_t> frames { 1, 2, 3 };
    CHECK(shareFrames(*window, frames) == Session::ShareEncoding::NothingToShare);
    CHECK(frames.empty());
    CHECK(window->images.kittyChunkActive);
}

TEST(localShareRefusesThinCheckpointPayloadsAndReceiverPolicyMismatches)
{
    auto window = windowWithHistory(20, 8);
    Protocol::Sync::ScreenPublisher publisher;
    Protocol::Sync::AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    REQUIRE(publisher.capture(*window) && publisher.checkpoint(*window, 1, checkpoint, baseline));
    // A screen-plane checkpoint is not a whole terminal: a thin body is refused.
    CHECK(!decodeAdopt(checkpoint));
    std::vector<std::uint8_t> frames;

    REQUIRE(shareFrames(*window, frames) == Session::ShareEncoding::Encoded);
    const auto complete = decodeAdopt(frames);
    REQUIRE(complete);
    CHECK(!Protocol::Sync::decodeHandoverTerminal(complete->payload, Session::localShareTerminalLimits(complete->payload.size() - 1)));
    window->images.kittyImageBytesBudget = Image::terminalImageBytesBudgetDefault + 1;
    frames = { 1, 2, 3 };
    CHECK(shareFrames(*window, frames) == Session::ShareEncoding::TooLong);
    CHECK(frames.empty());
    CHECK_EQ(window->images.kittyImageBytesBudget, Image::terminalImageBytesBudgetDefault + 1);
    CHECK(Session::shareFailureDetail(Session::ShareOutcome::TooLongToShare).find("Clear") == std::string::npos);
}

TEST(aShareRefusesOutOfRangeGeometryAndNegativeHistoryWithoutNarrowing)
{
    auto window = windowWithHistory(20, 8);
    const auto rows = window->rows;
    const auto columns = window->columns;
    std::vector<std::uint8_t> frames { 1, 2, 3 };
    window->rows = maxRows + 1;
    CHECK(shareFrames(*window, frames) == Session::ShareEncoding::NothingToShare);
    CHECK(frames.empty());
    CHECK_EQ(window->rows, maxRows + 1);
    window->rows = rows;
    window->columns = maxColumns + 1;
    frames = { 1, 2, 3 };
    CHECK(shareFrames(*window, frames) == Session::ShareEncoding::NothingToShare);
    CHECK(frames.empty());
    CHECK_EQ(window->columns, maxColumns + 1);
    window->columns = columns;
    const auto limit = window->scrollbackLimit;
    window->scrollbackLimit = -1;
    frames = { 1, 2, 3 };
    CHECK(shareFrames(*window, frames) == Session::ShareEncoding::NothingToShare);
    CHECK(frames.empty());
    CHECK_EQ(window->scrollbackLimit, -1);
    window->scrollbackLimit = limit;
}

TEST(localShareEncodingPreservesTheOrdinaryBoundAndNearLimitFixturePremises)
{
    struct SizeCase {
        int columns;
        int rows;
        int lines;
        std::size_t minimumPayloadBytes;
        std::size_t maximumPayloadBytes;
    };
    constexpr std::size_t mebibyte = std::size_t { 1 } << 20;
    // Exercise the real complete-record producer without opening a PTY or
    // running the Host fixtures whose ordinary-bound and near-limit premises these are.
    for (const auto each : {
             SizeCase { 10, 4, 12'000, 1, Protocol::Sync::maxClientFramePayloadLength },
             SizeCase { 400, 4, 1'000, Protocol::Sync::maxClientFramePayloadLength + 1, Local::maxAdoptBodyBytes },
             SizeCase { 400, 24, 80, mebibyte / 2, Protocol::Sync::maxClientFramePayloadLength },
             SizeCase { 400, 24, 640, Protocol::Sync::maxClientFramePayloadLength + 1, 10 * mebibyte },
             SizeCase { 400, 24, 2'800, 34 * mebibyte, Local::maxAdoptBodyBytes },
         }) {
        auto window = windowWithHistory(each.columns, each.lines, each.rows);
        std::vector<std::uint8_t> frames;
        REQUIRE(shareFrames(*window, frames) == Session::ShareEncoding::Encoded);
        const auto adopt = decodeAdopt(frames);
        REQUIRE(adopt);
        CHECK(adopt->payload.size() >= each.minimumPayloadBytes);
        CHECK(adopt->payload.size() <= each.maximumPayloadBytes);
        CHECK_EQ(adopt->continuation.terminal->rows, each.rows);
        CHECK_EQ(adopt->continuation.terminal->columns, each.columns);
        CHECK(holdTheSame(*window, *adopt->continuation.terminal));
    }
}

// A shared window's panes ride one carrier only while every PTY went to the
// same daemon process and is reached through the same bridge. A share into
// another daemon, or reached another way, is another authority; and a pane is
// never another endpoint of its own carrier.
TEST(aCommittedShareRidesAnothersCarrierOnlyUnderTheSameDaemonAndBridge)
{
    const Session::AdoptedSession anchor { .sessionID = 42, .daemonInstanceID = 17 };
    const std::string bridge = "'/Applications/Ubiquitty.app/Contents/Helpers/ubiquitty-host' --attach";
    const std::string otherBridge = "'/opt/ubiquitty/ubiquitty-host' --attach";
    CHECK(Session::isSameLocalShareAuthority(bridge, anchor, bridge, { .sessionID = 43, .daemonInstanceID = 17 }));
    CHECK(!Session::isSameLocalShareAuthority(bridge, anchor, bridge, { .sessionID = 43, .daemonInstanceID = 23 }));
    CHECK(!Session::isSameLocalShareAuthority(bridge, anchor, otherBridge, { .sessionID = 43, .daemonInstanceID = 17 }));
    CHECK(!Session::isSameLocalShareAuthority(bridge, anchor, bridge, anchor));
    CHECK(!Session::isSameLocalShareAuthority(bridge, anchor, bridge, { .sessionID = 0, .daemonInstanceID = 17 }));
    CHECK(!Session::isSameLocalShareAuthority(bridge, { .sessionID = 42, .daemonInstanceID = 0 }, bridge, { .sessionID = 43, .daemonInstanceID = 0 }));
    CHECK(Session::isSameLocalShareAuthority({}, anchor, {}, { .sessionID = 43, .daemonInstanceID = 17 }));
}
