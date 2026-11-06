// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Sync's notifies on the control channel (Docs/sync.md §6.10, §7.3): what
// the host tells a client unasked, each a `NotifyKind` and that kind's record
// in a `Notify` frame on channel 0.
//
// `Attachments` lists the attachments that share a pane with the recipient:
// which exist, what each is, whether it is live, and which owns the size and
// is the primary. A client can then say why its picture is letterboxed or
// silent, and that typing here takes the size.
//
// The connection and the client attachment hold the list as values. The
// record's codec is here, with the two calls an owner makes each pass, as
// `serveCalls` is for calls: `serveNotifies` on the host's side and
// `readNotifies` on the client's.

#pragma once

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Ubiquitty::Protocol::Sync {

/// Encodes an attachments list as an `Attachments` `Notify` body:
///
///     revision:u64 entries:[Entry] (at most 1,024)
///     Entry    := attachmentID:u128 role:u8 clientKind:u8 state:u8
///                 geometryOwner:u8 primary:u8 self:u8 panes:[u128]
///                 hasPrincipal:u8 [principal:utf8] hasPresence:u8 [Presence]
///     Presence := clientID:u32 processID:u32 process:utf8 claimedLabel:utf8
///                 grantIdentity:utf8 quic:u8 sharedPanes:[(paneID:u128 grant:u8)]
///
/// The revision is nonzero; IDs are nonzero and unique within their list; at
/// most one entry is the recipient's own. The producer owns audience filtering:
/// a principal is for an Owner recipient only, and presence requires the
/// eligible human audience and names exactly the entry's panes.
///
/// - Parameter list: The list.
/// - Returns: The whole body, or nothing for invalid or oversized input.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> attachmentsNotify(const AttachmentsList& list);

/// Validates and measures a list without building it.
/// - Parameter list: The complete candidate, with its actual revision.
/// - Returns: Its exact encoded byte count, or nothing when invalid or oversized.
[[nodiscard]] std::optional<std::size_t> attachmentsNotifySize(const AttachmentsList& list);

/// Reads an `Attachments` record, validating every row before copying any.
///
/// - Parameter record: An `Attachments` `Notify`'s record.
/// - Returns: The list, or `nullopt` for a record that does not read.
[[nodiscard]] std::optional<AttachmentsList> decodeAttachments(std::span<const std::uint8_t> record);

/// Sends a connection's client the notifies it is owed: the attachments
/// list, when it changed. A host calls it for each connection on each pass,
/// before it services the connection.
///
/// - Parameter connection: The connection.
void serveNotifies(HostConnection& connection);

/// Reads the notifies a client attachment received: the ones it knows are
/// applied to the attachment, and the rest are the caller's. A client calls
/// it after each read.
///
/// - Parameter attachment: The client's attachment.
/// - Returns: The notifies it did not read, in arrival order.
[[nodiscard]] std::vector<Notify> readNotifies(ClientAttachment& attachment);

} // namespace Ubiquitty::Protocol::Sync
