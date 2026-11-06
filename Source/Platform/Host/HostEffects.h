// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// What a host session's program does to its person's machine, held until the
// daemon delivers it. The host has no pasteboard and no notification centre:
// an OSC 52 write or an OSC 9 banner the capability gate admitted is the
// client's to perform, and only one client's — the session's primary human
// (Docs/sync.md §6.11). The session Terminal's clipboard and shell clients
// are the two halves below; they record and the daemon drains.

#include "Core/OSC/KittyNotification.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/Effects.h"
#include "Core/Session/BackgroundEvents.h"
#include "PAL/TerminalClients.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Ubiquitty::Host {

/// Owns a program notification until live delivery and durable retention.
struct HostNotification {
    std::string title;
    std::string body;
    std::string identifier;
    NotificationOccasion occasion = NotificationOccasion::Always;
    /// Chosen once on first live delivery, never retargeted after a refusal.
    std::optional<Protocol::Sync::ID128> primary;
    /// Pins a selected transport until its inner handshake identifies the primary.
    std::uint32_t pendingPrimaryClientID = 0;
};

/// The effects a session's program made since the daemon last delivered.
///
/// Bounded by count and, for a clipboard write, by size: a program looping on
/// OSC 52 holds at most `capacity` writes of at most
/// `Protocol::Sync::maxEffectClipboardWriteBytes` each, and the oldest gives way.
class HostEffectQueue {
public:
    /// Owns a complete admitted write and its immutable delivery recipient.
    struct ClipboardWrite {
        ClipboardBuffer buffer = ClipboardBuffer::Clipboard;
        Protocol::Sync::EffectCodecBytes encoded;
        std::optional<Protocol::Sync::ID128> primary;
        /// A pre-handshake write cannot move to a different connected person.
        std::uint32_t pendingPrimaryClientID = 0;
    };
    using Effect = std::variant<ClipboardWrite, HostNotification>;

    /// The most effects held between deliveries.
    static constexpr std::size_t capacity = 8;

    /// Records admitted notifications before the live delivery bound can drop them.
    ///
    /// - Parameter recorder: Borrows the notification during the call and must
    ///   not reenter this queue. Empty removes the recorder.
    void setNotificationRecorder(std::function<void(const HostNotification&)> recorder)
    {
        m_notificationRecorder = std::move(recorder);
    }

    /// Holds one effect, dropping the oldest when the queue is full.
    ///
    /// - Parameter effect: The clipboard write or notification to hold.
    void push(Effect effect)
    {
        if (const auto* notification = std::get_if<HostNotification>(&effect); notification && m_notificationRecorder)
            m_notificationRecorder(*notification);
        returnUndelivered(std::move(effect));
    }

    /// Returns an already recorded effect after live delivery was deferred.
    ///
    /// - Parameter effect: The same owner returned by take(), not a new event.
    void returnUndelivered(Effect effect)
    {
        if (m_effects.size() == capacity)
            m_effects.erase(m_effects.begin());
        m_effects.push_back(std::move(effect));
    }

    /// Removes and returns every held effect.
    ///
    /// - Returns: The effects, oldest first.
    [[nodiscard]] std::vector<Effect> take() { return std::exchange(m_effects, {}); }

    /// Whether nothing is held.
    [[nodiscard]] bool isEmpty() const { return m_effects.empty(); }

    /// Borrows retained delivery owners without notifying or consuming them.
    [[nodiscard]] std::span<const Effect> heldEffects() const { return m_effects; }

    /// Installs admitted handover owners without recording notifications again.
    /// - Parameter effects: Bounded owners decoded by the host handover codec.
    /// - Returns: Whether ownership moved into an empty queue.
    [[nodiscard]] bool restoreHeldEffects(std::vector<Effect>&& effects)
    {
        if (!m_effects.empty() || effects.size() > capacity)
            return false;
        m_effects = std::move(effects);
        return true;
    }

private:
    std::function<void(const HostNotification&)> m_notificationRecorder;
    std::vector<Effect> m_effects;
};

/// The session Terminal's clipboard: a write becomes a held CLIPBOARD_WRITE.
///
/// Reads answer empty, because the host has no pasteboard of its own; a read
/// the client should answer goes through CLIPBOARD_REQUEST instead.
class HostClipboardClient final : public ClipboardClient {
public:
    /// Creates a clipboard that holds its writes in `queue`.
    ///
    /// - Parameter queue: The session's effect queue, which outlives this.
    explicit HostClipboardClient(HostEffectQueue& queue)
        : m_queue(queue)
    {
    }

    /// Holds a typed write, whole or not at all.
    ///
    /// - Parameters:
    ///   - buffer: The OSC 52 buffer written.
    ///   - items: The payloads, one per MIME; empty clears the buffer.
    void writeClipboardItems(ClipboardBuffer buffer, std::span<const ClipboardItem> items) override
    {
        if (buffer >= ClipboardBuffer::Count || items.size() > Protocol::Sync::maxEffectClipboardItems)
            return;
        Protocol::Sync::EffectClipboardBundle borrowed;
        borrowed.count = items.size();
        for (std::size_t index = 0; index < items.size(); ++index) {
            const auto& item = items[index];
            borrowed.items[index] = { item.mime, { reinterpret_cast<const std::uint8_t*>(item.bytes.data()), item.bytes.size() } };
        }
        // Validate count, names, duplicates and aggregate bytes before any
        // pending owner copies input-sized data.
        auto encoded = Protocol::Sync::encodeEffectClipboard(borrowed, Protocol::Sync::maxEffectClipboardWriteBytes);
        if (!encoded)
            return;
        m_queue.push(HostEffectQueue::ClipboardWrite { .buffer = buffer, .encoded = std::move(*encoded), .primary = std::nullopt });
    }

    /// Holds an untyped write as one text/plain item.
    ///
    /// - Parameters:
    ///   - buffer: The OSC 52 buffer written.
    ///   - bytes: The text; empty clears the buffer.
    void writeClipboard(ClipboardBuffer buffer, std::span<const char> bytes) override
    {
        const ClipboardItem item { "text/plain", bytes };
        writeClipboardItems(buffer, bytes.empty() ? std::span<const ClipboardItem> {} : std::span<const ClipboardItem>(&item, 1));
    }

private:
    HostEffectQueue& m_queue;
};

/// The session Terminal's shell client: a notification becomes a held
/// NOTIFICATION, as the program wrote it.
class HostShellClient final : public ShellClient {
public:
    /// Creates a shell client that holds its notifications in `queue`.
    ///
    /// - Parameter queue: The session's effect queue, which outlives this.
    explicit HostShellClient(HostEffectQueue& queue)
        : m_queue(queue)
    {
    }

    /// Holds an unnamed notification with its default occasion.
    ///
    /// - Parameters:
    ///   - title: The title as parsed, possibly empty.
    ///   - body: The body as parsed, possibly empty.
    void postNotification(std::string_view title, std::string_view body) override
    {
        postNotificationRequest({ .title = title, .body = body, .identifier = {} });
    }

    /// Retains replacement and timing metadata independently of a live client.
    ///
    /// - Parameter request: Borrowed program text, copied within fixed bounds.
    void postNotificationRequest(const NotificationRequest& request) override
    {
        m_queue.push(HostNotification {
            Session::backgroundEventText(request.title, Session::BackgroundEvents::titleCapacity),
            Session::backgroundEventText(request.body, Session::BackgroundEvents::bodyCapacity),
            Session::backgroundEventText(request.identifier, OSC::kittyNotificationIdentifierCapacity),
            request.occasion, std::nullopt });
    }

private:
    HostEffectQueue& m_queue;
};

/// The session Terminal's effect route: counts the bells its program rang
/// until the daemon relays them to the session's clients.
///
/// One per session, so a bell is relayed only for the pane that rang it, and
/// as many bells as were rung: the effect log coalesces them into one
/// `Bell{count}` for a client that skipped the span (Docs/sync.md §6.11).
class HostEffectRoute final : public EffectRoute {
public:
    /// Counts one bell, saturating rather than wrapping.
    void ringBell() override
    {
        if (m_bells != UINT32_MAX)
            ++m_bells;
    }

    /// The bells rung since they were last taken.
    [[nodiscard]] std::uint32_t pendingBells() const { return m_bells; }

    /// Forgets the bells once the daemon has relayed or recorded them.
    void clearBells() { m_bells = 0; }

private:
    std::uint32_t m_bells = 0;
};

} // namespace Ubiquitty::Host
