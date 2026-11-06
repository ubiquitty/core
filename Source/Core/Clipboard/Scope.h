// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Which pasteboard a session's programs read, and what a frontend says about it.
//
// The question has two answers and no way to tell them apart from the screen: a
// program on a remote host that shells out for a clipboard either finds nothing
// — a headless host runs no display server — or is answered from the pasteboard
// of the machine the user is sitting at, which means every process on that host
// can read what the user copied.
//
// The DECISION is here rather than in a frontend because it is the same
// decision everywhere: the same three facts, the same three answers. A frontend
// resolving it privately would be a second copy of a security rule, and the two
// would drift the first time either was edited. What a frontend owns is the
// drawing — a title-bar badge on macOS, whatever Linux and the web do — and the
// words, which belong to the platform's own voice.
//
// The facts arrive as booleans rather than as a settings fold. This is an L1
// model module and the settings live in L2, so taking the fold would invert the
// layering; the caller already holds the settings and reading one key is not
// the part worth sharing.

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::Clipboard {

/// What a session's programs read when they ask for a clipboard.
enum class UBIQUITTY_CLOSED_ENUM Scope : std::uint8_t {
    /// Not a remote session: one machine, one pasteboard, nothing to report.
    ///
    /// A frontend shows nothing for this. An indicator that is always present
    /// is one nobody reads, and the local case carries no ambiguity to resolve.
    Local UBIQUITTY_SWIFT_NAME(local) = 0,
    /// The host's own clipboard, which a headless host does not have.
    ///
    /// Worth showing but not worth alarm: nothing of the user's is exposed, and
    /// a frontend that shouts at the safe state trains people past the badge
    /// that matters.
    Host UBIQUITTY_SWIFT_NAME(host) = 1,
    /// This machine's clipboard, served over the session on request.
    ///
    /// The state worth a colour. Every process on the remote host can ask for
    /// it while the session is open, and a disclosure the user opted into is
    /// still one they are entitled to see.
    Client UBIQUITTY_SWIFT_NAME(client) = 2,
};

/// Whether a client promises to answer the host's clipboard requests.
///
/// Named once and shared because every client asks the same question of itself
/// before it claims the capability, and a per-client spelling of a two-state
/// opt-in is a second place for the default to be wrong. Off is the default
/// everywhere: claiming this exposes the client's clipboard to every process on
/// the host, which is the user's call to make and not a build's.
enum class Serving : std::uint8_t {
    /// The client answers nothing, and does not claim the capability.
    Disabled = 0,
    /// The client claims the capability and undertakes to answer.
    ///
    /// The bit is a promise: a client that claims it and then cannot read its
    /// own clipboard leaves a program on the host waiting for a reply it will
    /// never produce, so a client that cannot answer must refuse rather than
    /// go quiet.
    Enabled = 1,
};

/// The facts a scope is resolved from.
///
/// A struct rather than three parameters: `scopeFor(true, false, true)` reads
/// as nothing at a call site, and two of the three transposed still compiles
/// and answers confidently. Named fields make a wrong call visible where it is
/// written, which for the decision that gates a clipboard is the point.
struct ScopeFacts {
    /// Whether the session's programs run on another machine.
    bool isRemoteSession = false;
    /// Whether the user turned the shims on: `[clipboard] remote_tools`.
    bool isServingEnabled = false;
    /// Whether the client claimed the serving capability on this connection.
    bool didClientClaimServe = false;
};

/// Resolves what a session's programs read.
///
/// All three facts must hold for the exposed answer. The third is not
/// redundant with the second: a client built before the capability existed, or
/// one whose claim the host refused, serves nothing however the setting reads
/// — and a badge promising otherwise would describe a session that cannot do
/// it.
///
/// - Parameter facts: What is known about the session.
/// - Returns: What to report.
[[nodiscard]] constexpr Scope scopeFor(ScopeFacts facts)
{
    if (!facts.isRemoteSession)
        return Scope::Local;
    return facts.isServingEnabled && facts.didClientClaimServe ? Scope::Client : Scope::Host;
}

/// Whether one clipboard request may be answered.
enum class ServeDecision : std::uint8_t {
    /// Read the clipboard and answer with what it holds.
    Answer = 0,
    /// Answer Refused without reading anything.
    Refuse = 1,
};

/// The facts a single request is decided from.
struct ServeFacts {
    /// Whether this connection's HELLO claimed the serving capability.
    bool didClaimServe = false;
    /// Whether the OSC 52 read policy is `deny`.
    bool isClipboardReadDenied = false;
};

/// Decides whether to answer one request.
///
/// Two gates rather than one, because the shims and OSC 52 are two routes to
/// the same secret. A program on the host reads a clipboard either by shelling
/// out to xclip or by writing an OSC 52 query, and it picks whichever works; a
/// route that consulted only its own switch would be the way around the other.
/// `clipboardRead = deny` is a person saying programs may not read their
/// clipboard, and it means that whichever syscall the program reached for.
///
/// `prompt` answers without asking, and it is the setting this chooses to read
/// differently rather than one it overlooks. It means
/// "ask me each time" for an escape sequence any program can emit unbidden;
/// turning the shims on is a person naming this one channel and opening it,
/// which is the asking, done once and on purpose. A prompt per `xclip` would
/// be answered by reflex within a day, which trains people past the prompt
/// that matters rather than protecting the clipboard.
///
/// - Parameter facts: What is known about the request.
/// - Returns: What to do with it.
[[nodiscard]] constexpr ServeDecision serveDecisionFor(ServeFacts facts)
{
    if (!facts.didClaimServe || facts.isClipboardReadDenied)
        return ServeDecision::Refuse;
    return ServeDecision::Answer;
}

} // namespace Ubiquitty::Clipboard
