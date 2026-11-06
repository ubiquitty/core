// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The daemon's CapabilityPolicy: makes the Core capability gates enforce
// server-side. Every grid-escaping side effect on a daemon session calls
// check(); the verdict keys on the driving principal's origin + identity,
// default-deny per the per-origin tier table. The daemon is the one place Agent
// and Remote principals reach Core, so the enforcing policy lives here.

#include "Core/Protocol/Sync/EffectCodec.h"
#include "PAL/Capability.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ubiquitty {

// The per-origin defaults (defaultDecision) and the configured table built from
// them (CapabilityDecisionTable) live in PAL/Capability.h beside tierOf, so the
// Preferences layer that resolves the configuration and this enforcing policy
// read one source.

/// Grant lifetime scope. OneShot: satisfies exactly one Allow-returning check, then
/// consumed. Session: until the granting session tears down. Always: persists
/// across daemon restarts — token-bound identities only (a non-token identity is
/// degraded to Session by the recording site).
enum class CapabilityGrantScope : std::uint8_t {
    OneShot,
    Session,
    Always
};

// A sticky grant identity: the exact (Capability, Origin, principal) a human
// authorized. Identity-keyed so a grant follows the principal across sessions but
// never satisfies a different principal's check.
struct CapabilityGrantKey {
    Capability capability;
    CapabilityOrigin origin;
    std::string identity;
    bool operator==(const CapabilityGrantKey&) const = default;
};

struct CapabilityGrantKeyHash {
    [[nodiscard]] std::size_t operator()(const CapabilityGrantKey& k) const
    {
        std::size_t h = std::hash<std::string> {}(k.identity);
        h = h * 31 + std::to_underlying(k.capability);
        h = h * 31 + std::to_underlying(k.origin);
        return h;
    }
};

// Per-grant record: the scope and (for Session) the session it is bound to.
struct GrantRecord {
    CapabilityGrantScope scope = CapabilityGrantScope::OneShot;
    std::uint32_t sessionID = 0; // meaningful for Session scope only
};

// Host-wide grant store (Daemon-owned). A granted triple short-circuits its
// default to Allow: Always follows the principal across restarts, Session is tied
// to its granting session, OneShot is consumed on first use.
class CapabilityGrantStore {
public:
    /// Where the store's Always grants are persisted, consulted before one authorizes.
    ///
    /// With a source installed, a cached Always grant authorizes only if it is
    /// still held after `confirm` returns. `confirm` brings every Always record
    /// up to date with the persisted grants through ``adoptAlwaysGrants``, and
    /// leaves none when they cannot be read whole. The source outlives the
    /// store's use; moving the store moves the source with it.
    struct StandingGrantSource {
        /// Handed back to `confirm` unchanged.
        void* context = nullptr;
        /// Brings the store's Always grants up to date; null for none.
        void (*confirm)(void* context, CapabilityGrantStore& store) = nullptr;
    };

    /// Installs where the Always grants are confirmed.
    ///
    /// - Parameter source: The persisted record, or an empty source for none.
    void setStandingGrantSource(StandingGrantSource source) { m_standingGrantSource = source; }
    /// The installed standing-grant source, for carrying across a replacement.
    ///
    /// - Returns: The source, empty when none is installed.
    [[nodiscard]] StandingGrantSource standingGrantSource() const { return m_standingGrantSource; }

    /// Brings the Always grants up to date with their persisted record, if any.
    void confirmStandingGrants()
    {
        if (m_standingGrantSource.confirm)
            m_standingGrantSource.confirm(m_standingGrantSource.context, *this);
    }

    /// Replaces every Always grant with `grants`.
    ///
    /// A Session or OneShot record keeps its key unless `grants` names that
    /// key, which then becomes Always.
    ///
    /// - Parameter grants: The complete Always set; an empty identity is skipped.
    void adoptAlwaysGrants(std::span<const CapabilityGrantKey> grants)
    {
        std::erase_if(m_grants, [](const auto& entry) { return entry.second.scope == CapabilityGrantScope::Always; });
        for (const auto& key : grants)
            if (!key.identity.empty())
                m_grants.insert_or_assign(key, GrantRecord { CapabilityGrantScope::Always, 0 });
    }

    void grant(Capability capability, CapabilityOrigin origin, std::string_view identity, CapabilityGrantScope scope, std::uint32_t sessionID = 0)
    {
        if (identity.empty())
            return; // an unverified principal (empty identity) never holds a grant
        m_grants.insert_or_assign(CapabilityGrantKey { capability, origin, std::string(identity) }, GrantRecord { scope, sessionID });
    }

    void revoke(Capability capability, CapabilityOrigin origin, std::string_view identity) { m_grants.erase({ capability, origin, std::string(identity) }); }

    // Drop every grant for a principal — the consent-revoke / StopAlways path.
    void revokeIdentity(std::string_view identity)
    {
        std::erase_if(m_grants, [&](const auto& entry) { return entry.first.identity == identity; });
    }

    // Drop one principal's Session-scoped grants on one session — the
    // revoke-session path, where the human withdrew this agent from this session
    // and not the persisted trust that outlives it. dropSession would take every
    // other agent's grants on the session with it.
    void revokeSessionGrantsFor(std::string_view identity, std::uint32_t sessionID)
    {
        if (identity.empty())
            return; // an empty key matches every unverified principal; never a selector
        std::erase_if(m_grants, [identity, sessionID](const auto& entry) {
            return entry.second.scope == CapabilityGrantScope::Session && entry.second.sessionID == sessionID && entry.first.identity == identity;
        });
    }

    // Drop all Session-scoped grants for a session — called on session teardown.
    void dropSession(std::uint32_t sessionID)
    {
        std::erase_if(m_grants, [sessionID](const auto& entry) { return entry.second.scope == CapabilityGrantScope::Session && entry.second.sessionID == sessionID; });
    }

    // Checks whether the triple holds a grant and, for OneShot, consumes it.
    // Returns true only when the grant authorizes an Allow return; the consume
    // happens only on that Allow path — never speculatively, never on Deny.
    // An Always grant is confirmed with the standing-grant source first, so a
    // withdrawal persisted elsewhere ends it at this check.
    [[nodiscard]] bool checkAndConsumeIfGranted(Capability capability, CapabilityOrigin origin, std::string_view identity)
    {
        if (identity.empty())
            return false;
        const CapabilityGrantKey key { capability, origin, std::string(identity) };
        auto it = m_grants.find(key);
        if (it == m_grants.end())
            return false;
        if (it->second.scope == CapabilityGrantScope::Always && m_standingGrantSource.confirm) {
            // Confirmation may replace every Always record, so the lookup repeats.
            confirmStandingGrants();
            it = m_grants.find(key);
            if (it == m_grants.end())
                return false;
        }
        if (it->second.scope == CapabilityGrantScope::OneShot)
            m_grants.erase(it); // consumed: the next check re-prompts
        return true;
    }

    // Pure query: does the triple hold a grant? Does not consume OneShot entries.
    // Use checkAndConsumeIfGranted on the Allow path; reserve this for tests
    // and out-of-band checks that must not affect grant state.
    [[nodiscard]] bool isGranted(Capability capability, CapabilityOrigin origin, std::string_view identity) const
    {
        if (identity.empty())
            return false;
        return m_grants.contains({ capability, origin, std::string(identity) });
    }

    // Count Session-scoped grants for `sessionID`. Used by tests to verify dropSession.
    [[nodiscard]] std::size_t sessionGrantCount(std::uint32_t sessionID) const
    {
        std::size_t count = 0;
        for (const auto& [key, record] : m_grants)
            if (record.scope == CapabilityGrantScope::Session && record.sessionID == sessionID)
                ++count;
        return count;
    }

    /// Returns an immutable snapshot for administration without consuming grants.
    [[nodiscard]] std::vector<std::pair<CapabilityGrantKey, GrantRecord>> records() const
    {
        return { m_grants.begin(), m_grants.end() };
    }

private:
    std::unordered_map<CapabilityGrantKey, GrantRecord, CapabilityGrantKeyHash> m_grants;
    StandingGrantSource m_standingGrantSource;
};

// Host-wide queue of capabilities awaiting a human ruling — populated when a
// check() resolves to Prompt with no standing grant. The daemon drains NEW entries
// to ask once per (capability, origin, identity); a repeat op re-enqueues nothing,
// so a flood is one prompt, not a storm. An empty (unverified) identity is never
// queued. Daemon-owned, outlives the per-session policies.
class CapabilityConsentQueue {
public:
    /// The pending set and ordered undrained notifications are distinct owners.
    /// Resolution does not remove a key from newlyPending; repeated keys there
    /// can arise from a resolution followed by another operation before draining.
    struct Continuation {
        std::vector<CapabilityGrantKey> pending;
        std::vector<CapabilityGrantKey> newlyPending;
    };

    /// Copies pending questions without consuming the notification queue.
    /// - Parameter maxEntries: Receiver-selected aggregate key ceiling.
    /// - Returns: Exact state or refusal before copying an oversized queue.
    [[nodiscard]] std::optional<Continuation> continuation(std::size_t maxEntries) const
    {
        if (m_pending.size() > maxEntries || m_newlyPending.size() > maxEntries - m_pending.size())
            return std::nullopt;
        return Continuation { { m_pending.begin(), m_pending.end() }, m_newlyPending };
    }

    /// Restores an unpublished empty queue without emitting a question.
    /// - Parameters:
    ///   - state: Owned exact keys; only successful import consumes them.
    ///   - maxEntries: Independent aggregate key ceiling.
    /// - Returns: Whether all keys and pending-set uniqueness were valid.
    [[nodiscard]] bool restoreContinuation(Continuation&& state, std::size_t maxEntries)
    {
        if (!m_pending.empty() || !m_newlyPending.empty() || state.pending.size() > maxEntries
            || state.newlyPending.size() > maxEntries - state.pending.size())
            return false;
        const auto isValid = [](const CapabilityGrantKey& key) {
            return key.capability <= Capability::None && key.origin <= CapabilityOrigin::Remote && !key.identity.empty();
        };
        std::unordered_set<CapabilityGrantKey, CapabilityGrantKeyHash> candidate;
        for (const auto& key : state.pending) {
            if (!isValid(key) || !candidate.insert(key).second)
                return false;
        }
        for (const auto& key : state.newlyPending) {
            if (!isValid(key))
                return false;
        }
        m_pending = std::move(candidate);
        m_newlyPending = std::move(state.newlyPending);
        state = {};
        return true;
    }

    // Record a capability as awaiting consent. Returns true only when it was not
    // already pending — a genuinely-new entry is also appended to the daemon's
    // drain list (takeNewlyPending) so it emits exactly one consent request.
    bool recordPending(Capability capability, CapabilityOrigin origin, std::string_view identity)
    {
        if (identity.empty())
            return false;
        auto [it, inserted] = m_pending.insert({ capability, origin, std::string(identity) });
        if (inserted)
            m_newlyPending.push_back(*it);
        return inserted;
    }

    // Drain the entries recorded since the last call and emit one consent question
    // each. Draining does not resolve them — they stay pending until the human
    // rules, so the chokepoint keeps denying and no duplicate request is emitted.
    [[nodiscard]] std::vector<CapabilityGrantKey> takeNewlyPending() { return std::move(m_newlyPending); }

    // Drop a pending entry once the human has ruled (granted or denied).
    void resolve(Capability capability, CapabilityOrigin origin, std::string_view identity) { m_pending.erase({ capability, origin, std::string(identity) }); }

    // Clear every pending entry for a principal (consent-revoke / disconnect).
    void resolveIdentity(std::string_view identity)
    {
        std::erase_if(m_pending, [&](const CapabilityGrantKey& k) { return k.identity == identity; });
    }

    [[nodiscard]] bool isPending(Capability capability, CapabilityOrigin origin, std::string_view identity) const
    {
        if (identity.empty())
            return false;
        return m_pending.contains({ capability, origin, std::string(identity) });
    }

private:
    std::unordered_set<CapabilityGrantKey, CapabilityGrantKeyHash> m_pending;
    std::vector<CapabilityGrantKey> m_newlyPending; // recorded-since-last-drain
};

// The per-session policy installed on a daemon Terminal. Holds the current driving
// identity (stamped per Input frame) and consults the host-wide grant store, so
// check(capability, origin, detail) resolves (capability, origin, identity) even
// though Core's interface carries no identity argument. The decision table is
// the configuration's verdicts, fixed at construction: a session keeps the
// policy it was created under.
class HostCapabilityPolicy : public CapabilityPolicy {
public:
    /// One immutable OSC 52 operation. Selectors are fixed-size; the single
    /// text/plain payload shares the existing wire byte ceiling.
    struct PendingClipboardWrite {
        using Allocator = std::unique_ptr<char[]> (*)(std::size_t);
        std::unique_ptr<char[]> bytes;
        std::size_t byteCount = 0;
        std::array<ClipboardBuffer, static_cast<std::size_t>(ClipboardBuffer::Count)> buffers {};
        std::size_t bufferCount = 0;
        CapabilityOrigin origin = CapabilityOrigin::Local;
        std::string identity;
        std::uint32_t requestID = 0;

        static std::unique_ptr<char[]> allocate(std::size_t size) { return std::unique_ptr<char[]>(new (std::nothrow) char[size]); }
        [[nodiscard]] static std::optional<PendingClipboardWrite> capture(CapabilityOrigin origin, std::string_view identity,
            std::span<const ClipboardBuffer> buffers, std::span<const char> bytes, Allocator allocator = allocate)
        {
            if (identity.empty() || buffers.empty() || buffers.size() > static_cast<std::size_t>(ClipboardBuffer::Count)
                || bytes.size() > Protocol::Sync::maxEffectClipboardWriteBytes)
                return std::nullopt;
            PendingClipboardWrite result;
            for (ClipboardBuffer buffer : buffers) {
                if (buffer >= ClipboardBuffer::Count)
                    return std::nullopt;
                result.buffers[result.bufferCount++] = buffer;
            }
            if (!bytes.empty()) {
                result.bytes = allocator(bytes.size());
                if (!result.bytes)
                    return std::nullopt;
                std::memcpy(result.bytes.get(), bytes.data(), bytes.size());
            }
            result.byteCount = bytes.size();
            result.origin = origin;
            result.identity = identity;
            return result;
        }
    };

    explicit HostCapabilityPolicy(const CapabilityDecisionTable& decisions = {})
        : m_decisions(decisions)
    {
    }

    /// Owns the portable policy state; host-wide grants and callbacks are rewired.
    struct Continuation {
        CapabilityDecisionTable decisions;
        std::string currentIdentity;
        std::optional<PendingClipboardWrite> pendingClipboardWrite;
        bool wasClipboardWriteRefused = false;
    };

    /// Borrows the current input principal without consuming pending consent.
    [[nodiscard]] std::string_view currentIdentity() const { return m_currentIdentity; }
    /// Borrows the exact held operation while this policy remains unchanged.
    [[nodiscard]] const PendingClipboardWrite* pendingClipboardWrite() const { return m_pendingClipboardWrite ? &*m_pendingClipboardWrite : nullptr; }
    /// Reads, but does not consume, the refused-capture indication.
    [[nodiscard]] bool hasClipboardWriteRefusal() const { return m_wasClipboardWriteRefused; }

    /// Installs an already admitted continuation into an unused policy.
    ///
    /// The immutable decision table must match the one used at construction.
    /// No grant is minted, checked or consumed, and no consent is reissued.
    /// - Parameter state: State decoded by the host handover codec.
    /// - Returns: Whether ownership moved; refusal leaves both sides unchanged.
    [[nodiscard]] bool restoreContinuation(Continuation&& state)
    {
        if (m_decisions != state.decisions || !m_currentIdentity.empty() || m_pendingClipboardWrite || m_wasClipboardWriteRefused)
            return false;
        m_currentIdentity = std::move(state.currentIdentity);
        m_pendingClipboardWrite = std::move(state.pendingClipboardWrite);
        m_wasClipboardWriteRefused = state.wasClipboardWriteRefused;
        return true;
    }

    void setGrantStore(CapabilityGrantStore* store) { m_store = store; }
    void setConsentQueue(CapabilityConsentQueue* queue) { m_queue = queue; }
    void setCurrentIdentity(std::string identity) { m_currentIdentity = std::move(identity); }
    /// Who asks the person about an effect the session delivers to them.
    enum class EffectQuestion : std::uint8_t {
        /// This host asks before it delivers.
        AskedByHost,
        /// The recipient asks on its own device, so this host forwards.
        AskedByRecipient,
    };
    /// Says who asks about the effects the session delivers to its recipient.
    ///
    /// While the recipient asks, a clipboard write or notification from the
    /// Remote origin that this host would ask about is forwarded without a
    /// question: the recipient judges it by its own Remote verdict, so the
    /// person is asked once, on the device the effect reaches. One from any
    /// other origin keeps this host's question, since only this host knows
    /// that origin's rule, an agent's grants being good for one use, and a
    /// configured Deny still refuses here.
    ///
    /// - Parameter question: Who asks.
    void setEffectQuestion(EffectQuestion question)
    {
        m_effectQuestion = question;
        m_didForwardNotification = false;
    }
    /// Whether the notification this policy last let through was forwarded on
    /// its recipient's own question rather than admitted here, consumed by
    /// the call.
    ///
    /// Asked as the notification is held, right after its check: a banner
    /// nobody here was asked about is not this host's to record for other
    /// devices.
    ///
    /// - Returns: Whether it was forwarded.
    [[nodiscard]] bool takeForwardedNotification() { return std::exchange(m_didForwardNotification, false); }

    /// Holds the first bounded clipboard write for an exact-operation consent question.
    ///
    /// - Parameters:
    ///   - origin: The capability origin that requested the write.
    ///   - buffers: The clipboard selections to retain with the operation.
    ///   - bytes: The complete decoded payload copied into bounded storage.
    void holdClipboardWrite(CapabilityOrigin origin, std::span<const ClipboardBuffer> buffers, std::span<const char> bytes) override
    {
        // The question identifies the first write, never a replacement that
        // arrived while the person was deciding.
        if (!m_pendingClipboardWrite) {
            m_pendingClipboardWrite = PendingClipboardWrite::capture(origin, m_currentIdentity, buffers, bytes);
            m_wasClipboardWriteRefused = !m_pendingClipboardWrite;
        }
    }
    [[nodiscard]] bool takeClipboardWriteRefusal() { return std::exchange(m_wasClipboardWriteRefused, false); }
    [[nodiscard]] PendingClipboardWrite* pendingClipboardWrite() { return m_pendingClipboardWrite ? &*m_pendingClipboardWrite : nullptr; }
    /// Releases the held operation only when its consent request matches.
    ///
    /// - Parameter requestID: The exact request to discard.
    void discardClipboardWrite(std::uint32_t requestID)
    {
        if (m_pendingClipboardWrite && m_pendingClipboardWrite->requestID == requestID)
            m_pendingClipboardWrite.reset();
    }
    [[nodiscard]] std::optional<PendingClipboardWrite> takeClipboardWrite(std::uint32_t requestID)
    {
        if (!m_pendingClipboardWrite || m_pendingClipboardWrite->requestID != requestID)
            return std::nullopt;
        return std::exchange(m_pendingClipboardWrite, std::nullopt);
    }

    [[nodiscard]] CapabilityDecision check(Capability capability, CapabilityOrigin origin, std::string_view /*detail*/) override
    {
        return checkFor(capability, origin, m_currentIdentity);
    }

    /// Rechecks an exact held operation without changing the live input owner.
    [[nodiscard]] CapabilityDecision checkFor(Capability capability, CapabilityOrigin origin, std::string_view identity)
    {
        const CapabilityDecision decision = m_decisions.decision(origin, capability);
        // Deny is hard: the grant store is not consulted, so no grant outranks
        // it — not one persisted under a laxer configuration, not a session one.
        // No consent path produces a grant for a Deny cell (a grant is recorded
        // only from a request a Prompt queued), so the only grants this refuses
        // are ones the owner's written restriction has since withdrawn, or ones
        // a tampered grant file planted.
        if (decision == CapabilityDecision::Deny)
            return CapabilityDecision::Deny;
        // Each notification's check says whether it was forwarded, so a stale
        // answer never outlives the next one.
        if (capability == Capability::DesktopNotification)
            m_didForwardNotification = false;
        // A grant a person gave here admits the effect here: it is this host's
        // own, and recorded as such.
        if (m_store && m_store->checkAndConsumeIfGranted(capability, origin, identity))
            return CapabilityDecision::Allow;
        // The recipient asks its own person about an effect it is delivered,
        // so a question here would be a second one about the same effect.
        if (decision == CapabilityDecision::Prompt && m_effectQuestion == EffectQuestion::AskedByRecipient && origin == CapabilityOrigin::Remote
            && isDeliveredToRecipient(capability)) {
            if (capability == Capability::DesktopNotification)
                m_didForwardNotification = true;
            return CapabilityDecision::Allow;
        }
        // A Prompt with no standing grant: queue it (deduped) so the daemon asks
        // once. check() still RETURNS Prompt (fail-closed now); a later grant flips
        // future checks to Allow. Never blocks, never auto-grants.
        if (decision == CapabilityDecision::Prompt && m_queue)
            m_queue->recordPending(capability, origin, identity);
        return decision;
    }

    [[nodiscard]] const CapabilityDecisionTable& decisions() const { return m_decisions; }

private:
    /// Whether an effect is one the session's recipient applies on its own
    /// device, bound to that recipient in the pass that made it: a write to
    /// its clipboard or a banner.
    ///
    /// A sound is not: it is emitted after its frame, by which time someone
    /// else may be the recipient, so the host keeps its own question for one.
    ///
    /// - Parameter capability: The effect.
    /// - Returns: Whether it is delivered to the recipient.
    [[nodiscard]] static constexpr bool isDeliveredToRecipient(Capability capability)
    {
        return capability == Capability::ClipboardWrite || capability == Capability::DesktopNotification;
    }

    const CapabilityDecisionTable m_decisions; // the configured verdicts, immutable for the session's life
    CapabilityGrantStore* m_store = nullptr; // host-wide, Daemon-owned, outlives the Terminal
    CapabilityConsentQueue* m_queue = nullptr; // host-wide, Daemon-owned; Prompt sink
    std::string m_currentIdentity; // the session's current driving principal
    std::optional<PendingClipboardWrite> m_pendingClipboardWrite;
    bool m_wasClipboardWriteRefused = false;
    EffectQuestion m_effectQuestion = EffectQuestion::AskedByHost;
    bool m_didForwardNotification = false;
};

} // namespace Ubiquitty
