// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/FallibleVector.h"
#include "Core/DragDrop/Protocol.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty::DragDrop {

/// Native operations contain decoded data and never terminal escape sequences.
enum class CommandKind : std::uint8_t {
    Registration,
    AcceptDrop,
    ReadDrop,
    CloseDirectory,
    FinishDrop,
    PrepareDrag,
    PrepareData,
    BeginDrag,
    SetDragImage,
    DragData,
    RemoteFileData,
    CancelDrag,
};

/// Commands borrow payloads only for the duration of Channel::sendCommand.
struct Command {
    CommandKind kind {};
    std::uint64_t sessionID = 0;
    std::uint64_t requestID = 0;
    Metadata metadata {};
    std::string_view payload {};
    bool isFinal = false;
};

/// The native owner supplies events after authenticating a human driver.
/// Session IDs increase within that owner's lifetime; changing owners revokes
/// the session before any new owner can deliver events.
enum class EventKind : std::uint8_t {
    Enter,
    Move,
    Leave,
    Drop,
    BeginGesture,
    EndGesture,
    Started,
    Failed,
    Accepted,
    OperationChanged,
    Dropped,
    Finished,
    RequestData,
    RequestFile,
    ReadData,
    ReadFailed,
    Cancel,
};

/// Native data completions carry both the drag lifetime and read identity.
struct Event {
    EventKind kind {};
    std::uint64_t sessionID = 0;
    std::uint64_t requestID = 0;
    Metadata metadata {};
    std::string_view payload {};
    Error error = Error::Unknown;
    bool isFinal = false;
    bool isSameWindow = false;
};

/// Native event fields suitable for interop; payload storage stays with the caller.
struct NativeEvent {
    EventKind kind {};
    std::uint64_t sessionID = 0;
    std::uint64_t requestID = 0;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t pixelX = 0;
    std::int64_t pixelY = 0;
    std::uint32_t operation = 0;
    Error error = Error::Unknown;
    bool isFinal = false;
    bool isSameWindow = false;
};

/// A synchronous copying seam. Implementations must not reenter Session.
class Channel {
public:
    virtual ~Channel() = default;
    /// Queues a complete framed reply, preserving its position among replies.
    [[nodiscard]] virtual bool writeReply(std::span<const char>) = 0;
    /// Copies a bounded command for the native owner or its authenticated carrier.
    [[nodiscard]] virtual bool sendCommand(const Command&) = 0;
};

/// Stable negotiation state. Native grants, handles and outstanding reads are excluded.
struct RegistrationState {
    std::string_view acceptedMIMEs {};
    std::string_view dropMachine {};
    std::string_view offerMachine {};
    std::optional<std::uint32_t> dropIdentity;
    std::optional<std::uint32_t> offerIdentity;
    std::uint64_t lastNativeSession = 0;
    bool acceptsDrops = false;
    bool isOfferingDrags = false;
};

/// Owns OSC 72 negotiation and the authority granted by native input events.
/// Filesystem handles and operating-system drag objects remain with the client.
class Session {
public:
    /// Consumes one OSC 72 body, excluding the numeric selector and terminator.
    void receive(std::string_view, Channel&);
    /// Applies an event from the current authenticated native owner.
    void receive(const Event&, Channel&);
    /// Revokes all transient authority and clears registrations.
    void reset(Channel&);
    /// Reports whether the program requests native incoming-drop events.
    [[nodiscard]] bool acceptsDrops() const { return m_acceptsDrops; }
    /// Reports whether the program requests native drag-start gestures.
    [[nodiscard]] bool offersDrags() const { return m_isOfferingDrags; }
    [[nodiscard]] bool hasPendingWork() const { return m_chain || m_dropPhase != DropPhase::None || m_offerPhase != OfferPhase::None; }
    [[nodiscard]] RegistrationState registrationState() const;
    /// Restores validated registration into a fresh session before publication.
    [[nodiscard]] std::expected<void, Error> restoreRegistration(const RegistrationState&);
    /// Replays registration to a newly bound native owner, without granting a drag.
    /// - Parameter channel: The new owner's command route.
    void publishRegistration(Channel& channel) { sendRegistration(channel); }
    /// Revokes native authority when the authenticated attachment changes.
    void rebindNativeOwner(Channel&);

private:
    /// Tracks whether a native drop authorizes data reads.
    enum class DropPhase : std::uint8_t {
        None,
        Hover,
        Dropped
    };
    /// Separates gesture authority, preparation and a running native drag.
    enum class OfferPhase : std::uint8_t {
        None,
        Gesture,
        Preparing,
        Starting,
        Running
    };

    /// Correlates a native completion with its MIME or directory request.
    struct PendingRead {
        std::uint64_t identity = 0;
        Metadata metadata {};
        bool isURIList = false;
    };
    /// Retains first-packet metadata and the total decoded byte count.
    struct PreparedItem {
        std::size_t bytes = 0;
        Metadata metadata {};
        bool hasMetadata = false;
        bool isComplete = false;
    };

    /// Applies a complete control message after chunk assembly.
    void dispatch(const Metadata&, std::string_view, Channel&);
    /// Decodes a bounded binary chunk under its first-packet metadata.
    void data(const Metadata&, std::string_view, Channel&);
    /// Authorizes a read against the active drop and its directory handles.
    void requestDropData(const Metadata&, Channel&);
    /// Revokes read grants and tells the native owner to release drop resources.
    /// - Parameters:
    ///   - channel: The current reply and native-command route.
    ///   - operation: The completed action, or zero for cancellation.
    void endDrop(Channel&, std::uint32_t operation = 0);
    /// Cancels the native offer and discards pending data and images.
    void endOffer(Channel&);
    /// Reports an outgoing error before revoking the offer.
    void failOffer(Error, Channel&);
    /// Reports a read error and terminates an active drop.
    void failRead(const Metadata&, Error, Channel&);
    /// Publishes incoming and outgoing registration without granting a gesture.
    void sendRegistration(Channel&);
    [[nodiscard]] bool reply(Metadata, std::string_view, Channel&);
    [[nodiscard]] bool send(CommandKind, std::uint64_t, const Metadata&, std::string_view, Channel&);
    [[nodiscard]] std::expected<std::span<char>, Error> decode(std::string_view, std::span<char>, const Metadata&);
    /// Discards incomplete metadata, text and base64 state.
    void clearChain();

    bool m_acceptsDrops = false;
    bool m_isOfferingDrags = false;
    FallibleVector<char> m_acceptedMIMEs;
    FallibleVector<char> m_dropMIMEs;
    FallibleVector<char> m_offerMIMEs;
    FallibleVector<char> m_dropMachine;
    FallibleVector<char> m_offerMachine;
    std::optional<std::uint32_t> m_dropIdentity;
    std::optional<std::uint32_t> m_offerIdentity;
    DropPhase m_dropPhase = DropPhase::None;
    OfferPhase m_offerPhase = OfferPhase::None;
    std::uint64_t m_lastNativeSession = 0;
    std::uint64_t m_dropSession = 0;
    std::uint64_t m_offerSession = 0;
    std::uint64_t m_nextRead = 0;
    std::uint32_t m_allowedDropOperations = 0;
    std::uint32_t m_selectedDropOperation = 0;
    std::uint32_t m_offerOperations = 0;
    bool m_isSameWindow = false;
    bool m_hasReadURIs = false;
    std::array<PendingRead, maxPendingReads> m_reads {};
    std::array<std::optional<std::int64_t>, maxDirectoryHandles> m_directories {};
    std::array<PreparedItem, maxMIMETypes> m_prepared {};
    std::array<PreparedItem, maxImages> m_images {};
    std::size_t m_imageCount = 0;
    std::size_t m_preparedBytes = 0;
    std::size_t m_imageBytes = 0;
    std::array<bool, maxMIMETypes> m_requestedMIMEs {};
    std::array<std::optional<std::int64_t>, maxRemoteRoots> m_requestedFiles {};
    std::optional<Metadata> m_chain;
    FallibleVector<char> m_chainText;
    std::array<char, 4> m_base64Tail {};
    std::size_t m_base64TailSize = 0;
};

} // namespace Ubiquitty::DragDrop
