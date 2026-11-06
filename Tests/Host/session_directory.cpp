// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/session_directory.cpp — the portable Session::SessionDirectoryCore
// against the REAL ubiquitty-host daemon over a REAL Transport::LocalSocketChannel:
// what a person is shown before anything attaches, and what ending a row does.
//
// Two sessions are created by explicit canonical Greetings
// so the directory has something to find that it did not create itself. Then the
// class under test lists them, calls the host over the Sync connection its
// errand is, ends one, lists again, and — with its token cleared — is refused
// by a daemon that requires one.
//
// argv[1] = the built ubiquitty-host binary. Exit 0 on success.
//
// With `--watch-silence` after it, the run is one case instead and takes 70
// seconds of wall time: a watch held against a real daemon on which nothing
// happens for more than twice the daemon's silence bound. The daemon probes a
// connection it has nothing to say to and drops one that does not answer, so
// a watch that serviced its connection only when bytes arrived died here.
//

#include "Core/App/SessionOutlineText.h"
#include "Core/Codec/JSON.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/HostDirectoryClient.h"
#include "Core/Runtime/HostOutline.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/SessionTree.h"
#include "Core/Transport/LocalSocketChannel.h"
#include "Host/HostConnection.h"
#include "Platform/Host/HostSocketName.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Ubiquitty;
using Ubiquitty::HostTest::Connection;

namespace {

constexpr std::uint8_t fullScope = 0x1F; // observe|drive|control|regionRead|newSession

int g_failures = 0;
void check(bool ok, const char* what) // bool-ok: the assertion's verdict, which is the argument this helper exists to take
{
    if (!ok) {
        std::fprintf(stderr, "session_directory: FAIL: %s\n", what);
        ++g_failures;
    }
}

// The dispatcher the core marshals onto. Delays are honoured rather than fired
// at once: the terminate grace exists to separate an ungated destroy from a held
// one, and a dispatcher that runs it immediately answers before the host has.
class TestDispatcher final : public Session::Dispatcher {
public:
    void post(Session::PostedWork work) override { enqueue(std::chrono::steady_clock::now(), std::move(work)); }
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        enqueue(std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0)), std::move(work));
    }

    // Run every piece of work whose time has come, in the order it was posted.
    void pump()
    {
        for (;;) {
            Session::PostedWork work;
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                const auto now = std::chrono::steady_clock::now();
                std::size_t index = m_queue.size();
                for (std::size_t i = 0; i < m_queue.size(); ++i) {
                    if (m_queue[i].due <= now) {
                        index = i;
                        break;
                    }
                }
                if (index == m_queue.size())
                    return;
                work = std::move(m_queue[index].work);
                m_queue.erase(m_queue.begin() + static_cast<long>(index));
            }
            std::move(work)();
        }
    }

private:
    struct Entry {
        std::chrono::steady_clock::time_point due;
        Session::PostedWork work;
    };
    void enqueue(std::chrono::steady_clock::time_point due, Session::PostedWork work)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back({ due, std::move(work) });
    }
    std::mutex m_mutex;
    std::vector<Entry> m_queue;
};

// One AF_UNIX carrier per errand, to the daemon this test spawned.
class LocalSocketFactory final : public Session::ChannelFactory {
public:
    explicit LocalSocketFactory(std::string socketPath)
        : m_socketPath(std::move(socketPath))
    {
    }
    void createChannel(Session::TransportMode, Done done) override
    {
        done(std::make_unique<Transport::LocalSocketChannel>(m_socketPath, Transport::Origin { Transport::OriginKind::Daemon, "local" }), false);
    }
    bool pinDaemonSocket(std::string_view socketName) override
    {
        return socketName == std::string_view(m_socketPath).substr(m_socketPath.find_last_of('/') + 1);
    }

private:
    std::string m_socketPath;
};

class RecordingDirectory final : public Session::SessionDirectoryObserver {
public:
    void onSessionList(const std::vector<Ubiquitty::Session::SessionRow>& sessions) override
    {
        m_sessions = sessions;
        ++m_lists;
    }
    void onDirectoryFailed(const Session::DirectoryFailureReport& report) override
    {
        m_failureReason = Session::listingFailureNotice(report, Session::NoticePlacement::Embedded);
        m_failure = report.failure;
        ++m_failures;
    }
    void onCallAnswered(const std::string& response) override
    {
        m_answer = response;
        ++m_answers;
    }
    void onSessionTree(const Session::SessionTree& tree) override
    {
        m_tree = tree;
        m_listsBeforeTree = m_lists;
        ++m_trees;
    }
    void onWatchInterrupted(const Session::DirectoryFailureReport& report) override
    {
        m_failureReason = Session::listingFailureNotice(report, Session::NoticePlacement::Embedded);
        ++m_interruptions;
    }

    /// The pane a session draws into, wherever the newest tree has it.
    [[nodiscard]] const Session::TreePane* pane(std::uint32_t sessionID) const
    {
        for (const Session::TreeWorkspace& workspace : m_tree.workspaces)
            for (const Session::TreeWindow& window : workspace.windows)
                for (const Session::TreeTab& tab : window.tabs)
                    for (const Session::TreePane& pane : tab.panes)
                        if (pane.sessionID == sessionID)
                            return &pane;
        return nullptr;
    }
    void onTerminateSettled(std::uint32_t sessionID, Session::TerminateOutcome outcome) override
    {
        m_terminatedSessionID = sessionID;
        m_outcome = outcome;
        ++m_terminates;
    }

    [[nodiscard]] const Ubiquitty::Session::SessionRow* row(std::uint32_t sessionID) const
    {
        for (const auto& session : m_sessions)
            if (session.id == sessionID)
                return &session;
        return nullptr;
    }

    std::vector<Ubiquitty::Session::SessionRow> m_sessions;
    std::string m_failureReason;
    Session::ListingFailure m_failure { Session::ListingFailure::None };
    std::uint32_t m_terminatedSessionID = 0;
    Session::TerminateOutcome m_outcome = Session::TerminateOutcome::Failed;
    std::string m_answer;
    Session::SessionTree m_tree;
    int m_trees = 0;
    int m_interruptions = 0;
    int m_listsBeforeTree = -1;
    int m_answers = 0;
    int m_lists = 0;
    int m_failures = 0;
    int m_terminates = 0;
};

// Pump the dispatcher until `pred` holds or the budget runs out.
template<class Pred> bool pumpUntil(TestDispatcher& dispatcher, int milliseconds, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        dispatcher.pump();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    dispatcher.pump();
    return pred();
}

// A fresh creation may be unpinned; subsequent creations retain the original
// receipt's daemon. Every receipt must name this fixture's actual socket.
std::uint32_t createSession(Connection& connection, const std::string& socketPath, const std::string& token,
    std::uint64_t daemonInstance = 0)
{
    connection.fileDescriptor = HostTest::connectDaemon(socketPath);
    if (connection.fileDescriptor < 0)
        return 0;
    namespace Sync = Protocol::Sync;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = static_cast<std::uint64_t>(connection.fileDescriptor) + 1, .high = 0x646972656374 };
    greeting.clientKind = Sync::ClientKind::Agent;
    greeting.requestedRole = Sync::Role::Agent;
    greeting.request = { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = daemonInstance };
    greeting.credential.emplace(token.begin(), token.end());
    greeting.facts.viewportColumns = 80;
    greeting.facts.viewportRows = 24;
    std::string body;
    if (!HostTest::sendGreeting(connection, std::move(greeting))
        || !HostTest::waitFor(connection, Sync::FrameKind::Welcome, body, 3000) || !connection.welcome
        || connection.welcome->admission != Sync::AttachmentAdmission::Granted
        || connection.welcome->selection.target != Sync::AdmissionTarget::Session
        || !connection.welcome->selection.daemonInstanceID || !connection.welcome->selection.sessionID
        || connection.welcome->selection.socketName != std::string_view(socketPath).substr(socketPath.find_last_of('/') + 1)
        || !HostTest::waitForPane(connection))
        return 0;
    return connection.welcome->selection.sessionID;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    // The daemon drops a session's clients as it destroys the session, so a
    // write racing that close is an error to handle rather than a signal.
    ::signal(SIGPIPE, SIG_IGN);

    char homeTemplate[] = "/tmp/ubiquitty-sessdir.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string socketPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(argv[1]);
    ::mkdir(macHome.c_str(), 0700);

    const std::string token = "directorytokendirectorytokendirectory";
    {
        const std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 directory\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(fullScope));
        std::fclose(file);
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::setenv("UBIQUITTY_REQUIRE_TOKEN", "1", 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        ::_exit(127);
    }

    check(HostTest::waitForDaemonReady(socketPath), "the daemon accepted a connection");

    Connection first;
    Connection second;
    const std::uint32_t firstSession = createSession(first, socketPath, token);
    const auto daemonInstance = first.welcome ? first.welcome->selection.daemonInstanceID : 0;
    const std::uint32_t secondSession = firstSession && daemonInstance ? createSession(second, socketPath, token, daemonInstance) : 0;
    check(firstSession != 0 && secondSession != 0 && firstSession != secondSession, "two distinct sessions were created to be found");

    TestDispatcher dispatcher;
    LocalSocketFactory factory(socketPath);
    RecordingDirectory observer;
    Session::SessionDirectoryCore directory(&dispatcher, &factory, &observer);
    directory.setAuthToken(token);

    const auto endDaemon = [&] {
        if (first.fileDescriptor >= 0)
            ::close(first.fileDescriptor);
        if (second.fileDescriptor >= 0)
            ::close(second.fileDescriptor);
        ::kill(daemon, SIGTERM);
        for (int i = 0; i < 200; ++i) {
            if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
                break;
            ::usleep(10 * 1000);
        }
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, WNOHANG);
    };
    if (!firstSession || !secondSession || firstSession == secondSession || !daemonInstance) {
        endDaemon();
        return 1;
    }

    const char* summary = "session_directory: SessionDirectoryCore over a local socket — list, terminate, re-list, and a tokenless refusal — all passed";
    if (argc > 2 && std::string_view(argv[2]) == "--watch-silence") {
        // ---- A watch outlasts a host with nothing to say. ----
        check(directory.watch(), "a watch starts");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.m_trees > 0; }), "the watch reported the host's tree");
        check(observer.pane(firstSession) != nullptr && observer.pane(secondSession) != nullptr, "the watched tree holds both sessions");
        const int treesBefore = observer.m_trees;
        // Seventy seconds in which nothing on the host changes: the two
        // sessions sit at their prompts and no client types. More than twice
        // the thirty seconds the daemon lets a connection stay silent.
        const bool didEndEarly = pumpUntil(dispatcher, 70000, [&] { return observer.m_failures > 0 || observer.m_interruptions > 0 || !directory.isWatching(); }); // bool-ok: a local reading
        check(!didEndEarly, "the watch ended or lost its host during the host's silence");
        if (didEndEarly)
            std::fprintf(stderr, "    the watch said: %s\n", observer.m_failureReason.c_str());
        check(directory.isWatching() && directory.isWatchLive(), "the watch is not following the host after the silence");
        check(observer.m_failures == 0 && observer.m_interruptions == 0, "the watch reported a failure or a drop");
        // The connection it held is the one it still holds, and it works: a
        // session made now reaches the watch without a new dial.
        Connection third;
        const std::uint32_t thirdSession = createSession(third, socketPath, token, daemonInstance);
        check(thirdSession != 0, "a session was created after the silence");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.pane(thirdSession) != nullptr; }), "the watch did not report a session made after the silence");
        check(observer.m_trees > treesBefore && observer.m_interruptions == 0, "the tree arrived by a redial, not on the connection held");
        directory.stopWatching();
        if (third.fileDescriptor >= 0)
            ::close(third.fileDescriptor);
        summary = "directory_watch_silence: a watch held through 70 s of a real daemon's silence and still following — passed";
    } else {

        // A shell still starting changes its directory and its program
        // from one read to the next. What follows compares two readers of
        // the same tree, so it waits for two reads a moment apart to agree.
        const char* ubqBinary = argc > 3 && std::string_view(argv[2]) == "--ubq" ? argv[3] : nullptr;
        check(ubqBinary != nullptr, "the stanza names the ubq whose tree is compared");
        const std::string configHome = std::string(home) + "/config";
        ::mkdir(configHome.c_str(), 0700);
        ::mkdir((configHome + "/ubiquitty").c_str(), 0700);
        if (FILE* file = std::fopen((configHome + "/ubiquitty/config.toml").c_str(), "w")) {
            std::fputs("[title]\ntab_dimensions = true\n", file);
            std::fclose(file);
        }
        ::setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
        const std::string treeCommand = "UBIQUITTY_HOME='" + macHome + "' UBIQUITTY_AGENT_TOKEN='" + token + "' XDG_CONFIG_HOME='" + configHome + "' '" + (ubqBinary ? ubqBinary : "false") + "' --json tree";
        // What `ubq tree` prints, and whether it exited zero.
        const auto readTree = [&](std::string& printed) {
            printed.clear();
            FILE* pipe = ::popen(treeCommand.c_str(), "r");
            if (!pipe)
                return false;
            char buffer[4096];
            while (const std::size_t got = std::fread(buffer, 1, sizeof(buffer), pipe))
                printed.append(buffer, got);
            return ::pclose(pipe) == 0;
        };
        // What a row says but how long ago, which each read measures at its
        // own moment.
        static constexpr std::array comparedMembers { "level", "depth", "id", "folded", "state", "title", "secondaryTitle", "labels", "tags", "status", "statusSlot", "statusColor", "badge", "program", "directory", "columns", "rows", "expandable", "expanded", "open", "own", "tabPlace", "accessory" };
        const auto comparedText = [](const Ubiquitty::Codec::JSON::Value& outlineValue) {
            std::string text;
            if (const Ubiquitty::Codec::JSON::Value* rows = outlineValue.find("rows")) {
                for (const Ubiquitty::Codec::JSON::Value& row : rows->items()) {
                    for (const char* member : comparedMembers) {
                        const Ubiquitty::Codec::JSON::Value* said = row.find(member);
                        text.append(said ? Ubiquitty::Codec::JSON::serialize(*said) : "<absent>").push_back('\n');
                    }
                }
            }
            return text;
        };
        if (ubqBinary) {
            std::string before;
            bool isSettled = false; // bool-ok: a local verdict
            for (int tries = 0; tries < 40 && !isSettled; ++tries) {
                std::string printed;
                bool isJSON = false; // bool-ok: the parser's out flag
                const std::string now = readTree(printed) ? comparedText(Ubiquitty::Codec::JSON::parse(printed, isJSON)) : std::string();
                isSettled = !now.empty() && now == before;
                before = now;
                if (!isSettled)
                    ::usleep(250'000);
            }
            check(isSettled, "the host's tree did not settle: two reads of ubq tree a moment apart never agreed");
        }

        // ---- The directory lists what the host is running. ----
        check(directory.refresh(), "a refresh starts when nothing is in flight");
        check(!directory.refresh(), "a refresh while one is in flight is refused rather than queued");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.m_lists > 0; }), "the host answered the refresh");
        check(observer.row(firstSession) != nullptr && observer.row(secondSession) != nullptr, "both created sessions are listed");
        if (const Ubiquitty::Session::SessionRow* row = observer.row(firstSession))
            check(row->columns == 80 && row->rows == 24, "a listed row carries the session's geometry");
        check(observer.m_failures == 0, "a refresh that was answered reports no failure");

        // ---- `ubq tree` draws the outline this directory's reader draws. ----
        //
        // Both build it through `Runtime::buildHostOutline`, from the same
        // person's settings: here a tab's title is set to show its grid,
        // which it does not by default, in the one file both read.
        //
        // Each reads the live tree at its own moment, and the host walks a
        // session's processes for whoever is reading. A shell whose
        // foreground moves between the two moments — a startup hook's child,
        // read as "tail ◂ -zsh" by one reader and gone by the next — makes
        // the two disagree about when, not about how a title is composed. So
        // the pair is read until it agrees, as the settling above read `ubq
        // tree` alone: the directory refreshed, `ubq tree` read just after,
        // and the two compared. The pair that agrees, or the last of a
        // bounded many, is the one judged row by row below.
        if (ubqBinary) {
            Ubiquitty::App::SessionOutline outline;
            Ubiquitty::Codec::JSON::Value mine;
            Ubiquitty::Codec::JSON::Value theirs;
            bool didPrint = false; // bool-ok: the last read's exit status
            bool isJSON = false; // bool-ok: the parser's out flag
            bool isAgreed = false; // bool-ok: a local verdict
            for (int tries = 0; tries < 40 && !isAgreed; ++tries) {
                if (tries > 0) {
                    const int listsBefore = observer.m_lists;
                    if (!directory.refresh() || !pumpUntil(dispatcher, 10000, [&] { return observer.m_lists > listsBefore; }))
                        break;
                }
                outline = Ubiquitty::Runtime::buildHostOutline(observer.m_tree, Ubiquitty::Preferences::values(), Ubiquitty::Themes::StatusColors {}, {}, observer.m_tree.hostNowUnixMilliseconds);
                mine = Ubiquitty::App::outlineJSON(outline);
                std::string printed;
                didPrint = readTree(printed);
                isJSON = false;
                theirs = didPrint ? Ubiquitty::Codec::JSON::parse(printed, isJSON) : Ubiquitty::Codec::JSON::Value();
                isAgreed = didPrint && isJSON && comparedText(mine) == comparedText(theirs);
            }
            check(didPrint, "ubq tree exited nonzero");
            const Ubiquitty::Codec::JSON::Value* myRows = mine.find("rows");
            const Ubiquitty::Codec::JSON::Value* theirRows = isJSON ? theirs.find("rows") : nullptr;
            check(theirRows != nullptr && myRows != nullptr && !myRows->items().empty() && theirRows->items().size() == myRows->items().size(), "ubq tree does not print the rows the directory's outline holds");
            if (theirRows && myRows && theirRows->items().size() == myRows->items().size()) {
                for (std::size_t index = 0; index < myRows->items().size(); ++index) {
                    for (const char* member : comparedMembers) {
                        const Ubiquitty::Codec::JSON::Value* said = myRows->items()[index].find(member);
                        const Ubiquitty::Codec::JSON::Value* printedMember = theirRows->items()[index].find(member);
                        const std::string here = said ? Ubiquitty::Codec::JSON::serialize(*said) : "<absent>";
                        const std::string there = printedMember ? Ubiquitty::Codec::JSON::serialize(*printedMember) : "<absent>";
                        if (here != there)
                            std::fprintf(stderr, "    row %zu, %s: the directory says %s and ubq tree says %s\n", index, member, here.c_str(), there.c_str());
                        check(here == there, "a row of ubq tree differs from the directory's outline");
                    }
                }
            }
            // Each session here is an unnamed workspace of one window and
            // one tab, so its row is called by the folder its pane is in
            // (`Session::TreeWorkspace::defaultName`) and the title composed
            // from the settings is the row's secondary text.
            const bool doesATitleShowItsGrid = std::ranges::any_of(outline.rows, [](const Ubiquitty::App::OutlineRow& row) {
                const auto showsGrid = [](const std::string& text) { return text.find("80") != std::string::npos && text.find("24") != std::string::npos; };
                return showsGrid(row.title) || showsGrid(row.secondaryTitle);
            }); // bool-ok: a local verdict
            check(doesATitleShowItsGrid, "the outline was not composed from the person's title settings");
            check(std::ranges::none_of(outline.rows, [](const Ubiquitty::App::OutlineRow& row) { return row.title.empty(); }), "a row of the outline has no title");
        }

        // ---- A reader of the tree asks the host for changes. ----
        //
        // This connection holds a token from the host's tokens file, which
        // makes it automation: an agent. Owning the session is no substitute
        // for exact-operation approval from a person attached to its workspace.
        {
            Ubiquitty::Runtime::HostDirectoryClient client(dispatcher, factory);
            client.setAuthToken(token);
            check(client.watch(), "the directory client's watch starts");
            check(pumpUntil(dispatcher, 10000, [&] { return client.snapshot().treeFreshness == Ubiquitty::Runtime::HostDirectoryClient::TreeFreshness::Live; }), "the directory client follows the host");
            const auto workspaceOf = [&](std::uint32_t sessionID) -> std::uint32_t {
                const std::shared_ptr<const Session::SessionTree> held = client.tree();
                for (const Session::TreeWorkspace& workspace : held ? held->workspaces : std::vector<Session::TreeWorkspace> {}) {
                    for (const Session::TreeWindow& window : workspace.windows) {
                        for (const Session::TreeTab& tab : window.tabs) {
                            for (const Session::TreePane& pane : tab.panes) {
                                if (pane.sessionID == sessionID)
                                    return workspace.id;
                            }
                        }
                    }
                }
                return 0;
            };
            const auto labelOf = [&](std::uint32_t sessionID) -> std::string {
                const std::shared_ptr<const Session::SessionTree> held = client.tree();
                for (const Session::TreeWorkspace& workspace : held ? held->workspaces : std::vector<Session::TreeWorkspace> {}) {
                    for (const Session::TreeWindow& window : workspace.windows) {
                        for (const Session::TreeTab& tab : window.tabs) {
                            for (const Session::TreePane& pane : tab.panes) {
                                if (pane.sessionID == sessionID)
                                    return pane.sessionLabel;
                            }
                        }
                    }
                }
                return "<not listed>";
            };
            const std::uint32_t workspaceID = workspaceOf(firstSession);
            check(workspaceID != 0, "the client's tree holds the first session's workspace");
            using Ubiquitty::Runtime::StructureOutcome;
            using Ubiquitty::Session::Identity::ObjectKind;
            const std::uint64_t renamed = client.renameWorkspace({ ObjectKind::Workspace, workspaceID }, "named by an agent");
            check(pumpUntil(dispatcher, 10000, [&] { return client.snapshot().structureResult.requestID == renamed; }), "the rename settled");
            const Ubiquitty::Runtime::StructureResult refused = client.snapshot().structureResult;
            check(refused.outcome == StructureOutcome::RefusedByHost, "an agent's rename was not refused by the host");
            check(refused.reason.find("workspace.rename") != std::string::npos && refused.reason.find("no such person is attached") != std::string::npos, "the rename did not report the missing exact-operation approver");
            if (refused.reason.find("no such person is attached") == std::string::npos)
                std::fprintf(stderr, "    the host said: %s\n", refused.reason.c_str());

            const std::string labelBefore = labelOf(firstSession);
            const std::uint64_t labelled = client.setLabel({ ObjectKind::Session, firstSession }, "labelled through the directory");
            check(pumpUntil(dispatcher, 10000, [&] { return client.snapshot().structureResult.requestID == labelled; }), "the label settled");
            check(client.snapshot().structureResult.outcome == StructureOutcome::RefusedByHost, "an agent's creator grant bypassed exact-operation label consent");
            check(client.snapshot().structureResult.reason.find("no such person is attached") != std::string::npos, "the label did not report the missing exact-operation approver");
            if (client.snapshot().structureResult.reason.find("no such person is attached") == std::string::npos)
                std::fprintf(stderr, "    the host said: %s\n", client.snapshot().structureResult.reason.c_str());
            check(labelOf(firstSession) == labelBefore, "a refused label changed the tree the watch follows");
            if (const auto held = client.tree()) {
                for (const auto& workspace : held->workspaces)
                    if (workspace.id == workspaceID)
                        check(workspace.name != "named by an agent", "a refused rename changed the directory's workspace");
            }
            // Asked of something the tree does not hold, nothing is sent.
            const std::uint64_t absent = client.closeWindow({ ObjectKind::Window, 4'000'000 });
            check(pumpUntil(dispatcher, 5000, [&] { return client.snapshot().structureResult.requestID == absent; }), "the local refusal settled");
            check(client.snapshot().structureResult.outcome == StructureOutcome::RefusedHere, "a window the tree does not hold was asked of the host");
            client.stopWatching();
        }

        // ---- The same refresh brought the host's tree, whole, before its rows. ----
        //
        // The errand subscribed to the daemon's listing on the connection it
        // listed on, was sent each workspace's structure and each pane's records,
        // and assembled them.
        check(observer.m_trees == observer.m_lists && observer.m_listsBeforeTree == observer.m_lists - 1, "the refresh did not report the host's tree before its rows");
        check(observer.m_tree.workspaces.size() == 2, "a session of its own is a workspace of its own in the tree");
        check(observer.m_tree.hostNowUnixMilliseconds > 1'600'000'000'000ull, "the tree does not state the host's clock");
        for (const std::uint32_t sessionID : { firstSession, secondSession }) {
            const Session::TreePane* pane = observer.pane(sessionID);
            check(pane != nullptr, "a session the host runs is not a pane of its tree");
            if (pane) {
                check(pane->facts.revision != 0, "the tree was delivered before a pane's facts arrived");
                check(pane->facts.columns == 80 && pane->facts.rows == 24, "a pane's facts do not carry its grid");
                check(pane->attendees.size() == 1, "the client attached to a pane is not named on it");
            }
        }

        // ---- The errand's channel is a Sync connection that bound no pane. ----
        //
        // The listing sent one Greet that the daemon welcomed. This
        // test is a binary the daemon does not know, so it is granted an agent's
        // role whatever it asked for. A call on a channel of its own is admitted
        // from its registry entry and answered with no session named.
        check(directory.grantedRole() && *directory.grantedRole() == Protocol::Sync::Role::Agent, "the listing errand was not welcomed, or was granted a person's role");
        check(directory.call(R"({"jsonrpc":"2.0","id":1,"method":"host.methods","params":{}})"), "a call starts once the refresh has settled");
        check(!directory.refresh(), "a refresh while a call is in flight is refused rather than queued");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.m_answers > 0; }), "the host answered the call");
        check(observer.m_answer.find("\"result\"") != std::string::npos && observer.m_answer.find("host.describe") != std::string::npos,
            "host.methods did not list the host's methods to a directory errand");
        // A pane method has no pane to name on a connection that bound none.
        check(directory.call(R"({"jsonrpc":"2.0","id":1,"method":"pane.capture","params":{"pane":1}})"), "a second call starts once the first settled");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.m_answers > 1; }), "the host answered the second call");
        check(observer.m_answer.find("no pane is bound") != std::string::npos, "a directory errand reached a pane it never bound");
        check(observer.m_failures == 0, "an answered call reported a failure");

        // ---- Ending one row ends that session and nothing else. ----
        check(directory.terminate(secondSession), "a terminate starts once the refresh has settled");
        check(pumpUntil(dispatcher, 40000, [&] { return observer.m_terminates > 0; }), "the terminate settled");
        check(observer.m_terminatedSessionID == secondSession, "the terminate answers for the session it named");
        check(observer.m_outcome == Session::TerminateOutcome::Terminated, "a session this client is attached to is terminated outright");

        const int listsBeforeSecond = observer.m_lists;
        const int treesBeforeSecond = observer.m_trees;
        check(directory.refresh(), "a refresh starts after the terminate settled");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.m_lists > listsBeforeSecond; }), "the host answered the second refresh");
        check(observer.row(secondSession) == nullptr, "the terminated session is gone from the next list");
        check(observer.row(firstSession) != nullptr, "the session that was not named is still listed");
        check(observer.m_trees == treesBeforeSecond + 1, "the second refresh brought no tree");
        check(observer.pane(secondSession) == nullptr && observer.pane(firstSession) != nullptr, "the tree did not follow the terminate");
        check(observer.m_tree.workspaces.size() == 1, "the ended session's workspace is still in the tree");

        // ---- A watch follows the host on a connection of its own. ----
        {
            RecordingDirectory watched;
            Session::SessionDirectoryCore watcher(&dispatcher, &factory, &watched);
            watcher.setAuthToken(token);
            check(watcher.watch(), "a watch starts beside the directory's errands");
            check(pumpUntil(dispatcher, 10000, [&] { return watched.m_trees > 0; }), "the watch reported no tree");
            check(watched.m_lists == 0, "a watch asked for rows");
            check(watched.pane(firstSession) != nullptr, "the watched tree lacks the running session");
            // A session made while it watches appears without another ask.
            Connection third;
            const std::uint32_t thirdSession = createSession(third, socketPath, token, daemonInstance);
            check(thirdSession != 0, "a third session was created");
            check(pumpUntil(dispatcher, 10000, [&] { return watched.pane(thirdSession) != nullptr && watched.pane(thirdSession)->facts.revision != 0; }), "the watch did not report a session made after it began");
            // And one that ends disappears.
            ::close(third.fileDescriptor);
            third.fileDescriptor = -1;
            check(directory.terminate(thirdSession), "a terminate runs while the watch is out");
            const int terminatesBeforeWatch = observer.m_terminates;
            check(pumpUntil(dispatcher, 40000, [&] { return observer.m_terminates > terminatesBeforeWatch; }), "the terminate beside the watch settled");
            check(pumpUntil(dispatcher, 10000, [&] { return watched.pane(thirdSession) == nullptr; }), "the watch still lists a session that ended");
            check(watched.m_failures == 0 && watcher.isWatching(), "the watch ended on its own");
            watcher.stopWatching();
            check(!watcher.isBusy(), "a stopped watch is still in flight");
        }

        // ---- Ending a row that is already gone ends nothing, and says so. ----
        //
        // The id above is the strongest one to name: the host had it, the terminate
        // took it, and the re-list just proved it gone — so this is the race a person
        // runs by clicking Terminate on a picker one refresh out of date.
        //
        // A non-zero session id otherwise means reattach-or-create, so without the
        // attach-only flag this handshake would mint the session it had been asked
        // to end, end the one it had just made, and report success. The row would
        // vanish from the picker, having briefly been a real shell.
        const int terminatesBefore = observer.m_terminates;
        check(directory.terminate(secondSession), "a terminate on a vanished id starts");
        check(pumpUntil(dispatcher, 40000, [&] { return observer.m_terminates > terminatesBefore; }),
            "the terminate on a vanished id settled");
        check(observer.m_outcome != Session::TerminateOutcome::Terminated,
            "a terminate reported success for a session it never reached");
        check(observer.m_outcome == Session::TerminateOutcome::Refused,
            "a vanished target settled as something other than a refusal");

        const int listsBeforeThird = observer.m_lists;
        check(directory.refresh(), "a refresh starts after the vanished terminate settled");
        check(pumpUntil(dispatcher, 10000, [&] { return observer.m_lists > listsBeforeThird; }), "the host answered the third refresh");
        check(observer.row(secondSession) == nullptr, "the vanished id was created by the terminate that named it");
        check(observer.row(firstSession) != nullptr, "the session that was not named survived the vanished terminate");

        // ---- A host that requires a token refuses a directory without one. ----
        {
            RecordingDirectory tokenlessObserver;
            Session::SessionDirectoryCore tokenless(&dispatcher, &factory, &tokenlessObserver);
            check(tokenless.refresh(), "a tokenless refresh starts");
            check(pumpUntil(dispatcher, 10000, [&] { return tokenlessObserver.m_failures > 0; }), "a tokenless refresh against a token-requiring host fails");
            check(tokenlessObserver.m_lists == 0, "a refused refresh delivers no session list");
            // The host's own wording, not the carrier's: a client shown "closed"
            // for a refused token is told the network failed when nothing did.
            check(!tokenlessObserver.m_failureReason.empty() && tokenlessObserver.m_failureReason != "closed",
                "the refusal carries the host's reason rather than the carrier's");
            check(tokenlessObserver.m_failure == Session::ListingFailure::Refused, "a refused refresh is typed as refused");
        }
    }
    endDaemon();

    if (g_failures == 0)
        std::printf("%s\n", summary);
    return g_failures == 0 ? 0 : 1;
}
