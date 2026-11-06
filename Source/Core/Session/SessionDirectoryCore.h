// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The control-only Sync client that answers what a host is running before
// anything attaches to it: typed host listing and termination calls for the
// directory and the exact row the person chose to end.
//
// One channel per errand and no reconnect machinery: HELLO, the one frame the
// errand is, the answer, then the channel is gone. Every errand's HELLO is
// followed by a Sync `Greet`, so the channel is a Sync connection that
// binds no pane (Docs/sync.md §6.1): it can call the host methods its role
// and scope admit, it answers the host's probes, and it says `Bye{Normal}` on
// the way out so the host holds no attachment for it.
//
// A listing asks for more than the flat rows: it subscribes to the host's
// listing on the same connection, is sent every workspace's structure and
// the facts and status of every pane, and assembles the host's tree from
// them (SessionTree.h). A refresh delivers the tree once, whole, beside the
// rows, and closes. A watch keeps the channel and delivers the tree again
// whenever what it is built from changes, until it is stopped: it is the one
// errand that outlives its first answer. A person asked for it and is looking
// at what it keeps current, so it holds its connection through any silence of
// the host's, as an attached session does, and when its carrier drops it says
// so and dials again, a second apart at first and then backing off
// (ReconnectBackoff.h), until it is stopped or the host refuses it.
//
// A carrier that drops says so. A path that dies underneath one does not, so
// a watch asks: when its connection has been quiet for
// `watchProbeIntervalSeconds` it sends the screen plane's probe, and a
// connection that has not answered within `proofOfLifeDeadlineSeconds` is
// dropped and dialled again like one whose carrier went (ProofOfLife.h).
//
// No errand waits on a carrier to say it is dead. A dial that has brought no
// welcome within `directoryHandshakeSeconds` is one that failed, and a host
// that welcomed an errand answers it within `directoryAnswerSeconds`: a
// refresh its rows, a call its result, a management request its reply. Past
// either the errand fails as a host that could not be reached, in Core's
// words, and a watch dials again.
//
// Every errand's connection is serviced on a timer as well as on arrival:
// the host probes a connection it has nothing to say to, and drops one whose
// answer does not come within its silence bound. A connection that survived
// between errands would hold an attach the person did not ask for, and a
// directory that reconnects on its own polls a host nobody asked it to poll.
//
// Nothing here mirrors a grid, so the sync client is built with no Terminal and
// the screen a bound session sends is decoded and dropped.
//
// Single-threaded by contract, as RemoteSessionCore is: the Channel fires on its
// own I/O thread and every callback is marshaled onto the Dispatcher's thread,
// which is also the only thread the public methods may be called from.

#include "Base/WeakPtr.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Transport/Channel.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Session {

class HostListing;
struct SessionTree;

/// How a terminate request settled.
///
/// `Denied` and `TimedOut` are the two halves of a consent gate that answered,
/// `Refused` is an authorization refusal from the host, and `Failed` is a
/// request that never left this machine — four different things a caller has to
/// word differently, which is why they are not one failure value.
///
/// The one definition of these outcomes: the agent CLI names this type rather
/// than declaring a twin, and the Swift mirrors pin the same numbers, so a
/// value means one thing everywhere it crosses. The values are explicit for
/// that reason — a mirror that trusted the order would drift the first time an
/// enumerator moved.
enum class TerminateOutcome : std::uint8_t {
    Terminated = 0,
    Denied = 1,
    TimedOut = 2,
    Failed = 3,
    Refused = 4
};

/// Why a directory errand produced no answer.
///
/// Typed rather than a sentence, because each one asks the person for a
/// different remedy — fix the network, unlock a key, install the host, hand
/// over a token — and a frontend that had to read the remedy out of an English
/// string would guess. The values are explicit because the Swift mirrors pin
/// them.
enum class ListingFailure : std::uint8_t {
    /// The errand produced an answer.
    None = 0,
    /// The carrier never reached a host, or went away before it answered.
    Unreachable = 1,
    /// ssh reached the host and could not sign in unattended: the key needs a
    /// passphrase or the host wants a password.
    SignInRequired = 2,
    /// The host answered over ssh and runs no `ubiquitty-host`.
    NotInstalled = 3,
    /// The host answered and refused, for the reason the report carries.
    Refused = 4,
    /// The host answered with bytes that are not the protocol.
    Malformed = 5,
    /// The SSH server ended the connection after too many authentication
    /// attempts: ssh offered more keys than the server's `MaxAuthTries`
    /// allows, which an agent holding keys for other hosts does when no key
    /// is configured for this one.
    AuthenticationAttemptsExhausted = 6,
    /// The carrier ended before the host sent a byte and ssh said nothing
    /// about why. ssh reports every connect and sign-in failure on its
    /// standard error, so a silent end is the remote command ending: the
    /// host was reached, and its `ubiquitty-host` never answered.
    EndedBeforeAnswer = 7,
    /// The errand was pinned to one of the host's daemons, and nothing
    /// listens for that daemon any longer: it stopped. Asking it again gets
    /// the same answer; the host's current daemon is asked without the pin.
    InstanceGone = 8,
    /// The answer cannot serve the picked daemon's listing: its protocol or identity differs.
    InstanceUnreadable = 9,
};

/// Everything a failed errand says about why, as facts.
struct DirectoryFailureReport {
    /// What kind of failure it was.
    ListingFailure failure { ListingFailure::Unreachable };
    /// The host's reason, read for a refusal or an unreadable picked daemon.
    Protocol::Sync::ByeReason deniedReason { Protocol::Sync::ByeReason::Normal };
    /// The `uname -sm` the host reported: by the bootstrap for
    /// ``ListingFailure::NotInstalled``, by the refusal for
    /// ``ListingFailure::Refused``; empty when it said none.
    std::string remotePlatform;
    /// Which end of a version refusal is older, read only for
    /// ``ListingFailure::Refused``.
    Protocol::Sync::OlderSide olderSide { Protocol::Sync::OlderSide::Unknown };
    /// What the refusing or unreadable host said about its build.
    Protocol::Sync::PeerBuild hostBuild;
    /// The carrier's or the host's own words, verbatim, for a person to read;
    /// may be empty.
    std::string detail;
};

/// Reads a carrier's closing words for the failure they describe.
///
/// ssh run with `BatchMode=yes` cannot prompt, and a key or password it needed
/// ends the carrier with ssh's "Permission denied" on its standard error.
/// That wording is a sign-in the person can fix by opening an interactive
/// shell. A server that stops counting attempts disconnects with "Too many
/// authentication failures", which ssh prints as the server's reason; no
/// shell fixes that, because an interactive ssh offers the same keys. Every
/// other close is a host that could not be reached.
///
/// - Parameter closeReason: The channel's reason, with ssh's standard error
///   appended when it wrote any.
/// - Returns: ``ListingFailure::AuthenticationAttemptsExhausted``,
///   ``ListingFailure::SignInRequired`` or ``ListingFailure::Unreachable``.
[[nodiscard]] ListingFailure classifyCarrierClose(std::string_view closeReason);

/// Where the words about a failure are put.
enum class NoticePlacement : std::uint8_t {
    /// On their own: a status line, an alert's text, a result's reason. They
    /// open with a capital and end as a sentence does.
    Standalone,
    /// After other words and a colon, as in `ubq tree: the host could not
    /// be reached`. They open in lower case and end with no stop.
    Embedded,
};

/// Cases a notice for where it is put: the one place that decides it.
///
/// Notices are written as a clause, lower case with no stop, which is the
/// embedded form. Standing alone, the same clause opens with a capital and
/// takes a full stop unless it already ends a sentence.
///
/// - Parameters:
///   - notice: The clause.
///   - placement: Where it is put.
/// - Returns: The notice as it reads there; empty for an empty one.
[[nodiscard]] std::string placedNotice(std::string notice, NoticePlacement placement);

/// The words for a failed errand.
///
/// - Parameters:
///   - report: The failure.
///   - placement: Whether they stand alone or follow other words.
/// - Returns: The notice, naming what failed without prescribing a remedy.
[[nodiscard]] std::string listingFailureNotice(const DirectoryFailureReport& report, NoticePlacement placement);

/// What a directory errand produced, delivered on the Dispatcher's thread.
class SessionDirectoryObserver {
public:
    virtual ~SessionDirectoryObserver() = default;

    /// Reports a correlated administration result after closing its channel.
    virtual void onManagementReply(const Protocol::Sync::ManagementReply&) { }

    /// Reports the sessions the host is running, as its tree read one row a
    /// session; ``onSessionTree`` has reported that tree just before.
    ///
    /// - Parameter sessions: The rows in the tree's order, whole. Valid only
    ///   for the duration of the call.
    virtual void onSessionList(const std::vector<SessionRow>& /*sessions*/) { }

    /// Reports an errand that produced no answer.
    ///
    /// - Parameter report: Why, as facts: the failure kind, the host's reason
    ///   for a refusal, the platform of a host with nothing installed, and the
    ///   carrier's own words.
    virtual void onDirectoryFailed(const DirectoryFailureReport& /*report*/) { }

    /// Reports everything the host holds, as one tree.
    ///
    /// A refresh reports it once, before its session list, when the host
    /// lists itself; a watch reports it whenever it changes. A host that
    /// refuses the listing, or speaks no version that has one, reports none.
    ///
    /// - Parameter tree: The tree, assembled from the workspaces' structures
    ///   and the panes' records the host sent. Valid only for the duration of
    ///   the call.
    virtual void onSessionTree(const SessionTree& /*tree*/) { }

    /// Reports that a watch lost its host and is dialling again.
    ///
    /// Once a drop: the tree last reported is no longer followed until
    /// `onSessionTree` reports one again, which is the watch back. A watch
    /// the host refuses ends instead, as `onDirectoryFailed`.
    ///
    /// - Parameter report: Why the carrier went.
    virtual void onWatchInterrupted(const DirectoryFailureReport& /*report*/) { }

    /// Reports the host's answer to a call.
    ///
    /// - Parameter response: The JSON-RPC 2.0 response text, a result or an
    ///   error the host stated. Valid only for the duration of the call.
    virtual void onCallAnswered(const std::string& /*response*/) { }

    /// Reports how a terminate request settled.
    ///
    /// - Parameters:
    ///   - sessionID: The session the request named.
    ///   - outcome: What the host did with it.
    virtual void onTerminateSettled(std::uint32_t /*sessionID*/, TerminateOutcome /*outcome*/) { }
};

/// A control-only client that asks a host what sessions it has, and asks it to
/// end one.
///
/// One errand at a time over one channel: `isBusy` is the whole scheduling rule,
/// and a second `refresh` or `terminate` while an errand is in flight is refused
/// rather than queued, so a caller never has two answers in flight for one
/// question. Every errand dials its own channel and tears it down on the way
/// out; nothing here holds a session open, which is what lets a directory run
/// beside an attached session without joining it.
class SessionDirectoryCore final : public Ubiquitty::CanMakeWeakPtr,
                                   private Ubiquitty::Transport::ChannelObserver,
                                   private Protocol::Sync::SyncClientObserver {
public:
    /// Returns the socket identity from the most recent accepted host handshake.
    [[nodiscard]] const std::string& daemonSocketName() const { return m_daemonSocketName; }
    /// Identifies the daemon process that supplied the directory.
    [[nodiscard]] std::uint64_t daemonInstanceID() const { return m_daemonInstanceID; }

    /// Builds a directory client over a caller-owned dispatcher, factory and
    /// observer, each of which outlives it.
    ///
    /// - Parameters:
    ///   - dispatcher: Runs the marshaled channel callbacks and the terminate
    ///     timers; its thread is this object's thread.
    ///   - factory: Builds one channel per errand.
    ///   - observer: Receives every answer.
    SessionDirectoryCore(Dispatcher* dispatcher, ChannelFactory* factory, SessionDirectoryObserver* observer);
    /// Severs the references posted work reads through, then tears down any channel still in
    /// hand: a closure the dispatcher already holds cannot be recalled, so reading this object as
    /// gone is what stops it.
    ~SessionDirectoryCore() override;

    SessionDirectoryCore(const SessionDirectoryCore&) = delete;
    SessionDirectoryCore& operator=(const SessionDirectoryCore&) = delete;

    /// Sets the capability token every later HELLO presents.
    ///
    /// A token also decides whether the HELLO claims `Auth` at all: a host with
    /// `require_token` on refuses a tokenless non-local peer, and a claim with
    /// no token behind it is a claim the host cannot verify.
    ///
    /// - Parameter token: The token, or empty to present none.
    void setAuthToken(std::string token) { m_authToken = std::move(token); }

    /// Pins the identity a listing must answer for, before an errand starts.
    ///
    /// The discovered version describes the running process even if its binary was replaced.
    /// - Parameter instance: The discovered daemon; empty for the host's default.
    void setExpectedDaemon(HostInstanceInfo instance)
    {
        assert(!isBusy());
        m_expectedDaemon = std::move(instance);
    }

    /// Asks the host for its session list.
    ///
    /// Binds no session: the HELLO names `controlOnlySession`, so a host that
    /// requires consent to attach is never asked for it here. The answer
    /// arrives as `SessionDirectoryObserver::onSessionList`, and a connection
    /// that dropped before it as `onDirectoryFailed`.
    ///
    /// - Returns: Whether the errand started. False while another is in
    ///   flight — refused rather than queued, because a caller that cannot see
    ///   its request start has no way to tell a queued one from a lost one.
    [[nodiscard]] bool refresh();

    /// Starts an administration request against the explicitly selected daemon.
    [[nodiscard]] bool manage(Protocol::Sync::ManagementRequest request);

    /// Whether a management request names its daemon when it must.
    ///
    /// Every operation but a listing changes a daemon, so it has to name the
    /// incarnation it was read from; a request that names none would act on
    /// whichever daemon answered.
    ///
    /// - Parameter request: The request.
    /// - Returns: Whether ``manage`` would accept it once the client is idle.
    [[nodiscard]] static bool isManagementRequestPinned(const Protocol::Sync::ManagementRequest& request)
    {
        return request.daemonInstanceID != 0 || request.operation == Protocol::Sync::ManagementOperation::List;
    }

    /// What the last answering host said about its build; not known before
    /// any did.
    [[nodiscard]] const Protocol::Sync::PeerBuild& hostBuild() const { return m_core->hostBuild(); }

    /// Asks the host to end one session.
    ///
    /// The HELLO binds the named session, which is what carries the host's
    /// attached-requester exemption through its consent gate: a client ending
    /// the session it is attached to has already passed the attach gate.
    ///
    /// - Parameter sessionID: The session to end.
    /// - Returns: Whether the errand started, under `refresh`'s rule.
    [[nodiscard]] bool terminate(std::uint32_t sessionID);

    /// Calls one host method over the errand's Sync connection.
    ///
    /// The host admits the call from its registry entry: this connection's
    /// granted role, where it connected from, its token's scopes and the
    /// parameters. A refusal is an answer, delivered like a result.
    ///
    /// - Parameter request: A JSON-RPC 2.0 request with an id, naming the
    ///   method by its Sync name, such as `host.methods`.
    /// - Returns: Whether the errand started, under `refresh`'s rule; false
    ///   too for an empty request. The answer arrives as
    ///   `SessionDirectoryObserver::onCallAnswered`, and a connection that
    ///   dropped before it, or a request the connection could not carry, as
    ///   `onDirectoryFailed`.
    [[nodiscard]] bool call(std::string request);

    /// Lists the host and keeps listing it: the tree arrives as
    /// `SessionDirectoryObserver::onSessionTree` once it is whole, and again
    /// whenever a workspace's structure or a pane's facts change. The errand
    /// stays in flight until `stopWatching` or until the host refuses it,
    /// which arrives as `onDirectoryFailed`: a credential it turns away, a
    /// host that does not list itself, a record this build cannot read, an
    /// ssh that cannot sign in. A carrier that drops is not an end: it
    /// arrives as `onWatchInterrupted` and the watch dials again.
    ///
    /// - Returns: Whether the errand started, under `refresh`'s rule.
    [[nodiscard]] bool watch();

    /// Ends a watch: the host is told goodbye and nothing more is reported.
    /// Does nothing when no watch is in flight.
    void stopWatching();

    /// Whether a watch is in flight: following the host, or dialling it.
    [[nodiscard]] bool isWatching() const { return m_errand == Errand::Watch; }

    /// Whether a watch is following the host now: it has reported a tree on
    /// the connection it holds, and that connection stands.
    [[nodiscard]] bool isWatchLive() const { return m_errand == Errand::Watch && m_watch.isLive; }

    /// Replaces the clock the connection's liveness is measured by.
    ///
    /// - Parameter clock: Returns microseconds, monotonic.
    void setScreenClock(Protocol::Sync::SyncClientCore::Clock clock) { m_core->setClock(std::move(clock)); }

    /// Replaces the wall clock a tree's ages are anchored to this machine by.
    ///
    /// - Parameter clock: Returns milliseconds since the Unix epoch.
    void setWallClock(std::function<std::uint64_t()> clock) { m_wallClock = std::move(clock); }

    /// The role the host granted the last errand's connection, or `nullopt`
    /// before any host welcomed one.
    [[nodiscard]] std::optional<Protocol::Sync::Role> grantedRole() const { return m_grantedRole; }

    /// Whether an errand is in flight.
    ///
    /// - Returns: True between a started errand and its observer callback.
    [[nodiscard]] bool isBusy() const { return m_errand != Errand::None; }

private:
    Protocol::Sync::ManagementRequest m_managementRequest;
    Protocol::Sync::ManagementReply m_managementReply;
    std::string m_managementCallID;
    std::string m_daemonSocketName;
    std::uint64_t m_daemonInstanceID = 0;

    /// The errand one channel exists for. None between them, which is also what
    /// makes the next start() legal.
    enum class Errand : std::uint8_t {
        None,
        List,
        Management,
        Terminate,
        Call,
        Watch
    };

    /// How the errand in hand ends. Recorded rather than acted on where it is
    /// decided: a SyncClientObserver callback runs inside feed(), and tearing
    /// the channel down there would free the carrier the decoder is reading.
    enum class Finish : std::uint8_t {
        None,
        ListDelivered,
        ManagementDelivered,
        Failed,
        TerminateSettled,
        CallDelivered,
        WatchStopped,
        WatchInterrupted
    };

    // Ubiquitty::Transport::ChannelObserver — fired on the Channel's I/O thread; each marshals
    // onto the Dispatcher's thread, weak-held so a callback outliving this object does nothing,
    // and generation-stamped so a superseded errand's is dropped.
    /// Marshals the connect onto the dispatcher's thread, where the HELLO is sent.
    void onConnect() override;
    /// Copies the inbound bytes and posts them to the dispatcher's thread.
    ///
    /// - Parameter data: Valid only for the duration of the call.
    void onData(std::span<const std::uint8_t> data) override;
    /// Marshals the close onto the dispatcher's thread, which settles whatever
    /// errand was in flight as a failure.
    ///
    /// - Parameter reason: The transport's own words, carried to the observer.
    void onClose(const std::string& reason) override;

    // Protocol::Sync::SyncClientObserver — fired synchronously inside feed(),
    // already on the dispatcher's thread.
    /// Sends the errand's own request, now that the host has answered the HELLO.
    ///
    /// - Parameters:
    ///   - sessionID: The session the host bound this connection to.
    ///   - socketName: The daemon socket identity retained with a successful listing.
    void onHelloAck(std::uint32_t sessionID, const std::string& socketName) override;
    /// Settles the errand as refused: an authorization answer, not a human's.
    ///
    /// - Parameter reason: The host's own reason, shown rather than retried.
    void onRefused(Protocol::Sync::ByeReason reason) override;
    /// Settles a terminate as declined, telling a refusal apart from a timeout.
    ///
    /// - Parameters:
    ///   - sessionID: The session the answer names; another's is ignored.
    ///   - reason: Whether a person declined or the ask ran out of time.
    /// Records an exit without inferring that a pending operation succeeded.
    void onExit() override;

    // Dispatcher-thread handlers + helpers.
    [[nodiscard]] bool startErrand(Errand errand, std::uint32_t sessionID);
    /// Takes ownership of the dialled channel, or fails the errand if the
    /// factory answered nothing.
    ///
    /// - Parameter channel: The channel to own, or null when the dial failed.
    void handleChannelReady(std::unique_ptr<Ubiquitty::Transport::Channel> channel);
    /// Sends the HELLO for the errand this generation belongs to.
    ///
    /// - Parameter generation: Stamps the errand; a superseded one is dropped.
    void handleConnect(std::uint64_t generation);
    /// Feeds the decoder, then acts on whatever finish the callbacks recorded.
    ///
    /// - Parameters:
    ///   - generation: Stamps the errand; a superseded one is dropped.
    ///   - bytes: The copied inbound bytes.
    void handleData(std::uint64_t generation, const std::vector<std::uint8_t>& bytes);
    /// Fails the errand this generation belongs to.
    ///
    /// - Parameters:
    ///   - generation: Stamps the errand; a superseded one is dropped.
    ///   - reason: The carrier's closing words, ssh's standard error included.
    void handleClose(std::uint64_t generation, const std::string& reason);
    /// Ends a held terminate that nobody decided.
    ///
    /// - Parameter generation: The errand this timer was armed for.
    void handleTerminateDeadline(std::uint64_t generation);
    /// Sends the control-only HELLO, stating what the errand is for.
    void sendHello();
    /// Moves the errand's Sync connection along after bytes were fed: sends
    /// the errand's call once the host has welcomed the connection, takes its
    /// answer, and writes what the connection owes the host.
    void serviceScreen();
    /// Moves a listing along after bytes were fed: subscribes once the host
    /// has welcomed the connection, reads the answer, assembles the tree
    /// again when what it is built from changed, finishes a refresh whose
    /// rows and tree are both in hand, and reports a watch's tree.
    void serviceListing();
    /// Ends the wait for a tree that stayed incomplete: a refresh is
    /// delivered with what it holds, and a watch reports what it holds from
    /// here on.
    ///
    /// - Parameter generation: The errand this timer was armed for.
    void handleTreeDeadline(std::uint64_t generation);
    /// Arms `handleTreeDeadline` for the errand in flight.
    void armTreeDeadline();
    /// The reader's wall clock, in milliseconds since the Unix epoch.
    [[nodiscard]] std::uint64_t wallNowUnixMilliseconds() const;
    /// Asks the factory for a channel for the errand in flight.
    void dialChannel();
    /// Arms the timer the connection's liveness is serviced on, when the
    /// connection has something due and no timer is out for it.
    void armScreenTick();
    /// Services the connection's liveness: sends what is due and arms the
    /// next. A directory connection types nothing and shows no pane, so the
    /// client's own rules for a path gone quiet (input that draws no receipt,
    /// a host silent past the bound it stated for a shown pane) never apply
    /// to it: a path that dies with no transport event is the carrier's to
    /// report.
    ///
    /// - Parameter generation: The errand this timer was armed for.
    void handleScreenTick(std::uint64_t generation);
    /// Fails a refresh whose host welcomed it and has not answered its rows.
    ///
    /// - Parameter generation: The errand this timer was armed for.
    void handleListAnswerDeadline(std::uint64_t generation);
    /// Arms the timer on which a watch probes a connection gone quiet.
    void armWatchProbe();
    /// Probes a watch's connection when nothing was heard on it since the
    /// timer was armed, and arms the judgement of the answer.
    ///
    /// - Parameter generation: The connection this timer was armed for.
    void handleWatchProbe(std::uint64_t generation);
    /// Drops a watch's connection that has not answered its probe.
    ///
    /// - Parameter generation: The connection the probe was sent on.
    void handleWatchProof(std::uint64_t generation);
    /// Drops a watch's carrier, tells the observer once, and arms the next
    /// dial.
    ///
    /// - Parameter report: Why the carrier went.
    void interruptWatch(const DirectoryFailureReport& report);
    /// Ends an errand whose call nobody answered.
    ///
    /// - Parameter generation: The errand this timer was armed for.
    void handleCallDeadline(std::uint64_t generation);
    /// Records how the errand ends, keeping the first answer.
    ///
    /// - Parameters:
    ///   - finish: What to report.
    ///   - outcome: The terminate outcome, read only for `TerminateSettled`.
    ///   - failure: Why the errand failed, read only for `Failed`.
    void requestFinish(Finish finish, TerminateOutcome outcome = TerminateOutcome::Failed, DirectoryFailureReport failure = {});
    /// Records a failure, or a failed terminate when the errand is one.
    ///
    /// - Parameter failure: Why.
    void requestFailure(DirectoryFailureReport failure);
    /// Tears the channel down and reports the recorded answer.
    ///
    /// The report comes after the teardown, so an observer may start the next
    /// errand from inside the callback.
    void completeFinish();
    /// Closes and releases the channel, leaving the object legal for a next
    /// errand. Safe to call with no channel in hand.
    void teardownChannel();
    /// Queues one built frame.
    ///
    /// - Parameter frame: The framed bytes; an empty one is not a frame.
    /// - Returns: Whether the frame reached the carrier.
    [[nodiscard]] bool writeFrame(const std::vector<std::uint8_t>& frame);

    Dispatcher* m_dispatcher;
    ChannelFactory* m_factory;
    SessionDirectoryObserver* m_observer;
    std::unique_ptr<Protocol::Sync::SyncClientCore> m_core;
    std::unique_ptr<Ubiquitty::Transport::Channel> m_channel;

    std::string m_authToken;
    HostInstanceInfo m_expectedDaemon;
    BootstrapResponse m_bootstrapResponse;
    Errand m_errand = Errand::None;
    Finish m_finish = Finish::None;
    DirectoryFailureReport m_finishFailure;
    TerminateOutcome m_finishOutcome = TerminateOutcome::Failed;
    // What the errand in flight has been sent of the host, and the tree
    // assembled from it; kept until the next errand so a tree reported last
    // stays readable.
    std::unique_ptr<HostListing> m_listing;
    std::function<std::uint64_t()> m_wallClock;
    /// How far the listing errand in flight has got.
    struct ListingProgress {
        bool didSubscribe = false; // the subscription was called for
        bool isTreeDue = false; // the wait for a whole tree is over
        bool hasUnreportedTree = false; // a watch's tree moved since it was reported
    };
    ListingProgress m_listed;
    // The generation a liveness timer is out for; zero for none.
    std::uint64_t m_screenTickGeneration = 0;
    /// Where a watch stands with its host.
    struct WatchProgress {
        bool isLive = false; // a tree was reported on the connection that stands
        bool didReportInterruption = false; // the observer was told of the drop being redialled
        unsigned attempts = 0; // dials since the watch last followed its host
    };
    WatchProgress m_watch;
    bool m_didHearSinceProbeArmed = false; // bytes arrived since the quiet timer was armed
    std::string m_callRequest; // the JSON-RPC request a call errand sends once welcomed
    std::string m_callResponse;
    std::optional<Protocol::Sync::Role> m_grantedRole;
    bool m_didSendCall = false;
    std::uint32_t m_targetSessionID = 0; // the session a terminate names; 0 for a list
    std::uint32_t m_nextRequestID = 0; // pre-incremented; 0 is never assigned
    bool m_didSeeAck = false;
    std::string m_terminationRequestID;
    // Bumped per errand and per teardown; a marshaled callback or a timer
    // carrying an older value belongs to a finished errand and is dropped.
    // Atomic: the I/O thread reads it to stamp a callback while the
    // dispatcher's thread bumps it.
    std::atomic<std::uint64_t> m_generation { 0 };
};

} // namespace Ubiquitty::Session
