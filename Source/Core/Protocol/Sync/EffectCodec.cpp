// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/EffectCodec.h"

#include "Core/OSC/KittyNotification.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"
#include "Core/VT/DECPSSynth.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

std::size_t maxBundleBytes(std::size_t maxDataBytes)
{
    return maxDataBytes <= maxEffectClipboardReadBytes ? maxEffectClipboardEncodedReadBytes - maxEffectClipboardReadBytes + maxDataBytes : 0;
}

// The measuring and writing passes share this bounded serializer. No vector
// capacity or input-sized allocation is needed to establish the exact size.
class Writer {
public:
    explicit Writer(std::size_t maximum, std::span<std::uint8_t> output = {})
        : m_maximum(maximum)
        , m_output(output)
    {
    }

    void fail(EffectCodecError error = EffectCodecError::Malformed)
    {
        if (!m_error)
            m_error = error;
    }
    void fixed(std::uint64_t value, std::size_t width)
    {
        for (std::size_t index = 0; index < width; ++index) {
            const auto byte = static_cast<std::uint8_t>(value >> (index * 8));
            raw({ &byte, 1 });
        }
    }
    void varint(std::uint64_t value)
    {
        while (value >= 0x80) {
            fixed((value & 0x7F) | 0x80, 1);
            value >>= 7;
        }
        fixed(value, 1);
    }
    void signedInteger(std::int64_t value)
    {
        const auto bits = static_cast<std::uint64_t>(value);
        varint((bits << 1) ^ (value < 0 ? ~std::uint64_t { 0 } : 0));
    }
    void raw(std::span<const std::uint8_t> bytes)
    {
        if (m_error)
            return;
        if (bytes.size() > m_maximum - m_size) {
            fail(EffectCodecError::TooLarge);
            return;
        }
        if (!m_output.empty() && !bytes.empty())
            std::memcpy(m_output.data() + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }
    void bytes(std::span<const std::uint8_t> value)
    {
        varint(value.size());
        raw(value);
    }
    void text(std::string_view value, std::size_t maximum)
    {
        if (value.size() > maximum) {
            fail(EffectCodecError::TooLarge);
            return;
        }
        // A string's object representation is the UTF-8 byte sequence.
        const std::span encoded { reinterpret_cast<const std::uint8_t*>(value.data()), value.size() };
        if (!isValidUTF8(encoded)) {
            fail();
            return;
        }
        bytes(encoded);
    }
    std::size_t size() const { return m_size; }
    std::optional<EffectCodecError> error() const { return m_error; }

private:
    const std::size_t m_maximum;
    const std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    std::optional<EffectCodecError> m_error;
};

template<class Write>
std::expected<EffectCodecBytes, EffectCodecError> encodeBounded(std::size_t maximum, Write write)
{
    Writer measure(maximum);
    write(measure);
    if (measure.error())
        return std::unexpected(*measure.error());
    EffectCodecBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[std::max<std::size_t>(1, measure.size())]), measure.size() };
    if (!result.bytes)
        return std::unexpected(EffectCodecError::NoMemory);
    Writer output(result.size, { result.bytes.get(), result.size });
    write(output);
    if (output.error() || output.size() != result.size)
        return std::unexpected(output.error().value_or(EffectCodecError::Malformed));
    return result;
}

bool isValidBuffer(ClipboardBuffer buffer)
{
    return std::to_underlying(buffer) < std::to_underlying(ClipboardBuffer::Count);
}

bool isValidSound(const PAL::PlayTonesRequest& request)
{
    const auto envelope = [](std::uint16_t value) { return value == PAL::PlayTonesRequest::waveDefaultEnvelope || value <= VT::toneMaxEnvelopeMilliseconds; };
    if (request.count < 0 || request.count > PAL::PlayTonesRequest::maxNotes || request.samplePCM || request.sampleFrames
        || request.volume > VT::toneMaxVolume || request.wave > VT::toneMaxWave || request.channel > VT::toneMaxChannel
        || !envelope(request.attackMilliseconds) || !envelope(request.releaseMilliseconds)
        || request.decayMilliseconds > VT::toneMaxEnvelopeMilliseconds || request.sustainLevel > VT::toneMaxVolume
        || request.semitonesDown > VT::toneMaxSemitonesDown || request.reverbSend > VT::toneMaxVolume
        || request.durationMilliseconds > VT::toneMaxDurationUnits * 1000u / 32u || request.gapMilliseconds > request.durationMilliseconds
        || request.lowpassHertz > VT::toneMaxCutoffHertz || request.highpassHertz > VT::toneMaxCutoffHertz
        || request.pan > VT::toneMaxPan || request.sampleSlot > VT::sampleSlotCount)
        return false;
    for (int index = 0; index < request.count; ++index) {
        if (request.noteVolumes[index] > VT::toneMaxVolume || request.noteEffects[index] > VT::toneMaxEffect)
            return false;
    }
    return true;
}

void writeSound(Writer& writer, const PAL::PlayTonesRequest& request)
{
    if (!isValidSound(request)) {
        writer.fail();
        return;
    }
    writer.fixed(request.volume, 1);
    writer.fixed(request.wave, 1);
    writer.fixed(request.channel, 1);
    writer.fixed(request.attackMilliseconds, 2);
    writer.fixed(request.releaseMilliseconds, 2);
    writer.fixed(request.semitonesDown, 1);
    writer.fixed(request.durationMilliseconds, 4);
    writer.fixed(request.gapMilliseconds, 4);
    writer.fixed(request.sampleSlot, 1);
    writer.fixed(request.decayMilliseconds, 2);
    writer.fixed(request.sustainLevel, 1);
    writer.fixed(request.reverbSend, 1);
    writer.fixed(request.gridUnits, 1);
    writer.fixed(request.lowpassHertz, 2);
    writer.fixed(request.highpassHertz, 2);
    writer.fixed(request.pan, 1);
    writer.fixed(request.repeat, 1);
    writer.fixed(static_cast<std::uint8_t>(request.count), 1);
    for (int index = 0; index < request.count; ++index) {
        writer.fixed(request.freqs[index], 4);
        writer.fixed(request.noteVolumes[index], 1);
        writer.fixed(request.noteEffects[index], 1);
        writer.fixed(request.noteParameters[index], 1);
    }
}

SoundEffect readSound(WireReader& reader)
{
    SoundEffect sound;
    auto& request = sound.request;
    request.volume = reader.u8();
    request.wave = reader.u8();
    request.channel = reader.u8();
    request.attackMilliseconds = reader.u16();
    request.releaseMilliseconds = reader.u16();
    request.semitonesDown = reader.u8();
    request.durationMilliseconds = reader.u32();
    request.gapMilliseconds = reader.u32();
    request.sampleSlot = reader.u8();
    request.decayMilliseconds = reader.u16();
    request.sustainLevel = reader.u8();
    request.reverbSend = reader.u8();
    request.gridUnits = reader.u8();
    request.lowpassHertz = reader.u16();
    request.highpassHertz = reader.u16();
    request.pan = reader.u8();
    request.repeat = reader.u8();
    request.count = reader.u8();
    if (!reader.ok() || request.count > PAL::PlayTonesRequest::maxNotes || static_cast<std::size_t>(request.count) > reader.remaining() / 7) {
        reader.fail();
        return sound;
    }
    for (int index = 0; index < request.count; ++index) {
        request.freqs[index] = reader.u32();
        request.noteVolumes[index] = reader.u8();
        request.noteEffects[index] = reader.u8();
        request.noteParameters[index] = reader.u8();
    }
    if (request.wave == 6) {
        sound.sample.serial = reader.u32();
        sound.sample.frames = reader.u32();
        for (auto& byte : sound.sample.sha256)
            byte = reader.u8();
        if (!isValidSampleIdentity(sound.sample) || (!request.sampleSlot && sound.sample.frames))
            reader.fail();
    }
    if (!isValidSound(request))
        reader.fail();
    return sound;
}

std::optional<EffectCodecError> payloadError(const EffectClipboardPayload& payload, std::size_t maxDataBytes)
{
    if (!payload.encodedBytes)
        return EffectCodecError::Malformed;
    if (payload.encodedBytes > maxBundleBytes(maxDataBytes))
        return EffectCodecError::TooLarge;
    if (payload.transfer)
        return payload.inlineBytes.empty() ? std::nullopt : std::optional(EffectCodecError::Malformed);
    if (payload.encodedBytes != payload.inlineBytes.size())
        return EffectCodecError::Malformed;
    const auto decoded = decodeEffectClipboard(payload.inlineBytes, maxDataBytes);
    return decoded ? std::nullopt : std::optional(decoded.error());
}

void writePayload(Writer& writer, const EffectClipboardPayload& payload, std::size_t maxDataBytes)
{
    if (const auto error = payloadError(payload, maxDataBytes)) {
        writer.fail(*error);
        return;
    }
    writer.fixed(payload.transfer, 4);
    writer.fixed(payload.encodedBytes, 4);
    writer.raw(payload.inlineBytes);
}

EffectClipboardPayload readPayload(WireReader& reader, std::size_t maxDataBytes)
{
    EffectClipboardPayload payload;
    payload.transfer = reader.u32();
    payload.encodedBytes = reader.u32();
    if (!reader.ok() || !payload.encodedBytes || payload.encodedBytes > maxBundleBytes(maxDataBytes)) {
        reader.fail();
        return payload;
    }
    if (!payload.transfer)
        payload.inlineBytes = reader.take(payload.encodedBytes);
    if (!reader.ok() || payloadError(payload, maxDataBytes))
        reader.fail();
    return payload;
}

void writeBody(Writer& writer, const EffectBody& body)
{
    switch (effectKind(body)) {
    case EffectKind::Bell:
    case EffectKind::Attention:
        return;
    case EffectKind::Notification: {
        const auto& notification = std::get<NotificationEffect>(body);
        if (std::to_underlying(notification.occasion) > std::to_underlying(NotificationOccasion::Invisible)) {
            writer.fail();
            return;
        }
        writer.text(notification.title, OSC::attentionTextCapacity);
        writer.text(notification.body, OSC::attentionTextCapacity);
        writer.text(notification.identifier, OSC::kittyNotificationIdentifierCapacity);
        writer.fixed(std::to_underlying(notification.occasion), 1);
        return;
    }
    case EffectKind::CommandFinished: {
        const auto& command = std::get<CommandFinishedEffect>(body);
        writer.signedInteger(command.exitStatus);
        writer.fixed(command.durationMicroseconds, 8);
        writer.text(command.currentDirectory, Terminal::maxCurrentDirectoryLength);
        writer.text(command.commandLine, Terminal::maxCommandLineLength);
        return;
    }
    case EffectKind::Sound: {
        const auto& sound = std::get<SoundEffect>(body);
        if (!isValidSampleIdentity(sound.sample)
            || (sound.request.wave != 6 && sound.sample != SampleIdentity {})
            || (!sound.request.sampleSlot && sound.sample.frames)) {
            writer.fail();
            return;
        }
        writeSound(writer, sound.request);
        if (sound.request.wave == 6) {
            writer.fixed(sound.sample.serial, 4);
            writer.fixed(sound.sample.frames, 4);
            writer.raw(sound.sample.sha256);
        }
        return;
    }
    case EffectKind::ClipboardWrite: {
        const auto& clipboard = std::get<ClipboardWriteEffect>(body);
        if (!isValidBuffer(clipboard.buffer)) {
            writer.fail();
            return;
        }
        writer.fixed(std::to_underlying(clipboard.buffer), 1);
        writePayload(writer, clipboard.payload, maxEffectClipboardWriteBytes);
        return;
    }
    }
}

void writeClipboardBundle(Writer& writer, const EffectClipboardBundle& bundle, std::size_t maxDataBytes)
{
    if (!maxBundleBytes(maxDataBytes) || bundle.count > bundle.items.size()) {
        writer.fail(EffectCodecError::TooLarge);
        return;
    }
    std::size_t total = 0;
    writer.varint(bundle.count);
    for (std::size_t index = 0; index < bundle.count; ++index) {
        const auto& item = bundle.items[index];
        if (item.mime.empty() || item.mime.find('\0') != std::string_view::npos) {
            writer.fail();
            return;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (bundle.items[prior].mime == item.mime) {
                writer.fail();
                return;
            }
        }
        if (item.data.size() > maxDataBytes - total) {
            writer.fail(EffectCodecError::TooLarge);
            return;
        }
        total += item.data.size();
        writer.text(item.mime, maxEffectClipboardMIMEBytes);
        writer.bytes(item.data);
    }
}

} // namespace

EffectKind effectKind(const EffectBody& body)
{
    constexpr std::array kinds { EffectKind::Bell, EffectKind::Notification, EffectKind::CommandFinished,
        EffectKind::Sound, EffectKind::Attention, EffectKind::ClipboardWrite };
    return kinds[body.index()];
}

std::expected<EffectCodecBytes, EffectCodecError> encodeEffectBody(const EffectBody& body)
{
    return encodeBounded(maxFrameBodyBytes - effectRecordHeaderBytes, [&](Writer& writer) { writeBody(writer, body); });
}

std::expected<EffectBody, EffectCodecError> decodeEffectBody(EffectKind kind, std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxFrameBodyBytes - effectRecordHeaderBytes)
        return std::unexpected(EffectCodecError::TooLarge);
    WireReader reader(bytes);
    EffectBody body;
    switch (kind) {
    case EffectKind::Bell:
        body = BellEffect {};
        break;
    case EffectKind::Attention:
        body = AttentionEffect {};
        break;
    case EffectKind::Notification: {
        NotificationEffect notification;
        notification.title = reader.utf8(OSC::attentionTextCapacity);
        notification.body = reader.utf8(OSC::attentionTextCapacity);
        notification.identifier = reader.utf8(OSC::kittyNotificationIdentifierCapacity);
        notification.occasion = static_cast<NotificationOccasion>(reader.u8());
        if (std::to_underlying(notification.occasion) > std::to_underlying(NotificationOccasion::Invisible))
            reader.fail();
        body = notification;
        break;
    }
    case EffectKind::CommandFinished: {
        const auto status = reader.svarint();
        if (status < std::numeric_limits<std::int32_t>::min() || status > std::numeric_limits<std::int32_t>::max()) {
            reader.fail();
            break;
        }
        CommandFinishedEffect command;
        command.exitStatus = static_cast<std::int32_t>(status);
        command.durationMicroseconds = reader.u64();
        command.currentDirectory = reader.utf8(Terminal::maxCurrentDirectoryLength);
        command.commandLine = reader.utf8(Terminal::maxCommandLineLength);
        body = command;
        break;
    }
    case EffectKind::Sound:
        body = readSound(reader);
        break;
    case EffectKind::ClipboardWrite: {
        ClipboardWriteEffect clipboard;
        clipboard.buffer = static_cast<ClipboardBuffer>(reader.u8());
        if (!isValidBuffer(clipboard.buffer))
            reader.fail();
        clipboard.payload = readPayload(reader, maxEffectClipboardWriteBytes);
        body = clipboard;
        break;
    }
    default:
        return std::unexpected(EffectCodecError::Malformed);
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(EffectCodecError::Malformed);
    return body;
}

std::expected<EffectCodecBytes, EffectCodecError> encodeEffect(const EffectView& effect)
{
    if (effect.paneID == ID128 {} || !effect.sequence || !effect.count)
        return std::unexpected(EffectCodecError::Malformed);
    const auto body = decodeEffectBody(effect.kind, effect.payload);
    if (!body)
        return std::unexpected(body.error());
    const auto* clipboard = std::get_if<ClipboardWriteEffect>(&*body);
    if (effect.target != (clipboard ? std::to_underlying(clipboard->buffer) : 0))
        return std::unexpected(EffectCodecError::Malformed);
    return encodeBounded(maxFrameBodyBytes, [&](Writer& writer) {
        writer.fixed(effect.generation, 8);
        writer.fixed(effect.sequence, 8);
        writer.fixed(effect.primary.low, 8);
        writer.fixed(effect.primary.high, 8);
        writer.fixed(std::to_underlying(effect.kind), 1);
        writer.fixed(effect.count, 4);
        writer.raw(effect.payload);
    });
}

std::expected<EffectView, EffectCodecError> decodeEffect(ID128 paneID, std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxFrameBodyBytes)
        return std::unexpected(EffectCodecError::TooLarge);
    WireReader reader(bytes);
    EffectView effect;
    effect.paneID = paneID;
    effect.generation = reader.u64();
    effect.sequence = reader.u64();
    effect.primary.low = reader.u64();
    effect.primary.high = reader.u64();
    effect.kind = static_cast<EffectKind>(reader.u8());
    effect.count = reader.u32();
    effect.payload = reader.take(reader.remaining());
    if (!reader.ok() || paneID == ID128 {} || !effect.sequence || !effect.count)
        return std::unexpected(EffectCodecError::Malformed);
    const auto body = decodeEffectBody(effect.kind, effect.payload);
    if (!body)
        return std::unexpected(body.error());
    if (const auto* clipboard = std::get_if<ClipboardWriteEffect>(&*body))
        effect.target = std::to_underlying(clipboard->buffer);
    return effect;
}

std::expected<EffectCodecBytes, EffectCodecError> encodeEffectClipboard(const EffectClipboardBundle& bundle, std::size_t maxDataBytes)
{
    return encodeBounded(maxBundleBytes(maxDataBytes), [&](Writer& writer) {
        writeClipboardBundle(writer, bundle, maxDataBytes);
    });
}

std::expected<std::size_t, EffectCodecError> effectClipboardEncodedBytes(const EffectClipboardBundle& bundle, std::size_t maxDataBytes)
{
    Writer measure(maxBundleBytes(maxDataBytes));
    writeClipboardBundle(measure, bundle, maxDataBytes);
    if (measure.error())
        return std::unexpected(*measure.error());
    return measure.size();
}

std::expected<void, EffectCodecError> writeEffectClipboard(const EffectClipboardBundle& bundle, std::size_t maxDataBytes, std::span<std::uint8_t> output)
{
    const auto size = effectClipboardEncodedBytes(bundle, maxDataBytes);
    if (!size)
        return std::unexpected(size.error());
    if (*size != output.size())
        return std::unexpected(EffectCodecError::Malformed);
    Writer writer(output.size(), output);
    writeClipboardBundle(writer, bundle, maxDataBytes);
    if (writer.error())
        return std::unexpected(*writer.error());
    return {};
}

std::expected<EffectClipboardBundle, EffectCodecError> decodeEffectClipboard(std::span<const std::uint8_t> bytes, std::size_t maxDataBytes)
{
    if (!maxBundleBytes(maxDataBytes) || bytes.size() > maxBundleBytes(maxDataBytes))
        return std::unexpected(EffectCodecError::TooLarge);
    WireReader reader(bytes);
    EffectClipboardBundle bundle;
    bundle.count = reader.count(bundle.items.size(), 2);
    std::size_t total = 0;
    for (std::size_t index = 0; reader.ok() && index < bundle.count; ++index) {
        auto& item = bundle.items[index];
        item.mime = reader.utf8(maxEffectClipboardMIMEBytes);
        if (!reader.ok() || item.mime.empty() || item.mime.find('\0') != std::string_view::npos)
            return std::unexpected(EffectCodecError::Malformed);
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (bundle.items[prior].mime == item.mime)
                return std::unexpected(EffectCodecError::Malformed);
        }
        item.data = reader.bytes(maxDataBytes - total);
        total += item.data.size();
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(EffectCodecError::Malformed);
    return bundle;
}

std::expected<EffectCodecBytes, EffectCodecError> encodeClipboardRead(const ClipboardRead& request)
{
    if (!request.requestID || !isValidBuffer(request.buffer)
        || std::to_underlying(request.kind) > std::to_underlying(EffectClipboardRequestKind::Item)
        || (request.kind == EffectClipboardRequestKind::Types) != request.mime.empty()
        || request.mime.find('\0') != std::string_view::npos)
        return std::unexpected(EffectCodecError::Malformed);
    return encodeBounded(maxFrameBodyBytes, [&](Writer& writer) {
        writer.fixed(request.requestID, 4);
        writer.fixed(std::to_underlying(request.buffer), 1);
        writer.fixed(std::to_underlying(request.kind), 1);
        writer.text(request.mime, maxEffectClipboardMIMEBytes);
    });
}

std::expected<ClipboardRead, EffectCodecError> decodeClipboardRead(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxFrameBodyBytes)
        return std::unexpected(EffectCodecError::TooLarge);
    WireReader reader(bytes);
    ClipboardRead request;
    request.requestID = reader.u32();
    request.buffer = static_cast<ClipboardBuffer>(reader.u8());
    request.kind = static_cast<EffectClipboardRequestKind>(reader.u8());
    request.mime = reader.utf8(maxEffectClipboardMIMEBytes);
    if (!reader.ok() || !reader.isAtEnd() || !request.requestID || !isValidBuffer(request.buffer)
        || std::to_underlying(request.kind) > std::to_underlying(EffectClipboardRequestKind::Item)
        || (request.kind == EffectClipboardRequestKind::Types) != request.mime.empty()
        || request.mime.find('\0') != std::string_view::npos)
        return std::unexpected(EffectCodecError::Malformed);
    return request;
}

std::expected<EffectCodecBytes, EffectCodecError> encodeClipboardData(const ClipboardData& answer)
{
    if (!answer.requestID || std::to_underlying(answer.status) > std::to_underlying(EffectClipboardStatus::TooLarge)
        || (answer.status != EffectClipboardStatus::OK && (answer.payload.transfer || answer.payload.encodedBytes || !answer.payload.inlineBytes.empty())))
        return std::unexpected(EffectCodecError::Malformed);
    return encodeBounded(maxFrameBodyBytes, [&](Writer& writer) {
        writer.fixed(answer.requestID, 4);
        writer.fixed(std::to_underlying(answer.status), 1);
        if (answer.status == EffectClipboardStatus::OK)
            writePayload(writer, answer.payload, maxEffectClipboardReadBytes);
    });
}

std::expected<ClipboardData, EffectCodecError> decodeClipboardData(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > maxFrameBodyBytes)
        return std::unexpected(EffectCodecError::TooLarge);
    WireReader reader(bytes);
    ClipboardData answer;
    answer.requestID = reader.u32();
    answer.status = static_cast<EffectClipboardStatus>(reader.u8());
    if (answer.status == EffectClipboardStatus::OK)
        answer.payload = readPayload(reader, maxEffectClipboardReadBytes);
    if (!reader.ok() || !reader.isAtEnd() || !answer.requestID
        || std::to_underlying(answer.status) > std::to_underlying(EffectClipboardStatus::TooLarge))
        return std::unexpected(EffectCodecError::Malformed);
    return answer;
}

} // namespace Ubiquitty::Protocol::Sync
