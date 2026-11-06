// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/tmux_real_e2e.cpp — the TmuxClient parser against a real tmux
// in control mode, over pipes on a private socket. The unit tests feed
// crafted lines; this closes the loop the crafts only approximate: our
// wrapKeyForPane output is what real tmux executes, and real tmux's %output,
// %session-renamed, %layout-change and list-panes rows are what our parser
// and record decoders read back. A grammar drift between tmux and the
// crafted strings fails in this stanza before it fails for a user.
//
// Skipped by the stanza when no tmux binary exists. Exit 0 on success.
//

#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/Tmux.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string guardedAttachOutput(const Ubiquitty::Protocol::Tmux::SessionReference& reference)
{
    auto arguments = Ubiquitty::Protocol::Tmux::restoreSessionArguments("tmux", reference);
    // Pipes require control mode without the terminal negotiation used by -CC.
    arguments[3] = "-C";
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    int input[2];
    int output[2];
    if (::pipe(input)) {
        return {};
    }
    if (::pipe(output)) {
        ::close(input[0]);
        ::close(input[1]);
        return {};
    }
    const auto child = ::fork();
    if (!child) {
        ::dup2(input[0], STDIN_FILENO);
        ::dup2(output[1], STDOUT_FILENO);
        ::dup2(output[1], STDERR_FILENO);
        ::close(input[0]);
        ::close(input[1]);
        ::close(output[0]);
        ::close(output[1]);
        ::execvp(pointers.front(), pointers.data());
        _exit(127);
    }
    ::close(input[0]);
    ::close(output[1]);
    std::string result;
    if (child > 0) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            pollfd descriptor { output[0], POLLIN, 0 };
            if (::poll(&descriptor, 1, 20) <= 0) {
                continue;
            }
            char bytes[4096];
            const auto count = ::read(output[0], bytes, sizeof(bytes));
            if (count <= 0) {
                break;
            }
            result.append(bytes, static_cast<std::size_t>(count));
            if (result.find("%session-changed ") != std::string::npos || result.find("%exit") != std::string::npos) {
                break;
            }
        }
    }
    ::close(input[1]);
    ::close(output[0]);
    if (child > 0) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::waitpid(child, nullptr, WNOHANG) == child) {
                return result;
            }
            ::usleep(10000);
        }
        ::kill(child, SIGKILL);
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    return result;
}

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "tmux_real_e2e: FAIL — %s\n", what);
        ++failures;
    }
}

struct Harness {
    Ubiquitty::Protocol::Tmux::TmuxClient client;
    std::string delivered; // pane bytes, post-decode
    std::string renamedTo;
    std::string lastLayout;
    std::vector<std::string> responseLines;
    bool exited = false;
};

void deliverByte(void* context, uint8_t byte)
{
    static_cast<Harness*>(context)->delivered.push_back(static_cast<char>(byte));
}

void deliverSpan(void* context, std::string_view bytes)
{
    static_cast<Harness*>(context)->delivered.append(bytes);
}

} // namespace

int main()
{
    namespace Tmux = Ubiquitty::Protocol::Tmux;

    char directoryTemplate[] = "/tmp/ubiquitty-tmux-e2e.XXXXXX";
    char* directory = ::mkdtemp(directoryTemplate);
    if (!directory)
        return 1;
    const std::string socketPath = std::string(directory) + "/sock 'quoted; path";

    int toTmux[2];
    int fromTmux[2];
    if (::pipe(toTmux) != 0 || ::pipe(fromTmux) != 0)
        return 1;

    const pid_t child = ::fork();
    if (child < 0)
        return 1;
    if (child == 0) {
        ::dup2(toTmux[0], STDIN_FILENO);
        ::dup2(fromTmux[1], STDOUT_FILENO);
        ::close(toTmux[0]);
        ::close(toTmux[1]);
        ::close(fromTmux[0]);
        ::close(fromTmux[1]);
        // -C control mode over the pipes; a private socket and no user config,
        // so nothing on the machine leaks in and nothing of ours leaks out.
        ::execlp("tmux", "tmux", "-S", socketPath.c_str(), "-f", "/dev/null", "-C",
            "new-session", "-x", "80", "-y", "24", "/bin/sh", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(toTmux[0]);
    ::close(fromTmux[1]);
    const int inputFileDescriptor = fromTmux[0];
    const int outputFileDescriptor = toTmux[1];
    ::fcntl(inputFileDescriptor, F_SETFL, O_NONBLOCK);

    Harness harness;
    Tmux::init(&harness.client);
    Tmux::configure(&harness.client, Tmux::IntegrationMode::Enabled, Tmux::AutoDetectMode::Automatic);
    Tmux::TmuxEventClient events;
    events.context = &harness;
    events.sessionRenamed = [](void* context, std::string_view name) {
        static_cast<Harness*>(context)->renamedTo = std::string(name);
    };
    events.layoutChange = [](void* context, int, std::string_view layout) {
        static_cast<Harness*>(context)->lastLayout = std::string(layout);
    };
    events.commandResponseLine = [](void* context, std::string_view line) {
        static_cast<Harness*>(context)->responseLines.emplace_back(line);
    };
    events.exit = [](void* context) { static_cast<Harness*>(context)->exited = true; };
    Tmux::setEventClient(&harness.client, events);

    // Pump tmux's output through the parser until `until` answers true or the
    // budget lapses.
    const auto pumpUntil = [&](int budgetMilliseconds, auto until) {
        for (int waited = 0; waited < budgetMilliseconds;) {
            if (until())
                return true;
            struct pollfd descriptor { inputFileDescriptor, POLLIN, 0 };
            const int ready = ::poll(&descriptor, 1, 25);
            waited += 25;
            if (ready <= 0)
                continue;
            char buffer[4096];
            const ssize_t bytesRead = ::read(inputFileDescriptor, buffer, sizeof(buffer));
            if (bytesRead > 0)
                Tmux::feedInputBytes(&harness.client, { buffer, static_cast<std::size_t>(bytesRead) },
                    &harness, deliverByte, deliverSpan);
            else if (bytesRead == 0)
                return until();
        }
        return until();
    };
    const auto sendCommand = [&](const std::string& command) {
        const std::string line = command + "\n";
        (void)!::write(outputFileDescriptor, line.data(), line.size());
    };

    // The control-mode banner enters the parser's control state.
    expect(pumpUntil(10000, [&] { return Tmux::isActive(&harness.client); }),
        "real tmux's control-mode banner activates the client");

    // Our own key wrapper drives the real pane, and the real %output stream
    // decodes back to the bytes the shell printed.
    char wrapped[512];
    const std::string keys = "printf 'e2e-mark\\n'\r";
    const int wrappedLength = Tmux::wrapKeyForPane(&harness.client, 0, keys, wrapped);
    expect(wrappedLength > 0, "wrapKeyForPane wraps against the live client");
    (void)!::write(outputFileDescriptor, wrapped, static_cast<std::size_t>(wrappedLength));
    expect(pumpUntil(10000, [&] { return harness.delivered.find("e2e-mark") != std::string::npos; }),
        "the wrapped keys ran and the %output stream decoded the shell's answer");

    // A real rename lands through %session-renamed with tmux 3.x's $id prefix
    // already stripped.
    sendCommand("rename-session e2e-renamed");
    expect(pumpUntil(10000, [&] { return harness.renamedTo == "e2e-renamed"; }),
        "%session-renamed hands the hook the bare new name");

    // A real split publishes a %layout-change whose layout string our own
    // grammar parses to both panes.
    sendCommand("split-window -h");
    expect(pumpUntil(10000, [&] {
        if (harness.lastLayout.empty())
            return false;
        Tmux::TmuxPaneGeometry geometry[8];
        return Tmux::parseLayout(harness.lastLayout, geometry) == 2;
    }),
        "the real %layout-change layout parses to two panes");

    // The real list-panes rows, in the enumeration Commands.h requests,
    // decode through parsePaneRecord.
    harness.responseLines.clear();
    sendCommand(Tmux::listPanesForSession());
    expect(pumpUntil(10000, [&] { return harness.responseLines.size() >= 2; }),
        "list-panes answers one row per pane");
    int decodedRows = 0;
    for (const std::string& row : harness.responseLines) {
        std::string_view view = row;
        if (view.size() >= 2 && view.front() == '"' && view.back() == '"')
            view = view.substr(1, view.size() - 2);
        int windowID = 0, width = 0, height = 0, active = 0;
        Tmux::PaneID paneID = 0;
        std::string layout, session, name;
        if (Tmux::parsePaneRecord(view, windowID, paneID, width, height, active, layout, session, name)) {
            decodedRows++;
            expect(width > 0 && height > 0, "a real pane row carries live geometry");
            expect(session == "e2e-renamed", "the row names the renamed session");
        }
    }
    expect(decodedRows >= 2, "every real pane row decodes through parsePaneRecord");

    harness.responseLines.clear();
    sendCommand(Tmux::sessionReferenceQuery());
    std::optional<Tmux::SessionReference> reference;
    expect(pumpUntil(10000, [&] {
        for (const auto& line : harness.responseLines) {
            if (auto parsed = Tmux::parseSessionReference(line)) {
                reference = std::move(parsed);
                return true;
            }
        }
        return false;
    }),
        "the real server supplies a complete session identity");
    if (reference) {
        expect(reference->socketPath == socketPath, "the identity preserves the actual server socket");
        expect(guardedAttachOutput(*reference).find("%session-changed ") != std::string::npos,
            "the saved identity attaches the surviving renamed session");
        for (const auto field : { &Tmux::SessionReference::serverProcessID, &Tmux::SessionReference::serverStartTime,
                 &Tmux::SessionReference::sessionID, &Tmux::SessionReference::sessionCreated }) {
            auto stale = *reference;
            ++(stale.*field);
            const auto output = guardedAttachOutput(stale);
            expect(output.find("%session-changed ") == std::string::npos, "a stale identity never attaches");
            expect(output.find("%exit") != std::string::npos, "a refused restore exits control mode");
        }
    }

    // Teardown: the server's death reaches the parser as %exit / stream end.
    sendCommand("kill-server");
    expect(pumpUntil(10000, [&] { return harness.exited; }), "kill-server reaches the exit hook");

    ::close(outputFileDescriptor);
    ::close(inputFileDescriptor);
    for (int i = 0; i < 200; ++i) {
        if (::waitpid(child, nullptr, WNOHANG) == child)
            break;
        ::usleep(10 * 1000);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, WNOHANG);
    ::unlink(socketPath.c_str());

    if (failures == 0) {
        std::printf("tmux_real_e2e: wrap, %%output decode, rename, layout and pane rows agree with tmux %s\n", "3.x");
        return 0;
    }
    std::fprintf(stderr, "tmux_real_e2e: %d failure(s)\n", failures);
    return 1;
}
