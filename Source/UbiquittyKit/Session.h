// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/UbiquittyKit/Session.h — the Tier-3 embedder facade: one object that
// drives a terminal session, local or remote, behind three embedder seams —
// an Observer (render/title/bell/exit/health hooks), an input sink
// (sendInput), and a Dispatcher (the embedder's UI thread + timer).
//
// A session is the same object wherever its bytes originate: the
// LOCAL origin pumps an embedder-opened PTY Stream through a self-driving
// carrier into the Terminal; the REMOTE origin composes the Sync client over
// a Transport::Channel. The embedder never branches on origin for behavior.
//
// Threading contract, inherited from the session cores it composes: every
// public method runs on the session thread (the Dispatcher's thread), and
// every Observer hook is delivered there. Carrier I/O runs on the channel's
// own thread and is marshalled across via the Dispatcher.
//
// Ownership: the Terminal, Dispatcher, ChannelFactory, and Observer are
// caller-owned and outlive the Session.
//
// Configuration: every session follows the one live configuration
// (Core/Preferences/Configuration.h). A `Preferences::configure` call — a
// reload, a settings edit — reaches every live session through one
// notification: each re-seeds its Terminal's per-terminal state and hands its
// Observer the renderer's whole configuration, on the session thread. A
// session is created and destroyed on the main thread, where the
// configuration lives.
//

#include "Core/Preferences/Configuration.h"
#include "Core/Rendering/RenderConfiguration.h"
#include "Core/Session/Health.h"
#include "Core/Session/InputInDoubtNotice.h"
#include "Core/Session/RemoteSessionCore.h"
#include "Core/Transport/Channel.h"
#include "Core/Transport/Origin.h"
#include "Forward.h"
#include "PAL/Stream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace UbiquittyKit {

class Session final {
public:
    // Every hook has a { } default (a new hook is a kit MINOR; a pure virtual
    // would break every embedder subclass and is a kit MAJOR by policy).
    class Observer {
    public:
        virtual ~Observer() = default;
        // The Terminal's grid changed — schedule a repaint/snapshot read.
        virtual void onFrame() { }
        /// Reports the window title the session most recently set.
        ///
        /// Fires for both origins — a local shell's OSC 0/2 and a remote
        /// host's canonical PaneFacts title reach the same callback.
        ///
        /// - Parameter utf8: Valid only for the duration of the call.
        virtual void onTitle(std::string_view /*utf8*/) { }
        virtual void onBell() { }
        // The session ended: the local child exited, or the remote host's
        // shell exited. Terminal state; no further hooks follow.
        virtual void onExit() { }
        virtual void onHealthChanged(Ubiquitty::Session::Health, const std::string& /*detail*/) { }
        /// Reports that the live configuration changed.
        ///
        /// The renderer's whole configuration, already resolved: a renderer
        /// applies it and needs nothing else. The Terminal's own state — the
        /// cursor's shape and blink, the bell, the encoding — is re-seeded
        /// before this fires, and an `onFrame` follows.
        ///
        /// - Parameter configuration: Valid only for the duration of the call.
        virtual void onConfiguration(const Ubiquitty::Rendering::RenderConfiguration& /*configuration*/) { }
        /// Reports that some of what was typed into a remote session may not
        /// have reached its host after a reconnect, or that the person
        /// decided what to do with it.
        ///
        /// Read `inputInDoubtNotice()` and show it beside the terminal without
        /// blocking it. What the person types waits behind the input in
        /// doubt until `resendInputInDoubt()` or `discardInputInDoubt()` is
        /// called, and only the person's answer may call either: sending
        /// again can run a command twice.
        virtual void onInputInDoubt() { }
    };

    // LOCAL origin: `pty` is an open PTY Stream the embedder created through
    // its PAL backend (openPTY is per-backend by design). The session owns it.
    static std::unique_ptr<Session> createLocal(Ubiquitty::Terminal&, Ubiquitty::Session::Dispatcher&, Observer&, std::unique_ptr<Ubiquitty::PAL::Stream> pty);

    /// Creates a session whose carrier's bytes are already the terminal stream.
    ///
    /// The third origin, sitting between local and remote: like LOCAL it feeds
    /// the Terminal directly with no Sync framing, and like REMOTE its bytes may
    /// cross a machine boundary. An SSH channel running a login shell on an
    /// ordinary sshd is the case this exists for — such a host runs no
    /// ubiquitty-host to speak Sync, and an embedder that cannot fork/exec (iOS)
    /// cannot reach it through a subprocess `ssh` either.
    ///
    /// The session reports the CHANNEL's origin, so a carrier that stamps itself
    /// SSH is rendered as remote without the facade branching on transport.
    ///
    /// - Parameters:
    ///   - channel: The carrier. Owned by the session; started by `start`.
    ///   - applyWindowSize: Carries a resize to the carrier's own geometry — an
    ///     SSH `window-change` request. May be empty for a carrier that has no
    ///     geometry to resize.
    static std::unique_ptr<Session> createDirect(Ubiquitty::Terminal&, Ubiquitty::Session::Dispatcher&, Observer&,
        std::unique_ptr<Ubiquitty::Transport::Channel> channel, std::function<void(int, int)> applyWindowSize);

    // REMOTE origin: the factory builds the transport per (re)connect attempt;
    // `attachSessionID` 0 = a fresh daemon session, nonzero = reattach.
    static std::unique_ptr<Session> createRemote(Ubiquitty::Terminal&, Ubiquitty::Session::Dispatcher&, Ubiquitty::Session::ChannelFactory&, Observer&, std::uint32_t attachSessionID);

    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Begin the session with the initial geometry. Idempotent.
    void start(int columns, int rows);
    // End it: the local carrier closes (the child sees hangup); the remote
    // detaches cleanly (the daemon keeps the session). Idempotent.
    void stop();
    /// Sends raw input bytes toward the shell.
    ///
    /// A no-op before the session is up, for both origins: a local carrier
    /// would otherwise defer the bytes until the child exists, and a remote
    /// one has no session to attribute them to.
    ///
    /// - Parameter bytes: Copied before the call returns. A sequence
    ///   carrying an embedded NUL — Ctrl-Space is one — has to be built
    ///   from a pointer and a length, since the one-argument form stops at
    ///   the first NUL.
    void sendInput(std::string_view bytes);
    // Propagate a client resize (grid reflow + PTY/host geometry).
    void setSize(int columns, int rows);

    /// Returns Core's notice while some of a remote session's typing may not
    /// have reached its host; see ``Observer/onInputInDoubt``.
    ///
    /// - Returns: The notice; every field empty while nothing awaits the
    ///   person's decision, and always for a local session.
    [[nodiscard]] Ubiquitty::Session::InputInDoubtNotice inputInDoubtNotice() const;

    /// Sends the typing in doubt again, ahead of what waited behind it, at the
    /// person's word. The host writes none of it twice. A no-op for a local
    /// session.
    void resendInputInDoubt();

    /// Drops the typing in doubt, at the person's word, and lets what waited
    /// behind it go. A no-op for a local session.
    void discardInputInDoubt();

    /// Returns where this session's bytes come from.
    ///
    /// Stamped when the session is created and fixed for its lifetime, so an
    /// embedder renders it as a badge rather than re-deriving it from the
    /// arguments it passed. The dynamic axis is `health()`.
    ///
    /// - Returns: The session's origin.
    [[nodiscard]] Ubiquitty::Transport::Origin origin() const;

    /// Returns the session's live link state.
    ///
    /// Orthogonal to `origin()`: a local session is always connected, while a
    /// remote one moves between connecting, connected, reconnecting and failed.
    ///
    /// - Returns: The session's health.
    [[nodiscard]] Ubiquitty::Session::Health health() const;

private:
    struct LocalState;
    struct RemoteState;
    Session();
    /// Subscribes this session to the live configuration.
    ///
    /// Each replacement is carried to the session thread by value, where it
    /// re-seeds the terminal and reaches the observer, unless the session has
    /// ended by then.
    ///
    /// - Parameters:
    ///   - terminal: The session's terminal, re-seeded on each change.
    ///   - dispatcher: The session thread.
    ///   - observer: Handed the renderer's configuration on each change.
    void followConfiguration(Ubiquitty::Terminal& terminal, Ubiquitty::Session::Dispatcher& dispatcher, Observer& observer);

    // Shared: the caller-owned Dispatcher can hold queued work referencing the
    // local state (weakly) after the Session is destroyed.
    std::shared_ptr<LocalState> m_local;
    std::unique_ptr<RemoteState> m_remote;
    // Held weakly by queued configuration work, which re-checks it on the
    // session thread before touching the Terminal or the Observer.
    std::shared_ptr<int> m_lifetime;
    std::unique_ptr<Ubiquitty::Preferences::ConfigurationSubscription> m_configurationSubscription;
};

} // namespace UbiquittyKit
