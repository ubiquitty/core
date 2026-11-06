// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/tmux_control_stress.cpp — tmux control mode under load, hosted
// the way a frontend hosts it.
//
// A session launches `tmux -CC` against a private server: its own -S socket
// and HOME under /tmp, and no configuration file. The test is the frontend.
// It drains the session's hosted-window events and binds a view session to
// every pane the way a window does: the pane's model adopted for output,
// send-keys for input, the hosting source for size. It answers every close.
// tmux is the oracle: each screen is held against what `capture-pane` says
// the pane shows, row by row and, where tmux prints its line flags, wrap by
// wrap, and the panes against what `list-panes` lists.
//
// The scenarios:
// - flood: ten megabytes into one pane while another is typed into;
// - churn: windows and panes created, killed and swapped while output runs;
// - storm: the client resized again and again while output runs;
// - split: the control stream re-read in every chunking, so a %begin/%end
//   block cut anywhere decodes as it does whole;
// - death: the server killed under a live session;
// - reattach: the client detached and attached again.
//
// argv[1..] = the scenarios to run; none runs them all. Exit 0 on success.
//

#include "Core/GridBounds.h"
#include "Core/Runtime/HostedWindows.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/PostedWorkTable.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Runtime/TmuxControlHost.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <random>
#include <set>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

using Ubiquitty::Runtime::discardPostedSessionWork;
using Ubiquitty::Runtime::HostedWindowEvent;
using Ubiquitty::Runtime::LaunchCommand;
using Ubiquitty::Runtime::PostedWorkLoop;
using Ubiquitty::Runtime::PostedWorkToken;
using Ubiquitty::Runtime::runPostedSessionWork;
using Ubiquitty::Runtime::TerminalSession;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t exitedBit = 1u << 1;

int failures = 0;

void fail(std::string_view scenario, const std::string& what)
{
    std::fprintf(stderr, "tmux_control_stress: %.*s: FAIL — %s\n", static_cast<int>(scenario.size()), scenario.data(), what.c_str());
    ++failures;
}

std::string trimmedRight(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r'))
        text.pop_back();
    return text;
}

/// Rows of a screen as text, without the empty rows below the last written.
using Rows = std::vector<std::string>;

void dropTrailingEmptyRows(Rows& rows)
{
    while (!rows.empty() && rows.back().empty())
        rows.pop_back();
}

/// The frontend's loop: work posted from any thread waits here until the
/// test pumps, as it waits for a run loop's next turn.
class FrontendLoop {
public:
    /// The one loop the test runs, as a frontend has one main loop.
    ///
    /// - Returns: The loop.
    static FrontendLoop& shared()
    {
        static FrontendLoop loop;
        return loop;
    }

    /// The loop's entry points, as the sessions are handed them.
    ///
    /// - Returns: Entry points that queue each token on ``shared()``.
    static PostedWorkLoop entryPoints() { return { .runSoon = &post, .runAfter = &postDelayed }; }

    static void post(PostedWorkToken token) { shared().enqueue(Clock::now(), token); }

    static void postDelayed(double seconds, PostedWorkToken token)
    {
        const auto delay = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        shared().enqueue(Clock::now() + delay, token);
    }

    /// Runs the work that is due, in the order it was posted.
    void runDue()
    {
        std::vector<PostedWorkToken> due;
        {
            const std::scoped_lock hold(m_lock);
            const Clock::time_point now = Clock::now();
            std::erase_if(m_work, [&](const Work& work) {
                if (work.due > now)
                    return false;
                due.push_back(work.token);
                return true;
            });
        }
        for (const PostedWorkToken token : due)
            runPostedSessionWork(token);
    }

    ~FrontendLoop()
    {
        for (const Work& work : m_work)
            discardPostedSessionWork(work.token);
    }

private:
    struct Work {
        Clock::time_point due;
        PostedWorkToken token;
    };

    void enqueue(Clock::time_point due, PostedWorkToken token)
    {
        const std::scoped_lock hold(m_lock);
        m_work.push_back({ due, token });
    }

    std::mutex m_lock;
    std::vector<Work> m_work;
};

/// A tmux server of the test's own: its socket and HOME in a directory under
/// /tmp, no configuration file, killed and removed with the object.
class PrivateServer {
public:
    explicit PrivateServer(std::string tmux)
        : m_tmux(std::move(tmux))
    {
        char root[] = "/tmp/ubq-tmux.XXXXXX";
        if (::mkdtemp(root)) {
            m_root = root;
            m_socket = m_root + "/s";
        }
    }

    ~PrivateServer()
    {
        if (m_root.empty())
            return;
        (void)run({ "kill-server" });
        std::error_code ignored;
        std::filesystem::remove_all(m_root, ignored);
    }

    PrivateServer(const PrivateServer&) = delete;
    PrivateServer& operator=(const PrivateServer&) = delete;

    [[nodiscard]] bool isReady() const { return !m_root.empty(); }

    /// What a tmux of this server's runs with in place of the reader's own.
    [[nodiscard]] std::vector<std::string> environment() const
    {
        return { "HOME=" + m_root, "TMUX_TMPDIR=" + m_root, "SHELL=/bin/sh", "ENV=/dev/null", "PS1=$ ", "HISTFILE=/dev/null" };
    }

    /// A control client's argument vector, ending in `tail`.
    [[nodiscard]] std::vector<std::string> controlArguments(const std::vector<std::string>& tail) const
    {
        std::vector<std::string> arguments { m_tmux, "-S", m_socket, "-f", "/dev/null", "-CC" };
        arguments.insert(arguments.end(), tail.begin(), tail.end());
        return arguments;
    }

    struct Answer {
        int status = -1;
        std::string output;
    };

    /// Runs one tmux command against the server and returns what it printed.
    [[nodiscard]] Answer run(const std::vector<std::string>& command) const
    {
        std::vector<std::string> arguments { m_tmux, "-S", m_socket, "-f", "/dev/null" };
        arguments.insert(arguments.end(), command.begin(), command.end());
        std::vector<std::string> entries = childEnvironment();
        std::vector<char*> argv;
        for (std::string& argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::vector<char*> envp;
        for (std::string& entry : entries)
            envp.push_back(entry.data());
        envp.push_back(nullptr);

        Answer answer;
        int output[2];
        if (::pipe(output) != 0)
            return answer;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&actions, output[0]);
        posix_spawn_file_actions_addclose(&actions, output[1]);
        pid_t child = -1;
        const int spawned = posix_spawn(&child, argv[0], &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        ::close(output[1]);
        if (spawned != 0) {
            ::close(output[0]);
            return answer;
        }
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        char buffer[65'536];
        for (;;) {
            pollfd readable { .fd = output[0], .events = POLLIN, .revents = 0 };
            if (::poll(&readable, 1, 100) > 0) {
                const ssize_t count = ::read(output[0], buffer, sizeof(buffer));
                if (count <= 0)
                    break;
                answer.output.append(buffer, static_cast<std::size_t>(count));
            }
            if (Clock::now() >= deadline) {
                ::kill(child, SIGKILL);
                break;
            }
        }
        ::close(output[0]);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        answer.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return answer;
    }

    /// What `capture-pane` says a pane shows.
    [[nodiscard]] Rows capture(int paneID) const
    {
        Rows rows;
        const Answer answer = run({ "capture-pane", "-p", "-t", "%" + std::to_string(paneID) });
        std::size_t start = 0;
        while (start < answer.output.size()) {
            const std::size_t end = answer.output.find('\n', start);
            rows.push_back(trimmedRight(answer.output.substr(start, end == std::string::npos ? std::string::npos : end - start)));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        dropTrailingEmptyRows(rows);
        return rows;
    }

    /// Which of a pane's rows tmux wrapped into the next, `W` or `-` per row,
    /// from the line flags `capture-pane -F` prints; empty from a tmux that
    /// prints none.
    [[nodiscard]] std::string captureWraps(int paneID) const
    {
        const Answer answer = run({ "capture-pane", "-p", "-F", "-t", "%" + std::to_string(paneID) });
        std::string wraps;
        if (answer.status != 0)
            return wraps;
        std::size_t start = 0;
        while (start < answer.output.size()) {
            const std::size_t end = answer.output.find('\n', start);
            const std::string row = answer.output.substr(start, end == std::string::npos ? std::string::npos : end - start);
            wraps.push_back(row.substr(0, row.find(' ')).find('W') != std::string::npos ? 'W' : '-');
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return wraps;
    }

    /// A pane as `list-panes` lists it.
    struct ListedPane {
        int windowID = -1;
        int columns = 0;
        int rows = 0;
    };

    /// Every pane of the server's sessions, by id.
    [[nodiscard]] std::map<int, ListedPane> listPanes() const
    {
        std::map<int, ListedPane> panes;
        const Answer answer = run({ "list-panes", "-a", "-F", "#{window_id} #{pane_id} #{pane_width} #{pane_height}" });
        std::size_t start = 0;
        while (start < answer.output.size()) {
            const std::size_t end = answer.output.find('\n', start);
            const std::string line = answer.output.substr(start, end == std::string::npos ? std::string::npos : end - start);
            int window = 0;
            int pane = 0;
            int columns = 0;
            int rows = 0;
            if (std::sscanf(line.c_str(), "@%d %%%d %d %d", &window, &pane, &columns, &rows) == 4)
                panes[pane] = { .windowID = window, .columns = columns, .rows = rows };
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return panes;
    }

    /// One number tmux formats, or -1.
    [[nodiscard]] long long number(const std::string& format) const
    {
        const Answer answer = run({ "display-message", "-p", format });
        return answer.output.empty() ? -1 : std::atoll(answer.output.c_str());
    }

private:
    std::vector<std::string> childEnvironment() const
    {
        std::vector<std::string> entries;
        const std::vector<std::string> overrides = environment();
        for (char** entry = environ; *entry; ++entry) {
            const std::string_view text(*entry);
            const std::string_view name = text.substr(0, text.find('='));
            const bool isOverridden = name == "TMUX" || std::any_of(overrides.begin(), overrides.end(), [&](const std::string& over) {
                return std::string_view(over).substr(0, over.find('=')) == name;
            });
            if (!isOverridden)
                entries.emplace_back(text);
        }
        entries.insert(entries.end(), overrides.begin(), overrides.end());
        return entries;
    }

    std::string m_tmux;
    std::string m_root;
    std::string m_socket;
};

/// A pane's view: the session a window renders it through.
struct PaneView {
    TerminalSession* session = nullptr;
    Ubiquitty::Terminal* terminal = nullptr;
    std::mutex* lock = nullptr;
};

/// What a pane's model shows, as `capture-pane` would print it.
Rows modelRows(const PaneView& view)
{
    Rows rows;
    const std::scoped_lock hold(*view.lock);
    const Ubiquitty::Terminal* terminal = view.terminal;
    for (int row = 0; row < terminal->rows; ++row) {
        std::string text;
        for (int column = 0; column < terminal->columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(terminal, 0, row, column);
            const std::uint32_t codePoint = cell ? cell->codePoint : 0;
            text.push_back(codePoint >= 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : ' ');
        }
        rows.push_back(trimmedRight(std::move(text)));
    }
    dropTrailingEmptyRows(rows);
    return rows;
}

/// Which of a pane's model rows wrap into the next, as `captureWraps` prints them.
std::string modelWraps(const PaneView& view)
{
    std::string wraps;
    const std::scoped_lock hold(*view.lock);
    for (int row = 0; row < view.terminal->rows; ++row)
        wraps.push_back(Ubiquitty::Screen::visibleRowWrapped(view.terminal, 0, row) ? 'W' : '-');
    return wraps;
}

/// The windows a hosting session shows, kept as a window controller keeps
/// them: a view bound to every pane a layout names, unbound when a layout
/// drops it or its window closes, and every close answered.
class Frontend {
public:
    explicit Frontend(TerminalSession* primary)
        : m_primary(primary)
    {
    }

    ~Frontend() { unbindAll(); }

    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;

    /// Takes the session's events and answers them.
    void pump()
    {
        m_flags |= m_primary->takeEventFlags();
        for (HostedWindowEvent event = m_primary->takeHostedWindowEvent(); event.kind != HostedWindowEvent::Kind::None; event = m_primary->takeHostedWindowEvent())
            apply(event);
        (void)m_primary->takeRepaintedHostedPanes();
        // A pane a layout named before its model existed binds once it does.
        for (auto it = m_waiting.begin(); it != m_waiting.end();)
            it = bind(*it) ? m_waiting.erase(it) : std::next(it);
    }

    [[nodiscard]] bool hasEnded() const { return m_hasEnded; }
    [[nodiscard]] bool hasExited() const { return (m_flags & exitedBit) != 0; }
    [[nodiscard]] const std::map<int, std::vector<int>>& windows() const { return m_windows; }
    [[nodiscard]] const std::map<int, PaneView>& panes() const { return m_panes; }

    [[nodiscard]] PaneView* pane(int paneID)
    {
        const auto found = m_panes.find(paneID);
        return found == m_panes.end() ? nullptr : &found->second;
    }

    /// Types into a pane through its view, as a keystroke reaches it.
    bool type(int paneID, std::string_view text)
    {
        PaneView* view = pane(paneID);
        if (!view)
            return false;
        view->session->sendInput(Ubiquitty::Runtime::ByteSpan(text.data(), text.size()));
        return true;
    }

    /// Any bound pane's view, for a verb about the whole session.
    [[nodiscard]] TerminalSession* anyView() { return m_panes.empty() ? nullptr : m_panes.begin()->second.session; }

private:
    void apply(const HostedWindowEvent& event)
    {
        switch (event.kind) {
        case HostedWindowEvent::Kind::WindowOpened:
            (void)m_windows[event.windowID];
            break;
        case HostedWindowEvent::Kind::LayoutChanged: {
            const std::vector<int> previous = m_windows[event.windowID];
            m_windows[event.windowID] = event.paneIDs;
            for (int paneID : event.paneIDs) {
                if (!m_panes.contains(paneID) && !bind(paneID))
                    m_waiting.insert(paneID);
            }
            for (int paneID : previous) {
                if (!isListed(paneID))
                    unbind(paneID);
            }
            break;
        }
        case HostedWindowEvent::Kind::WindowClosed: {
            const std::vector<int> closed = m_windows[event.windowID];
            m_windows.erase(event.windowID);
            for (int paneID : closed) {
                if (!isListed(paneID))
                    unbind(paneID);
            }
            m_primary->releaseHostedWindow(event.windowID);
            break;
        }
        case HostedWindowEvent::Kind::Ended:
            unbindAll();
            m_windows.clear();
            m_hasEnded = true;
            break;
        default:
            break;
        }
    }

    [[nodiscard]] bool isListed(int paneID) const
    {
        return std::any_of(m_windows.begin(), m_windows.end(), [&](const auto& window) {
            return std::find(window.second.begin(), window.second.end(), paneID) != window.second.end();
        });
    }

    bool bind(int paneID)
    {
        const Ubiquitty::Runtime::HostedPaneFacts facts = m_primary->hostedPaneFacts(paneID);
        if (!facts.terminalAddress || !facts.lockAddress)
            return false;
        TerminalSession* view = TerminalSession::make();
        if (!view)
            return false;
        auto* terminal = reinterpret_cast<Ubiquitty::Terminal*>(facts.terminalAddress);
        view->adoptExternalTerminal(terminal, facts.lockAddress);
        view->routeInputViaTmuxAddresses(facts.inputClientAddress, facts.inputStreamAddress, paneID);
        view->routeResizeThroughHostedSource(m_primary->hostedWindowSourceAddress(), paneID);
        m_panes[paneID] = { .session = view, .terminal = terminal, .lock = reinterpret_cast<std::mutex*>(facts.lockAddress) };
        return true;
    }

    void unbind(int paneID)
    {
        m_waiting.erase(paneID);
        const auto found = m_panes.find(paneID);
        if (found == m_panes.end())
            return;
        found->second.session->endExternalAdoption();
        releaseTerminalSession(found->second.session);
        m_panes.erase(found);
        m_primary->releaseHostedPane(paneID);
    }

    void unbindAll()
    {
        while (!m_panes.empty())
            unbind(m_panes.begin()->first);
        m_waiting.clear();
    }

    TerminalSession* m_primary;
    std::map<int, std::vector<int>> m_windows;
    std::map<int, PaneView> m_panes;
    std::set<int> m_waiting;
    std::uint32_t m_flags = 0;
    bool m_hasEnded = false;
};

/// Every byte the control client wrote, as the session read it.
struct StreamRecording {
    std::mutex lock;
    std::string bytes;

    static void tap(void* context, std::string_view bytes)
    {
        auto* recording = static_cast<StreamRecording*>(context);
        const std::scoped_lock hold(recording->lock);
        recording->bytes.append(bytes);
    }
};

/// The session running the control client, launched as a frontend launches
/// a local `tmux -CC`: hosting from the start, waiting for the handshake.
class ControlClient {
public:
    ControlClient(const PrivateServer& server, const std::vector<std::string>& tail, StreamRecording* recording = nullptr)
        : m_session(TerminalSession::make())
    {
        if (!m_session)
            return;
        m_session->start(24, 80);
        if (recording)
            m_session->setRawByteTap(&StreamRecording::tap, recording);
        LaunchCommand command = LaunchCommand::fromArguments(server.controlArguments(tail));
        command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
        command.setTmuxControlHost("");
        command.setEnvironmentOverrides(server.environment());
        m_isRunning = m_session->launch(command) && m_session->startIngest();
    }

    ~ControlClient()
    {
        if (!m_session)
            return;
        m_session->stopIngest();
        m_session->endHostedWindows();
        m_session->close();
        releaseTerminalSession(m_session);
    }

    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;

    [[nodiscard]] bool isRunning() const { return m_isRunning; }
    [[nodiscard]] TerminalSession* session() const { return m_session; }

private:
    TerminalSession* m_session;
    bool m_isRunning = false;
};

/// Pumps the frontend until `isDone` holds or the time is up.
bool pumpUntil(Frontend& frontend, int milliseconds, const std::function<bool()>& isDone)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        FrontendLoop::shared().runDue();
        frontend.pump();
        if (isDone())
            return true;
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void pumpFor(Frontend& frontend, int milliseconds)
{
    (void)pumpUntil(frontend, milliseconds, [] { return false; });
}

std::string describe(const Rows& rows)
{
    std::string text;
    for (const std::string& row : rows)
        text += "    |" + row + "|\n";
    return text;
}

/// Whether every pane tmux lists is bound, at tmux's size, showing what
/// tmux says it shows. Polled until it holds or the time is up.
///
/// - Returns: Empty when they agree; otherwise what differed last.
std::string disagreement(Frontend& frontend, const PrivateServer& server, int milliseconds)
{
    std::string differed;
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        pumpFor(frontend, 150);
        differed.clear();
        const std::map<int, PrivateServer::ListedPane> listed = server.listPanes();
        std::set<int> bound;
        for (const auto& [paneID, view] : frontend.panes())
            bound.insert(paneID);
        std::set<int> expected;
        for (const auto& [paneID, pane] : listed)
            expected.insert(paneID);
        if (bound != expected) {
            differed = "bound panes " + std::to_string(bound.size()) + " against " + std::to_string(expected.size()) + " listed";
        } else {
            for (const auto& [paneID, pane] : listed) {
                PaneView* view = frontend.pane(paneID);
                int columns = 0;
                int rows = 0;
                {
                    const std::scoped_lock hold(*view->lock);
                    columns = view->terminal->columns;
                    rows = view->terminal->rows;
                }
                // A grid smaller than Core's floor is held at the floor
                // (GridBounds.h): one row holds no scroll region. Such a pane
                // scrolls where tmux's does not, so only its size is held to
                // tmux's.
                const int heldColumns = Ubiquitty::clampGridColumns(pane.columns);
                const int heldRows = Ubiquitty::clampGridRows(pane.rows);
                if (columns != heldColumns || rows != heldRows) {
                    differed = "pane %" + std::to_string(paneID) + " is " + std::to_string(columns) + "x" + std::to_string(rows) + ", tmux says "
                        + std::to_string(pane.columns) + "x" + std::to_string(pane.rows);
                    break;
                }
                if (heldColumns != pane.columns || heldRows != pane.rows)
                    continue;
                const Rows ours = modelRows(*view);
                const Rows theirs = server.capture(paneID);
                if (ours != theirs) {
                    differed = "pane %" + std::to_string(paneID) + " shows\n" + describe(ours) + "  tmux shows\n" + describe(theirs);
                    break;
                }
                if (const std::string tmuxWraps = server.captureWraps(paneID); !tmuxWraps.empty()) {
                    if (const std::string ourWraps = modelWraps(*view); ourWraps != tmuxWraps) {
                        differed = "pane %" + std::to_string(paneID) + " wraps rows " + ourWraps + ", tmux wraps " + tmuxWraps;
                        break;
                    }
                }
            }
        }
        if (differed.empty() || Clock::now() >= deadline)
            return differed;
    }
}

/// Starts a server with one session and hosts it.
struct HostedSession {
    PrivateServer server;
    std::unique_ptr<ControlClient> client;
    std::unique_ptr<Frontend> frontend;

    HostedSession(const std::string& tmux, StreamRecording* recording = nullptr)
        : server(tmux)
    {
        if (!server.isReady())
            return;
        client = std::make_unique<ControlClient>(server, std::vector<std::string> { "new-session", "-s", "stress", "-x", "80", "-y", "24", "/bin/sh" }, recording);
        if (client->isRunning())
            frontend = std::make_unique<Frontend>(client->session());
    }

    ~HostedSession()
    {
        frontend.reset();
        client.reset();
    }

    /// Whether the session's first pane is hosted and its shell has drawn.
    bool waitForFirstPane(std::string_view scenario)
    {
        if (!frontend) {
            fail(scenario, "the control client did not start");
            return false;
        }
        if (!pumpUntil(*frontend, 20'000, [&] { return !frontend->panes().empty(); })) {
            fail(scenario, "no pane was hosted");
            return false;
        }
        const int first = frontend->panes().begin()->first;
        if (!pumpUntil(*frontend, 10'000, [&] { return !modelRows(*frontend->pane(first)).empty(); })) {
            fail(scenario, "the first pane's shell never drew");
            return false;
        }
        return true;
    }

    /// The panes tmux lists, in id order.
    [[nodiscard]] std::vector<int> listedPaneIDs() const
    {
        std::vector<int> ids;
        for (const auto& [paneID, pane] : server.listPanes())
            ids.push_back(paneID);
        return ids;
    }
};

bool rowsContain(const Rows& rows, std::string_view needle)
{
    return std::any_of(rows.begin(), rows.end(), [&](const std::string& row) { return row.find(needle) != std::string::npos; });
}

// ---- the scenarios ----

void runFlood(const std::string& tmux)
{
    constexpr std::string_view scenario = "flood";
    HostedSession hosted(tmux);
    if (!hosted.waitForFirstPane(scenario))
        return;
    Frontend& frontend = *hosted.frontend;
    const int flooded = frontend.panes().begin()->first;
    (void)hosted.server.run({ "split-window", "-h", "-t", "%" + std::to_string(flooded), "/bin/sh" });
    if (!pumpUntil(frontend, 10'000, [&] { return frontend.panes().size() == 2; })) {
        fail(scenario, "the split's pane was not hosted");
        return;
    }
    const int typed = std::next(frontend.panes().begin())->first;
    if (!pumpUntil(frontend, 10'000, [&] { return !modelRows(*frontend.pane(typed)).empty(); })) {
        fail(scenario, "the second pane's shell never drew");
        return;
    }

    // About ten megabytes of output; the marker is computed, so the command
    // line that names it never reads as it.
    const auto started = Clock::now();
    frontend.type(flooded, "head -c 7864320 /dev/urandom | base64; echo FLOOD-$((6*7))\r");
    constexpr int typedLines = 40;
    for (int line = 0; line < typedLines; ++line) {
        char text[32];
        std::snprintf(text, sizeof(text), "echo typed-%02d\r", line);
        if (!frontend.type(typed, text))
            fail(scenario, "the typed pane lost its view mid-flood");
        pumpFor(frontend, 25);
    }
    if (!pumpUntil(frontend, 180'000, [&] { return rowsContain(modelRows(*frontend.pane(flooded)), "FLOOD-42"); })) {
        fail(scenario, "the flood's end never reached the flooded pane");
        return;
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    std::printf("tmux_control_stress: flood: the flood landed after %.1f s\n", seconds);

    // Every line typed during the flood reached tmux once, in order.
    const PrivateServer::Answer history = hosted.server.run({ "capture-pane", "-p", "-S", "-", "-t", "%" + std::to_string(typed) });
    std::vector<std::string> echoed;
    std::size_t start = 0;
    while (start < history.output.size()) {
        const std::size_t end = history.output.find('\n', start);
        const std::string line = trimmedRight(history.output.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (line.starts_with("typed-"))
            echoed.push_back(line);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    std::vector<std::string> wanted;
    for (int line = 0; line < typedLines; ++line) {
        char text[16];
        std::snprintf(text, sizeof(text), "typed-%02d", line);
        wanted.emplace_back(text);
    }
    if (echoed != wanted)
        fail(scenario, "the typed pane echoed " + std::to_string(echoed.size()) + " of " + std::to_string(typedLines) + " lines in order");
    if (const std::string differed = disagreement(frontend, hosted.server, 15'000); !differed.empty())
        fail(scenario, differed);
}

void runChurn(const std::string& tmux, StreamRecording* recording)
{
    constexpr std::string_view scenario = "churn";
    HostedSession hosted(tmux, recording);
    if (!hosted.waitForFirstPane(scenario))
        return;
    Frontend& frontend = *hosted.frontend;
    const int talker = frontend.panes().begin()->first;
    frontend.type(talker, "while :; do echo churn-out; sleep 0.01; done\r");
    std::mt19937 random(5759);
    for (int step = 0; step < 48; ++step) {
        std::vector<int> panes = hosted.listedPaneIDs();
        std::erase(panes, talker);
        const auto pick = [&] { return "%" + std::to_string(panes[random() % panes.size()]); };
        switch (step % 6) {
        case 0:
            (void)hosted.server.run({ "new-window", "-d", "-t", "stress", "/bin/sh" });
            break;
        case 1:
        case 2:
            (void)hosted.server.run({ "split-window", "-d", "-t", panes.empty() ? "%" + std::to_string(talker) : pick(), "/bin/sh" });
            break;
        case 3:
            if (panes.size() >= 2)
                (void)hosted.server.run({ "swap-pane", "-d", "-s", pick(), "-t", pick() });
            else
                (void)hosted.server.run({ "swap-pane", "-d", "-s", "%" + std::to_string(talker), "-t", "%" + std::to_string(talker) });
            break;
        case 4:
            if (!panes.empty())
                (void)hosted.server.run({ "kill-pane", "-t", pick() });
            break;
        case 5:
            (void)hosted.server.run({ "select-layout", "-t", "%" + std::to_string(talker), "tiled" });
            break;
        }
        pumpFor(frontend, 20);
    }
    // A window closed while output ran.
    const std::map<int, PrivateServer::ListedPane> listed = hosted.server.listPanes();
    const int talkerWindow = listed.contains(talker) ? listed.at(talker).windowID : -1;
    for (const auto& [paneID, pane] : listed) {
        if (pane.windowID != talkerWindow) {
            (void)hosted.server.run({ "kill-window", "-t", "@" + std::to_string(pane.windowID) });
            break;
        }
    }
    pumpFor(frontend, 200);
    frontend.type(talker, "\x03");
    frontend.type(talker, "clear\r");
    if (const std::string differed = disagreement(frontend, hosted.server, 15'000); !differed.empty())
        fail(scenario, differed);
}

void runStorm(const std::string& tmux, StreamRecording* recording)
{
    constexpr std::string_view scenario = "storm";
    HostedSession hosted(tmux, recording);
    if (!hosted.waitForFirstPane(scenario))
        return;
    Frontend& frontend = *hosted.frontend;
    const int talker = frontend.panes().begin()->first;
    (void)hosted.server.run({ "split-window", "-v", "-d", "-t", "%" + std::to_string(talker), "/bin/sh" });
    frontend.type(talker, "while :; do echo storm-out; sleep 0.005; done\r");
    std::mt19937 random(5760);
    for (int step = 0; step < 60; ++step) {
        const int columns = 30 + static_cast<int>(random() % 131);
        const int rows = 8 + static_cast<int>(random() % 43);
        if (TerminalSession* view = frontend.anyView())
            (void)view->reportTmuxClientSize(columns, rows);
        pumpFor(frontend, 5 + static_cast<int>(random() % 16));
    }
    if (TerminalSession* view = frontend.anyView())
        (void)view->reportTmuxClientSize(100, 30);
    pumpFor(frontend, 300);
    frontend.type(talker, "\x03");
    frontend.type(talker, "clear\r");
    // The session's windows take the size its one client reported. Read from
    // the windows: tmux leaves #{client_height} empty for a control client.
    std::string windowSizes;
    if (pumpUntil(frontend, 5'000, [&] {
            windowSizes = hosted.server.run({ "list-windows", "-t", "stress", "-F", "#{window_width}x#{window_height}" }).output;
            return windowSizes == "100x30\n";
        })
        == false)
        fail(scenario, "tmux's window is " + trimmedRight(windowSizes) + " after the storm settled at 100x30");
    if (const std::string differed = disagreement(frontend, hosted.server, 15'000); !differed.empty())
        fail(scenario, differed);
}

/// A bare host fed a recorded control stream in chunks, as reads cut it.
std::map<int, Rows> replay(const std::string& stream, const std::function<std::size_t(std::size_t)>& nextChunk)
{
    using namespace Ubiquitty::Runtime;
    struct Discarding final : Ubiquitty::PAL::ByteStream {
        int read(std::span<char>) override { return 0; }
        int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
        int writeAll(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
        bool drainPendingWrite() override { return false; }
        bool hasPendingWrite() const override { return false; }
        int lastError() const override { return 0; }
    };
    Discarding channel;
    auto primary = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*primary, 24, 80);
    std::mutex lock;
    HostedWindowSeams seams;
    seams.primary = primary.get();
    seams.primaryLock = &lock;
    seams.stream = &channel;
    seams.initialControlState = HostedControlState::AwaitingHandshake;
    seams.clockMicroseconds = [](void*) -> std::uint64_t { return 1'000'000; };
    // Posted work runs at once and delayed work never does: the same in every
    // chunking, so only where the reads were cut differs between them.
    seams.loop.runSoon = [](PostedWorkToken token) { runPostedSessionWork(token); };
    seams.loop.runAfter = [](double, PostedWorkToken token) { discardPostedSessionWork(token); };
    auto host = std::make_unique<TmuxControlHost>(seams);
    host->attach();
    std::map<int, std::vector<int>> windows;
    const auto takeEvents = [&] {
        for (HostedWindowEvent event = host->takeEvent(); event.kind != HostedWindowEvent::Kind::None; event = host->takeEvent()) {
            if (event.kind == HostedWindowEvent::Kind::LayoutChanged)
                windows[event.windowID] = event.paneIDs;
            else if (event.kind == HostedWindowEvent::Kind::WindowClosed) {
                windows.erase(event.windowID);
                host->releaseWindow(event.windowID);
            } else if (event.kind == HostedWindowEvent::Kind::Ended)
                windows.clear();
        }
    };
    for (std::size_t offset = 0; offset < stream.size();) {
        const std::size_t count = std::min(nextChunk(stream.size() - offset), stream.size() - offset);
        Ubiquitty::processBytes(primary.get(), std::string_view(stream).substr(offset, count));
        offset += count;
        takeEvents();
    }
    std::map<int, Rows> screens;
    for (const auto& [windowID, paneIDs] : windows) {
        for (int paneID : paneIDs) {
            const HostedPaneFacts facts = host->paneFacts(paneID);
            if (!facts.terminalAddress)
                continue;
            PaneView view { .session = nullptr, .terminal = reinterpret_cast<Ubiquitty::Terminal*>(facts.terminalAddress), .lock = reinterpret_cast<std::mutex*>(facts.lockAddress) };
            screens[paneID] = modelRows(view);
        }
    }
    host->detach();
    return screens;
}

void runSplit(const std::string& tmux)
{
    constexpr std::string_view scenario = "split";
    StreamRecording recording;
    runChurn(tmux, &recording);
    runStorm(tmux, &recording);
    std::string stream;
    {
        const std::scoped_lock hold(recording.lock);
        stream = recording.bytes;
    }
    // Two sessions' streams back to back: the second begins with its own
    // handshake once the first has exited, as a reattach's does.
    const std::size_t blocks = [&] {
        std::size_t count = 0;
        for (std::size_t at = stream.find("%begin "); at != std::string::npos; at = stream.find("%begin ", at + 1))
            ++count;
        return count;
    }();
    if (blocks < 20) {
        fail(scenario, "the recording holds " + std::to_string(blocks) + " command blocks, too few to cut");
        return;
    }
    const std::map<int, Rows> whole = replay(stream, [](std::size_t remaining) { return remaining; });
    if (whole.empty()) {
        fail(scenario, "the whole stream left no pane to compare");
        return;
    }
    struct Chunking {
        const char* name;
        std::function<std::size_t(std::size_t)> next;
    };
    std::mt19937 random(5761);
    const std::vector<Chunking> chunkings {
        { "one byte", [](std::size_t) { return std::size_t { 1 }; } },
        { "1-17 bytes", [&](std::size_t) { return std::size_t { 1 + random() % 17 }; } },
        { "1-4096 bytes", [&](std::size_t) { return std::size_t { 1 + random() % 4096 }; } },
        // Every cut lands inside a protocol line: just past the `%` of the next one.
        { "after each %", [&](std::size_t remaining) {
             const std::size_t offset = stream.size() - remaining;
             const std::size_t next = stream.find('%', offset + 1);
             return next == std::string::npos ? remaining : next - offset + 1;
         } },
    };
    for (const Chunking& chunking : chunkings) {
        const std::map<int, Rows> cut = replay(stream, chunking.next);
        if (cut != whole) {
            std::string differed = std::string("cut ") + chunking.name + ", the stream decodes differently from whole:";
            for (const auto& [paneID, rows] : whole) {
                const auto found = cut.find(paneID);
                if (found == cut.end())
                    differed += " pane %" + std::to_string(paneID) + " is missing;";
                else if (found->second != rows)
                    differed += " pane %" + std::to_string(paneID) + " shows\n" + describe(found->second) + "  whole, it shows\n" + describe(rows);
            }
            fail(scenario, differed);
        }
    }
    std::printf("tmux_control_stress: split: %zu bytes, %zu command blocks, %zu panes, decoded alike in %zu chunkings\n", stream.size(), blocks, whole.size(), chunkings.size());
}

/// Which end of a control session dies under it.
enum class Victim : std::uint8_t {
    /// The tmux server: its client says so with an %exit, wherever the
    /// stream was when it died.
    Server,
    /// The control client itself: its stream just stops.
    Client,
};

void runDeath(const std::string& tmux, Victim victim)
{
    const std::string_view scenario = victim == Victim::Server ? "death of the server" : "death of the client";
    StreamRecording recording;
    HostedSession hosted(tmux, &recording);
    if (!hosted.waitForFirstPane(scenario))
        return;
    Frontend& frontend = *hosted.frontend;
    const int talker = frontend.panes().begin()->first;
    (void)hosted.server.run({ "split-window", "-h", "-d", "-t", "%" + std::to_string(talker), "/bin/sh" });
    (void)hosted.server.run({ "new-window", "-d", "-t", "stress", "/bin/sh" });
    frontend.type(talker, "while :; do echo death-out; done\r");
    if (!pumpUntil(frontend, 10'000, [&] { return frontend.panes().size() == 3; }))
        fail(scenario, "the session's three panes were not all hosted");
    pumpFor(frontend, 300);
    const long long target = victim == Victim::Server ? hosted.server.number("#{pid}")
                                                      : std::atoll(hosted.server.run({ "list-clients", "-F", "#{client_pid}" }).output.c_str());
    if (target <= 0) {
        fail(scenario, "tmux did not name the pid to kill");
        return;
    }
    ::kill(static_cast<pid_t>(target), SIGKILL);
    const auto killed = Clock::now();
    if (!pumpUntil(frontend, 10'000, [&] { return frontend.hasEnded() && frontend.hasExited(); })) {
        fail(scenario, std::string("the hosting did not end after the kill (ended ") + (frontend.hasEnded() ? "yes" : "no") + ", exited " + (frontend.hasExited() ? "yes" : "no") + ", " + std::to_string(frontend.panes().size()) + " panes still bound)");
        const std::scoped_lock hold(recording.lock);
        const std::string tail = recording.bytes.substr(recording.bytes.size() > 600 ? recording.bytes.size() - 600 : 0);
        std::string shown;
        for (const char byte : tail)
            shown += byte == '\n' ? std::string("\\n\n") : (static_cast<unsigned char>(byte) < 0x20 ? "^" + std::to_string(static_cast<int>(byte)) : std::string(1, byte));
        std::fprintf(stderr, "tmux_control_stress: %.*s: the stream ended with\n%s\n", static_cast<int>(scenario.size()), scenario.data(), shown.c_str());
        return;
    }
    std::printf("tmux_control_stress: %.*s: the hosting ended %.0f ms after the kill\n", static_cast<int>(scenario.size()), scenario.data(),
        std::chrono::duration<double, std::milli>(Clock::now() - killed).count());
    if (!frontend.panes().empty() || !frontend.windows().empty())
        fail(scenario, "panes or windows outlived the hosting");
    if (!hosted.client->session()->sessionEnd().didEnd)
        fail(scenario, "the control client's session never ended");
    // Nothing more arrives, and nothing breaks for a while after.
    pumpFor(frontend, 500);
}

void runReattach(const std::string& tmux)
{
    constexpr std::string_view scenario = "reattach";
    HostedSession hosted(tmux);
    if (!hosted.waitForFirstPane(scenario))
        return;
    const int first = hosted.frontend->panes().begin()->first;
    (void)hosted.server.run({ "split-window", "-h", "-d", "-t", "%" + std::to_string(first), "/bin/sh" });
    (void)hosted.server.run({ "new-window", "-d", "-t", "stress", "/bin/sh" });
    if (!pumpUntil(*hosted.frontend, 10'000, [&] { return hosted.frontend->panes().size() == 3; })) {
        fail(scenario, "the session's three panes were not all hosted");
        return;
    }
    for (int paneID : hosted.listedPaneIDs())
        (void)hosted.server.run({ "send-keys", "-t", "%" + std::to_string(paneID), "echo marker-" + std::to_string(paneID * 7), "Enter" });
    // A line wider than its pane, so the screens recovered below carry a wrap.
    (void)hosted.server.run({ "send-keys", "-t", "%" + std::to_string(first), "printf '%0130d\\n' 0", "Enter" });
    if (const std::string differed = disagreement(*hosted.frontend, hosted.server, 15'000); !differed.empty()) {
        fail(scenario, "before the detach: " + differed);
        return;
    }

    // The reader detaches; the session stays on the server.
    TerminalSession* view = hosted.frontend->anyView();
    if (!view || !view->requestTmuxSessionClose(TerminalSession::TmuxSessionClose::Detach)) {
        fail(scenario, "the detach was not sent");
        return;
    }
    if (!pumpUntil(*hosted.frontend, 10'000, [&] { return hosted.frontend->hasEnded() && hosted.frontend->hasExited(); }))
        fail(scenario, "the hosting did not end on the detach");
    if (hosted.server.run({ "has-session", "-t", "stress" }).status != 0) {
        fail(scenario, "the detach ended the session on the server");
        return;
    }
    hosted.frontend.reset();
    hosted.client.reset();

    // And attaches again: the windows, the panes and their screens come back.
    hosted.client = std::make_unique<ControlClient>(hosted.server, std::vector<std::string> { "attach-session", "-t", "stress" });
    if (!hosted.client->isRunning()) {
        fail(scenario, "the second control client did not start");
        return;
    }
    hosted.frontend = std::make_unique<Frontend>(hosted.client->session());
    if (!pumpUntil(*hosted.frontend, 20'000, [&] { return hosted.frontend->panes().size() == 3; }))
        fail(scenario, "the reattach hosted " + std::to_string(hosted.frontend->panes().size()) + " of 3 panes");
    if (const std::string differed = disagreement(*hosted.frontend, hosted.server, 15'000); !differed.empty())
        fail(scenario, "after the reattach: " + differed);
    // The reattached panes take input.
    const int typed = hosted.frontend->panes().begin()->first;
    hosted.frontend->type(typed, "echo after-$((2*21))\r");
    if (!pumpUntil(*hosted.frontend, 10'000, [&] { return rowsContain(hosted.server.capture(typed), "after-42"); }))
        fail(scenario, "a reattached pane did not take input");
}

std::string findTmux()
{
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "/usr/bin:/bin";
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const std::string directory(rest.substr(0, colon));
        const std::string candidate = directory + "/tmux";
        if (::access(candidate.c_str(), X_OK) == 0)
            return candidate;
        if (colon == std::string_view::npos)
            break;
        rest.remove_prefix(colon + 1);
    }
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    ::signal(SIGPIPE, SIG_IGN);
    ::unsetenv("TMUX");
    const std::string tmux = findTmux();
    if (tmux.empty()) {
        std::fprintf(stderr, "tmux_control_stress: no tmux on PATH\n");
        return 77;
    }
    TerminalSession::enableHostedWindows(Ubiquitty::Runtime::tmuxControlHostFactoryAddress(), FrontendLoop::entryPoints());
    const std::map<std::string, std::function<void()>> scenarios {
        { "flood", [&] { runFlood(tmux); } },
        { "churn", [&] { runChurn(tmux, nullptr); } },
        { "storm", [&] { runStorm(tmux, nullptr); } },
        { "split", [&] { runSplit(tmux); } },
        { "death", [&] {
             runDeath(tmux, Victim::Server);
             runDeath(tmux, Victim::Client);
         } },
        { "reattach", [&] { runReattach(tmux); } },
    };
    std::vector<std::string> chosen(argv + 1, argv + argc);
    if (chosen.empty())
        chosen = { "flood", "churn", "storm", "split", "death", "reattach" };
    for (const std::string& name : chosen) {
        const auto found = scenarios.find(name);
        if (found == scenarios.end()) {
            std::fprintf(stderr, "tmux_control_stress: no scenario named %s\n", name.c_str());
            return 2;
        }
        const int before = failures;
        const auto started = Clock::now();
        found->second();
        std::printf("tmux_control_stress: %s: %s (%.1f s)\n", name.c_str(), failures == before ? "ok" : "FAILED",
            std::chrono::duration<double>(Clock::now() - started).count());
    }
    return failures == 0 ? 0 : 1;
}
