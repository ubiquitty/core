// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Tests read a `Notify` as one kind: the kind's typed value when the notify is
// of that kind and its record reads, nothing otherwise. A frame body read off
// the wire goes through `notifyOf` first, as a client's connection reads it.

#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/StructureConsent.h"

#include <optional>
#include <span>
#include <vector>

namespace UbiquittyTest {

/// Reads a frame body as a client keeps it: a known kind and its record.
///
/// - Parameter body: A `Notify` frame's body.
/// - Returns: The notify, or nothing for a body with no canonical kind or a
///   kind this build does not know.
inline std::optional<Ubiquitty::Protocol::Sync::Notify> notifyOf(std::span<const std::uint8_t> body)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    const auto head = Sync::decodeNotifyHead(body);
    const auto kind = head ? Sync::notifyKindOf(head->kind) : std::nullopt;
    if (!kind)
        return std::nullopt;
    return Sync::Notify { .kind = *kind, .record = { head->record.begin(), head->record.end() } };
}

/// Reads a notify as the own consent status a `ConsentStatus` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<Ubiquitty::Protocol::Sync::ConsentStatus> consentStatusOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::ConsentStatus ? Sync::decodeConsentStatus(notify.record) : std::nullopt;
}

/// Reads a notify as the attach, terminate or capability question a `ConsentQuestion` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<Ubiquitty::Protocol::Sync::ConsentQuestion> consentQuestionOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::ConsentQuestion ? Sync::decodeConsentQuestion(notify.record) : std::nullopt;
}

/// Reads a notify as the structure question a `StructureConsent` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<Ubiquitty::Protocol::Sync::StructureConsent> structureConsentOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::StructureConsent ? Sync::decodeStructureConsent(notify.record) : std::nullopt;
}

/// Reads a notify as the retired question a `ConsentResolved` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<Ubiquitty::Protocol::Sync::ConsentResolution> consentResolvedOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::ConsentResolved ? Sync::decodeConsentResolved(notify.record) : std::nullopt;
}

/// Reads a notify as the attachments list a `Attachments` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<Ubiquitty::Protocol::Sync::AttachmentsList> attachmentsOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::Attachments ? Sync::decodeAttachments(notify.record) : std::nullopt;
}

/// Reads a notify as the event batch a `Events` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<std::vector<Ubiquitty::Session::Event>> eventsOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::Events ? Ubiquitty::Protocol::Control::decodeEvents(notify.record) : std::nullopt;
}

/// Reads a notify as the paste credit a `PasteReady` record carries.
///
/// - Parameter notify: A notify the client kept.
/// - Returns: The value, or nothing when the notify is another kind or its
///   record does not read.
inline std::optional<Ubiquitty::Protocol::Control::PasteReady> pasteReadyOf(const Ubiquitty::Protocol::Sync::Notify& notify)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    return notify.kind == Sync::NotifyKind::PasteReady ? Ubiquitty::Protocol::Control::decodePasteReady(notify.record) : std::nullopt;
}

} // namespace UbiquittyTest
