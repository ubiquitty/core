// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The write side's chokepoint, as respond() is for replies.
//
// A reply leaves Core only through respond(), which takes a Capability, so no
// reply is written without one. An effect reaches a frontend only through a
// GatedClient: its pointer is private and the only functions that read it take
// a Capability, so an effect site that names no capability does not compile.
//
// Installation is a plain assignment, `terminal.windowClient = &myClient`;
// only the read is gated.

#include "Base/CheckedPtr.h"
#include "PAL/Capability.h"

#include <span>
#include <string_view>

namespace Ubiquitty {

struct Terminal;
class CapabilityPolicy;
/// Identifies the clipboard selection addressed by an OSC 52 operation.
enum class ClipboardBuffer : uint8_t;

/// Admits one complete OSC 52 write, or offers its exact decoded payload to
/// an installed policy that can hold it for consent. No effect runs on Prompt,
/// and with no policy installed PAL's default decides and only Allow writes.
void requestClipboardWrite(Terminal&, std::span<const ClipboardBuffer>, std::span<const char>);

template<typename Client> class GatedClient;

/// Returns the client only when `capability` is allowed for the current origin.
///
/// The decision comes from the terminal's `CapabilityPolicy` at its
/// `currentCapabilityOrigin`, or from PAL's `defaultDecision` for that origin
/// when no policy is installed. Anything but Allow yields null, including
/// Prompt: Core cannot block for an answer, so a parked request is a denial for
/// this pass and the frontend re-drives the sequence once a grant arrives.
///
/// - Parameters:
///   - terminal: The terminal whose policy and origin decide.
///   - gated: The client to unlock.
///   - capability: What the caller is about to do.
///   - detail: The request payload a policy may key on — a file path, an OSC 52
///     selection. Empty when the capability carries no argument.
/// - Returns: The client, or null when the effect must not happen.
template<typename Client>
[[nodiscard]] Client* effectClient(Terminal& terminal, GatedClient<Client>& gated, Capability capability, std::string_view detail = {});

/// Reports whether an effect that reaches no client may happen.
///
/// Some host escapes are not a client call — CSI 8 t resizes the model itself
/// when no frontend is wired, and that resize is the same escape whether a
/// window moves with it. Those ask here. It is the same question
/// `effectClient` answers, without a client to hand back.
///
/// A terminal with no policy installed is answered from PAL's `defaultDecision`
/// for its origin. Local is allowed everything, as every GUI frontend's policy
/// and the daemon's default allow it. Agent and Remote are refused host-escaping
/// and exfil effects and keep their benign, informational and rendering ones, so
/// a program waiting on a device-attributes reply does not hang. A Prompt is a
/// refusal here, because nothing is installed to ask.
///
/// - Parameters:
///   - terminal: The terminal whose policy and origin decide.
///   - capability: What the caller is about to do.
///   - detail: The request payload a policy may key on, empty when there is none.
/// - Returns: Whether the effect may happen.
[[nodiscard]] bool effectAllowed(Terminal& terminal, Capability capability, std::string_view detail = {});

/// Reports whether an effect may happen, refusing when no policy is installed.
///
/// The twin of `effectAllowed` for a gate that is the only gate on its path. An
/// unwired terminal falls back to PAL's defaults under `effectAllowed`, and they
/// allow every Local effect; a sole gate cannot afford that, because nothing
/// upstream has already refused. Kitty's `t=s` shared-memory read is the case —
/// Core's pre-gate answers PAL's Local Allow without a policy, so the frontend's
/// re-gate is authoritative and answers no rather than yes.
///
/// Two functions rather than a flag: which way an unwired terminal falls is a
/// property of the path, so a reviewer should learn it from the name rather than
/// by finding the argument.
///
/// - Parameters:
///   - terminal: The terminal whose policy and origin decide.
///   - capability: What the caller is about to do.
///   - detail: The request payload a policy may key on, empty when there is none.
/// - Returns: Whether a policy is installed and allows the effect.
[[nodiscard]] bool effectGranted(Terminal& terminal, Capability capability, std::string_view detail = {});

/// Returns the client without consulting the policy, for an effect that escapes
/// nothing.
///
/// A repaint request and a diagnostic line reveal no host state and change none:
/// gating them would spend a virtual call per frame to ask about a capability
/// whose tier is Benign. This is the escape hatch that admits that. Its name is
/// long and unlike anything else here so that `grep clientForEffectThatEscapesNothing`
/// returns every use and a reader can judge each one. A use of it for anything
/// that discloses clipboard or file data, acquires resources, changes the window,
/// or posts a notification is a bug. Revocation may release resources already
/// owned by the client; denying that cleanup would retain a revoked grant.
///
/// - Parameters:
///   - gated: The client to read.
/// - Returns: The client, or null when none is installed.
template<typename Client>
[[nodiscard]] Client* clientForEffectThatEscapesNothing(GatedClient<Client>& gated);

/// A frontend client that cannot be reached without naming a capability.
///
/// Holds a non-owning pointer; the frontend outlives the terminal and installs
/// itself once. The pointer is private so the two functions above are the only
/// way in, which is the whole mechanism: an effect site has no `.get()` to
/// call and must use one of them.
template<typename Client> class GatedClient {
public:
    /// Installs the client, or clears it with nullptr.
    ///
    /// Spelled as assignment, so a frontend or a fixture wires itself with a
    /// plain `=`; the gate is on the read, not the write.
    ///
    /// - Parameters:
    ///   - client: The client to install, or nullptr.
    /// - Returns: This, for chaining.
    GatedClient& operator=(Client* client)
    {
        m_client = client;
        return *this;
    }

    /// Reports whether a client is installed.
    ///
    /// A presence test is not a capability question, so it needs no policy: a
    /// caller uses it to skip work it would otherwise do before asking, and
    /// learning that nothing is wired reveals nothing about the host.
    ///
    /// - Returns: Whether a client is installed.
    [[nodiscard]] bool installed() const { return m_client.get() != nullptr; }

    /// Reports whether the installed client is this one.
    ///
    /// The caller already holds the client it is asking about — a frontend
    /// unwiring the pane it owns, a fixture checking what it installed — so the
    /// answer hands out nothing it did not bring, and no effect can be reached
    /// through a bool.
    ///
    /// - Parameters:
    ///   - candidate: The client to compare against, or nullptr to test for none.
    /// - Returns: Whether the installed client is that one.
    [[nodiscard]] bool isInstalled(const Client* candidate) const { return m_client.get() == candidate; }

private:
    Ubiquitty::CheckedPtr<Client> m_client;

    template<typename C>
    friend C* effectClient(Terminal&, GatedClient<C>&, Capability, std::string_view);
    template<typename C>
    friend C* clientForEffectThatEscapesNothing(GatedClient<C>&);
};

/// The capability policy, reachable only by the functions that ask it a question.
///
/// The same mechanism `GatedClient` is, applied to the decider rather than to
/// the deciders' subjects. The pointer is private, so an effect site cannot
/// read it and spell the check itself, where `!= Allow` could be `== Deny`, a
/// detail argument could be dropped, or an unset policy could be read the wrong
/// way round. It asks `effectAllowed`, or `effectGranted` when it is the only
/// gate on its path, and the two differ only in how an unset policy falls.
///
/// Installation is a plain assignment, `terminal.capabilityPolicy = &myPolicy`,
/// and the whole object is copyable so a reset can save and restore it without
/// ever holding the pointer.
class GatedPolicy {
public:
    /// Installs the policy, or clears it with nullptr.
    ///
    /// Spelled as assignment, as `GatedClient` is; the gate is on the read, not
    /// the write.
    ///
    /// - Parameters:
    ///   - policy: The policy to install, or nullptr.
    /// - Returns: This, for chaining.
    GatedPolicy& operator=(CapabilityPolicy* policy)
    {
        m_policy = policy;
        return *this;
    }

    /// Reports whether a policy is installed.
    ///
    /// A presence test is not a capability question: it reveals whether the
    /// frontend wired a gate, which the frontend already knows. Effects ask
    /// `effectAllowed` or `effectGranted` instead — a bool cannot carry a
    /// verdict, so nothing can be reached through this one.
    ///
    /// - Returns: Whether a policy is installed.
    [[nodiscard]] bool installed() const { return m_policy.get() != nullptr; }

    /// Reports whether the installed policy is this one.
    ///
    /// The caller already holds the policy it is asking about — a frontend
    /// unwiring what it owns, a fixture checking that a reset preserved what it
    /// installed — so the answer hands out nothing it did not bring, and no
    /// verdict can be reached through a bool.
    ///
    /// - Parameters:
    ///   - candidate: The policy to compare against, or nullptr to test for none.
    /// - Returns: Whether the installed policy is that one.
    [[nodiscard]] bool isInstalled(const CapabilityPolicy* candidate) const { return m_policy.get() == candidate; }

private:
    Ubiquitty::CheckedPtr<CapabilityPolicy> m_policy;

    friend bool effectAllowed(Terminal&, Capability, std::string_view);
    friend bool effectGranted(Terminal&, Capability, std::string_view);
    friend void requestClipboardWrite(Terminal&, std::span<const ClipboardBuffer>, std::span<const char>);
};

} // namespace Ubiquitty
