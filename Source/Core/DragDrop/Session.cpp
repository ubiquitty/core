// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/DragDrop/Session.h"

#include "Core/Codec/Base64.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace Ubiquitty::DragDrop {
namespace {

std::string_view view(const FallibleVector<char>& bytes)
{
    return bytes.empty() ? std::string_view {} : std::string_view(bytes.data(), bytes.size());
}

std::expected<void, Error> assign(FallibleVector<char>& destination, std::string_view bytes)
{
    if (!destination.tryAssign(bytes.begin(), bytes.end()))
        return std::unexpected(Error::NoMemory);
    return {};
}

bool isBinary(Type type)
{
    return type == Type::Prepare || type == Type::Event || type == Type::File;
}

bool isOperation(std::uint32_t operation)
{
    return operation <= 2;
}

bool isMachineID(std::string_view machine)
{
    return machine.size() <= maxPacketPayloadBytes && machine.find(':') != std::string_view::npos
        && std::ranges::all_of(machine, [](unsigned char character) { return character >= 0x20 && character < 0x7f; });
}

bool containsMIME(std::string_view list, std::string_view mime)
{
    for (std::size_t index = 0; const auto item = mimeAt(list, index); ++index) {
        if (*item == mime)
            return true;
    }
    return false;
}

Metadata readResponse(const Metadata& request)
{
    Metadata response;
    response.type = Type::Read;
    response.x = request.x;
    response.y = request.y;
    response.pixelY = request.pixelY;
    response.identity = request.identity;
    return response;
}

bool isLocation(const Metadata& metadata)
{
    return metadata.x.value_or(-1) >= 0 && metadata.y.value_or(-1) >= 0
        && metadata.pixelX.value_or(-1) >= 0 && metadata.pixelY.value_or(-1) >= 0;
}

} // namespace

bool Session::reply(Metadata metadata, std::string_view payload, Channel& channel)
{
    std::array<char, maxPacketPayloadBytes + maxMetadataBytes + 2> packet;
    do {
        const auto chunk = payload.substr(0, maxPacketPayloadBytes);
        const bool hasMore = payload.size() > chunk.size();
        if (hasMore || metadata.hasMore) {
            metadata.hasMore = true;
            metadata.hasMoreChunks = hasMore;
        }
        const auto length = framePacket(metadata, chunk, packet);
        if (!length || !channel.writeReply(std::span(packet).first(*length)))
            return false;
        payload.remove_prefix(chunk.size());
    } while (!payload.empty());
    return true;
}

bool Session::send(CommandKind kind, std::uint64_t session, const Metadata& metadata, std::string_view payload, Channel& channel)
{
    return channel.sendCommand({ .kind = kind, .sessionID = session, .metadata = metadata, .payload = payload });
}

void Session::clearChain()
{
    m_chain.reset();
    m_chainText = {};
    m_base64TailSize = 0;
}

void Session::endDrop(Channel& channel, std::uint32_t operation)
{
    if (m_dropSession) {
        Metadata metadata;
        metadata.operation = operation;
        (void)send(CommandKind::FinishDrop, m_dropSession, metadata, {}, channel);
    }
    m_dropPhase = DropPhase::None;
    m_dropSession = 0;
    m_dropMIMEs = {};
    m_selectedDropOperation = 0;
    m_allowedDropOperations = 0;
    m_hasReadURIs = false;
    m_isSameWindow = false;
    m_reads = {};
    m_directories = {};
}

void Session::endOffer(Channel& channel)
{
    if (m_offerSession)
        (void)send(CommandKind::CancelDrag, m_offerSession, {}, {}, channel);
    m_offerPhase = OfferPhase::None;
    m_offerSession = 0;
    m_offerMIMEs = {};
    m_offerOperations = 0;
    m_prepared = {};
    m_images = {};
    m_imageCount = 0;
    m_preparedBytes = 0;
    m_imageBytes = 0;
    m_requestedMIMEs = {};
    m_requestedFiles = {};
}

void Session::failOffer(Error error, Channel& channel)
{
    Metadata metadata;
    metadata.type = Type::Error;
    metadata.identity = m_offerIdentity;
    (void)reply(metadata, errorName(error), channel);
    endOffer(channel);
    clearChain();
}

void Session::failRead(const Metadata& request, Error error, Channel& channel)
{
    auto metadata = readResponse(request);
    metadata.type = Type::ReadError;
    metadata.identity = m_dropIdentity ? m_dropIdentity : request.identity;
    (void)reply(metadata, errorName(error), channel);
    if (m_dropPhase == DropPhase::Dropped)
        endDrop(channel);
}

void Session::sendRegistration(Channel& channel)
{
    Metadata metadata;
    metadata.type = m_acceptsDrops ? Type::Accept : Type::StopAccepting;
    metadata.identity = m_dropIdentity;
    (void)send(CommandKind::Registration, 0, metadata, view(m_acceptedMIMEs), channel);
    if (!m_dropMachine.empty()) {
        metadata.type = Type::Accept;
        metadata.x = 1;
        (void)send(CommandKind::Registration, 0, metadata, view(m_dropMachine), channel);
    }
    metadata = {};
    metadata.type = Type::Offer;
    metadata.x = m_isOfferingDrags ? 1 : 2;
    metadata.identity = m_offerIdentity;
    (void)send(CommandKind::Registration, 0, metadata, view(m_offerMachine), channel);
}

void Session::reset(Channel& channel)
{
    endDrop(channel);
    endOffer(channel);
    clearChain();
    m_acceptsDrops = false;
    m_isOfferingDrags = false;
    m_acceptedMIMEs = {};
    m_dropMachine = {};
    m_offerMachine = {};
    m_dropIdentity.reset();
    m_offerIdentity.reset();
    sendRegistration(channel);
}

void Session::rebindNativeOwner(Channel& channel)
{
    endDrop(channel);
    endOffer(channel);
    clearChain();
    m_lastNativeSession = 0;
}

RegistrationState Session::registrationState() const
{
    return { .acceptedMIMEs = view(m_acceptedMIMEs), .dropMachine = view(m_dropMachine), .offerMachine = view(m_offerMachine), .dropIdentity = m_dropIdentity, .offerIdentity = m_offerIdentity, .lastNativeSession = m_lastNativeSession, .acceptsDrops = m_acceptsDrops, .isOfferingDrags = m_isOfferingDrags };
}

std::expected<void, Error> Session::restoreRegistration(const RegistrationState& state)
{
    if (hasPendingWork() || !countMIMETypes(state.acceptedMIMEs)
        || (!state.dropMachine.empty() && !isMachineID(state.dropMachine))
        || (!state.offerMachine.empty() && !isMachineID(state.offerMachine)))
        return std::unexpected(Error::Invalid);
    FallibleVector<char> accepted, dropMachine, offerMachine;
    if (!assign(accepted, state.acceptedMIMEs) || !assign(dropMachine, state.dropMachine) || !assign(offerMachine, state.offerMachine))
        return std::unexpected(Error::NoMemory);
    m_acceptedMIMEs = std::move(accepted);
    m_dropMachine = std::move(dropMachine);
    m_offerMachine = std::move(offerMachine);
    m_dropIdentity = state.dropIdentity;
    m_offerIdentity = state.offerIdentity;
    m_lastNativeSession = state.lastNativeSession;
    m_acceptsDrops = state.acceptsDrops;
    m_isOfferingDrags = state.isOfferingDrags;
    return {};
}

std::expected<std::span<char>, Error> Session::decode(std::string_view encoded, std::span<char> output, const Metadata& metadata)
{
    std::array<char, maxPacketPayloadBytes + 4> joined;
    std::copy_n(m_base64Tail.begin(), m_base64TailSize, joined.begin());
    if (!encoded.empty())
        std::memcpy(joined.data() + m_base64TailSize, encoded.data(), encoded.size());
    const auto size = m_base64TailSize + encoded.size();
    const auto complete = metadata.hasMoreChunks ? size - size % 4 : size;
    const auto input = std::string_view(joined.data(), complete);
    if (metadata.hasMoreChunks && input.find('=') != std::string_view::npos)
        return std::unexpected(Error::Invalid);
    const auto decoded = Codec::base64Decode(input, output);
    if (!decoded)
        return std::unexpected(Error::Invalid);
    m_base64TailSize = size - complete;
    std::copy_n(joined.begin() + static_cast<std::ptrdiff_t>(complete), m_base64TailSize, m_base64Tail.begin());
    return *decoded;
}

void Session::receive(std::string_view body, Channel& channel)
{
    const auto parsed = parsePacket(body);
    if (!parsed) {
        failOffer(parsed.error(), channel);
        return;
    }
    Metadata metadata = parsed->metadata;
    if (metadata.type == Type::Query || metadata.type == Type::QueryResponse) {
        if (!metadata.hasMoreChunks && parsed->payload.empty()) {
            Metadata answer;
            answer.type = Type::Query;
            answer.identity = metadata.identity;
            (void)reply(answer, {}, channel);
        }
        return;
    }
    if (m_chain) {
        if (!metadata.hasMore) {
            failOffer(Error::Invalid, channel);
            return;
        }
        const bool hasMore = metadata.hasMoreChunks;
        metadata = *m_chain;
        metadata.hasMore = true;
        metadata.hasMoreChunks = hasMore;
    } else if (metadata.hasMoreChunks)
        m_chain = metadata;
    if (isBinary(metadata.type)) {
        data(metadata, parsed->payload, channel);
        if (!metadata.hasMoreChunks)
            clearChain();
        return;
    }
    if (m_chain) {
        if (parsed->payload.size() > maxMIMEListBytes - m_chainText.size()
            || !m_chainText.tryReserve(std::min(maxMIMEListBytes, std::max(m_chainText.size() + parsed->payload.size(), m_chainText.capacity() * 2)))) {
            failOffer(Error::TooLarge, channel);
            return;
        }
        const auto before = m_chainText.size();
        if (!m_chainText.tryResize(before + parsed->payload.size())) {
            failOffer(Error::NoMemory, channel);
            return;
        }
        if (!parsed->payload.empty())
            std::memcpy(m_chainText.data() + before, parsed->payload.data(), parsed->payload.size());
        if (metadata.hasMoreChunks)
            return;
        auto owned = std::move(m_chainText);
        clearChain();
        dispatch(metadata, view(owned), channel);
    } else
        dispatch(metadata, parsed->payload, channel);
}

void Session::dispatch(const Metadata& metadata, std::string_view payload, Channel& channel)
{
    switch (metadata.type) {
    case Type::Accept: {
        if (metadata.x.value_or(0) == 1) {
            endDrop(channel);
            if (!isMachineID(payload) || !assign(m_dropMachine, payload)) {
                failOffer(Error::Invalid, channel);
                return;
            }
            if (metadata.identity)
                m_dropIdentity = metadata.identity;
        } else {
            const auto count = countMIMETypes(payload);
            if (!count || !assign(m_acceptedMIMEs, payload)) {
                failOffer(count ? Error::NoMemory : count.error(), channel);
                return;
            }
            endDrop(channel);
            m_acceptsDrops = true;
            if (metadata.identity)
                m_dropIdentity = metadata.identity;
        }
        sendRegistration(channel);
        return;
    }
    case Type::StopAccepting:
        endDrop(channel);
        m_acceptsDrops = false;
        m_dropIdentity.reset();
        m_dropMachine = {};
        m_acceptedMIMEs = {};
        sendRegistration(channel);
        return;
    case Type::Move: {
        if (m_dropPhase != DropPhase::Hover)
            return;
        const auto operation = metadata.operation.value_or(0);
        if (!isOperation(operation) || (operation & m_allowedDropOperations) != operation || !countMIMETypes(payload)) {
            m_selectedDropOperation = 0;
            auto rejected = metadata;
            rejected.operation = 0;
            (void)send(CommandKind::AcceptDrop, m_dropSession, rejected, {}, channel);
            return;
        }
        for (std::size_t index = 0; const auto mime = mimeAt(payload, index); ++index) {
            if (!containsMIME(view(m_dropMIMEs), *mime)) {
                m_selectedDropOperation = 0;
                auto rejected = metadata;
                rejected.operation = 0;
                (void)send(CommandKind::AcceptDrop, m_dropSession, rejected, {}, channel);
                return;
            }
        }
        m_selectedDropOperation = operation;
        if (!send(CommandKind::AcceptDrop, m_dropSession, metadata, payload, channel))
            endDrop(channel);
        return;
    }
    case Type::Read:
        if (!payload.empty())
            failRead(metadata, Error::Invalid, channel);
        else
            requestDropData(metadata, channel);
        return;
    case Type::Offer: {
        if (metadata.identity)
            m_offerIdentity = metadata.identity;
        const auto selection = metadata.x.value_or(0);
        if (selection == 1 || selection == 2) {
            if (!payload.empty() && !isMachineID(payload)) {
                failOffer(Error::Invalid, channel);
                return;
            }
            endOffer(channel);
            m_isOfferingDrags = selection == 1;
            if (!m_isOfferingDrags)
                m_offerIdentity.reset();
            if (!assign(m_offerMachine, payload)) {
                m_isOfferingDrags = false;
                failOffer(Error::NoMemory, channel);
            }
            sendRegistration(channel);
            return;
        }
        if (!m_isOfferingDrags || m_offerPhase != OfferPhase::Gesture) {
            failOffer(Error::NotPermitted, channel);
            return;
        }
        const auto count = countMIMETypes(payload);
        const auto operations = metadata.operation.value_or(0);
        if (!count || !*count || !operations || operations > 3) {
            failOffer(count ? Error::Invalid : count.error(), channel);
            return;
        }
        if (!assign(m_offerMIMEs, payload)) {
            failOffer(Error::NoMemory, channel);
            return;
        }
        m_offerOperations = operations;
        m_offerPhase = OfferPhase::Preparing;
        if (!send(CommandKind::PrepareDrag, m_offerSession, metadata, payload, channel))
            failOffer(Error::NoMemory, channel);
        return;
    }
    case Type::Present: {
        if (!payload.empty()) {
            failOffer(Error::Invalid, channel);
            return;
        }
        if (metadata.x.value_or(0) == -1) {
            if (m_offerPhase != OfferPhase::Preparing) {
                failOffer(Error::NotPermitted, channel);
                return;
            }
            if (std::ranges::any_of(m_prepared, [](const auto& item) { return item.hasMetadata && !item.isComplete; })
                || std::ranges::any_of(m_images, [](const auto& item) { return item.hasMetadata && !item.isComplete; })) {
                failOffer(Error::Invalid, channel);
                return;
            }
            m_offerPhase = OfferPhase::Starting;
            if (!send(CommandKind::BeginDrag, m_offerSession, metadata, {}, channel))
                failOffer(Error::NotPermitted, channel);
        } else if (m_offerPhase == OfferPhase::Running) {
            if (!send(CommandKind::SetDragImage, m_offerSession, metadata, {}, channel))
                failOffer(Error::InputOutput, channel);
        } else
            failOffer(Error::NotPermitted, channel);
        return;
    }
    case Type::Error: {
        if (m_offerPhase != OfferPhase::Running) {
            failOffer(Error::Invalid, channel);
            return;
        }
        const auto index = metadata.y.value_or(-1);
        if (index == -1) {
            endOffer(channel);
            return;
        }
        if (index < 0 || index >= static_cast<std::int64_t>(maxMIMETypes)
            || !m_requestedMIMEs[static_cast<std::size_t>(index)]) {
            failOffer(Error::Invalid, channel);
            return;
        }
        m_requestedMIMEs[static_cast<std::size_t>(index)] = false;
        if (!send(CommandKind::DragData, m_offerSession, metadata, payload, channel))
            failOffer(Error::NoMemory, channel);
        return;
    }
    default:
        failOffer(Error::Invalid, channel);
        return;
    }
}

void Session::requestDropData(const Metadata& request, Channel& channel)
{
    if (m_dropPhase != DropPhase::Dropped) {
        failRead(request, Error::NotPermitted, channel);
        return;
    }
    Metadata metadata = request;
    metadata.identity = m_dropIdentity;
    if (request.pixelY) {
        const auto found = std::ranges::find(m_directories, request.pixelY);
        if (found == m_directories.end()) {
            failRead(metadata, Error::Invalid, channel);
            return;
        }
        if (!request.x.value_or(0)) {
            if (!send(CommandKind::CloseDirectory, m_dropSession, metadata, {}, channel)) {
                failRead(metadata, Error::InputOutput, channel);
                return;
            }
            found->reset();
            return;
        }
    } else if (!request.x.value_or(0)) {
        const auto operation = request.operation.value_or(0);
        if (!isOperation(operation) || (operation & m_allowedDropOperations) != operation)
            failRead(metadata, Error::Invalid, channel);
        else
            endDrop(channel, operation);
        return;
    }
    if (request.x.value_or(0) < 1 || request.y.value_or(0) < 0) {
        failRead(metadata, Error::NotFound, channel);
        return;
    }
    const auto mime = request.pixelY ? std::optional<std::string_view> {} : mimeAt(view(m_dropMIMEs), static_cast<std::size_t>(*request.x - 1));
    if (!request.pixelY && !mime) {
        failRead(metadata, Error::NotFound, channel);
        return;
    }
    const bool isURIList = mime && *mime == "text/uri-list";
    if (request.y.value_or(0) && (!isURIList || !m_hasReadURIs)) {
        failRead(metadata, Error::Invalid, channel);
        return;
    }
    if (m_isSameWindow && (request.y.value_or(0) || request.pixelY)) {
        failRead(metadata, Error::NotPermitted, channel);
        return;
    }
    const auto slot = std::ranges::find_if(m_reads, [](const auto& read) { return !read.identity; });
    if (slot == m_reads.end() || m_nextRead == std::numeric_limits<std::uint64_t>::max()) {
        failRead(metadata, Error::TooManyFiles, channel);
        return;
    }
    *slot = { .identity = ++m_nextRead, .metadata = metadata, .isURIList = isURIList && !request.y.value_or(0) };
    if (!channel.sendCommand({ .kind = CommandKind::ReadDrop, .sessionID = m_dropSession, .requestID = slot->identity, .metadata = metadata }))
        failRead(metadata, Error::InputOutput, channel);
}

void Session::data(const Metadata& metadata, std::string_view payload, Channel& channel)
{
    Command command;
    command.sessionID = m_offerSession;
    command.metadata = metadata;
    command.isFinal = payload.empty() && !metadata.hasMoreChunks;
    PreparedItem* item = nullptr;
    std::size_t* total = nullptr;
    std::size_t limit = 0;
    if (metadata.type == Type::Prepare) {
        if (m_offerPhase != OfferPhase::Preparing) {
            failOffer(Error::NotPermitted, channel);
            return;
        }
        const auto index = metadata.x.value_or(0);
        if (index < 0) {
            const auto imageIndex = static_cast<std::size_t>(-index - 1);
            if (imageIndex >= maxImages || imageIndex > m_imageCount
                || (imageIndex && !m_images[imageIndex - 1].isComplete)) {
                failOffer(imageIndex >= maxImages ? Error::TooLarge : Error::Invalid, channel);
                return;
            }
            item = &m_images[imageIndex];
            if (imageIndex == m_imageCount)
                ++m_imageCount;
            total = &m_imageBytes;
            limit = maxImageBytes;
        } else {
            if (!mimeAt(view(m_offerMIMEs), static_cast<std::size_t>(index))) {
                failOffer(Error::Invalid, channel);
                return;
            }
            item = &m_prepared[static_cast<std::size_t>(index)];
            total = &m_preparedBytes;
            limit = maxPreparedDataBytes;
        }
        if (item->isComplete) {
            failOffer(Error::Invalid, channel);
            return;
        }
        if (!item->hasMetadata) {
            item->metadata = metadata;
            item->hasMetadata = true;
        }
        command.metadata = item->metadata;
        command.kind = CommandKind::PrepareData;
    } else if (metadata.type == Type::Event) {
        const auto index = metadata.y.value_or(0);
        if (m_offerPhase != OfferPhase::Running || index < 0 || index >= static_cast<std::int64_t>(maxMIMETypes)
            || !m_requestedMIMEs[static_cast<std::size_t>(index)]) {
            failOffer(Error::Invalid, channel);
            return;
        }
        command.kind = CommandKind::DragData;
        if (command.isFinal)
            m_requestedMIMEs[static_cast<std::size_t>(index)] = false;
    } else {
        if ((m_offerPhase != OfferPhase::Starting && m_offerPhase != OfferPhase::Running)
            || !metadata.x || std::ranges::find(m_requestedFiles, metadata.x) == m_requestedFiles.end()) {
            failOffer(Error::Invalid, channel);
            return;
        }
        command.kind = CommandKind::RemoteFileData;
    }
    std::array<char, maxPacketPayloadBytes> decoded;
    const auto received = decode(payload, decoded, metadata);
    if (!received) {
        failOffer(received.error(), channel);
        return;
    }
    if (item) {
        if (received->size() > limit - *total) {
            failOffer(Error::TooLarge, channel);
            return;
        }
        item->bytes += received->size();
        *total += received->size();
        if (command.isFinal && metadata.x.value_or(0) < 0) {
            const auto& first = item->metadata;
            const auto width = first.pixelX.value_or(0), height = first.pixelY.value_or(0);
            const auto format = first.y.value_or(0);
            if (width <= 0 || height <= 0 || (format != 0 && format != 24 && format != 32 && format != 100)) {
                failOffer(Error::Invalid, channel);
                return;
            }
            if (format == 24 || format == 32) {
                const auto pixelBytes = static_cast<std::uint64_t>(format / 8);
                if (static_cast<std::uint64_t>(width) > maxImageBytes / pixelBytes
                    || static_cast<std::uint64_t>(height) > maxImageBytes / pixelBytes / static_cast<std::uint64_t>(width)) {
                    failOffer(Error::TooLarge, channel);
                    return;
                }
                if (item->bytes != static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * pixelBytes) {
                    failOffer(Error::Invalid, channel);
                    return;
                }
            }
            if (format == 0 && first.operation.value_or(0) > 1024) {
                failOffer(Error::Invalid, channel);
                return;
            }
        }
        item->isComplete = command.isFinal;
    }
    command.payload = { received->data(), received->size() };
    if (!channel.sendCommand(command))
        failOffer(Error::NoMemory, channel);
}

void Session::receive(const Event& event, Channel& channel)
{
    if (event.kind == EventKind::Cancel) {
        if (!event.sessionID || event.sessionID == m_dropSession)
            endDrop(channel);
        if (!event.sessionID || event.sessionID == m_offerSession)
            endOffer(channel);
        if (!m_dropSession && !m_offerSession)
            clearChain();
        return;
    }
    if (event.kind == EventKind::Enter || event.kind == EventKind::BeginGesture) {
        if (!event.sessionID || event.sessionID <= m_lastNativeSession || !isLocation(event.metadata))
            return;
        m_lastNativeSession = event.sessionID;
        if (event.kind == EventKind::Enter) {
            if (!m_acceptsDrops)
                return;
            const auto count = countMIMETypes(event.payload);
            if (!count || !*count || !event.metadata.operation.value_or(0) || event.metadata.operation.value_or(0) > 3)
                return;
            endDrop(channel);
            if (!assign(m_dropMIMEs, event.payload))
                return;
            m_dropSession = event.sessionID;
            m_dropPhase = DropPhase::Hover;
            m_allowedDropOperations = *event.metadata.operation;
            m_isSameWindow = event.isSameWindow;
            auto metadata = event.metadata;
            metadata.type = Type::Move;
            metadata.identity = m_dropIdentity;
            if (!reply(metadata, event.payload, channel))
                endDrop(channel);
        } else if (m_isOfferingDrags) {
            endOffer(channel);
            m_offerSession = event.sessionID;
            m_offerPhase = OfferPhase::Gesture;
            auto metadata = event.metadata;
            metadata.type = Type::Offer;
            metadata.identity = m_offerIdentity;
            if (!reply(metadata, {}, channel))
                endOffer(channel);
        }
        return;
    }
    if (event.kind == EventKind::Move || event.kind == EventKind::Leave || event.kind == EventKind::Drop) {
        if (m_dropPhase != DropPhase::Hover || event.sessionID != m_dropSession)
            return;
        auto metadata = event.metadata;
        metadata.type = Type::Move;
        metadata.identity = m_dropIdentity;
        if (event.kind == EventKind::Leave) {
            metadata.x = -1;
            metadata.y = -1;
            metadata.pixelX = 0;
            metadata.pixelY = 0;
            metadata.operation = 0;
            (void)reply(metadata, {}, channel);
            endDrop(channel);
            return;
        }
        if (!isLocation(metadata) || !metadata.operation.value_or(0) || metadata.operation.value_or(0) > 3)
            return;
        const bool hasChangedMIMEs = !event.payload.empty() && event.payload != view(m_dropMIMEs);
        if (hasChangedMIMEs) {
            m_selectedDropOperation = 0;
            Metadata refusal;
            refusal.operation = 0;
            (void)send(CommandKind::AcceptDrop, m_dropSession, refusal, {}, channel);
            if (!countMIMETypes(event.payload) || !assign(m_dropMIMEs, event.payload)) {
                endDrop(channel);
                return;
            }
        }
        m_allowedDropOperations = *metadata.operation;
        if (event.kind == EventKind::Drop) {
            if (!m_selectedDropOperation || (m_selectedDropOperation & m_allowedDropOperations) != m_selectedDropOperation) {
                endDrop(channel);
                return;
            }
            metadata.type = Type::Drop;
            m_dropPhase = DropPhase::Dropped;
            if (!reply(metadata, view(m_dropMIMEs), channel))
                endDrop(channel);
        } else if (!reply(metadata, hasChangedMIMEs ? view(m_dropMIMEs) : std::string_view {}, channel))
            endDrop(channel);
        return;
    }
    if (event.kind == EventKind::ReadData || event.kind == EventKind::ReadFailed) {
        if (m_dropPhase != DropPhase::Dropped || event.sessionID != m_dropSession || !event.requestID)
            return;
        const auto read = std::ranges::find(m_reads, event.requestID, &PendingRead::identity);
        if (read == m_reads.end())
            return;
        if (event.kind == EventKind::ReadFailed) {
            failRead(read->metadata, event.error, channel);
            return;
        }
        auto metadata = readResponse(read->metadata);
        metadata.hasMore = true;
        metadata.pixelX = event.metadata.pixelX;
        if (event.metadata.pixelX && *event.metadata.pixelX != 0 && *event.metadata.pixelX != 1) {
            if (std::ranges::find(m_directories, event.metadata.pixelX) == m_directories.end()) {
                const auto slot = std::ranges::find(m_directories, std::optional<std::int64_t> {});
                if (slot == m_directories.end()) {
                    failRead(read->metadata, Error::NoMemory, channel);
                    return;
                }
                *slot = event.metadata.pixelX;
            }
        }
        std::array<char, maxPacketPayloadBytes> encoded;
        const auto characters = Codec::base64Encode(std::span(event.payload.data(), event.payload.size()), encoded);
        if (!characters || (event.isFinal && !event.payload.empty())) {
            failRead(read->metadata, Error::Invalid, channel);
            return;
        }
        if (!reply(metadata, { characters->data(), characters->size() }, channel)) {
            failRead(read->metadata, Error::InputOutput, channel);
            return;
        }
        if (event.isFinal) {
            m_hasReadURIs |= read->isURIList;
            *read = {};
        }
        return;
    }
    if (!m_offerSession || event.sessionID != m_offerSession)
        return;
    Metadata metadata;
    metadata.identity = m_offerIdentity;
    metadata.type = Type::Event;
    switch (event.kind) {
    case EventKind::EndGesture:
        if (m_offerPhase != OfferPhase::Running)
            endOffer(channel);
        return;
    case EventKind::Failed:
        failOffer(event.error, channel);
        return;
    case EventKind::Started:
        if (m_offerPhase != OfferPhase::Starting)
            return;
        m_offerPhase = OfferPhase::Running;
        metadata.type = Type::Error;
        if (!reply(metadata, "OK", channel))
            endOffer(channel);
        return;
    case EventKind::RequestFile: {
        const auto index = event.metadata.x.value_or(0);
        if ((m_offerPhase != OfferPhase::Starting && m_offerPhase != OfferPhase::Running) || index <= 0)
            return;
        if (std::ranges::find(m_requestedFiles, std::optional<std::int64_t> { index }) != m_requestedFiles.end())
            return;
        const auto slot = std::ranges::find(m_requestedFiles, std::optional<std::int64_t> {});
        if (slot == m_requestedFiles.end()) {
            failOffer(Error::TooManyFiles, channel);
            return;
        }
        *slot = index;
        metadata.type = Type::File;
        metadata.x = index;
        if (!reply(metadata, {}, channel))
            endOffer(channel);
        return;
    }
    default:
        break;
    }
    if (m_offerPhase != OfferPhase::Running)
        return;
    switch (event.kind) {
    case EventKind::Accepted:
    case EventKind::RequestData: {
        const auto index = event.metadata.y.value_or(0);
        if (index < 0 || !mimeAt(view(m_offerMIMEs), static_cast<std::size_t>(index)))
            return;
        metadata.x = event.kind == EventKind::Accepted ? 1 : 5;
        metadata.y = index;
        if (event.kind == EventKind::RequestData)
            m_requestedMIMEs[static_cast<std::size_t>(index)] = true;
        break;
    }
    case EventKind::OperationChanged:
        if (!isOperation(event.metadata.operation.value_or(0))
            || (event.metadata.operation.value_or(0) & m_offerOperations) != event.metadata.operation.value_or(0))
            return;
        metadata.x = 2;
        metadata.operation = event.metadata.operation.value_or(0);
        break;
    case EventKind::Dropped:
        metadata.x = 3;
        break;
    case EventKind::Finished:
        metadata.x = 4;
        metadata.y = event.metadata.y.value_or(0) ? 1 : 0;
        break;
    default:
        return;
    }
    if (!reply(metadata, {}, channel) || event.kind == EventKind::Finished)
        endOffer(channel);
}

} // namespace Ubiquitty::DragDrop
