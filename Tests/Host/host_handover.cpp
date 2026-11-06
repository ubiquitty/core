// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/host_handover.cpp — a running daemon hands every live pane to the
// binary that replaced it at its install path (Docs/sync.md §6.13), between
// real processes, real PTYs and real `--attach` relays.
//
//   1. A copy of the binary at another path is refused: the holder keeps its
//      panes and still serves the person typing into them.
//   2. The binary at the holder's path is replaced and started. The holder
//      says `Bye{HandingOver}` on every connection and exits 0; each pane
//      keeps its paneID, session and scrollback under the successor; each
//      client resumes its attachment on the successor, which names the holder
//      as its predecessor; typed input reaches the inherited shells, and the
//      successor ends when they do. The holder's socket path stays bound to
//      the successor, so a reattach pinned to it still arrives. Another
//      install started beside the successor takes in none of the records the
//      successor keeps beside that socket.
//   3. Twelve more installs replace the binary in turn, each successor taking
//      every pane and both clients from the one before, with QUIC on. The
//      chain passes `maxHandoverListeners` sockets: each holder keeps the one
//      its records are kept beside, which the next successor reaches it
//      through, and lets the oldest of the others go, removing their paths.
//      The QUIC port and certificate are the first holder's at the end.
//
// argv[1] = the built ubiquitty-host. Exit 0 on success.
//

#include "Assertions.h"
#include "Host/HostConnection.h"
#include "quic_test_mint.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/SessionTree.h"
#include "Platform/Host/HostSocketName.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace Ubiquitty::HostTest;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// Every process this test started, each an exact unreaped direct child.
std::vector<pid_t> children;

pid_t startDaemon(const std::string& binary, const std::string& home)
{
    const pid_t daemon = ::fork();
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        // A shell that reads no rc file of the person running the test, and
        // starts and keeps its history in the private root, never their home.
        ::setenv("HOME", std::filesystem::path(home).parent_path().c_str(), 1);
        ::setenv("HISTFILE", "/dev/null", 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        // An ephemeral QUIC port, as `--ensure-daemon` opens: every Release
        // then carries the datagram socket beside the listeners.
        char* arguments[] = { const_cast<char*>(binary.c_str()), const_cast<char*>("--daemon"), const_cast<char*>("--quic-port"),
            const_cast<char*>("0"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(binary.c_str(), arguments);
        _exit(127);
    }
    if (daemon > 0)
        children.push_back(daemon);
    return daemon;
}

/// Waits for one direct child, for a bounded time.
/// - Returns: Its exit status, or nullopt if it is still running at the deadline.
std::optional<int> waitForExit(pid_t child, int milliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        int status = 0;
        const pid_t reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == child) {
            std::erase(children, child);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        if (reaped < 0 && errno != EINTR)
            return std::nullopt;
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        ::usleep(10'000);
    }
}

bool copyFile(const std::string& from, const std::string& to, std::string_view suffix = {})
{
    {
        std::ifstream source(from, std::ios::binary);
        std::ofstream target(to, std::ios::binary | std::ios::trunc);
        if (!source || !target)
            return false;
        target << source.rdbuf();
        target.write(suffix.data(), static_cast<std::streamsize>(suffix.size()));
        if (!target)
            return false;
    }
    return ::chmod(to.c_str(), 0755) == 0;
}

/// One person's connection through the binary's own `--attach` relay.
struct Client {
    std::optional<Connection> connection;
    Sync::ID128 instance;
    std::string workspace;
    std::uint32_t sessionID = 0;
    Sync::ID128 paneID;
    Sync::ID128 attachmentID;
    Sync::ID128 resumeSecret;
    int nextCall = 1;
};

bool connect(Client& client, const std::string& binary, const std::string& home, const std::string& socketLeaf,
    std::optional<Sync::ResumeRequest> resume)
{
    // A resumed attachment continues its input numbering: the host writes
    // each numbered event once, across connections and across the handover.
    const std::uint64_t nextInputSeq = client.connection ? client.connection->nextInputSeq : 1;
    client.connection.emplace();
    client.connection->nextInputSeq = nextInputSeq;
    const auto bridge = connectDaemonAsLocalBridge(binary, home);
    if (bridge.bridgeProcessID > 0)
        children.push_back(bridge.bridgeProcessID);
    client.connection->fileDescriptor = bridge.fileDescriptor;
    if (client.connection->fileDescriptor < 0)
        return false;
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = client.instance;
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request = resume ? Sync::AdmissionRequest { .target = Sync::AdmissionTarget::Session, .sessionID = client.sessionID, .isAttachOnly = true }
                              : Sync::AdmissionRequest { .target = Sync::AdmissionTarget::Session, .workspaceName = client.workspace };
    greeting.resume = std::move(resume);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    greeting.historyCacheLines = Sync::defaultHistoryCacheLines;
    greeting.backfillDepthLines = Sync::defaultBackfillDepthLines;
    std::string body;
    return sendGreeting(*client.connection, std::move(greeting)) && waitFor(*client.connection, Sync::FrameKind::Welcome, body, 5000)
        && client.connection->welcome && client.connection->admission == FixtureAdmission::Granted
        && client.connection->welcome->selection.socketName == socketLeaf && waitForPane(*client.connection, 5000);
}

/// Writes the record file of a host no longer running: one workspace,
/// `name`, with one pane.
bool writeDepartedRecords(const std::string& path, std::string_view name)
{
    Ubiquitty::Session::HostLayoutRecord record;
    record.arrangement.name = std::string(name);
    Ubiquitty::Session::WorkspaceWindow window;
    Ubiquitty::Session::WorkspaceTab tab;
    tab.root = Ubiquitty::Session::PaneNode::makeLeaf({}, "shell");
    tab.root.directory = "/tmp";
    window.tabs.push_back(tab);
    record.arrangement.windows.push_back(window);
    record.panes = { { 80, 24, "", "", {}, {} } };
    std::vector<std::uint8_t> bytes;
    Ubiquitty::Session::encodeHostLayoutRecords(std::array { record }, bytes);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return file && ::chmod(path.c_str(), 0600) == 0;
}

/// Opens `--attach --socket leaf`, the relay a pinned reattach runs.
int connectPinnedBridge(const std::string& binary, const std::string& home, const std::string& leaf)
{
    int ends[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
        return -1;
    const pid_t bridge = ::fork();
    if (bridge == 0) {
        ::dup2(ends[1], STDIN_FILENO);
        ::dup2(ends[1], STDOUT_FILENO);
        ::close(ends[0]);
        ::close(ends[1]);
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        // A local attach: a bridge that inherited an SSH session's variables
        // would mark its client as relayed from SSH.
        ::unsetenv("SSH_CONNECTION");
        ::unsetenv("SSH_CLIENT");
        ::execl(binary.c_str(), binary.c_str(), "--attach", "--socket", leaf.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(ends[1]);
    if (bridge < 0) {
        ::close(ends[0]);
        return -1;
    }
    children.push_back(bridge);
    return ends[0];
}

/// The `Welcome` a reattach pinned to `pinnedLeaf` is given through the
/// `--attach` relay of `binary`. A host answers only once it serves, after
/// its startup.
std::optional<Sync::Welcome> pinnedWelcome(const std::string& binary, const std::string& home, const std::string& pinnedLeaf)
{
    Connection pinned;
    pinned.fileDescriptor = connectPinnedBridge(binary, home, pinnedLeaf);
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 0x70696e6e6564, .high = 9 };
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    greeting.request = { .target = Sync::AdmissionTarget::Control };
    std::string body;
    std::optional<Sync::Welcome> welcome;
    if (pinned.fileDescriptor >= 0 && sendGreeting(pinned, std::move(greeting)) && waitFor(pinned, Sync::FrameKind::Welcome, body, 5000))
        welcome = pinned.welcome;
    if (pinned.fileDescriptor >= 0)
        ::close(pinned.fileDescriptor);
    return welcome;
}

/// Whether a reattach pinned to `pinnedLeaf` is admitted by the host serving
/// at `servingLeaf`, the instance `hostInstance`.
bool pinnedReattachReaches(const std::string& binary, const std::string& home, const std::string& pinnedLeaf,
    const std::string& servingLeaf, Sync::ID128 hostInstance)
{
    const auto welcome = pinnedWelcome(binary, home, pinnedLeaf);
    return welcome && welcome->selection.socketName == servingLeaf && welcome->hostInstanceID == hostInstance;
}

/// Reads the pane's retained text, scrollback included, through `pane.capture`.
std::optional<std::string> capture(Client& client)
{
    const std::string id = std::format("capture-{}", client.nextCall++);
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Call { .text = std::format(R"({{"jsonrpc":"2.0","id":"{}","method":"pane.capture","params":{{"pane":{},"source":"tail","lines":1000}}}})", id, client.connection->paneHandle) });
    if (!sendFrame(*client.connection, Sync::FrameKind::Call, { reinterpret_cast<const char*>(body.data()), body.size() }))
        return std::nullopt;
    std::string frame;
    if (!waitFor(*client.connection, Sync::FrameKind::Result, frame, 5000))
        return std::nullopt;
    const auto result = Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size() });
    if (!result || result->transfer) {
        std::fprintf(stderr, "host_handover: capture answered %s\n", !result ? "nothing decodable" : "through a transfer");
        return std::nullopt;
    }
    bool isParsed = false;
    const auto parsed = Ubiquitty::Codec::JSON::parse(result->text, isParsed);
    const auto* answer = isParsed ? parsed.find("result") : nullptr;
    const auto* text = answer ? answer->find("text") : nullptr;
    if (!text || !text->isString())
        return std::nullopt;
    return text->asString();
}

/// Polls the pane's text until it holds `needle`, for a bounded time.
bool waitForText(Client& client, std::string_view needle, int milliseconds = 10'000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto text = capture(client);
        if (text && text->find(needle) != std::string::npos)
            return true;
        if (!text && client.connection->hasFailed)
            return false;
        ::usleep(50'000);
    }
    return false;
}

void run(const std::string& built)
{
    // Under /tmp, not $TMPDIR: a Mac's per-user temporary directory is long
    // enough that the daemon's socket path inside this home would pass the
    // 103 bytes a Unix socket's path may hold.
    std::string pattern = "/tmp/ubq-hh.XXXXXX";
    if (!::mkdtemp(pattern.data())) {
        CHECK_MSG(false, "a private directory");
        return;
    }
    const std::string root = pattern;
    const std::string home = root + "/.ubiquitty";
    const std::string binary = root + "/bin/ubiquitty-host";
    ::mkdir(home.c_str(), 0700);
    ::mkdir((root + "/bin").c_str(), 0755);
    ::mkdir((root + "/other").c_str(), 0755);
    if (!copyFile(built, binary)) {
        CHECK_MSG(false, "the holder's install");
        return;
    }
    const std::string holderLeaf = Ubiquitty::Host::socketLeafForBinary(binary.c_str());
    const pid_t holder = startDaemon(binary, home);
    if (holder <= 0 || !waitForDaemonReady(home + "/" + holderLeaf)) {
        CHECK_MSG(false, "the holder listens");
        return;
    }
    std::string firstQUICToken;
    std::uint16_t firstQUICPort = 0;
    std::string firstQUICPin;
    CHECK_MSG(mintQUICTokenViaBootstrap(binary, home, firstQUICToken, firstQUICPort, firstQUICPin) && firstQUICPort,
        "premise: the holder serves QUIC");

    std::array<Client, 2> clients;
    for (std::size_t index = 0; index < clients.size(); ++index) {
        auto& client = clients[index];
        client.instance = { .low = 0x68616e646f766572, .high = index + 1 };
        client.workspace = std::format("handover-{}", index);
        if (!connect(client, binary, home, holderLeaf, std::nullopt)) {
            CHECK_MSG(false, "a person attaches to the holder");
            return;
        }
        const auto& welcome = *client.connection->welcome;
        client.sessionID = welcome.selection.sessionID;
        client.paneID = client.connection->panes.front().paneID;
        client.attachmentID = welcome.attachmentID;
        client.resumeSecret = welcome.resumeSecret;
        // Two hundred lines push the first ones into the pane's scrollback.
        CHECK_MSG(sendInput(*client.connection, std::format("i=0; while [ $i -lt 200 ]; do echo scroll-{}-$i; i=$((i+1)); done; echo rea\"\"dy-{}\r", index, index)),
            "input reaches the holder");
        CHECK_MSG(waitForText(client, std::format("ready-{}", index)), "the holder's shell answers");
    }
    const auto holderInstance = clients[0].connection->welcome->hostInstanceID;
    CHECK_MSG(clients[1].connection->welcome->hostInstanceID == holderInstance, "premise: one holder serves both panes");
    CHECK_MSG(clients[0].paneID != clients[1].paneID, "premise: two panes");
    const auto before = capture(clients[0]);
    CHECK_MSG(before && before->find("scroll-0-0\n") != std::string::npos, "premise: the first lines are in the holder's scrollback");
    if (before && before->find("scroll-0-0\n") == std::string::npos)
        std::fprintf(stderr, "host_handover: the holder's text begins %s\n", before->substr(0, 400).c_str());

    // 1. A successor not found at the holder's install path is refused.
    {
        const std::string stranger = root + "/other/ubiquitty-host";
        CHECK_MSG(copyFile(built, stranger, "\nanother install\n"), "the stranger's install");
        const pid_t other = startDaemon(stranger, home);
        CHECK_MSG(other > 0 && waitForDaemonReady(home + "/" + Ubiquitty::Host::socketLeafForBinary(stranger.c_str())), "the stranger starts");
        const auto refusedExit = waitForExit(holder, 1000);
        CHECK_MSG(!refusedExit, "a refused handover leaves the holder running");
        if (refusedExit)
            std::fprintf(stderr, "host_handover: the holder ended with status %d\n", *refusedExit);
        CHECK_MSG(sendInput(*clients[0].connection, "echo still\"\"-served\r") && waitForText(clients[0], "still-served"),
            "a refused handover leaves the holder serving its panes");
        if (other > 0) {
            ::kill(other, SIGTERM);
            CHECK_MSG(waitForExit(other, 10'000).has_value(), "the stranger ends");
        }
    }

    // A host that stopped long before left its records beside a socket of its
    // own; the successor takes them in as it starts.
    constexpr std::string_view departedName = "left by a stopped host";
    CHECK_MSG(writeDepartedRecords(home + "/host-00000000000de9a1.sock.layouts", departedName), "premise: a stopped host's records");

    // 2. The binary at the holder's path is replaced, and the new one started.
    const std::string staged = binary + ".next";
    CHECK_MSG(copyFile(binary, staged, "\nhandover successor\n") && ::rename(staged.c_str(), binary.c_str()) == 0, "the successor is installed in place");
    const std::string successorLeaf = Ubiquitty::Host::socketLeafForBinary(binary.c_str());
    CHECK_MSG(successorLeaf != holderLeaf, "premise: the successor owns another socket");
    const pid_t successor = startDaemon(binary, home);
    const auto holderExit = waitForExit(holder, 30'000);
    CHECK_MSG(holderExit == std::optional { 0 }, "the holder exits cleanly after handing over");
    for (auto& client : clients) {
        std::string body;
        const bool sawBye = waitFor(*client.connection, Sync::FrameKind::Bye, body, 5000);
        const auto bye = sawBye ? Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }) : std::nullopt;
        CHECK_MSG(bye && bye->reason == Sync::ByeReason::HandingOver, "every connection is told the holder is handing over");
        ::close(client.connection->fileDescriptor);
    }
    CHECK_MSG(successor > 0 && waitForDaemonReady(home + "/" + successorLeaf), "the successor listens");

    for (std::size_t index = 0; index < clients.size(); ++index) {
        auto& client = clients[index];
        const Sync::ResumeRequest resume { .attachmentID = client.attachmentID, .resumeSecret = client.resumeSecret, .panes = {} };
        if (!connect(client, binary, home, successorLeaf, resume)) {
            CHECK_MSG(false, "a person reconnects to the successor");
            continue;
        }
        const auto& welcome = *client.connection->welcome;
        CHECK_MSG(welcome.attachmentID == client.attachmentID, "the reconnect resumes the attachment the holder issued");
        CHECK_MSG(welcome.hostInstanceID != holderInstance, "the successor is a new host instance");
        CHECK_MSG(welcome.predecessorHostInstanceID == std::optional { holderInstance }, "the successor names the holder as its predecessor");
        CHECK_MSG(welcome.selection.sessionID == client.sessionID, "the session keeps its id");
        CHECK_MSG(client.connection->panes.front().paneID == client.paneID, "the pane keeps its paneID");
        // Each resume rotates the secret; the next one presents the new one.
        client.resumeSecret = welcome.resumeSecret;
        const auto text = capture(client);
        CHECK_MSG(text && text->find(std::format("scroll-{}-0\n", index)) != std::string::npos && text->find(std::format("ready-{}\n", index)) != std::string::npos,
            "the pane keeps its scrollback");
        CHECK_MSG(sendInput(*client.connection, std::format("echo aft\"\"er-{}\r", index)) && waitForText(client, std::format("after-{}", index)),
            "the inherited shell takes input and its output reaches the client");
    }
    // What the successor took in at startup moved with its takeover into the
    // records the chain keeps beside the first holder's socket, and the file
    // it had first written them to is gone.
    {
        std::ifstream chainRecords(home + "/" + holderLeaf + ".layouts", std::ios::binary);
        const std::string held((std::istreambuf_iterator<char>(chainRecords)), std::istreambuf_iterator<char>());
        CHECK_MSG(held.find(departedName) != std::string::npos, "the chain's records keep what the successor took in at startup");
        CHECK_MSG(::access((home + "/" + successorLeaf + ".layouts").c_str(), F_OK) != 0, "the successor's startup records file is retired");
    }
    // A client whose reattach pinned the holder's socket reaches the successor
    // there: the path stays bound to the listener the successor inherited.
    struct stat holderSocket {};
    CHECK_MSG(::stat((home + "/" + holderLeaf).c_str(), &holderSocket) == 0 && S_ISSOCK(holderSocket.st_mode),
        "the holder's socket path outlives the holder");
    CHECK_MSG(pinnedReattachReaches(binary, home, holderLeaf, successorLeaf, clients[0].connection->welcome->hostInstanceID),
        "a reattach pinned to the holder's socket reaches the successor");

    // Another install started beside the successor takes in none of the
    // records it keeps: the first holder has gone and nothing holds the lock
    // beside them, but the successor serves the socket they are kept beside.
    {
        const std::string records = home + "/" + holderLeaf + ".layouts";
        CHECK_MSG(::access(records.c_str(), F_OK) == 0, "premise: the first holder recorded its workspaces");
        const std::string stranger = root + "/other/ubiquitty-host";
        CHECK_MSG(copyFile(built, stranger, "\nanother install beside a successor\n"), "the second stranger's install");
        const std::string strangerLeaf = Ubiquitty::Host::socketLeafForBinary(stranger.c_str());
        const pid_t other = startDaemon(stranger, home);
        CHECK_MSG(other > 0 && waitForDaemonReady(home + "/" + strangerLeaf) && pinnedWelcome(stranger, home, strangerLeaf),
            "the second stranger finishes starting");
        CHECK_MSG(::access(records.c_str(), F_OK) == 0, "the successor's records stay where it keeps them");
        CHECK_MSG(::access((home + "/" + strangerLeaf + ".layouts").c_str(), F_OK) != 0, "the stranger takes in none of the successor's records");
        if (other > 0) {
            ::kill(other, SIGTERM);
            CHECK_MSG(waitForExit(other, 10'000).has_value(), "the second stranger ends");
        }
    }

    // 3. Twelve more installs, each successor taking every pane and both
    //    clients from the one before. From the ninth on, a holder serves more
    //    sockets than one Release carries.
    std::vector<std::string> leaves { holderLeaf, successorLeaf };
    pid_t serving = successor;
    constexpr int laterInstalls = 12;
    for (int install = 0; install < laterInstalls && !failures; ++install) {
        const std::size_t generation = leaves.size();
        CHECK_MSG(copyFile(binary, staged, std::format("\ngeneration {}\n", generation)) && ::rename(staged.c_str(), binary.c_str()) == 0,
            "the next successor is installed in place");
        const std::string leaf = Ubiquitty::Host::socketLeafForBinary(binary.c_str());
        const pid_t next = startDaemon(binary, home);
        const auto exited = waitForExit(serving, 30'000);
        CHECK_MSG(exited == std::optional { 0 }, std::format("the holder of generation {} hands every pane on and exits", generation - 1).c_str());
        if (exited != std::optional { 0 } || next <= 0 || !waitForDaemonReady(home + "/" + leaf)) {
            CHECK_MSG(false, std::format("generation {} serves its panes", generation).c_str());
            break;
        }
        for (std::size_t index = 0; index < clients.size(); ++index) {
            auto& client = clients[index];
            std::string body;
            CHECK_MSG(waitFor(*client.connection, Sync::FrameKind::Bye, body, 5000), "every connection is told the holder is handing over");
            ::close(client.connection->fileDescriptor);
            const Sync::ResumeRequest resume { .attachmentID = client.attachmentID, .resumeSecret = client.resumeSecret, .panes = {} };
            if (!connect(client, binary, home, leaf, resume)) {
                CHECK_MSG(false, std::format("a person reconnects to generation {}", generation).c_str());
                continue;
            }
            const auto& welcome = *client.connection->welcome;
            CHECK_MSG(welcome.attachmentID == client.attachmentID && client.connection->panes.front().paneID == client.paneID,
                std::format("generation {} resumes the attachment and its pane", generation).c_str());
            client.resumeSecret = welcome.resumeSecret;
            CHECK_MSG(sendInput(*client.connection, std::format("echo gen\"\"eration-{}-{}\r", generation, index))
                    && waitForText(client, std::format("generation-{}-{}", generation, index)),
                std::format("generation {} drives the inherited shell", generation).c_str());
        }
        serving = next;
        leaves.push_back(leaf);
    }
    if (failures) {
        std::fprintf(stderr, "host_handover: logs kept in %s\n", home.c_str());
        return;
    }
    {
        // The last successor serves its own socket and the eight it was
        // handed: the first holder's, beside which every holder kept the
        // records, and the newest seven others. The oldest others were let go.
        const std::size_t newest = leaves.size() - 1;
        const std::size_t oldestKept = newest - (std::size_t { Sync::maxHandoverListeners } - 1);
        for (std::size_t index = 0; index < newest; ++index) {
            const bool isKept = index == 0 || index >= oldestKept;
            const std::string path = home + "/" + leaves[index];
            const int probe = connectDaemon(path);
            if (probe >= 0)
                ::close(probe);
            CHECK_MSG((probe >= 0) == isKept,
                std::format("generation {}'s socket is {} by the last successor", index, isKept ? "served" : "let go").c_str());
            // The holder that let a socket go removed its path.
            CHECK_MSG(isKept || ::access(path.c_str(), F_OK) != 0, std::format("generation {}'s let-go socket leaves no path", index).c_str());
        }
        CHECK_MSG(pinnedReattachReaches(binary, home, holderLeaf, leaves.back(), clients[0].connection->welcome->hostInstanceID),
            "a reattach pinned to the first holder's socket reaches the last successor");
        std::string token;
        std::uint16_t port = 0;
        std::string pin;
        CHECK_MSG(mintQUICTokenViaBootstrap(binary, home, token, port, pin) && port == firstQUICPort && pin == firstQUICPin,
            "the last successor serves QUIC on the first holder's port and certificate");
    }

    for (auto& client : clients) {
        std::string body;
        CHECK_MSG(sendInput(*client.connection, "exit\r") && waitFor(*client.connection, Sync::FrameKind::PaneUnbind, body, 10'000),
            "the inherited shell ends on the successor");
        ::close(client.connection->fileDescriptor);
        client.connection->fileDescriptor = -1;
    }
    const auto successorExit = waitForExit(serving, 15'000);
    CHECK_MSG(successorExit == std::optional { 0 }, "the successor ends when its inherited shells exit");
    // A failing run keeps the daemons' logs for the reader.
    if (failures) {
        std::fprintf(stderr, "host_handover: logs kept in %s\n", home.c_str());
        return;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    run(argv[1]);
    // Only this test's own direct children, each still unreaped, are ended.
    for (const pid_t child : std::vector(children)) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
    }
    if (failures == 0)
        std::printf("host_handover: a stranger's request is refused while the holder serves; the replaced binary takes every pane with its id, "
                    "session and scrollback, the holder exits 0 after Bye{HandingOver}, and each client resumes on the successor; "
                    "twelve more installs each take every pane, keep the first holder's socket and QUIC port, and let the oldest others go\n");
    return failures == 0 ? 0 : 1;
}
