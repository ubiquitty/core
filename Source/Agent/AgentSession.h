// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A headless ubiquitty-host client: attaches to the local daemon's host.sock,
// speaks the Sync protocol, and applies its Sync screen plane into a passive Core Terminal
// it owns (no PTY, no VT parser). The agent surface reads that Terminal
// programmatically (screenText / cursor / dimensions) and sends keystrokes as Sync input.
// Transport is a direct AF_UNIX connect to host.sock; the same frames ride QUIC or
// ssh-stdio unchanged over a network.

#include "Agent/AgentCapabilityPolicy.h"
#include "Core/Codec/JSON.h"
#include "Core/OSC/Activity.h"
#include "Core/Protocol/Control/CaptureReply.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Session/EventLog.h"
#include "Core/Session/SessionDirectoryCore.h"
#include "Core/Session/SessionRows.h"
#include "Core/Session/SessionTree.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty {
namespace Agent {

class RemoteCarrier;

/// Where an event subscription starts.
enum class EventStart : uint8_t {
    /// After a cursor the caller saved; zero replays everything the host holds.
    AfterCursor,
    /// At the newest record, skipping the backlog.
    FromLatest,
};

/// Where a connection's token comes from, which decides what a refusal of it
/// tells a person to set.
enum class TokenSource : std::uint8_t {
    /// This machine's daemon: `UBIQUITTY_AGENT_TOKEN`, else the agent-token
    /// file in the control directory.
    Local,
    /// Another host's daemon reached over ssh: `UBQ_REMOTE_TOKEN`, which that
    /// host issued.
    Remote,
};

class AgentSession : public Ubiquitty::Protocol::Sync::SyncClientObserver {
public:
    AgentSession();
    ~AgentSession() override;

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;

    // Connect to the local daemon and HELLO. sessionID 0 attaches a fresh
    // session; echo a prior id to reattach. Returns false when no daemon
    // is listening on host.sock.
    bool connect(int columns, int rows, uint32_t sessionID = 0);

    // Connect as a control-only client (controlOnlySession): the daemon
    // binds no session, so ls/kill don't spawn a throwaway shell. The
    // connection is greeted on the screen plane and binds no pane there, so
    // it can make the host calls its role and scope admit. Waits for
    // HELLO_ACK. False when no daemon is listening. Use listSessions(),
    // listHost(), callHost() and terminateSession() after this; the
    // screen/cursor reads are meaningless on a control-only connection.
    bool connectControl();

    /// A host's tree as a listing of it assembled, and the host's clock.
    struct HostTree {
        Ubiquitty::Session::SessionTree tree;
        /// The host's wall clock now, which ages in the tree are read against.
        std::uint64_t hostNowUnixMilliseconds = 0;
        /// Whether every promised workspace and every live pane's facts arrived.
        bool isWhole = false; // bool-ok: the listing's completeness verdict
    };

    /// Subscribes this connection to the host's listing and waits for the
    /// tree: every workspace the host said it holds and the facts of every
    /// pane that runs at least at the revisions named by the answer. The
    /// subscription is retired before returning, including after timeout.
    ///
    /// - Parameter timeoutMilliseconds: How long to wait for the host's
    ///   answer, and then for the tree to be whole.
    /// - Returns: The tree, or nothing when the host refused the listing,
    ///   sent one that does not read, or did not finish the promised records.
    [[nodiscard]] std::optional<HostTree> listHost(int timeoutMilliseconds = localAnswerWaitMilliseconds);

    /// Calls one host method over this connection and waits for its answer.
    ///
    /// - Parameters:
    ///   - method: The Sync method.
    ///   - parameters: Its parameters, an object.
    ///   - timeoutMilliseconds: How long to wait, or zero for the method's
    ///     bounded default (35 seconds for a tree edit, five otherwise).
    /// - Returns: The JSON-RPC 2.0 response, with a `result` or the `error`
    ///   the host stated, or nothing when no answer arrived.
    [[nodiscard]] std::optional<Ubiquitty::Codec::JSON::Value> callHost(std::string_view method, const Ubiquitty::Codec::JSON::Value& parameters, int timeoutMilliseconds = 0);

    // Drive the socket for up to timeoutMilliseconds: read frames, apply the
    // screen into the retained Terminal, capture title/bell/exit. Returns
    // false once the peer closed (exited()) or the decoder faulted.
    bool pump(int timeoutMilliseconds);

    /// Pumps until a condition holds, the connection closes, or a deadline
    /// passes.
    ///
    /// The deadline is time on the clock from the call. A pump returns as
    /// soon as a frame arrives, so a session that prints continuously wakes
    /// a wait many times a second, and a wait that counted each wake as a
    /// fixed step would give up long before its timeout. Each wake checks the
    /// condition once; nothing is polled while nothing arrives.
    ///
    /// - Parameters:
    ///   - timeoutMilliseconds: How long to wait; nothing waits until the
    ///     condition holds or the connection closes.
    ///   - isDone: The condition, checked before each pump.
    /// - Returns: Whether the condition held.
    template<typename Predicate>
    bool pumpUntil(std::optional<int> timeoutMilliseconds, Predicate isDone)
    {
        const auto start = std::chrono::steady_clock::now();
        while (!isDone()) {
            int slice = 1000;
            if (timeoutMilliseconds) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                if (elapsed >= *timeoutMilliseconds)
                    return false;
                slice = static_cast<int>(std::min<long long>(slice, *timeoutMilliseconds - elapsed));
            }
            slice = boundedWaitMilliseconds(slice);
            if (!slice)
                return false;
            if (!pump(slice))
                return isDone();
        }
        return true;
    }

    /// Pumps until the host welcomes and binds the pane, for a verb that reads
    /// records and regions rather than the screen: the pane stays
    /// `RecordsOnly`, so the host sends it no rows.
    ///
    /// An attach the host holds for a person's consent waits for the answer,
    /// as `waitForFirstScreen` does.
    ///
    /// - Parameter timeoutMilliseconds: How long to wait before the consent
    ///   extension.
    /// - Returns: Whether the authenticated inner attachment has a bound pane.
    bool waitForAttach(int timeoutMilliseconds);

    /// How long an attach the host holds for consent waits: the daemon's
    /// 30-second question, and margin for its answer to arrive.
    static constexpr int consentWaitMilliseconds = 35000;

    /// Pumps until the first checkpoint applies or the deadline passes, having
    /// asked the host for the screen: an agent's pane is `RecordsOnly` until
    /// a verb that reads it sets it `Live`.
    ///
    /// The handshake reply is HELLO_ACK, then the screen plane's checkpoint,
    /// so a checkpoint is what "ready" means. When the host holds the attach
    /// for a human's consent (AwaitingConsent Welcome and Pending status), the
    /// wait extends to cover the daemon's consent timeout; the checkpoint
    /// arrives after Approved, or a terminal status/close refuses it (see
    /// `consentDenied()`).
    ///
    /// - Parameter timeoutMilliseconds: How long to wait before the consent
    ///   extension.
    /// - Returns: Whether a checkpoint applied.
    bool waitForFirstScreen(int timeoutMilliseconds);

    /// Reports whether the last first-screen wait exhausted its deadline.
    ///
    /// A failed carrier, refusal, or detach ends the wait without expiring it.
    ///
    /// - Returns: Whether the deadline, rather than the connection, ended the wait.
    [[nodiscard]] bool didFirstScreenWaitExpire() const { return m_didFirstScreenWaitExpire; }

    // ---- consent gate ----------------------------------
    // The daemon holds an agent's attach to a HUMAN-occupied session until
    // the user approves. awaitingConsent() is true between exact Pending status
    // and its resolution; consentDenied() latches once the user refused or
    // the prompt timed out (Denied or Expired) — distinct from
    // a plain peer-close so the caller reports "denied" vs "session ended".
    [[nodiscard]] bool awaitingConsent() const { return m_awaitingConsent; }
    [[nodiscard]] bool consentDenied() const { return m_consentDenied; }
    [[nodiscard]] Protocol::Sync::ConsentStatusPhase consentDeniedReason() const { return m_consentDeniedReason; }

    // ---- token auth ---------------------------------------
    // Latched once the host refuses our capability token, or its absence;
    // reason is a AuthDenied* code. Distinct from a plain close so the caller
    // reports "auth required/denied" with a distinct exit code.
    [[nodiscard]] bool authDenied() const { return m_authDenied; }
    [[nodiscard]] Protocol::Sync::ByeReason authDeniedReason() const { return m_authDeniedReason; }

    /// Says why the host refused this connection, naming the token the way
    /// this connection found it (`UBQ_REMOTE_TOKEN` over ssh, the local
    /// token otherwise), followed by what the host's `Bye` said.
    ///
    /// - Returns: The words; meaningful once `authDenied()` is true.
    [[nodiscard]] std::string refusalText() const;

    /// What the host's `Bye` said beyond its reason, such as which scope a
    /// token lacked; empty while the connection serves, or when it said
    /// nothing more.
    [[nodiscard]] std::string closingDetail() const;

    /// Whether this connection reaches another host's daemon over ssh, whose
    /// tokens that host issues.
    [[nodiscard]] TokenSource tokenSource() const { return m_remoteHost.empty() ? TokenSource::Local : TokenSource::Remote; }

    /// Says why the connection ended, for a request it left unanswered.
    ///
    /// The host's `Bye` names its reason; a connection the host closed with
    /// none, or one this side lost, says so instead.
    ///
    /// - Returns: The words, or empty while the connection serves.
    [[nodiscard]] std::string endedReason() const;

    /// Words for a request the host did not answer: why the connection ended,
    /// when it has, rather than a silence it did not keep.
    ///
    /// - Parameter request: What was asked, as "the screen capture".
    /// - Returns: The words.
    [[nodiscard]] std::string unansweredText(std::string_view request) const;

    // Invoked once, the instant the host's exact Pending status arrives, with the
    // held session id — for a live "awaiting consent" notice while the wait
    // blocks on the human. Optional.
    void setConsentPendingNotice(std::function<void(uint32_t)> notice) { m_onConsentPending = std::move(notice); }

    // The agent's self-identification, sent in HELLO and shown to the human
    // in the consent prompt as an agent-claimed (spoofable) display hint —
    // not the grant key (that's the daemon-verified peer id). Default
    // "ubq"; the MCP server sets the name its client gave in `initialize`'s
    // clientInfo, or "mcp-client". The env override UBIQUITTY_AGENT_LABEL
    // wins, letting the controlling tool self-name.
    void setAgentLabel(std::string label) { m_agentLabel = std::move(label); }

    /// Reaches the daemon on another machine over ssh rather than the local socket.
    ///
    /// - Parameter host: A `[user@]host`; validated when the connection opens.
    void setRemoteHost(std::string host) { m_remoteHost = std::move(host); }

    /// How long a request waits for a local daemon's answer.
    ///
    /// A bound for a daemon that has stopped answering, not a guess at how
    /// long an answer takes: a closed connection ends the wait at once, and a
    /// live daemon answers in milliseconds unless the machine is saturated.
    static constexpr int localAnswerWaitMilliseconds = 10000;
    /// How long a request waits for the first answer through ssh, which comes
    /// after the connection, the login and a daemon the remote end may start.
    static constexpr int remoteAnswerWaitMilliseconds = 30000;
    /// The bound for the daemon's first answer on this connection.
    ///
    /// - Returns: The wait, in milliseconds.
    [[nodiscard]] int answerWaitMilliseconds() const { return m_remoteHost.empty() ? localAnswerWaitMilliseconds : remoteAnswerWaitMilliseconds; }

    /// Returns why the last connection attempt failed, when it can say.
    [[nodiscard]] const std::string& connectFailure() const { return m_connectFailure; }

    /// Whether the connection reached the daemon `UBIQUITTY_SOCKET` names.
    ///
    /// The pane IDs a process's environment carries are that daemon's own
    /// numbering, so they name the caller's pane only on a connection to it;
    /// the app's control socket and any other daemon number theirs apart.
    [[nodiscard]] bool isConnectedToPaneDaemon() const { return m_isConnectedToPaneDaemon; }

    /// Asks the host to push its events on this connection.
    ///
    /// - Parameters:
    ///   - start: Whether to replay from a cursor or begin at the newest record.
    ///   - sinceCursor: The last cursor the caller saw, for ``EventStart/AfterCursor``.
    /// - Returns: Whether the host acknowledged the subscription; a refusal
    ///   or transport silence is never treated as success.
    bool subscribeEvents(EventStart start, std::uint64_t sinceCursor = 0);

    /// Removes and returns the events received since the last call, oldest first.
    [[nodiscard]] std::vector<Session::Event> takeEvents();

    /// Returns how many screen changes have been applied, so a waiter can
    /// tell that the screen moved without comparing it.
    [[nodiscard]] std::uint64_t screenUpdates() const { return m_screenUpdates; }

    /// What became of input this client sent.
    enum class InputDelivery : std::uint8_t {
        /// The host says it wrote every byte to the session's program.
        Written,
        /// The pane was never bound, a frame could not be sent, or the
        /// connection closed.
        NotSent,
        /// Sent, and the host did not say it wrote it within
        /// `inputWrittenWaitMilliseconds`: it may be written later, or never.
        Unconfirmed,
    };
    /// How long a sender waits for the host to say it wrote the input.
    ///
    /// A bound for a host that has stopped answering, not a guess at how long
    /// a write takes: the host answers when the bytes reach the program, which
    /// is milliseconds unless the program has stopped reading.
    static constexpr int inputWrittenWaitMilliseconds = 10000;

    /// Sends keystrokes, paste or report bytes to the session's PTY, and waits
    /// for the host to say it wrote them.
    ///
    /// A caller that sends and leaves is held until then, so the connection
    /// does not go while the input it carried is still on its way; a sender
    /// reporting success without that answer reports a command typed that
    /// may never have reached the shell.
    ///
    /// - Parameter bytes: Already encoded for the wire. A sequence carrying
    ///   an embedded NUL has to be built from a pointer and a length, since
    ///   the one-argument form stops at the first NUL.
    /// - Returns: What became of the input.
    [[nodiscard]] InputDelivery sendInput(std::string_view bytes);

    /// Sets the session's size once, as an agent's explicit resize
    /// (`pane.overrideGeometry`): it holds until a driver types or reports a
    /// different size.
    ///
    /// - Parameters:
    ///   - columns: The columns asked for.
    ///   - rows: The rows asked for.
    /// - Returns: Whether the host set it; false when it refused, when the
    ///   pane was never bound, or when no answer came in time.
    bool resize(int columns, int rows);

    // Detach cleanly (EXIT); the daemon keeps the session alive.
    void detach();

    // ---- programmatic reads (the agent surface) ----------------------
    [[nodiscard]] int columns() const;
    [[nodiscard]] int rows() const;
    void cursor(int& row, int& column) const;
    // Count of inline-image placements mirrored into the passive Ubiquitty::Terminal from
    // IMAGE_PLACEMENTS frames. 0 until the host transmits
    // graphics. Lets a caller wait for an image to land over the wire.
    [[nodiscard]] int imagePlacementCount() const;
    [[nodiscard]] uint32_t sessionID() const { return m_core->sessionID(); }
    /// The handle the attached session's pane is bound under on this
    /// connection: what a pane call is addressed by.
    /// - Returns: The handle, or zero before the host bound the pane.
    [[nodiscard]] std::uint32_t paneHandle() const
    {
        const Ubiquitty::Protocol::Sync::ClientConnection* screen = m_core->screen();
        return screen && m_core->isScreenAttached() ? screen->handleForPane({ .low = m_core->sessionID(), .high = m_core->daemonInstanceID() }) : 0;
    }
    [[nodiscard]] bool ready() const { return m_core->hasScreen(); }
    [[nodiscard]] bool exited() const { return m_exited; }
    [[nodiscard]] bool sawHelloAck() const { return m_core->sawHelloAck(); }
    /// The screen-plane updates applied since the handshake.
    [[nodiscard]] int updatesApplied() const
    {
        const Ubiquitty::Protocol::Sync::ClientConnection* screen = m_core->screen();
        return screen ? static_cast<int>(screen->counters().updatesApplied) : 0;
    }

    // Project the visible grid to UTF-8 text: rows joined by '\n', each
    // row's trailing blanks trimmed, wide-continuation cells skipped.
    [[nodiscard]] std::string screenText() const;

    /// Reads a complete visible screen through the host's RegionRead-gated
    /// call after this client has received its first coherent screen.
    ///
    /// - Parameter timeoutMilliseconds: The bounded reply wait.
    /// - Returns: Complete text or an explicit invalid/refused/incomplete
    ///   answer; nothing if unavailable, timed out, or the target changed.
    [[nodiscard]] std::optional<Protocol::Control::CaptureReply> captureScreen(int timeoutMilliseconds = localAnswerWaitMilliseconds);

    /// Reads a host-selected range through the RegionRead-gated typed call.
    ///
    /// - Parameters:
    ///   - request: The source and its bounded range parameters.
    ///   - timeoutMilliseconds: The bounded reply wait.
    /// - Returns: The correlated complete/refused answer, or nothing after
    ///   timeout, transport loss or target replacement. Never a cached fallback.
    [[nodiscard]] std::optional<Protocol::Control::CaptureReply> captureText(const Protocol::Control::CaptureRequest& request, int timeoutMilliseconds = localAnswerWaitMilliseconds);

    /// Waits on fresh authorized captures, never searching a cached replica.
    ///
    /// - Parameters:
    ///   - needle: The text to find in a complete visible screen.
    ///   - timeoutMilliseconds: The total deadline; absent waits until found.
    ///   - isFound: Set only after a complete capture contains the needle.
    /// - Returns: The last complete capture on match/timeout, a refusal, or
    ///   nothing on transport loss or a capture timeout.
    [[nodiscard]] std::optional<Protocol::Control::CaptureReply> waitForText(std::string_view needle, std::optional<int> timeoutMilliseconds, bool& isFound);

    /// Why a command stopped waiting for completion.
    enum class RunCompletion : std::uint8_t {
        /// The shell reported an explicit command completion.
        CommandDone,
        /// An unmarked shell regained the foreground and its screen settled.
        Settled,
        /// The shared deadline expired before completion.
        TimedOut,
        /// Input or transport failed before completion could be established.
        Unavailable,
    };

    /// The source that correlates an explicit command completion.
    enum class RunObservation : std::uint8_t {
        /// Reads the pane's completion effects without an event subscription.
        Effects,
        /// Subscribes to events and requires a command start before its end.
        Events,
    };

    /// A command's completion and the separately authorized capture of its output.
    struct RunResult {
        RunCompletion completion = RunCompletion::Unavailable;
        std::optional<Protocol::Control::CaptureReply> capture;
        /// The shell's status, absent without CommandDone; INT32_MIN is unknown.
        std::optional<std::int32_t> exitStatus;
        /// The completed event's cursor, or the last observed cursor on settling.
        std::optional<std::uint64_t> cursor;
        /// The delivery result when input failed before completion was observed.
        std::optional<InputDelivery> inputDelivery;
    };

    /// Sends a command and captures its output after completion.
    ///
    /// The pane must be at a prompt; input otherwise goes to its foreground
    /// program. Marked prompts require CommandDone. An unmarked shell can
    /// complete through a changed, settled screen with the shell in front.
    /// Startup, input delivery, completion and capture share one deadline.
    ///
    /// - Parameters:
    ///   - command: The line to send, without its terminating Enter.
    ///   - timeoutMilliseconds: The whole run's budget, or no deadline if absent.
    ///   - observation: Whether completion effects or correlated events finish it.
    /// - Returns: The completion reason, capture answer and any reported status.
    [[nodiscard]] RunResult runCommandWithExit(const std::string& command, std::optional<int> timeoutMilliseconds, RunObservation observation = RunObservation::Effects);

    // ---- command lifecycle -----------------------
    // Block until the next CommandDone (a foreground command finished) or
    // the deadline; on success returns true with the exit code (INT32_MIN
    // if the shell reported none) and currentDirectory. Needs shell integration
    // (OSC 133 ;D) on the host.
    bool waitForExit(int timeoutMilliseconds, int32_t& exitCode, std::string& currentDirectory);
    // Block until the foreground returns to the shell (Status idle) or the
    // deadline; returns true when idle.
    bool waitForIdle(int timeoutMilliseconds);
    [[nodiscard]] bool foregroundIsShell() const { return m_foregroundIsShell; }
    [[nodiscard]] int32_t lastExitCode() const { return m_lastExitCode; }
    // The explicit command line (OSC 633;E) of the most recent completed command,
    // propagated over Sync. Empty when the shell reported no OSC 633;E.
    [[nodiscard]] const std::string& lastCommandLine() const { return m_lastCommandLine; }

    // ---- bounded reads ------------------------------
    // Read the last command's output (between the last two OSC-133 marks)
    // or an absolute row range. Returns true with the UTF-8 text, false on
    // timeout or a non-ok host status (out-of-range / no command).
    bool readLastCommand(std::string& textOut, int timeoutMilliseconds = localAnswerWaitMilliseconds);
    bool readRegion(int64_t absoluteRowStart, int64_t absoluteRowEnd, std::string& textOut, int timeoutMilliseconds = localAnswerWaitMilliseconds);
    /// Returns the OSC 7501 records the session's programs reported, as the
    /// host's snapshot carried them.
    [[nodiscard]] const OSC::ProgramStatusStore& programStatus() const;

    // ---- session control -------------------------------
    bool listSessions(std::vector<Ubiquitty::Session::SessionRow>& out, int timeoutMilliseconds = localAnswerWaitMilliseconds);

    /// The directory's outcome vocabulary for a correlated termination result.
    using TerminateOutcome = Ubiquitty::Session::TerminateOutcome;

    /// Asks the host to end one exact session under its current authority.
    /// Attached and standing-grant exemptions remain host decisions; otherwise
    /// one human approval is required. Silence and disconnect are not success.
    /// - Parameters:
    ///   - id: The nonzero target session.
    ///   - timeoutMilliseconds: How long to wait for the correlated result.
    /// - Returns: The confirmed outcome, or a refusal/failure/timeout.
    TerminateOutcome terminateSession(uint32_t id, int timeoutMilliseconds = 35000);

    /// What the host answered when it refused the last `terminateSession`
    /// with an error rather than by closing: its words and its JSON-RPC code.
    struct HostRefusal {
        std::int32_t code = 0;
        std::string message;
    };
    /// The host's refusal of the last `terminateSession`, when it answered
    /// one; nothing when it ended the session or closed the connection.
    [[nodiscard]] const std::optional<HostRefusal>& terminationRefusal() const { return m_terminationRefusal; }

    // ---- screenshot --------------------------------------------------
    // Render the current screen to a PNG (Unifont glyphs, palette colors,
    // block cursor) — for visual TUIs that don't linearize as text.
    // renderPNG() returns the encoded bytes (empty on failure);
    // screenshot() writes them to `path`.
    std::vector<uint8_t> renderPNG();
    bool screenshot(const std::string& path);

private:
    /// Bounds a nested wait by the current command's deadline, when present.
    /// - Parameter milliseconds: The phase's own wait limit.
    /// - Returns: The smaller remaining wait, never negative.
    [[nodiscard]] int boundedWaitMilliseconds(int milliseconds) const;
    /// Performs one correlated capture without retrying pending font geometry.
    ///
    /// - Parameters:
    ///   - request: The source and range to capture.
    ///   - timeoutMilliseconds: The remaining deadline budget.
    /// - Returns: The correlated answer, or absence on timeout or detach.
    [[nodiscard]] std::optional<Protocol::Control::CaptureReply> captureTextOnce(const Protocol::Control::CaptureRequest& request, int timeoutMilliseconds);

    friend struct AgentCaptureAccess;
    bool openSocketAndPrepare(); // connect (+autostart daemon), set non-blocking, store fd
    bool writeFrame(const std::vector<uint8_t>& frame);
    bool drainReadable();
    /// Writes what the screen plane owes the host.
    void flushScreen();
    [[nodiscard]] std::string resolveAgentLabel() const; // env override else m_agentLabel
    [[nodiscard]] std::string resolveAuthToken() const; // env UBIQUITTY_AGENT_TOKEN else ~/.ubiquitty/agent-token

    // SyncClientObserver: the core owns the screen + handshake state; these
    // capture the agent-only signals (consent/auth latches, bell, exit,
    // command/region/list results).
    /// Counts a screen change, which a waiter reads through `screenUpdates()`,
    /// and ends a held attach's wait for consent.
    ///
    /// - Parameter sessionID: The session whose screen changed.
    void onScreenChanged(uint32_t sessionID) override;
    void onBell() override;
    void onExit() override;
    void onCommandDone(const Protocol::Sync::EffectView&, const Protocol::Sync::CommandFinishedEffect&, Protocol::Sync::EffectDisposition) override;
    /// Updates the shell-prompt state from the host's pane facts.
    ///
    /// - Parameters:
    ///   - sessionID: The session whose facts were published.
    ///   - facts: The pane's current foreground and prompt facts.
    void onPaneFacts(uint32_t sessionID, const Ubiquitty::Protocol::Sync::PaneFacts& facts) override;
    void onConsentPending(uint32_t sessionID) override;
    void onConsentDenied(uint32_t sessionID, Protocol::Sync::ConsentStatusPhase phase) override;
    /// Records the host's refusal for `authDenied` and `authDeniedReason`.
    ///
    /// - Parameter reason: Why the host refused.
    void onRefused(Protocol::Sync::ByeReason reason) override;
    // Declared before the descriptor it feeds, so the descriptor closes first
    // and the carrier's threads see the client gone before they stop.
    std::unique_ptr<RemoteCarrier> m_remoteCarrier;
    std::string m_remoteHost;
    std::string m_connectFailure;
    bool m_isConnectedToPaneDaemon = false;
    PAL::UnixFileDescriptor m_socketFileDescriptor;
    // Declared before the Terminal so it outlives what points at it: members
    // are destroyed in reverse declaration order.
    Ubiquitty::Agent::AgentCapabilityPolicy m_capabilityPolicy;
    std::unique_ptr<Ubiquitty::Terminal> m_term; // retained passive reference frame
    std::unique_ptr<Ubiquitty::Protocol::Sync::SyncClientCore> m_core;
    bool m_bellPending = false;
    bool m_exited = false;
    bool m_didScreenWriteFail = false;
    bool m_didFirstScreenWaitExpire = false;

    // consent gate
    std::string m_agentLabel = "ubq";
    bool m_awaitingConsent = false;
    bool m_consentDenied = false;
    Protocol::Sync::ConsentStatusPhase m_consentDeniedReason = Protocol::Sync::ConsentStatusPhase::Denied;
    std::function<void(uint32_t)> m_onConsentPending;
    bool m_authDenied = false;
    Protocol::Sync::ByeReason m_authDeniedReason = Protocol::Sync::ByeReason::TokenMissing;

    // command lifecycle
    std::optional<std::chrono::steady_clock::time_point> m_runDeadline;
    bool m_foregroundIsShell = false;
    bool m_hasMarkedPrompt = false;
    std::uint32_t m_foregroundGeneration = 0;
    int32_t m_lastExitCode = 0;
    std::string m_lastCurrentDirectory;
    std::string m_lastCommandLine; // OSC 633;E command line of the last completion
    /// An owned command completion retained after the borrowed effect returns.
    struct CompletedCommand {
        std::int32_t exitCode = 0;
        std::string currentDirectory;
    };
    // Owning completions in arrival order. waitForExit latches the FIRST arrival
    // after a command is sent, so a command's own OSC-133 D isn't masked by
    // the shell's prompt-redraw D landing in the same pump batch.
    std::vector<CompletedCommand> m_commandDones;

    // Pending request/response state for session and control requests.
    /// The number in the id of the last host call this session made.
    std::uint64_t m_nextHostCallID = 0;
    /// Never reused, even when a timed-out answer arrives on a later request.
    std::uint64_t m_nextCaptureRequestID = 0;
    /// Each physical socket attempt retires every prior capture binding.
    std::uint64_t m_captureConnectionGeneration = 0;
    // The host's events, held until the caller takes them; bounded so a
    // caller that never takes them cannot grow without limit.
    std::vector<Session::Event> m_events;
    std::optional<std::uint64_t> m_eventSubscriptionCursor;
    std::uint64_t m_screenUpdates = 0;
    std::optional<HostRefusal> m_terminationRefusal;
};

// Line-level diff between two screen projections (screenText output): one
// line per changed/added/removed row — "~rowN: <text>", "+rowN: <text>",
// "-rowN" — or "(no change)". Powers read_changes ("what changed since I
// last looked") purely client-side, independent of the wire's Updates. A
// blank `previous` returns `current` verbatim (the baseline read).
std::string computeScreenDiff(const std::string& previous, const std::string& current);

/// Says why a host refused a connection, for the command line and the MCP
/// server alike.
///
/// - Parameters:
///   - reason: The host's refusal.
///   - source: Where the refused token came from.
/// - Returns: The words.
const char* authDeniedReasonText(Protocol::Sync::ByeReason reason, TokenSource source = TokenSource::Local);

/// Reports whether this process's standard input is a terminal.
///
/// A confirmation of the agent policy is a person's answer, and a person
/// types at a terminal; a process without one on its standard input is run
/// by something else, whatever its arguments claim.
///
/// - Returns: Whether standard input is a terminal.
[[nodiscard]] bool isStandardInputTerminal();

} // namespace Agent
} // namespace Ubiquitty
