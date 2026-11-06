// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/smoke_client.cpp — a minimal Sync-protocol client that
// drives the ubiquitty-host PERSISTENCE smoke test.
//
// It is not the macOS app: it is a tiny headless stand-in that speaks the
// same wire protocol over the same transport (`ubiquitty-host --attach`'s
// stdio, which the daemon bridges to the session). It is spawned by
// Tests/Host/smoke.sh.
//
// Subcommands:
//
//   probe          Attach, Greet(columns,rows), read frames until the first
//                  checkpoint, print "ROWSxCOLS cursor=R,C title='...'
//                  cell00=<cp>" and a one-line dump of the cursor row's
//                  text, then exit 0. Optionally write INPUT bytes first
//                  (--input "uname\r") and wait a beat so the shell runs
//                  them before reading the screen.
//
//   expect <substr>  Like probe, but keep reading updates for up to a
//                  few seconds until the visible grid contains <substr>;
//                  exit 0 if found, 1 otherwise. Used to confirm a
//                  command's output reached the screen, and — on reattach
//                  — that the same session still shows it (persistence).
//
// The transport is stdin/stdout. Tests/Host/smoke.sh wires those file
// descriptors to `ubiquitty-host --attach`, so the test exercises the full
// attach bridge exactly as the real macOS client would over ssh.
//
// The screen is the Sync screen plane's own client end, so the assertion is
// the real encode/apply path, not a hand-rolled parser.
//

#include "Core/CellGrid.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/HostListing.h"
#include "Core/TerminalOperations.h"
#include "ScreenClient.h"
#include <memory>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace Ubiquitty;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

void writeAll(int fileDescriptor, const uint8_t* p, std::size_t n)
{
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fileDescriptor, p + off, n - off);
        if (w > 0) {
            off += static_cast<std::size_t>(w);
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        break;
    }
}

// Pull the visible grid out of an applied passive Ubiquitty::Terminal into a flat string
// (row-major, trailing blanks trimmed per row, rows joined by '\n').
std::string gridText(const Ubiquitty::Terminal& t)
{
    std::string out;
    int rows = t.rows, columns = t.columns;
    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (int c = 0; c < columns; ++c) {
            const Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            uint32_t ch = (cell && cell->codePoint) ? cell->codePoint : ' ';
            if (ch < 0x20 || ch > 0x7E)
                ch = (ch == 0) ? ' ' : '?';
            line.push_back(static_cast<char>(ch));
        }
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out += line;
        out.push_back('\n');
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    // argv: smoke_client <probe|expect> [--input STR] [--columns N] [--rows N]
    //                    [--wait-ms N] [--session ID --daemon ID] [--detach] [--until-screen]
    //                    [<expect-substr>]
    // --detach ends with a normal Bye, as a closing window does, instead of a
    // dropped transport the host holds for a resume. --until-screen leaves as
    // soon as the first checkpoint has applied, rather than at the deadline.
    const char* cmd = (argc > 1) ? argv[1] : "probe";
    const char* inputString = nullptr;
    const char* expectSub = nullptr;
    int columns = 80, rows = 24, waitMs = 600;
    uint32_t sessionID = 0; // 0 = create a new session; nonzero = reattach
    std::uint64_t daemonInstanceID = 0;
    bool shouldDetach = false;
    bool shouldLeaveAtScreen = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            inputString = argv[++i];
        else if (std::strcmp(argv[i], "--columns") == 0 && i + 1 < argc)
            columns = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc)
            waitMs = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--session") == 0 && i + 1 < argc)
            sessionID = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--daemon") == 0 && i + 1 < argc)
            daemonInstanceID = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--detach") == 0)
            shouldDetach = true;
        else if (std::strcmp(argv[i], "--until-screen") == 0)
            shouldLeaveAtScreen = true;
        else
            expectSub = argv[i];
    }
    if (sessionID && !daemonInstanceID) {
        std::fprintf(stderr, "smoke: reattach requires the original --daemon pin\n");
        return 1;
    }

    // The screen plane's client end; its pane replica is the screen read.
    // A client instance of this process's own: a pane numbers a client's
    // input by its instance, and a second run that reattaches is another
    // client, whose first event is not a repeat of the first run's.
    UbiquittyTest::ScreenClient screen(rows, columns, Ubiquitty::Protocol::Sync::ID128 { .low = static_cast<std::uint64_t>(::getpid()), .high = UbiquittyTest::screenClientNow() });
    // What the status lines read before any checkpoint has applied.
    const Ubiquitty::Terminal blank;
    const auto term = [&]() -> const Ubiquitty::Terminal* { return screen.pane() ? screen.pane() : &blank; };

    // One explicit canonical request. sessionID 0 creates a NEW session; a
    // nonzero --session reattaches that session (the persistence test passes
    // the id from the first attach to the reattach).
    Sync::AdmissionRequest request;
    request.target = Sync::AdmissionTarget::Session;
    request.sessionID = sessionID;
    request.expectedDaemonInstanceID = daemonInstanceID;
    request.isAttachOnly = sessionID != 0;
    if (!screen.start(std::move(request))) {
        std::fprintf(stderr, "smoke: the canonical greeting was not encoded\n");
        return 1;
    }
    const std::vector<uint8_t> greeting = screen.takeUpstream(UbiquittyTest::screenClientNow());
    writeAll(STDOUT_FILENO, greeting.data(), greeting.size());

    // Optional keystrokes (e.g. a command + CR): Raw input on the pane, owed
    // until the host has bound it.
    bool isInputOwed = inputString && inputString[0];

    Session::HostListing listing("smoke.list");
    bool didSubscribe = false;

    int inputFileDescriptor = STDIN_FILENO;
    int fl = ::fcntl(inputFileDescriptor, F_GETFL, 0);
    if (fl != -1)
        ::fcntl(inputFileDescriptor, F_SETFL, fl | O_NONBLOCK);

    bool gotAck = false;
    uint32_t ackSessionID = 0; // the daemon's id for our session (for reattach)
    bool gotScreen = false;
    int sessionListCount = -1; // Complete typed directory rows, or no response yet.
    int statusCount = 0; // Revisions of canonical foreground records received.
    std::uint64_t factsRevision = 0;
    int commandDoneCount = 0; // validated typed command-completion records
    std::string lastCommandLine; // OSC 633;E context from the latest completion
    bool sentReadRegion = false; // typed capture issued once
    bool regionResponded = false; // complete typed Result received
    std::string regionText; // text returned by the region read
    std::string title;
    std::string lastGrid;

    // Deadline: enough to let the shell echo a prompt / run a command.
    const long deadlineMs = (expectSub != nullptr) ? 4000 : (waitMs + 2000);
    long elapsed = 0;
    const int stepMs = 50;

    char buffer[16384];
    while (elapsed < deadlineMs) {
        struct pollfd pf;
        pf.fd = inputFileDescriptor;
        pf.events = POLLIN;
        pf.revents = 0;
        int r = ::poll(&pf, 1, stepMs);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0) {
            elapsed += stepMs;
            continue;
        }
        if (pf.revents & POLLIN) {
            ssize_t n = ::read(inputFileDescriptor, buffer, sizeof(buffer));
            if (n <= 0) {
                if (n == 0)
                    break; // daemon/relay closed
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                break;
            }
            const std::span<const uint8_t> received(reinterpret_cast<const uint8_t*>(buffer), static_cast<std::size_t>(n));
            screen.read(received, UbiquittyTest::screenClientNow());
            if (!didSubscribe && screen.connection().welcome()) {
                if (!screen.call(Session::HostListing::subscribeRequest("smoke.list")))
                    break;
                didSubscribe = true;
            }
            for (const auto& command : screen.takeCompletedCommands()) {
                ++commandDoneCount;
                if (!command.empty())
                    lastCommandLine = command;
            }
            if (isInputOwed && screen.pane()) {
                const std::span<const uint8_t> typed(reinterpret_cast<const uint8_t*>(inputString), std::strlen(inputString));
                if (screen.sendRaw(typed).has_value())
                    isInputOwed = false;
            }
            if (screen.generation() != 0) {
                gotScreen = true;
                lastGrid = gridText(*screen.pane());
            }
            const std::vector<uint8_t> upstream = screen.takeUpstream(UbiquittyTest::screenClientNow());
            writeAll(STDOUT_FILENO, upstream.data(), upstream.size());
            if (const auto welcome = screen.welcome()) {
                gotAck = welcome->admission == Sync::AttachmentAdmission::Granted;
                ackSessionID = welcome->selection.sessionID;
                daemonInstanceID = welcome->selection.daemonInstanceID;
            }
            if (const auto* facts = screen.connection().paneFacts(1); facts && facts->revision != factsRevision) {
                factsRevision = facts->revision;
                title = facts->programTitle;
                ++statusCount;
            }
            for (const auto& text : screen.takeResults()) {
                (void)listing.noteAnswer(text, 0);
                bool isParsed = false;
                const auto response = Ubiquitty::Codec::JSON::parse(text, isParsed);
                const auto* result = response.find("result");
                if (isParsed && response.numberOr("id") == 2 && result && result->boolOr("complete")) {
                    regionResponded = true;
                    regionText = result->stringOr("text");
                }
            }
            if (sessionListCount < 0 && listing.isListing()
                && listing.refresh(screen.connection(), UbiquittyTest::screenClientNow(), 0) != Session::HostListing::Change::Malformed
                && listing.isWhole()) {
                sessionListCount = static_cast<int>(Session::sessionRows(listing.tree()).size());
                if (!screen.call(Session::HostListing::unsubscribeRequest("smoke.list.end")))
                    break;
                const auto request = screen.takeUpstream(UbiquittyTest::screenClientNow());
                writeAll(STDOUT_FILENO, request.data(), request.size());
            }
            if (screen.connection().state() == Sync::ConnectionState::Closed) {
                std::fprintf(stderr, "smoke: the canonical connection closed\n");
                break;
            }
            if (shouldLeaveAtScreen && gotAck && gotScreen)
                break;
        }
        if (pf.revents & (POLLHUP | POLLERR)) {
            // Drain anything buffered, then stop.
            break;
        }

        if (expectSub) {
            if (gotScreen && lastGrid.find(expectSub) != std::string::npos) {
                // Marker is on the live grid; verify pane.capture returns it too.
                if (!sentReadRegion) {
                    if (!screen.call(R"({"jsonrpc":"2.0","id":2,"method":"pane.capture","params":{"pane":1,"source":"rows","start":0,"end":1000}})"))
                        break;
                    const auto request = screen.takeUpstream(UbiquittyTest::screenClientNow());
                    writeAll(STDOUT_FILENO, request.data(), request.size());
                    sentReadRegion = true;
                }
                if (regionResponded)
                    break; // marker found on the grid and in the region read
            }
        }
    }

    if (shouldDetach) {
        screen.detach();
        const std::vector<uint8_t> bye = screen.takeUpstream(UbiquittyTest::screenClientNow());
        writeAll(STDOUT_FILENO, bye.data(), bye.size());
    }

    int rc = 0;
    if (!gotAck || !gotScreen) {
        std::fprintf(stderr, "smoke: incomplete handshake (ack=%d screen=%d)\n", gotAck ? 1 : 0, gotScreen ? 1 : 0);
        rc = 1;
    }

    if (std::strcmp(cmd, "expect") == 0 && expectSub != nullptr) {
        bool found = (lastGrid.find(expectSub) != std::string::npos);
        std::fprintf(stderr, "smoke: expect '%s' -> %s\n", expectSub, found ? "FOUND" : "MISSING");
        if (!found)
            rc = 1;
    }

    // The daemon's session id, so the harness can reattach the same session.
    std::fprintf(stderr, "smoke: sessionID=%u\n", ackSessionID);
    std::fprintf(stderr, "smoke: daemonInstanceID=%llu\n", static_cast<unsigned long long>(daemonInstanceID));
    // Always print a compact status line on stderr for the shell harness.
    const Cell* c00 = Ubiquitty::Screen::visibleCell(term(), 0, 0, 0);
    const int regionHasMarker = (expectSub && regionText.find(expectSub) != std::string::npos) ? 1 : 0;
    const std::size_t updateCount = screen.connection().counters().updatesApplied;
    std::fprintf(stderr, "smoke: %dx%d cursor=%d,%d title='%s' cell00=%u updates=%zu sessions=%d statuses=%d cmddone=%d region=%zu/%d\n", term()->rows, term()->columns, term()->screens.active->cursorY, term()->screens.active->cursorX, title.c_str(), (c00 ? c00->codePoint : 0u), updateCount, sessionListCount, statusCount,
        commandDoneCount, regionText.size(), regionHasMarker);
    const auto* program = term()->programStatus.primary();
    std::fprintf(stderr, "smoke: activity context='%s' app='%s'\n",
        term()->contexts.active.empty() ? "" : term()->contexts.active.back().id.c_str(),
        program ? program->app.c_str() : "");
    // The explicit command line (OSC 633;E) propagated over Sync (capabilityCommandLine).
    std::fprintf(stderr, "smoke: cmdline='%s'\n", lastCommandLine.c_str());
    // First non-empty grid line, for eyeballing the prompt.
    {
        std::size_t nl = lastGrid.find('\n');
        std::string firstLine = (nl == std::string::npos) ? lastGrid : lastGrid.substr(0, nl);
        std::fprintf(stderr, "smoke: row0='%s'\n", firstLine.c_str());
    }

    return rc;
}
