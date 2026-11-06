// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/SessionHandover.h"

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
using Error = Sync::HandoverBodyError;
constexpr std::uint16_t version = 1;
constexpr std::uint32_t magic = 0x53455348;
constexpr std::size_t sectionCount = 4;
constexpr std::size_t headerBytes = 2 + 4 + sectionCount * 4;
constexpr std::size_t digestBytes = 16;

template<typename E> Error bodyError(E error)
{
    if (error == E::NewerVersion)
        return Error::NewerVersion;
    if (error == E::OverCapacity)
        return Error::OverCapacity;
    if (error == E::DigestMismatch)
        return Error::DigestMismatch;
    if constexpr (requires { E::AllocationRefused; }) {
        if (error == E::AllocationRefused)
            return Error::AllocationRefused;
    }
    return Error::Malformed;
}

template<typename T> void writeFixed(std::span<std::uint8_t>& output, T value)
{
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index)
        output[index] = static_cast<std::uint8_t>(value >> (index * 8));
    output = output.subspan(sizeof(T));
}

std::expected<Sync::HandoverBytes, Error> join(const std::array<std::span<const std::uint8_t>, sectionCount>& sections,
    std::size_t maxBytes)
{
    std::size_t size = headerBytes + digestBytes;
    if (size > maxBytes)
        return std::unexpected(Error::OverCapacity);
    for (const auto section : sections) {
        if (section.size() > std::numeric_limits<std::uint32_t>::max() || section.size() > maxBytes - size)
            return std::unexpected(Error::OverCapacity);
        size += section.size();
    }
    Sync::HandoverBytes bytes { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]), size };
    if (!bytes.data)
        return std::unexpected(Error::AllocationRefused);
    auto output = std::span(bytes.data.get(), size);
    writeFixed(output, version);
    writeFixed(output, magic);
    for (const auto section : sections)
        writeFixed(output, static_cast<std::uint32_t>(section.size()));
    for (const auto section : sections) {
        if (!section.empty())
            std::memcpy(output.data(), section.data(), section.size());
        output = output.subspan(section.size());
    }
    const auto digest = Sync::digestOf(bytes.view().first(size - digestBytes));
    writeFixed(output, digest.low);
    writeFixed(output, digest.high);
    return bytes;
}

std::expected<std::array<std::span<const std::uint8_t>, sectionCount>, Error> split(
    std::span<const std::uint8_t> bytes, const SessionHandoverLimits& limits)
{
    if (bytes.size() > limits.maxRecordBytes)
        return std::unexpected(Error::OverCapacity);
    Sync::WireReader reader(bytes);
    const auto carriedVersion = reader.u16();
    if (reader.ok() && carriedVersion > version)
        return std::unexpected(Error::NewerVersion);
    if (!reader.ok() || carriedVersion != version || reader.u32() != magic || bytes.size() < headerBytes + digestBytes)
        return std::unexpected(Error::Malformed);
    const std::array bounds { limits.pane.maxRecordBytes, limits.effects.maxSectionBytes,
        limits.runtime.maxSectionBytes, limits.configuration.maxSectionBytes };
    std::array<std::uint32_t, sectionCount> sizes;
    for (std::size_t index = 0; index < sectionCount; ++index) {
        sizes[index] = reader.u32();
        if (sizes[index] > bounds[index])
            return std::unexpected(Error::OverCapacity);
    }
    std::array<std::span<const std::uint8_t>, sectionCount> sections;
    for (std::size_t index = 0; index < sectionCount; ++index)
        sections[index] = reader.take(sizes[index]);
    const Sync::Digest128 digest { reader.u64(), reader.u64() };
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(Error::Malformed);
    if (digest != Sync::digestOf(bytes.first(bytes.size() - digestBytes)))
        return std::unexpected(Error::DigestMismatch);
    return sections;
}

} // namespace

std::expected<Sync::HandoverBytes, Error> SessionHandover::capture(const Session& session, const SessionHandoverLimits& limits)
{
    if (!session.term || !session.stream || !session.screenSource || !session.inputEncoder)
        return std::unexpected(Error::Malformed);
    auto pending = session.stream->copyPendingWrite();
    const auto coherence = session.coherence.continuation();
    const auto input = session.inputEncoder->continuation();
    if (!pending || !coherence || !input)
        return std::unexpected(Error::NotAtGround);
    const auto pane = Sync::encodePaneHandover(*session.screenSource, SessionHandoverFacts::capture(session), *pending, limits.pane);
    if (!pane)
        return std::unexpected(pane.error());
    const auto effects = encodeHandoverEffects(session.capabilityPolicy, session.effects, limits.effects);
    if (!effects)
        return std::unexpected(bodyError(effects.error()));
    const auto runtime = encodeHandoverRuntime({ *coherence, *input }, limits.runtime);
    if (!runtime)
        return std::unexpected(bodyError(runtime.error()));
    const auto configuration = Preferences::encodeHandoverValues(session.configuration, limits.configuration);
    if (!configuration)
        return std::unexpected(bodyError(configuration.error()));
    return join({ pane->view(), *effects, *runtime, *configuration }, limits.maxRecordBytes);
}

std::expected<std::unique_ptr<SessionHandover::Session>, Error> SessionHandover::restore(Daemon& daemon,
    std::span<const std::uint8_t> bytes, std::unique_ptr<PAL::Stream> stream, const SessionHandoverLimits& limits)
{
    if (!stream)
        return std::unexpected(Error::Malformed);
    stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    const auto sections = split(bytes, limits);
    if (!sections)
        return std::unexpected(sections.error());
    auto configuration = Preferences::decodeHandoverValues((*sections)[3], limits.configuration);
    if (!configuration)
        return std::unexpected(bodyError(configuration.error()));
    auto runtime = decodeHandoverRuntime((*sections)[2], limits.runtime);
    if (!runtime)
        return std::unexpected(bodyError(runtime.error()));
    auto effects = decodeHandoverEffects((*sections)[1], limits.effects);
    if (!effects)
        return std::unexpected(bodyError(effects.error()));
    auto pane = Sync::decodePaneHandover((*sections)[0], limits.pane);
    if (!pane)
        return std::unexpected(pane.error());
    std::unique_ptr<Session> session(new (std::nothrow) Session(effects->policy.decisions));
    if (!session)
        return std::unexpected(Error::AllocationRefused);
    if (!SessionHandoverFacts::restore(*session, std::move(pane->session)))
        return std::unexpected(Error::Malformed);
    session->configuration = std::move(*configuration);
    session->term = std::move(pane->terminal);
    session->screenSource = std::move(pane->source);
    session->stream = std::move(stream);
    session->coherence = Runtime::FrameCoherence(runtime->coherence.tuning);
    if (!session->stream->restorePendingWrite(pane->unwrittenPTY)
        || !session->coherence.restoreContinuation(runtime->coherence)
        || !session->capabilityPolicy.restoreContinuation(std::move(effects->policy))
        || !session->effects.restoreHeldEffects(std::move(effects->effects)))
        return std::unexpected(Error::Malformed);
    session->capabilityPolicy.setGrantStore(&daemon.m_grantStore);
    session->capabilityPolicy.setConsentQueue(&daemon.m_consentQueue);
    session->term->capabilityPolicy = &session->capabilityPolicy;
    Daemon::installTerminalClients(*session);
    if (!daemon.wireRemoteInput(*session))
        return std::unexpected(Error::AllocationRefused);
    if (!session->inputEncoder->restoreContinuation(runtime->input))
        return std::unexpected(Error::Malformed);
    daemon.wireBackgroundNotifications(*session);
    return session;
}

} // namespace Ubiquitty::Host
