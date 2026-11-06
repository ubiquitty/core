// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Notify.h"

#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

constexpr std::size_t maxAttachmentEntries = 1'024;
constexpr std::size_t maxProcessBytes = UINT16_MAX;
constexpr std::size_t maxClaimedLabelBytes = 256;
constexpr std::size_t idBytes = 16;

bool textIsValid(std::string_view text, std::size_t maximum)
{
    return text.size() <= maximum && text.find('\0') == std::string_view::npos
        && isValidUTF8({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
}

bool panesAreValid(std::span<const ID128> panes, bool isSelf)
{
    if (panes.size() > maxPanesPerConnection || (panes.empty() && !isSelf))
        return false;
    for (std::size_t index = 0; index < panes.size(); ++index) {
        if (panes[index] == ID128 {} || std::find(panes.begin(), panes.begin() + index, panes[index]) != panes.begin() + index)
            return false;
    }
    return true;
}

bool entryIsValid(const AttachmentEntry& entry)
{
    if (entry.attachmentID == ID128 {} || entry.role > AttachmentRole::Driver || entry.clientKind > ClientKind::Viewer
        || entry.state > AttachmentState::Lost || !panesAreValid(entry.panes, entry.isSelf)
        || (entry.principal && !textIsValid(*entry.principal, maxFrameBodyBytes)))
        return false;
    if (!entry.presence)
        return true;
    const auto& presence = *entry.presence;
    if (entry.state != AttachmentState::Live || !presence.clientID
        || (!presence.processID && !presence.process.empty())
        || !textIsValid(presence.process, maxProcessBytes) || !textIsValid(presence.claimedLabel, maxClaimedLabelBytes)
        || !textIsValid(presence.grantIdentity, maxConsentIdentityBytes)
        || presence.sharedPanes.size() != entry.panes.size())
        return false;
    for (const auto& shared : presence.sharedPanes) {
        if (shared.grant > PresenceGrant::Always || std::ranges::find(entry.panes, shared.paneID) == entry.panes.end())
            return false;
    }
    for (const auto pane : entry.panes) {
        if (std::ranges::count(presence.sharedPanes, pane, &SharedPaneGrant::paneID) != 1)
            return false;
    }
    return true;
}

std::size_t entryBytes(const AttachmentEntry& entry)
{
    std::size_t bytes = idBytes + 6 + varintBytes(entry.panes.size()) + entry.panes.size() * idBytes + 1 + 1;
    if (entry.principal)
        bytes += utf8Bytes(*entry.principal);
    if (entry.presence) {
        const auto& presence = *entry.presence;
        bytes += 4 + 4 + utf8Bytes(presence.process) + utf8Bytes(presence.claimedLabel) + utf8Bytes(presence.grantIdentity) + 1
            + varintBytes(presence.sharedPanes.size()) + presence.sharedPanes.size() * (idBytes + 1);
    }
    return bytes;
}

void putID(WireWriter& writer, const ID128& value)
{
    writer.u64(value.low);
    writer.u64(value.high);
}

ID128 takeID(WireReader& reader)
{
    ID128 value;
    value.low = reader.u64();
    value.high = reader.u64();
    return value;
}

bool takeBool(WireReader& reader)
{
    const std::uint8_t value = reader.u8();
    if (value > 1)
        reader.fail();
    return value == 1;
}

template<typename Enumeration>
Enumeration takeEnumeration(WireReader& reader, Enumeration last)
{
    const std::uint8_t value = reader.u8();
    if (value > std::to_underlying(last))
        reader.fail();
    return static_cast<Enumeration>(reader.ok() ? value : 0);
}

void putEntry(WireWriter& writer, const AttachmentEntry& entry)
{
    putID(writer, entry.attachmentID);
    writer.u8(std::to_underlying(entry.role));
    writer.u8(std::to_underlying(entry.clientKind));
    writer.u8(std::to_underlying(entry.state));
    writer.u8(entry.isGeometryOwner ? 1 : 0);
    writer.u8(entry.isPrimary ? 1 : 0);
    writer.u8(entry.isSelf ? 1 : 0);
    writer.varint(entry.panes.size());
    for (const auto pane : entry.panes)
        putID(writer, pane);
    writer.u8(entry.principal ? 1 : 0);
    if (entry.principal)
        writer.utf8(*entry.principal);
    writer.u8(entry.presence ? 1 : 0);
    if (!entry.presence)
        return;
    const auto& presence = *entry.presence;
    writer.u32(presence.clientID);
    writer.u32(presence.processID);
    writer.utf8(presence.process);
    writer.utf8(presence.claimedLabel);
    writer.utf8(presence.grantIdentity);
    writer.u8(presence.isQUIC ? 1 : 0);
    writer.varint(presence.sharedPanes.size());
    for (const auto& shared : presence.sharedPanes) {
        putID(writer, shared.paneID);
        writer.u8(std::to_underlying(shared.grant));
    }
}

AttachmentEntry takeEntry(WireReader& reader)
{
    AttachmentEntry entry;
    entry.attachmentID = takeID(reader);
    entry.role = takeEnumeration(reader, AttachmentRole::Driver);
    entry.clientKind = takeEnumeration(reader, ClientKind::Viewer);
    entry.state = takeEnumeration(reader, AttachmentState::Lost);
    entry.isGeometryOwner = takeBool(reader);
    entry.isPrimary = takeBool(reader);
    entry.isSelf = takeBool(reader);
    const std::size_t panes = reader.count(maxPanesPerConnection, idBytes);
    for (std::size_t index = 0; reader.ok() && index < panes; ++index)
        entry.panes.push_back(takeID(reader));
    if (takeBool(reader))
        entry.principal = std::string(reader.utf8(maxFrameBodyBytes));
    if (!takeBool(reader) || !reader.ok())
        return entry;
    AttachmentPresence presence;
    presence.clientID = reader.u32();
    presence.processID = reader.u32();
    presence.process = reader.utf8(maxProcessBytes);
    presence.claimedLabel = reader.utf8(maxClaimedLabelBytes);
    presence.grantIdentity = reader.utf8(maxConsentIdentityBytes);
    presence.isQUIC = takeBool(reader);
    const std::size_t shared = reader.count(maxPanesPerConnection, idBytes + 1);
    for (std::size_t index = 0; reader.ok() && index < shared; ++index) {
        SharedPaneGrant grant;
        grant.paneID = takeID(reader);
        grant.grant = takeEnumeration(reader, PresenceGrant::Always);
        presence.sharedPanes.push_back(grant);
    }
    entry.presence = std::move(presence);
    return entry;
}

} // namespace

std::optional<std::size_t> attachmentsNotifySize(const AttachmentsList& list)
{
    if (!list.revision || list.attachments.size() > maxAttachmentEntries)
        return std::nullopt;
    std::size_t bytes = varintBytes(std::to_underlying(NotifyKind::Attachments)) + 8 + varintBytes(list.attachments.size());
    bool hasSelf = false;
    for (std::size_t index = 0; index < list.attachments.size(); ++index) {
        const auto& entry = list.attachments[index];
        if (!entryIsValid(entry) || (entry.isSelf && std::exchange(hasSelf, true))
            || std::ranges::find(list.attachments.begin(), list.attachments.begin() + index, entry.attachmentID, &AttachmentEntry::attachmentID) != list.attachments.begin() + index)
            return std::nullopt;
        const std::size_t row = entryBytes(entry);
        if (row > maxFrameBodyBytes - bytes)
            return std::nullopt;
        bytes += row;
    }
    return bytes <= maxFrameBodyBytes ? std::optional { bytes } : std::nullopt;
}

std::optional<std::vector<std::uint8_t>> attachmentsNotify(const AttachmentsList& list)
{
    const auto size = attachmentsNotifySize(list);
    if (!size)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(*size);
    WireWriter writer(body);
    writer.varint(std::to_underlying(NotifyKind::Attachments));
    writer.u64(list.revision);
    writer.varint(list.attachments.size());
    for (const auto& entry : list.attachments)
        putEntry(writer, entry);
    return body.size() == *size ? std::optional { std::move(body) } : std::nullopt;
}

std::optional<AttachmentsList> decodeAttachments(std::span<const std::uint8_t> record)
{
    WireReader reader(record);
    AttachmentsList list;
    list.revision = reader.u64();
    // The smallest entry is a self row with no panes: an ID, six bytes, a
    // zero count and two absent flags.
    const std::size_t count = reader.count(maxAttachmentEntries, idBytes + 6 + 1 + 1 + 1);
    list.attachments.reserve(reader.ok() ? count : 0);
    for (std::size_t index = 0; reader.ok() && index < count; ++index)
        list.attachments.push_back(takeEntry(reader));
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    if (!attachmentsNotifySize(list))
        return std::nullopt;
    return list;
}

void serveNotifies(HostConnection& connection)
{
    if (const auto list = connection.owedAttachments()) {
        if (const auto body = attachmentsNotify(*list); body && connection.sendNotify(*body))
            connection.noteAttachmentsSent(*list);
    }
}

std::vector<Notify> readNotifies(ClientAttachment& attachment)
{
    std::vector<Notify> unread;
    for (Notify& notify : attachment.takeNotifies()) {
        if (notify.kind != NotifyKind::Attachments) {
            unread.push_back(std::move(notify));
            continue;
        }
        if (std::optional<AttachmentsList> list = decodeAttachments(notify.record))
            attachment.noteAttachments(std::move(*list));
    }
    return unread;
}

} // namespace Ubiquitty::Protocol::Sync
