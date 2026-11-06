// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Agent/AgentSession.h"
#include "Agent/AgentCapabilityPolicy.h"
#include "Agent/RemoteCarrier.h"

#include "Core/Accessibility/TextProjection.h"
#include "Core/Codec/JSON.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/Control/SessionTermination.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/OverlayAssembly.h"
#include "Core/Rendering/PNGEncoder.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/HostListing.h"
#include "Core/Session/LaunchContext.h"
#include "Core/TermPalette.h"
#include "Core/TerminalOperations.h"
#include "PAL/Socket.h"
#include "Platform/Host/HostPaths.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;

namespace Ubiquitty {
namespace Agent {

namespace {

// The most events held for a caller that has not taken them. Past it the
// oldest are folded into a gap (Session::holdEvent), so a caller that fell
// this far behind is told, and resumes from its cursor.
constexpr std::size_t maxHeldEvents = 4096;

// Connect a fresh SOCK_STREAM to a daemon socket. Returns a valid fd owner, or empty.
//
// The peer must run as this account: the token goes in the first frame, and a
// socket another user listens on, wherever it sits, is not this account's
// daemon.
PAL::UnixFileDescriptor connectToDaemon(const char* socketPath)
{
    if (!Host::isOwnHostSocketPath(socketPath))
        return {};
    PAL::UnixFileDescriptor socketFileDescriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socketFileDescriptor)
        return {};
    struct sockaddr_un address;
    address = {};
    address.sun_family = AF_UNIX;
    if (std::strlen(socketPath) >= sizeof(address.sun_path))
        return {};
    std::strncpy(address.sun_path, socketPath, sizeof(address.sun_path) - 1);
    if (::connect(socketFileDescriptor.get(), reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0)
        return {};
    if (!PAL::peerIsSameUser(socketFileDescriptor))
        return {};
    return socketFileDescriptor;
}

// Spawn `hostBinary --ensure-daemon` (it double-forks, detaches, and
// exits 0 once the socket listens) and reap the launcher; its status is
// whether a daemon now accepts. Lets the
// agent bring a daemon up on demand when $UBIQUITTY_HOST_BIN points at
// the ubiquitty-host binary (the MCP registration sets it).
//
// The daemon gets this process's environment without its credentials: every
// shell the daemon starts inherits the daemon's environment, and a token
// handed to ubq is not for them.
bool spawnEnsureDaemon(const char* hostBinary)
{
    std::vector<char*> environment;
    for (char** entry = environ; *entry; ++entry) {
        if (!Ubiquitty::Session::isCredentialEnvironmentEntry(*entry))
            environment.push_back(*entry);
    }
    environment.push_back(nullptr);
    std::array<char, 16> ensureFlag { "--ensure-daemon" };
    std::array<char*, 3> arguments { const_cast<char*>(hostBinary), ensureFlag.data(), nullptr };
    pid_t processID = ::fork();
    if (processID < 0)
        return false;
    if (processID == 0) {
        ::execve(hostBinary, arguments.data(), environment.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(processID, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// 8-bit RGB resolved from a Pen + live palette. Colors resolve through
// the one portable resolver (Rendering::ColorResolve) the GPU + Metal +
// Linux backends share, so bold-as-bright / reverse / hidden / dim are
// handled identically everywhere.
using RGB8 = Ubiquitty::Rendering::RGB8;

} // namespace

AgentSession::AgentSession()
{
    m_term = std::make_unique<Ubiquitty::Terminal>();
    m_core = std::make_unique<Ubiquitty::Protocol::Sync::SyncClientCore>(m_term.get(), this);
    // This grid mirrors the daemon's, so a local clear must invalidate the
    // baseline the next difference frame is computed against. Nothing reports
    // the divergence otherwise: no frame is missed, so the difference applies
    // cleanly to the wrong content.
    m_term->mirrorClient = m_core.get();
    // Installed with the client rather than after it: an unset policy answers
    // PAL's defaults, which allow a Local principal everything, rather than this
    // module's refusal of every effect.
    m_term->capabilityPolicy = &m_capabilityPolicy;
}

AgentSession::~AgentSession()
{
    // The descriptor is the carrier's client end; closing it first lets the
    // carrier's outbound thread see end of file before the carrier stops.
    m_socketFileDescriptor.reset();
    // m_core is declared after m_term, so it is destroyed first and the Terminal
    // would briefly point at a destroyed client. A destructor body runs before
    // any member is destroyed; the members cannot be reordered, because the core
    // is constructed from the Terminal's address.
    m_term->mirrorClient = nullptr;
}

bool AgentSession::openSocketAndPrepare()
{
    if (m_captureConnectionGeneration == std::numeric_limits<std::uint64_t>::max())
        return false;
    ++m_captureConnectionGeneration;
    m_connectFailure.clear();
    m_isConnectedToPaneDaemon = false;
    if (!m_remoteHost.empty()) {
        m_remoteCarrier = std::make_unique<RemoteCarrier>();
        PAL::UnixFileDescriptor carried = m_remoteCarrier->open(m_remoteHost, m_connectFailure);
        if (!carried)
            return false;
        int flags = ::fcntl(carried.get(), F_GETFL, 0);
        if (flags != -1)
            ::fcntl(carried.get(), F_SETFL, flags | O_NONBLOCK);
        m_socketFileDescriptor = std::move(carried);
        return true;
    }
    // A program running in a pane is told the socket of the daemon that runs
    // it; that daemon is the one its `here` names, so it is asked first.
    if (const char* serving = ::getenv(std::string(Ubiquitty::Session::socketEnvironmentName).c_str()); serving && *serving) {
        if (PAL::UnixFileDescriptor fileDescriptor = connectToDaemon(serving)) {
            int flags = ::fcntl(fileDescriptor.get(), F_GETFL, 0);
            if (flags != -1)
                ::fcntl(fileDescriptor.get(), F_SETFL, flags | O_NONBLOCK);
            m_socketFileDescriptor = std::move(fileDescriptor);
            m_isConnectedToPaneDaemon = true;
            return true;
        }
    }
    // Versioned sockets (HostSocketName.h) mean several daemons can coexist
    // during an auto-update, and the agent can't derive the host binary's
    // hash. Probe candidates newest-first and take the first that ANSWERS —
    // connecting (not just stat) skips a stale newest socket (a crashed
    // daemon) for a live older one.
    auto tryConnect = [](const char* candidate, void* ctx) -> bool {
        PAL::UnixFileDescriptor fileDescriptor = connectToDaemon(candidate);
        if (!fileDescriptor)
            return false;
        *static_cast<PAL::UnixFileDescriptor*>(ctx) = std::move(fileDescriptor);
        return true;
    };
    PAL::UnixFileDescriptor socketFileDescriptor;
    Ubiquitty::Host::eachHostSocketNewestFirst(tryConnect, &socketFileDescriptor);
    if (!socketFileDescriptor) {
        // No live daemon — start one if we know where the host binary is
        // ($UBIQUITTY_HOST_BIN), then probe again. The launcher returns once
        // the daemon's socket listens, however long a loaded machine takes,
        // so one probe answers; the daemon binds a versioned socket whose
        // name is not known here, so the probe is newest-first.
        const char* hostBinary = ::getenv("UBIQUITTY_HOST_BIN");
        if (hostBinary && hostBinary[0] && spawnEnsureDaemon(hostBinary))
            Ubiquitty::Host::eachHostSocketNewestFirst(tryConnect, &socketFileDescriptor);
        if (!socketFileDescriptor)
            return false;
    }
    int flags = ::fcntl(socketFileDescriptor.get(), F_GETFL, 0);
    if (flags != -1)
        ::fcntl(socketFileDescriptor.get(), F_SETFL, flags | O_NONBLOCK);
    m_socketFileDescriptor = std::move(socketFileDescriptor);
    return true;
}

bool AgentSession::connect(int columns, int rows, uint32_t sessionID)
{
    if (!m_term || m_socketFileDescriptor)
        return false;
    if (!openSocketAndPrepare())
        return false;

    // Size the passive Ubiquitty::Terminal to match the geometry we advertise so a read
    // before the first checkpoint has sane dims; the checkpoint re-sizes it.
    Ubiquitty::resize(*m_term, rows, columns, Ubiquitty::Modes::ReflowMode::Fixed);

    Ubiquitty::Protocol::Sync::HandshakeRequest hello;
    hello.columns = static_cast<uint16_t>(columns);
    hello.rows = static_cast<uint16_t>(rows);
    hello.sessionID = sessionID;
    hello.agentLabel = resolveAgentLabel();
    hello.authToken = resolveAuthToken();
    std::vector<uint8_t> frame;
    m_core->buildHello(hello, frame);
    return writeFrame(frame);
}

bool AgentSession::connectControl()
{
    if (m_socketFileDescriptor)
        return false;
    if (!openSocketAndPrepare())
        return false;
    Ubiquitty::Protocol::Sync::HandshakeRequest hello;
    hello.columns = 80; // ignored by the daemon for a control-only client
    hello.rows = 24;
    hello.target = Ubiquitty::Protocol::Sync::HandshakeTarget::Control;
    // Control-only binds no session, so the consent gate never fires here;
    // the label and token still go, because a token-required daemon gates
    // ls/kill too.
    hello.agentLabel = resolveAgentLabel();
    hello.authToken = resolveAuthToken();
    std::vector<uint8_t> frame;
    m_core->buildHello(hello, frame);
    if (!writeFrame(frame))
        return false;
    pumpUntil(answerWaitMilliseconds(), [&] { return m_core->sawHelloAck() || m_authDenied; });
    return m_core->sawHelloAck();
}

namespace {

/// The reader's wall clock, which a listing keeps the host's running by.
std::uint64_t wallNowUnixMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

std::optional<AgentSession::HostTree> AgentSession::listHost(int timeoutMilliseconds)
{
    using Ubiquitty::Session::HostListing;
    if (m_nextHostCallID == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    const std::string id = "ubq.list." + std::to_string(++m_nextHostCallID);
    const std::uint64_t generation = m_captureConnectionGeneration;
    // A call is sent once the host has welcomed the screen plane.
    pumpUntil(timeoutMilliseconds, [&] { return m_core->isScreenAttached() || m_authDenied; });
    if (!m_core->isScreenAttached() || !m_core->call(HostListing::subscribeRequest(id)))
        return std::nullopt;
    flushScreen();
    const auto* connection = m_core->screen();
    HostListing listing(id);
    bool isMalformed = false; // bool-ok: a local verdict
    const auto read = [&] {
        if (!m_socketFileDescriptor || generation != m_captureConnectionGeneration || m_core->screen() != connection || !m_core->isScreenAttached())
            return true;
        for (const std::string& response : m_core->takeCallResults())
            (void)listing.noteAnswer(response, wallNowUnixMilliseconds());
        const Ubiquitty::Protocol::Sync::ClientConnection* screen = m_core->screen();
        if (screen && listing.isListing() && listing.refresh(*screen, m_core->listingRevision(), wallNowUnixMilliseconds()) == HostListing::Change::Malformed)
            isMalformed = true;
        return isMalformed || listing.isRefused() || (listing.isListing() && listing.isWhole());
    };
    pumpUntil(timeoutMilliseconds, read);
    const bool isCurrent = m_socketFileDescriptor && generation == m_captureConnectionGeneration && m_core->screen() == connection && m_core->isScreenAttached();
    if (isCurrent) {
        // Even a timed-out list must stop subscribing on a reusable socket.
        // If the cleanup cannot be admitted, closing retires it at the host.
        if (!m_core->call(HostListing::unsubscribeRequest(id + ".end")))
            detach();
        else
            flushScreen();
    }
    if (!isCurrent || !m_socketFileDescriptor || generation != m_captureConnectionGeneration || isMalformed || !listing.isListing() || !listing.isWhole())
        return std::nullopt;
    return HostTree { .tree = listing.tree(), .hostNowUnixMilliseconds = listing.hostNowUnixMilliseconds(wallNowUnixMilliseconds()), .isWhole = listing.isWhole() };
}

std::optional<Ubiquitty::Codec::JSON::Value> AgentSession::callHost(std::string_view method, const Ubiquitty::Codec::JSON::Value& parameters, int timeoutMilliseconds)
{
    using Ubiquitty::Codec::JSON::Value;
    if (m_nextHostCallID == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    if (timeoutMilliseconds == 0)
        timeoutMilliseconds = Ubiquitty::Protocol::Sync::hostCallWaitMilliseconds(method);
    // The id tells this call's answer from any other's on the connection.
    const std::string id = "ubq." + std::to_string(++m_nextHostCallID);
    Value request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(id));
    request.set("method", Value::string(std::string(method)));
    request.set("params", parameters);
    const std::string encoded = Ubiquitty::Codec::JSON::serialize(request);
    if (std::optional<std::string> refusal = Ubiquitty::Protocol::Sync::callSizeRefusal(encoded.size()))
        return Ubiquitty::Protocol::Control::rpcError(request.find("id"), Ubiquitty::Protocol::Control::ErrorCode::InvalidParameters, *refusal);
    pumpUntil(timeoutMilliseconds, [&] { return m_core->isScreenAttached() || m_authDenied; });
    if (!m_core->isScreenAttached() || !m_core->call(encoded))
        return std::nullopt;
    flushScreen();
    std::optional<Value> answer;
    pumpUntil(timeoutMilliseconds, [&] {
        for (const std::string& response : m_core->takeCallResults()) {
            bool isJSON = false; // bool-ok: the parser's out flag
            Value parsed = Ubiquitty::Codec::JSON::parse(response, isJSON);
            const Value* answered = isJSON ? parsed.find("id") : nullptr;
            if (answered && answered->isString() && answered->asString() == id)
                answer = std::move(parsed);
        }
        return answer.has_value();
    });
    return answer;
}

int AgentSession::boundedWaitMilliseconds(int milliseconds) const
{
    milliseconds = std::max(milliseconds, 0);
    if (!m_runDeadline)
        return milliseconds;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*m_runDeadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(remaining, 0, milliseconds));
}

bool AgentSession::writeFrame(const std::vector<uint8_t>& frame)
{
    if (!m_socketFileDescriptor)
        return false;
    std::size_t written = 0;
    while (written < frame.size()) {
        const int waitMilliseconds = boundedWaitMilliseconds(1000);
        if (!waitMilliseconds)
            return false;
        ssize_t n = ::write(m_socketFileDescriptor.get(), frame.data() + written, frame.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pollDescriptor;
            pollDescriptor.fd = m_socketFileDescriptor.get();
            pollDescriptor.events = POLLOUT;
            pollDescriptor.revents = 0;
            if (::poll(&pollDescriptor, 1, waitMilliseconds) <= 0)
                return false;
            continue;
        }
        return false; // EPIPE / fatal — peer gone
    }
    return true;
}

AgentSession::InputDelivery AgentSession::sendInput(std::string_view bytes)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    // An agent's input is bytes as they are: `Raw` events on the pane's
    // channel, which exists once the host has bound the pane. The binding is
    // the host's answer, so it gets the answer bound.
    constexpr std::size_t maxRawEventBytes = 16 * 1024;
    const std::uint32_t sessionID = m_core->sessionID();
    pumpUntil(answerWaitMilliseconds(), [&] { return m_core->isPaneBound(sessionID) || m_exited; });
    if (m_exited || !m_core->isPaneBound(sessionID))
        return InputDelivery::NotSent;
    std::uint64_t lastSent = 0;
    for (; !bytes.empty(); bytes.remove_prefix(std::min(bytes.size(), maxRawEventBytes))) {
        const std::string_view piece = bytes.substr(0, maxRawEventBytes);
        const auto sent = m_core->sendIntent(sessionID, Sync::RawInput { .bytes = { piece.begin(), piece.end() } });
        if (!sent)
            return InputDelivery::NotSent;
        lastSent = *sent;
        flushScreen();
    }
    const Sync::ID128 paneID { .low = sessionID, .high = m_core->daemonInstanceID() };
    const auto isWritten = [&] {
        const Sync::ClientConnection* screen = m_core->screen();
        return screen && screen->receipt(screen->handleForPane(paneID)).inputWritten >= lastSent;
    };
    pumpUntil(inputWrittenWaitMilliseconds, [&] { return m_exited || isWritten(); });
    if (isWritten())
        return InputDelivery::Written;
    return m_exited ? InputDelivery::NotSent : InputDelivery::Unconfirmed;
}

bool AgentSession::resize(int columns, int rows)
{
    // An agent has no viewport of the pane; its resize is a call the host
    // answers, on the pane's connection once the pane is bound. Both waits are
    // for the host's answer and share its bound: a shorter one reports a
    // resize the host then carries out as refused.
    const std::uint32_t sessionID = m_core->sessionID();
    pumpUntil(answerWaitMilliseconds(), [&] { return m_core->isPaneBound(sessionID) || m_exited; });
    if (m_exited || !m_core->overridePaneGeometry(sessionID, rows, columns))
        return false;
    flushScreen();
    // One call is outstanding at a time, so the first answer is this one's.
    std::vector<std::string> answers;
    pumpUntil(answerWaitMilliseconds(), [&] {
        if (answers.empty())
            answers = m_core->takeCallResults();
        return !answers.empty() || m_exited;
    });
    return !answers.empty() && answers.front().find("\"error\"") == std::string::npos;
}

void AgentSession::detach()
{
    if (!m_socketFileDescriptor)
        return;
    std::vector<uint8_t> frame;
    m_core->buildExit(frame);
    writeFrame(frame);
    m_socketFileDescriptor.reset();
}

std::string AgentSession::resolveAgentLabel() const
{
    // An env override lets the controlling tool self-name in the prompt;
    // otherwise the role default (m_agentLabel: "ubq" for the CLI, and for
    // the MCP server the name its client gave in `initialize`, "mcp-client"
    // when it gave none). The label is a spoofable hint, not the grant key —
    // so its exact value never widens access.
    if (const char* env = ::getenv("UBIQUITTY_AGENT_LABEL"); env && *env)
        return env;
    return m_agentLabel;
}

std::string AgentSession::resolveAuthToken() const
{
    // The capability token: an env override for ad-hoc/wrapped use,
    // else <control-dir>/agent-token (minted by `ubiquitty-host token issue`). Empty
    // when none is configured, which a default daemon refuses at HELLO because an
    // agent is not a Local peer.
    // A token is minted by one host's daemon and means nothing to another's,
    // so over ssh only a token the remote host issued is presented.
    if (!m_remoteHost.empty()) {
        const char* remote = ::getenv("UBQ_REMOTE_TOKEN");
        return remote ? std::string(remote) : std::string();
    }
    if (const char* env = ::getenv("UBIQUITTY_AGENT_TOKEN"); env && *env)
        return env;
    char directory[1024];
    if (Ubiquitty::Host::controlDirectory(directory) == 0)
        return std::string();
    std::string path(directory);
    path += "/agent-token";
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file)
        return std::string();
    char buffer[1024];
    std::string token;
    if (std::fgets(buffer, sizeof(buffer), file))
        token = buffer;
    std::fclose(file);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ' || token.back() == '\t'))
        token.pop_back();
    return token;
}

void AgentSession::onScreenChanged(uint32_t)
{
    m_awaitingConsent = false; // a held attach was just granted
    ++m_screenUpdates;
}

bool AgentSession::subscribeEvents(EventStart start, std::uint64_t sinceCursor)
{
    auto parameters = Codec::JSON::Value::object();
    parameters.set("after", Codec::JSON::Value::string(std::to_string(start == EventStart::FromLatest ? 0 : sinceCursor)));
    parameters.set("fromLatest", Codec::JSON::Value::boolean(start == EventStart::FromLatest));
    const auto answer = callHost("events.subscribe", parameters);
    const auto* result = answer && !answer->find("error") ? answer->find("result") : nullptr;
    m_eventSubscriptionCursor = result ? Protocol::Control::eventSubscriptionCursor(*result) : std::nullopt;
    return m_eventSubscriptionCursor.has_value();
}

std::vector<Session::Event> AgentSession::takeEvents() { return std::exchange(m_events, {}); }

void AgentSession::flushScreen()
{
    for (const auto& notify : m_core->takeHostNotifies()) {
        if (notify.kind != Protocol::Sync::NotifyKind::Events)
            continue;
        auto events = Protocol::Control::decodeEvents(notify.record);
        if (!events) {
            // A malformed batch is not a filtered record or a retention gap.
            // Close rather than silently skip its cursor and pretend that the
            // stream remained complete.
            m_exited = true;
            m_socketFileDescriptor.reset();
            return;
        }
        for (auto& event : *events)
            Session::holdEvent(m_events, std::move(event), maxHeldEvents);
    }
    // An acknowledgement already due — a checkpoint's is owed at once — goes
    // with this write rather than after the next poll.
    m_core->tickScreen();
    std::vector<uint8_t> frame;
    m_core->takeScreenOutput(frame);
    if (!frame.empty() && !writeFrame(frame)) {
        m_didScreenWriteFail = true;
        m_exited = true;
        m_socketFileDescriptor.reset();
    }
}

std::string AgentSession::endedReason() const
{
    if (m_authDenied)
        return std::string(Protocol::Sync::byeNotice(m_authDeniedReason));
    const Protocol::Sync::ClientConnection* connection = m_core->screen();
    if (connection && connection->state() == Protocol::Sync::ConnectionState::Closed && connection->byeReason())
        return std::string(Protocol::Sync::byeNotice(*connection->byeReason()));
    if (m_didScreenWriteFail)
        return "writing to the host failed";
    if (connection && connection->state() == Protocol::Sync::ConnectionState::Lost)
        return "the connection to the host was lost";
    // This side's own detach is not an end the host caused.
    if (m_exited)
        return "the host closed the connection without saying why";
    return {};
}

std::string AgentSession::unansweredText(std::string_view request) const
{
    std::string reason = endedReason();
    return reason.empty() ? "the host did not answer " + std::string(request) : reason;
}

void AgentSession::onBell() { m_bellPending = true; }

void AgentSession::onExit() { m_exited = true; }

void AgentSession::onCommandDone(const Protocol::Sync::EffectView&, const Protocol::Sync::CommandFinishedEffect& command, Protocol::Sync::EffectDisposition)
{
    m_lastExitCode = command.exitStatus;
    m_lastCurrentDirectory = command.currentDirectory;
    // Command state is readable even with Ignore presentation. Copy every
    // retained string before the synchronous borrowed effect call returns.
    if (!command.commandLine.empty())
        m_lastCommandLine = command.commandLine;
    m_commandDones.push_back({ .exitCode = command.exitStatus, .currentDirectory = std::string(command.currentDirectory) });
}

void AgentSession::onPaneFacts(uint32_t, const Ubiquitty::Protocol::Sync::PaneFacts& facts)
{
    m_foregroundIsShell = facts.isAtPrompt;
    m_hasMarkedPrompt = facts.hasMarkedPrompt;
    m_foregroundGeneration = facts.foreground.generation;
}

void AgentSession::onConsentPending(uint32_t sessionID)
{
    if (m_awaitingConsent)
        return;
    m_awaitingConsent = true;
    if (m_onConsentPending)
        m_onConsentPending(sessionID);
}

std::string AgentSession::refusalText() const
{
    std::string text = authDeniedReasonText(m_authDeniedReason, tokenSource());
    if (const std::string detail = closingDetail(); !detail.empty())
        text += "; the host said: " + detail;
    return text;
}

std::string AgentSession::closingDetail() const
{
    const Protocol::Sync::ClientConnection* connection = m_core->screen();
    return connection && connection->bye() ? connection->bye()->detail : std::string();
}

void AgentSession::onRefused(Protocol::Sync::ByeReason reason)
{
    m_authDenied = true;
    m_authDeniedReason = reason;
}

void AgentSession::onConsentDenied(uint32_t sessionID, Protocol::Sync::ConsentStatusPhase phase)
{
    (void)sessionID;
    m_consentDenied = true;
    m_consentDeniedReason = phase;
    m_awaitingConsent = false;
}

bool AgentSession::drainReadable()
{
    unsigned char buffer[65536];
    for (;;) {
        ssize_t n = ::read(m_socketFileDescriptor.get(), buffer, sizeof(buffer));
        if (n > 0) {
            const bool isFed = m_core->feed({ reinterpret_cast<const std::byte*>(buffer), static_cast<std::size_t>(n) });
            // What the screen plane owes the host — its acknowledgements, or
            // its Bye — goes out with the read that owed it.
            flushScreen();
            if (!isFed) {
                m_exited = true; // decoder fault
                return false;
            }
            if (n < static_cast<ssize_t>(sizeof(buffer)))
                return true; // a short read means the socket buffer is empty
            continue;
        }
        if (n == 0) { // peer closed — session ended / detached
            m_exited = true;
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        if (errno == EINTR)
            continue;
        m_exited = true;
        return false;
    }
}

bool AgentSession::pump(int timeoutMilliseconds)
{
    if (!m_socketFileDescriptor || m_exited)
        return false;
    struct pollfd pollDescriptor;
    pollDescriptor.fd = m_socketFileDescriptor.get();
    pollDescriptor.events = POLLIN;
    pollDescriptor.revents = 0;
    // An acknowledgement the screen plane holds is due before the caller's
    // timeout may be: the host paces this client by what it acknowledged.
    int pollMilliseconds = boundedWaitMilliseconds(timeoutMilliseconds);
    if (const std::optional<std::uint64_t> tick = m_core->screenTickDelayMicroseconds())
        pollMilliseconds = std::min<int>(pollMilliseconds, static_cast<int>((*tick + 999) / 1000));
    int r = ::poll(&pollDescriptor, 1, pollMilliseconds);
    flushScreen();
    if (r < 0)
        return errno == EINTR; // transient; caller may retry
    if (r == 0)
        return true; // idle, still connected
    if (pollDescriptor.revents & (POLLHUP | POLLERR)) {
        drainReadable(); // drain any tail the kernel still holds
        m_exited = true;
        return false;
    }
    return drainReadable();
}

bool AgentSession::waitForAttach(int timeoutMilliseconds)
{
    const auto hasBinding = [&] {
        const auto* connection = m_core->screen();
        return connection && m_core->isScreenAttached()
            && connection->handleForPane({ .low = m_core->sessionID(), .high = m_core->daemonInstanceID() });
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    bool didExtendForConsent = false;
    while (!hasBinding() && !m_exited && !m_consentDenied && !m_authDenied) {
        const auto now = std::chrono::steady_clock::now();
        if (m_awaitingConsent && !didExtendForConsent) {
            didExtendForConsent = true;
            deadline = std::max(deadline, now + std::chrono::milliseconds(consentWaitMilliseconds));
        }
        if (now >= deadline)
            break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (!pump(static_cast<int>(std::min<long long>(remaining, 1000))))
            break;
    }
    return hasBinding() && !m_exited && !m_consentDenied && !m_authDenied;
}

bool AgentSession::waitForFirstScreen(int timeoutMilliseconds)
{
    m_didFirstScreenWaitExpire = false;
    // An exact Pending status extends the deadline to cover the daemon's 30s
    // consent prompt plus margin. Approval admits the pane; Denied, Expired
    // or a close ends the wait without exposing a checkpoint.
    constexpr auto consentWait = std::chrono::milliseconds(consentWaitMilliseconds);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    bool didExtendForConsent = false;
    bool isScreenAsked = false;
    while (!m_core->hasScreen() && !m_exited && !m_consentDenied && !m_authDenied) {
        // An agent's pane starts RecordsOnly on the host (Docs/sync.md §6.8):
        // a verb that reads the screen asks for it once the host has named the
        // session, and holds it for as long as this session runs.
        if (!isScreenAsked && m_core->sawHelloAck()) {
            isScreenAsked = true;
            m_core->setPaneView(m_core->sessionID(), Ubiquitty::Protocol::Sync::ViewState::Live);
            flushScreen();
        }
        if (m_exited || !m_socketFileDescriptor)
            break;
        const auto now = std::chrono::steady_clock::now();
        if (m_awaitingConsent && !didExtendForConsent) {
            didExtendForConsent = true;
            deadline = std::max(deadline, now + consentWait);
        }
        if (now >= deadline) {
            m_didFirstScreenWaitExpire = true;
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (!pump(static_cast<int>(std::min<long long>(remaining, 1000))))
            break;
    }
    return m_core->hasScreen();
}

const OSC::ProgramStatusStore& AgentSession::programStatus() const { return m_term->programStatus; }

int AgentSession::columns() const { return m_term ? m_term->columns : 0; }
int AgentSession::rows() const { return m_term ? m_term->rows : 0; }

void AgentSession::cursor(int& row, int& column) const
{
    if (m_term && m_term->screens.active) {
        row = m_term->screens.active->cursorY;
        column = m_term->screens.active->cursorX;
    } else {
        row = 0;
        column = 0;
    }
}

int AgentSession::imagePlacementCount() const { return m_term ? Ubiquitty::Image::placementCount(m_term.get()) : 0; }

std::string AgentSession::screenText() const
{
    return Ubiquitty::Accessibility::visibleScreenText(*m_term, 0);
}

std::optional<Protocol::Control::CaptureReply> AgentSession::captureScreen(int timeoutMilliseconds)
{
    return captureText({}, timeoutMilliseconds);
}

std::optional<Protocol::Control::CaptureReply> AgentSession::captureText(const Protocol::Control::CaptureRequest& selection, int timeoutMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundedWaitMilliseconds(std::max(timeoutMilliseconds, 0)));
    for (;;) {
        const auto remaining = std::max<int>(0, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()));
        auto result = captureTextOnce(selection, remaining);
        if (!result || result->status != Protocol::Control::CaptureReplyStatus::Pending || std::chrono::steady_clock::now() >= deadline)
            return result;
        pump(std::min(remaining, 25));
    }
}

std::optional<Protocol::Control::CaptureReply> AgentSession::captureTextOnce(const Protocol::Control::CaptureRequest& selection, int timeoutMilliseconds)
{
    using Codec::JSON::Value;
    using Protocol::Control::CaptureReply;
    using Protocol::Control::CaptureReplyStatus;
    const bool isScreen = selection.source == Protocol::Control::CaptureSource::Screen;
    if (!m_socketFileDescriptor || m_exited || m_authDenied || m_consentDenied
        || m_nextCaptureRequestID == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    if (selection.source == Protocol::Control::CaptureSource::Tail && (!selection.lines || selection.lines > 100000))
        return std::nullopt;
    // A capture names the pane by its connection-local handle, which exists
    // once the host has bound the pane, and a screen capture is sized to a
    // screen the host has sent. Both are the host's answers and can trail the
    // attach, so they are waited for within the capture's own bound.
    const auto started = std::chrono::steady_clock::now();
    const std::uint32_t boundSessionID = m_core->sessionID();
    const auto isAddressable = [&] { return m_core->isScreenAttached() && m_core->isPaneBound(boundSessionID) && (!isScreen || ready()); };
    pumpUntil(timeoutMilliseconds, [&] { return m_exited || m_authDenied || m_consentDenied || isAddressable(); });
    timeoutMilliseconds = std::max(0, timeoutMilliseconds - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
    const auto* connection = m_core->screen();
    if (!m_socketFileDescriptor || m_exited || m_authDenied || m_consentDenied || !connection || !isAddressable())
        return std::nullopt;
    const Protocol::Sync::ID128 paneID { .low = m_core->sessionID(), .high = m_core->daemonInstanceID() };
    const Protocol::Control::CaptureTarget target {
        .pane = connection->handleForPane(paneID),
        .rows = static_cast<std::uint16_t>(m_term->rows),
        .columns = static_cast<std::uint16_t>(m_term->columns),
        .request = selection,
    };
    if (!target.pane)
        return std::nullopt;
    const std::uint64_t generation = m_captureConnectionGeneration;
    const auto isCurrent = [&] {
        return m_socketFileDescriptor && generation == m_captureConnectionGeneration && !m_exited && !m_authDenied && !m_consentDenied
            && m_core->screen() == connection && m_core->isScreenAttached() && m_core->sessionID() == paneID.low && m_core->daemonInstanceID() == paneID.high
            && connection->handleForPane(paneID) == target.pane && (!isScreen || (m_term->rows == target.rows && m_term->columns == target.columns));
    };
    // A timeout retires this id locally; there is no callback or result slot
    // left for a late answer to fill. A later request always gets another id.
    const std::string id = "ubq.capture." + std::to_string(++m_nextCaptureRequestID);
    Value parameters = Value::object();
    parameters.set("pane", Value::number(target.pane));
    if (!isScreen)
        parameters.set("source", Value::string(std::string(Protocol::Control::captureSourceName(selection.source))));
    if (selection.source == Protocol::Control::CaptureSource::Rows) {
        parameters.set("start", Value::number(selection.start));
        parameters.set("end", Value::number(selection.end));
    } else if (selection.source == Protocol::Control::CaptureSource::Tail) {
        parameters.set("lines", Value::number(selection.lines));
    }
    Value request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(id));
    request.set("method", Value::string("pane.capture"));
    request.set("params", std::move(parameters));
    if (!m_core->call(Codec::JSON::serialize(request)))
        return std::nullopt;
    m_core->tickScreen();
    std::vector<std::uint8_t> frame;
    m_core->takeScreenOutput(frame);
    if (frame.empty() || !writeFrame(frame)) {
        // A partial write cannot leave a reusable framed connection.
        m_socketFileDescriptor.reset();
        m_exited = true;
        return std::nullopt;
    }
    std::optional<CaptureReply> answer;
    pumpUntil(timeoutMilliseconds, [&] {
        if (!isCurrent())
            return true;
        for (const std::string& response : m_core->takeCallResults()) {
            bool isParsed = false;
            const Value parsed = Codec::JSON::parse(response, isParsed);
            CaptureReply decoded = isParsed ? Protocol::Control::decodeCaptureReply(parsed, id, target) : CaptureReply {};
            if (decoded.status == CaptureReplyStatus::Unrelated)
                continue;
            if (answer) {
                answer = CaptureReply {}; // two answers cannot define one capture
                break;
            }
            answer = std::move(decoded);
        }
        return answer.has_value();
    });
    return isCurrent() ? answer : std::nullopt;
}

std::optional<Protocol::Control::CaptureReply> AgentSession::waitForText(std::string_view needle, std::optional<int> timeoutMilliseconds, bool& isFound)
{
    isFound = false;
    if (needle.empty())
        return std::nullopt;
    const auto started = std::chrono::steady_clock::now();
    const auto remaining = [&] {
        if (!timeoutMilliseconds)
            return 4000;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        return static_cast<int>(std::clamp<std::int64_t>(*timeoutMilliseconds - elapsed, 0, 4000));
    };
    while (true) {
        auto captured = captureScreen(remaining());
        if (!captured || captured->status != Protocol::Control::CaptureReplyStatus::Complete)
            return captured;
        if (captured->text.find(needle) != std::string::npos) {
            isFound = true;
            return captured;
        }
        const int pause = std::min(remaining(), 250);
        if (!pause)
            return captured;
        // One outstanding Call and at most four new captures per second.
        // Pumping here preserves input/ACK liveness without nested read loops.
        pumpUntil(pause, [&] { return m_exited || m_authDenied || m_consentDenied; });
        if (m_exited || m_authDenied || m_consentDenied)
            return std::nullopt;
        if (!remaining())
            return captured;
    }
}

AgentSession::RunResult AgentSession::runCommandWithExit(const std::string& command, std::optional<int> timeoutMilliseconds, RunObservation observation)
{
    const auto previousDeadline = m_runDeadline;
    m_runDeadline = timeoutMilliseconds ? std::optional { std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(*timeoutMilliseconds, 0)) } : std::nullopt;
    // The input receipt, settling and authorized capture share this deadline.
    const auto incomplete = [&] {
        return RunResult {
            .completion = boundedWaitMilliseconds(INT_MAX) ? RunCompletion::Unavailable : RunCompletion::TimedOut,
            .capture = std::nullopt,
            .exitStatus = std::nullopt,
            .cursor = std::nullopt,
            .inputDelivery = std::nullopt,
        };
    };
    const auto run = [&]() -> RunResult {
        if (observation == RunObservation::Events && !subscribeEvents(EventStart::FromLatest))
            return incomplete();
        pumpUntil(1500, [&] { return screenText().find_first_not_of(" \n\r\t") != std::string::npos; });
        if (!boundedWaitMilliseconds(INT_MAX))
            return incomplete();
        std::optional<std::uint64_t> cursor;
        if (observation == RunObservation::Events) {
            cursor = m_eventSubscriptionCursor;
            for (const auto& event : takeEvents())
                cursor = std::max(cursor.value_or(0), event.cursor);
        }
        bool didStart = false;
        std::string previous = screenText();
        const std::size_t completedBeforeSend = m_commandDones.size();
        std::uint32_t foregroundGeneration = m_foregroundGeneration;
        std::optional<std::uint32_t> childGeneration;
        auto lastChange = std::chrono::steady_clock::now();
        bool sawChange = false;
        if (const InputDelivery delivery = sendInput(command + "\n"); delivery != InputDelivery::Written) {
            auto result = incomplete();
            result.inputDelivery = delivery;
            return result;
        }
        while (boundedWaitMilliseconds(INT_MAX)) {
            std::optional<std::int32_t> exitStatus;
            if (observation == RunObservation::Events) {
                for (const auto& event : takeEvents()) {
                    if (exitStatus)
                        break;
                    cursor = std::max(cursor.value_or(0), event.cursor);
                    if (event.sessionID != m_core->sessionID())
                        continue;
                    if (event.kind == Session::EventKind::CommandStarted)
                        didStart = true;
                    else if (didStart && event.kind == Session::EventKind::CommandFinished)
                        exitStatus = event.exitStatus;
                }
            } else if (m_commandDones.size() != completedBeforeSend)
                exitStatus = m_commandDones[completedBeforeSend].exitCode;
            if (exitStatus) {
                auto output = captureText({ .source = Protocol::Control::CaptureSource::LastCommand });
                if (output && output->status == Protocol::Control::CaptureReplyStatus::Refused
                    && output->errorCode == static_cast<std::int32_t>(Protocol::Control::ErrorCode::NotFound))
                    output = captureScreen();
                return { .completion = RunCompletion::CommandDone, .capture = std::move(output), .exitStatus = exitStatus, .cursor = cursor, .inputDelivery = std::nullopt };
            }
            if (!m_hasMarkedPrompt && !didStart) {
                const auto now = std::chrono::steady_clock::now();
                std::string current = screenText();
                if (current != previous) {
                    sawChange = true;
                    lastChange = now;
                    previous = std::move(current);
                }
                if (m_foregroundGeneration != foregroundGeneration) {
                    foregroundGeneration = m_foregroundGeneration;
                    lastChange = now;
                    if (!m_foregroundIsShell)
                        childGeneration = foregroundGeneration;
                }
                const bool hasChildReturned = !childGeneration || foregroundGeneration != *childGeneration;
                if (sawChange && m_foregroundIsShell && hasChildReturned && now - lastChange >= std::chrono::milliseconds(500))
                    return { .completion = RunCompletion::Settled, .capture = captureScreen(), .exitStatus = std::nullopt, .cursor = cursor, .inputDelivery = std::nullopt };
            }
            if (!pump(boundedWaitMilliseconds(500)))
                return incomplete();
        }
        return incomplete();
    };
    auto result = run();
    m_runDeadline = previousDeadline;
    return result;
}

bool AgentSession::waitForExit(int timeoutMilliseconds, int32_t& exitCode, std::string& currentDirectory)
{
    // Latch the FIRST CommandDone that arrives after we start waiting; when a
    // command emits its own OSC-133 D and the session shell then emits a
    // prompt-redraw D in the same pump batch, reading the last-seen exit code
    // would mask the command's own. baseline indexes the first post-send one.
    const std::size_t baseline = m_commandDones.size();
    pumpUntil(timeoutMilliseconds, [&] { return m_commandDones.size() != baseline; });
    if (m_commandDones.size() == baseline)
        return false;
    exitCode = m_commandDones[baseline].exitCode;
    currentDirectory = m_commandDones[baseline].currentDirectory;
    return true;
}

bool AgentSession::waitForIdle(int timeoutMilliseconds)
{
    if (m_foregroundIsShell)
        return true;
    // Status fires only on a foreground/idle CHANGE — if the session is
    // already at the prompt, no Status is coming. Query the current state
    // from the session registry (atPrompt flag) before waiting.
    std::vector<Ubiquitty::Session::SessionRow> sessions;
    if (listSessions(sessions, timeoutMilliseconds < 1500 ? timeoutMilliseconds : 1500)) {
        for (const auto& row : sessions)
            if (row.sessionID == m_core->sessionID() && row.isAtPrompt()) {
                m_foregroundIsShell = true;
                return true;
            }
    }
    // Busy now — wait for a Status idle transition (e.g. a command finishing).
    pumpUntil(timeoutMilliseconds, [&] { return m_foregroundIsShell; });
    return m_foregroundIsShell;
}

bool AgentSession::readLastCommand(std::string& textOut, int timeoutMilliseconds)
{
    textOut.clear();
    const auto result = captureText({ .source = Protocol::Control::CaptureSource::LastCommand }, timeoutMilliseconds);
    if (!result || result->status != Protocol::Control::CaptureReplyStatus::Complete)
        return false;
    textOut = result->text;
    return true;
}

bool AgentSession::readRegion(int64_t absoluteRowStart, int64_t absoluteRowEnd, std::string& textOut, int timeoutMilliseconds)
{
    textOut.clear();
    const auto narrow = [](std::int64_t row) { return static_cast<std::int32_t>(std::clamp<std::int64_t>(row, INT32_MIN, INT32_MAX)); };
    const auto result = captureText({ .source = Protocol::Control::CaptureSource::Rows, .start = narrow(absoluteRowStart), .end = narrow(absoluteRowEnd) }, timeoutMilliseconds);
    if (!result || result->status != Protocol::Control::CaptureReplyStatus::Complete)
        return false;
    textOut = result->text;
    return true;
}

bool AgentSession::listSessions(std::vector<Ubiquitty::Session::SessionRow>& out, int timeoutMilliseconds)
{
    out.clear();
    const auto listed = listHost(timeoutMilliseconds);
    if (!listed || !listed->isWhole)
        return false;
    out = Ubiquitty::Session::sessionRows(listed->tree);
    return true;
}

AgentSession::TerminateOutcome AgentSession::terminateSession(uint32_t id, int timeoutMilliseconds)
{
    using Ubiquitty::Protocol::Control::TerminationOutcome;
    if (!id)
        return TerminateOutcome::Failed;
    m_terminationRefusal.reset();
    auto parameters = Ubiquitty::Codec::JSON::Value::object();
    parameters.set("session", Ubiquitty::Codec::JSON::Value::number(id));
    const auto answer = callHost("session.terminate", parameters, timeoutMilliseconds);
    if (!answer)
        return m_authDenied ? TerminateOutcome::Refused : TerminateOutcome::TimedOut;
    const auto outcome = Ubiquitty::Protocol::Control::terminationOutcome(*answer, answer->stringOr("id"), id);
    if (!outcome)
        return TerminateOutcome::Failed;
    if (const auto* error = answer->find("error"); error && *outcome == TerminationOutcome::Refused)
        m_terminationRefusal = HostRefusal { .code = static_cast<std::int32_t>(error->numberOr("code")), .message = error->stringOr("message") };
    switch (*outcome) {
    case TerminationOutcome::Terminated:
    case TerminationOutcome::Forgotten:
        return TerminateOutcome::Terminated;
    case TerminationOutcome::Denied:
        return TerminateOutcome::Denied;
    case TerminationOutcome::TimedOut:
        return TerminateOutcome::TimedOut;
    case TerminationOutcome::Refused:
        return TerminateOutcome::Refused;
    case TerminationOutcome::Failed:
        return TerminateOutcome::Failed;
    }
    return TerminateOutcome::Failed;
}

std::vector<uint8_t> AgentSession::renderPNG()
{
    std::vector<uint8_t> empty;
    if (!m_term)
        return empty;
    const int columns = m_term->columns;
    const int rows = m_term->rows;
    const int cellWidth = 8; // Unifont half-width cell; wide glyphs span two
    const int cellHeight = 16;
    const int gutter = 5; // px reserved each side for the OSC 133 prompt brackets
    const int imageWidth = gutter * 2 + columns * cellWidth;
    const int imageHeight = rows * cellHeight;
    if (imageWidth <= 0 || imageHeight <= 0)
        return empty;
    // Bound the raster: refuse an absurd geometry rather than attempt a
    // multi-hundred-MB allocation. A real terminal screenshot is far under
    // this (16 Mpx ~ 700x190 cells).
    constexpr long long maxPixels = 16LL * 1024 * 1024;
    if (static_cast<long long>(imageWidth) * imageHeight > maxPixels)
        return empty;

    std::vector<uint8_t> rgba(static_cast<std::size_t>(imageWidth) * imageHeight * 4, 0);
    const Ubiquitty::TermPalette& palette = m_term->palette;
    const bool reverseScreen = Ubiquitty::isReverseVideo(m_term.get());
    // Pre-fill the whole canvas (the gutters included) with the screen's
    // default background so the bracket margins match the terminal instead of
    // rendering transparent.
    {
        const RGB8 background = Ubiquitty::Rendering::resolvePaletteColor(palette, colorDefault, reverseScreen);
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            rgba[i] = background.red;
            rgba[i + 1] = background.green;
            rgba[i + 2] = background.blue;
            rgba[i + 3] = 255;
        }
    }
    int cursorRow = 0;
    int cursorColumn = 0;
    cursor(cursorRow, cursorColumn);
    // Draw the block cursor only when the terminal isn't hiding it (DECTCEM
    // \033[?25l) — full-screen TUIs (vim, less, htop) hide it. The passive
    // Terminal's isCursorVisible is set by the screen plane's checkpoints and updates.
    const bool isCursorVisible = m_term->modes.isCursorVisible != 0;

    auto putPixel = [&](int x, int y, RGB8 c) {
        if (x < 0 || y < 0 || x >= imageWidth || y >= imageHeight)
            return;
        std::size_t o = (static_cast<std::size_t>(y) * imageWidth + x) * 4;
        rgba[o] = c.red;
        rgba[o + 1] = c.green;
        rgba[o + 2] = c.blue;
        rgba[o + 3] = 255;
    };

    // Render the cell grid through the shared SoftwareRenderer (cell background +
    // glyph + focused-cursor bake + the full decoration pass — underline
    // styles / strike / overline / framed), then composite the tight grid
    // into this gutter'd canvas at the left margin. The grid logic lives
    // entirely in the portable core (same path the Linux window + the
    // CPU-vs-GPU oracle use); this function keeps only the gutter + OSC 133
    // chrome.
    {
        // The reset-pen blank: defaultBlankCell carries the colorDefault channels.
        Cell blank = Ubiquitty::defaultBlankCell;
        std::vector<Cell> grid(static_cast<std::size_t>(rows) * columns, blank);
        // Per-row shaped VISUAL order: getDisplayRow runs the full Arabic
        // joining + RTL pass, so the rendered PNG shows cursive/RTL text the way
        // a terminal draws it (not the raw logical cells). A row with no shaping
        // content copies straight through, so Latin output is unchanged.
        // (screenText() above stays logical order — that text feeds
        // an LLM, which wants reading order, not display order.)
        for (int row = 0; row < rows; ++row)
            Ubiquitty::Screen::getDisplayRow(*m_term, 0, row, &grid[static_cast<std::size_t>(row) * columns], columns);

        Ubiquitty::Rendering::BuildInputs in;
        in.cells = grid.data();
        // A cell's cluster past its inline mark lives in the mirror's pool, so
        // the reader travels with the copied rows: the marks of a combining
        // sequence, and the row and column a kitty placeholder cell names.
        in.clusterResolver = [](const Cell* cell, std::span<std::uint32_t> codePoints, void* terminal) -> int {
            return Ubiquitty::cellClusterCodePoints(static_cast<const Ubiquitty::Terminal*>(terminal), cell, codePoints.data(), static_cast<int>(codePoints.size()));
        };
        in.clusterResolverContext = m_term.get();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.screenReverse = reverseScreen;
        in.brightenBold = false;
        // Inline images (kitty / sixel / iTerm): hand the renderer the Terminal's
        // image + placement tables so image-backed UI is composited into the
        // screenshot — z<0 under the text, z>=0 over, matching the live Metal
        // renderer. The passive Ubiquitty::Terminal renders its live tail (viewOffset 0).
        in.images = m_term->images.stored.data();
        in.imageCount = Image::maxTerminalImageCount;
        in.placements = m_term->images.placements.data();
        in.placementCount = Image::maxTerminalImagePlacementCount;
        in.viewOffset = 0;
        if (isCursorVisible) {
            const Ubiquitty::PaletteRGB cursorColor = palette.cursor;
            in.cursor.row = cursorRow;
            in.cursor.column = cursorColumn;
            in.cursor.mixA = 1.0f;
            in.cursor.shouldInvertText = true;
            in.cursor.rgb = { Ubiquitty::channel16To8(cursorColor.red) / 255.0f, Ubiquitty::channel16To8(cursorColor.green) / 255.0f, Ubiquitty::channel16To8(cursorColor.blue) / 255.0f, 1.0f };
            Ubiquitty::Rendering::setCursorTextColor(in.cursor, Ubiquitty::Rendering::cursorTextColor(*m_term));
        }

        Ubiquitty::Rendering::SoftwareRenderer renderer(cellWidth, cellHeight);
        std::vector<uint8_t> gridRGBA;
        int gridWidth = 0;
        int gridHeight = 0;
        if (renderer.render(in, gridRGBA, gridWidth, gridHeight)) {
            for (int y = 0; y < gridHeight && y < imageHeight; ++y) {
                const uint8_t* src = &gridRGBA[static_cast<std::size_t>(y) * gridWidth * 4];
                for (int x = 0; x < gridWidth && (gutter + x) < imageWidth; ++x) {
                    const std::size_t o = (static_cast<std::size_t>(y) * imageWidth + (gutter + x)) * 4;
                    rgba[o] = src[x * 4];
                    rgba[o + 1] = src[x * 4 + 1];
                    rgba[o + 2] = src[x * 4 + 2];
                    rgba[o + 3] = 255;
                }
            }
        }
    }

    // OSC 133 prompt marks: a bracket in each gutter next to every row whose
    // command actually ran (CMD_EXECUTED, promoted at the A→C transition),
    // red when that command exited non-zero — the same gutter affordance the
    // Metal frontend draws, so a headless screenshot shows it too. The spine
    // frames the glyph cell (a little inset top/bottom); a side is dropped if
    // the gutter is too narrow.
    {
        const int history = Ubiquitty::Screen::scrollbackCount(m_term.get());
        const RGB8 okColor = Ubiquitty::Rendering::resolvePaletteColor(palette, colorDefault, true);
        const RGB8 failColor = Ubiquitty::Rendering::resolvePaletteColor(palette, 1, false); // ANSI red
        const int gridRight = gutter + columns * cellWidth;
        for (int row = 0; row < rows; ++row) {
            const uint8_t mark = Ubiquitty::Screen::rowMark(m_term.get(), history + row);
            if (!(mark & Screen::osc133MarkCmdExecuted))
                continue;
            const RGB8 c = (mark & Screen::osc133MarkCmdFailed) ? failColor : okColor;
            const int y0 = row * cellHeight;
            const int yTop = y0 + 2; // frame the 16px cell's ink
            const int yBot = y0 + cellHeight - 3;
            // Left bracket: spine 1px in from the edge, stubs reach toward the
            // grid → [ . Right bracket mirrors it → ].
            const int lSpine = 1;
            for (int y = yTop; y <= yBot; ++y)
                putPixel(lSpine, y, c);
            for (int x = lSpine; x <= gutter - 2; ++x) {
                putPixel(x, yTop, c);
                putPixel(x, yBot, c);
            }
            const int rSpine = imageWidth - 2;
            for (int y = yTop; y <= yBot; ++y)
                putPixel(rSpine, y, c);
            for (int x = gridRight + 1; x <= rSpine; ++x) {
                putPixel(x, yTop, c);
                putPixel(x, yBot, c);
            }
        }
    }
    return Rendering::encodePNG(rgba, { imageWidth, imageHeight });
}

bool AgentSession::screenshot(const std::string& path)
{
    std::vector<uint8_t> png = renderPNG();
    if (png.empty())
        return false;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
    return true;
}

bool isStandardInputTerminal() { return ::isatty(STDIN_FILENO) == 1; }

const char* authDeniedReasonText(Protocol::Sync::ByeReason reason, TokenSource source)
{
    // A token means something only to the daemon that issued it, so over ssh
    // the words name the variable ubq reads there and the host that issues it.
    if (source == TokenSource::Remote) {
        switch (reason) {
        case Ubiquitty::Protocol::Sync::ByeReason::TokenMissing:
            return "authentication required — no token was presented to the remote host (set UBQ_REMOTE_TOKEN to a token "
                   "that host's own `ubiquitty-host token issue` printed; a token from this machine is never sent to another)";
        case Ubiquitty::Protocol::Sync::ByeReason::TokenInvalid:
            return "the remote host does not recognize UBQ_REMOTE_TOKEN; a token counts only on the host whose "
                   "`ubiquitty-host token issue` printed it";
        case Ubiquitty::Protocol::Sync::ByeReason::TokenExpired:
            return "UBQ_REMOTE_TOKEN has expired — issue a new one on the remote host with `ubiquitty-host token issue`";
        case Ubiquitty::Protocol::Sync::ByeReason::TokenRevoked:
            return "UBQ_REMOTE_TOKEN was revoked on the remote host";
        default:
            break;
        }
    }
    switch (reason) {
    case Ubiquitty::Protocol::Sync::ByeReason::TokenMissing:
        return "authentication required — no agent token presented (set UBIQUITTY_AGENT_TOKEN "
               "or ~/.ubiquitty/agent-token; mint one with `ubiquitty-host token issue`)";
    case Ubiquitty::Protocol::Sync::ByeReason::TokenInvalid:
        return "agent token not recognized";
    case Ubiquitty::Protocol::Sync::ByeReason::TokenExpired:
        return "agent token expired — mint a new one";
    case Ubiquitty::Protocol::Sync::ByeReason::TokenRevoked:
        return "agent token was revoked";
    case Ubiquitty::Protocol::Sync::ByeReason::OutsideScope:
        return "operation outside the agent token's scope";
    case Ubiquitty::Protocol::Sync::ByeReason::VersionMismatch:
        return "the host and this ubq are from different builds of Ubiquitty — update the older of the two";
    case Ubiquitty::Protocol::Sync::ByeReason::DuplicateHandshake:
        return "a second HELLO on this connection is not allowed";
    case Ubiquitty::Protocol::Sync::ByeReason::NoConsent:
        return "no live human is available to grant consent, and this agent has no standing grant";
    case Ubiquitty::Protocol::Sync::ByeReason::RemoteHostRefused:
        return "the requested ssh host is not a plain [user@]host — no session was created";
    case Ubiquitty::Protocol::Sync::ByeReason::SessionGone:
        return "that session is no longer on this host";
    case Ubiquitty::Protocol::Sync::ByeReason::HostFull:
        return "the host is running as many sessions as it allows";
    default:
        return "authentication denied";
    }
}

std::string computeScreenDiff(const std::string& previous, const std::string& current)
{
    if (previous.empty())
        return current; // first read — the baseline is the whole screen

    auto splitLines = [](const std::string& text) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        for (;;) {
            std::size_t newline = text.find('\n', start);
            if (newline == std::string::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, newline - start));
            start = newline + 1;
        }
        return lines;
    };
    std::vector<std::string> before = splitLines(previous);
    std::vector<std::string> after = splitLines(current);
    std::size_t rowCount = before.size() > after.size() ? before.size() : after.size();
    std::string out;
    char tag[32];
    for (std::size_t i = 0; i < rowCount; ++i) {
        const bool hasBefore = i < before.size();
        const bool hasAfter = i < after.size();
        if (hasBefore && hasAfter) {
            if (before[i] != after[i]) {
                std::snprintf(tag, sizeof(tag), "~row%zu: ", i);
                out += tag;
                out += after[i];
                out.push_back('\n');
            }
        } else if (hasAfter) {
            std::snprintf(tag, sizeof(tag), "+row%zu: ", i);
            out += tag;
            out += after[i];
            out.push_back('\n');
        } else {
            std::snprintf(tag, sizeof(tag), "-row%zu\n", i);
            out += tag;
        }
    }
    if (out.empty())
        out = "(no change)";
    return out;
}

} // namespace Agent
} // namespace Ubiquitty
