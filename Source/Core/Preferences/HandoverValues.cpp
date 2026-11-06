// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/HandoverValues.h"
#include "Core/Preferences/HandoverValuesSchema.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace Ubiquitty::Preferences {
namespace {

namespace Sync = Protocol::Sync;
using HandoverDetail::Schema;
/// The version a record is written as, and the only one read.
constexpr std::uint16_t version = 7;
constexpr std::size_t digestBytes = 16;

template<typename T> struct OptionalValue : std::false_type { };
template<typename T> struct OptionalValue<std::optional<T>> : std::true_type {
    using Value = T;
};
template<typename T> struct VectorValue : std::false_type { };
template<typename T> struct VectorValue<std::vector<T>> : std::true_type {
    using Value = T;
};

template<typename T> constexpr auto lastEnumerator = T::UnsupportedHandoverEnum;
template<> constexpr auto lastEnumerator<SkipReason> = SkipReason::MalformedLine;
template<> constexpr auto lastEnumerator<Themes::TerminalMarkStyle> = Themes::TerminalMarkStyle::Tertiary;
template<> constexpr auto lastEnumerator<ProfileScope> = ProfileScope::Ephemeral;
template<> constexpr auto lastEnumerator<CapabilityDecision> = CapabilityDecision::Prompt;
template<> constexpr auto lastEnumerator<TitlebarStyle> = TitlebarStyle::Native;
template<> constexpr auto lastEnumerator<BeamSurface> = BeamSurface::InTerminal;
template<> constexpr auto lastEnumerator<BackgroundImageMode> = BackgroundImageMode::Center;
template<> constexpr auto lastEnumerator<CursorShape> = CursorShape::Bar;
template<> constexpr auto lastEnumerator<CursorMotionMode> = CursorMotionMode::Always;
template<> constexpr auto lastEnumerator<RemoteClipboardTools> = RemoteClipboardTools::Client;
template<> constexpr auto lastEnumerator<TabShortcutHints> = TabShortcutHints::OnCommand;
template<> constexpr auto lastEnumerator<OptionAsAlternate> = OptionAsAlternate::Both;

class Admission {
public:
    explicit Admission(const HandoverValuesLimits& limits)
        : m_limits(limits)
    {
        charge(sizeof(Values));
    }

    void fail(HandoverValuesError error = HandoverValuesError::Malformed)
    {
        if (!m_error)
            m_error = error;
    }
    [[nodiscard]] std::optional<HandoverValuesError> error() const { return m_error; }
    void charge(std::size_t size)
    {
        if (m_error)
            return;
        if (size > m_limits.maxDecodedBytes - m_decodedBytes)
            fail(HandoverValuesError::OverCapacity);
        else
            m_decodedBytes += size;
    }
    void string(std::uint64_t size)
    {
        if (size > m_limits.maxStringBytes)
            fail(HandoverValuesError::OverCapacity);
        if (!m_error)
            charge(static_cast<std::size_t>(size));
    }
    template<typename T> void collection(std::uint64_t count)
    {
        if (count > m_limits.maxCollectionEntries || count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            fail(HandoverValuesError::OverCapacity);
        if (!m_error)
            charge(static_cast<std::size_t>(count) * sizeof(T));
    }

private:
    const HandoverValuesLimits& m_limits;
    std::size_t m_decodedBytes = 0;
    std::optional<HandoverValuesError> m_error;
};

// A null output measures the same field walk before any section allocation.
class Encoder : public Admission {
public:
    explicit Encoder(const HandoverValuesLimits& limits, std::span<std::uint8_t> output = {})
        : Admission(limits)
        , m_limit(limits.maxSectionBytes)
        , m_output(output)
    {
    }

    void raw(std::span<const std::uint8_t> bytes)
    {
        if (error())
            return;
        if (bytes.size() > m_limit - m_size) {
            fail(HandoverValuesError::OverCapacity);
            return;
        }
        if (!m_output.empty() && !bytes.empty())
            std::memcpy(m_output.data() + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }
    template<typename T> void scalar(T value)
    {
        static_assert(std::is_unsigned_v<T>);
        std::array<std::uint8_t, sizeof(T)> bytes {};
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
        raw(bytes);
    }
    template<typename T> void write(const T& value)
    {
        if (error())
            return;
        if constexpr (OptionalValue<T>::value) {
            scalar(static_cast<std::uint8_t>(value.has_value()));
            if (value)
                write(*value);
        } else if constexpr (VectorValue<T>::value) {
            collection<typename T::value_type>(value.size());
            scalar(static_cast<std::uint64_t>(value.size()));
            for (const auto& entry : value) {
                if (error())
                    break;
                write(entry);
            }
        } else if constexpr (std::is_array_v<T>) {
            for (const auto& entry : value)
                write(entry);
        } else if constexpr (std::is_same_v<T, std::string>) {
            string(value.size());
            scalar(static_cast<std::uint64_t>(value.size()));
            raw({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
        } else if constexpr (std::is_same_v<T, bool>)
            scalar(static_cast<std::uint8_t>(value));
        else if constexpr (std::is_enum_v<T>) {
            const auto rawValue = static_cast<std::uint32_t>(value);
            if (rawValue > static_cast<std::uint32_t>(lastEnumerator<T>))
                fail();
            scalar(rawValue);
        } else if constexpr (std::is_same_v<T, double>) {
            static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
            scalar(std::bit_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, int>) {
            static_assert(sizeof(int) == 4);
            scalar(std::bit_cast<std::uint32_t>(value));
        } else if constexpr (std::is_unsigned_v<T>)
            scalar(value);
        else
            std::apply([&](const auto&... field) { (write(value.*field.member), ...); }, Schema<T>::fields);
    }
    [[nodiscard]] std::size_t size() const { return m_size; }

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
};

class Decoder : public Admission {
public:
    Decoder(std::span<const std::uint8_t> bytes, const HandoverValuesLimits& limits)
        : Admission(limits)
        , m_reader(bytes)
    {
    }

    template<typename T> void read(T* destination)
    {
        if (error())
            return;
        if constexpr (OptionalValue<T>::value) {
            const auto present = m_reader.u8();
            if (present > 1)
                fail();
            else if (present)
                read<typename OptionalValue<T>::Value>(destination ? &destination->emplace() : nullptr);
            else if (destination)
                destination->reset();
        } else if constexpr (VectorValue<T>::value) {
            const auto count = m_reader.u64();
            collection<typename T::value_type>(count);
            // Every encoded element has at least one byte, even when empty.
            if (count > m_reader.remaining())
                fail();
            if (error())
                return;
            if (destination)
                destination->resize(static_cast<std::size_t>(count));
            for (std::size_t i = 0; i < count && !error(); ++i)
                read<typename T::value_type>(destination ? &(*destination)[i] : nullptr);
        } else if constexpr (std::is_array_v<T>) {
            for (std::size_t i = 0; i < std::extent_v<T>; ++i)
                read<std::remove_extent_t<T>>(destination ? &(*destination)[i] : nullptr);
        } else if constexpr (std::is_same_v<T, std::string>) {
            const auto size = m_reader.u64();
            string(size);
            if (size > m_reader.remaining())
                fail();
            if (error())
                return;
            const auto bytes = m_reader.take(static_cast<std::size_t>(size));
            if (destination)
                destination->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        } else if constexpr (std::is_same_v<T, bool>) {
            const auto value = m_reader.u8();
            if (value > 1)
                fail();
            else if (destination)
                *destination = value != 0;
        } else if constexpr (std::is_enum_v<T>) {
            const auto value = m_reader.u32();
            if (value > static_cast<std::uint32_t>(lastEnumerator<T>))
                fail();
            else if (destination)
                *destination = static_cast<T>(value);
        } else if constexpr (std::is_same_v<T, double>) {
            const auto value = m_reader.u64();
            if (destination)
                *destination = std::bit_cast<double>(value);
        } else if constexpr (std::is_same_v<T, int>) {
            const auto value = m_reader.u32();
            if (destination)
                *destination = std::bit_cast<int>(value);
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
            const auto value = m_reader.u32();
            if (destination)
                *destination = value;
        } else if constexpr (std::is_same_v<T, std::uint8_t>) {
            const auto value = m_reader.u8();
            if (destination)
                *destination = value;
        } else
            std::apply([&](const auto&... field) {
                (read<std::remove_reference_t<decltype(std::declval<T>().*field.member)>>(destination ? &(destination->*field.member) : nullptr), ...);
            },
                Schema<T>::fields);
        if (!m_reader.ok())
            fail();
    }
    [[nodiscard]] bool isAtEnd() const { return m_reader.isAtEnd(); }

private:
    Sync::WireReader m_reader;
};

} // namespace

std::expected<std::vector<std::uint8_t>, HandoverValuesError> encodeHandoverValues(
    const Values& values, const HandoverValuesLimits& limits)
{
    Encoder measure(limits);
    measure.scalar(version);
    measure.write(values);
    measure.scalar(std::uint64_t { 0 });
    measure.scalar(std::uint64_t { 0 });
    if (const auto error = measure.error())
        return std::unexpected(*error);
    std::vector<std::uint8_t> bytes(measure.size());
    Encoder writer(limits, bytes);
    writer.scalar(version);
    writer.write(values);
    const auto digest = Sync::digestOf(std::span(bytes).first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    return bytes;
}

std::expected<Values, HandoverValuesError> decodeHandoverValues(
    std::span<const std::uint8_t> bytes, const HandoverValuesLimits& limits)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(HandoverValuesError::OverCapacity);
    Sync::WireReader header(bytes);
    const auto carriedVersion = header.u16();
    if (header.ok() && carriedVersion > version)
        return std::unexpected(HandoverValuesError::NewerVersion);
    if (!header.ok() || carriedVersion != version || bytes.size() < 2 + digestBytes)
        return std::unexpected(HandoverValuesError::Malformed);
    Sync::WireReader trailer(bytes.last(digestBytes));
    const Sync::Digest128 digest { trailer.u64(), trailer.u64() };
    if (digest != Sync::digestOf(bytes.first(bytes.size() - digestBytes)))
        return std::unexpected(HandoverValuesError::DigestMismatch);
    const auto body = bytes.subspan(2, bytes.size() - 2 - digestBytes);
    // The whole record is admitted before any variable-size owner exists.
    Decoder admission(body, limits);
    admission.read<Values>(nullptr);
    if (const auto error = admission.error())
        return std::unexpected(*error);
    if (!admission.isAtEnd())
        return std::unexpected(HandoverValuesError::Malformed);
    Values values;
    Decoder materializer(body, limits);
    materializer.read(&values);
    if (const auto error = materializer.error())
        return std::unexpected(*error);
    return values;
}

} // namespace Ubiquitty::Preferences
