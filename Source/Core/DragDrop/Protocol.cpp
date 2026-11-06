// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/DragDrop/Protocol.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <utility>

namespace Ubiquitty::DragDrop {
namespace {

constexpr std::int64_t minInteger = std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t maxInteger = std::numeric_limits<std::uint32_t>::max();

std::expected<std::int64_t, Error> integer(std::string_view text)
{
    std::int64_t value = 0;
    if (text.empty())
        return std::unexpected(Error::Invalid);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size() || value < minInteger || value > maxInteger)
        return std::unexpected(Error::Invalid);
    return value;
}

bool isType(char value)
{
    return std::string_view("aAmMrRopPeEkqQ").find(value) != std::string_view::npos;
}

bool isMIMECharacter(unsigned char character)
{
    return character > 0x20 && character < 0x7f;
}

} // namespace

std::expected<PacketView, Error> parsePacket(std::string_view body)
{
    const auto separator = body.find(';');
    std::string_view fields = body.substr(0, separator);
    PacketView result;
    if (separator != std::string_view::npos)
        result.payload = body.substr(separator + 1);
    if (fields.size() > maxMetadataBytes || result.payload.size() > maxPacketPayloadBytes)
        return std::unexpected(Error::TooLarge);
    std::array<bool, 128> seen {};
    while (!fields.empty()) {
        const auto colon = fields.find(':');
        const auto pair = fields.substr(0, colon);
        fields = colon == std::string_view::npos ? std::string_view {} : fields.substr(colon + 1);
        if (pair.size() < 3 || pair[1] != '=')
            return std::unexpected(Error::Invalid);
        const auto key = static_cast<unsigned char>(pair[0]);
        if (key >= seen.size() || seen[key])
            return std::unexpected(Error::Invalid);
        seen[key] = true;
        const auto value = pair.substr(2);
        if (key == 't') {
            if (value.size() != 1 || !isType(value[0]))
                return std::unexpected(Error::Invalid);
            result.metadata.type = static_cast<Type>(value[0]);
            result.metadata.hasType = true;
            continue;
        }
        if (std::string_view("xyXYiom").find(static_cast<char>(key)) == std::string_view::npos)
            continue;
        const auto number = integer(value);
        if (!number)
            return std::unexpected(number.error());
        switch (key) {
        case 'x':
            result.metadata.x = *number;
            break;
        case 'y':
            result.metadata.y = *number;
            break;
        case 'X':
            result.metadata.pixelX = *number;
            break;
        case 'Y':
            result.metadata.pixelY = *number;
            break;
        case 'i':
        case 'o':
            if (*number < 0)
                return std::unexpected(Error::Invalid);
            if (key == 'i')
                result.metadata.identity = static_cast<std::uint32_t>(*number);
            else
                result.metadata.operation = static_cast<std::uint32_t>(*number);
            break;
        case 'm':
            if (*number != 0 && *number != 1)
                return std::unexpected(Error::Invalid);
            result.metadata.hasMore = true;
            result.metadata.hasMoreChunks = *number == 1;
            break;
        }
    }
    return result;
}

std::expected<std::size_t, Error> framePacket(const Metadata& metadata, std::string_view payload, std::span<char> output)
{
    if (payload.size() > maxPacketPayloadBytes || !isType(std::to_underlying(metadata.type)))
        return std::unexpected(Error::Invalid);
    std::array<char, maxMetadataBytes> header;
    std::size_t length = 0;
    const auto append = [&](std::string_view text) {
        if (text.size() > header.size() - length)
            return false;
        std::memcpy(header.data() + length, text.data(), text.size());
        length += text.size();
        return true;
    };
    if (!append("\033]72;t="))
        return std::unexpected(Error::TooLarge);
    header[length++] = std::to_underlying(metadata.type);
    const auto field = [&](char key, std::optional<std::int64_t> value) {
        if (!value)
            return true;
        if (*value < minInteger || *value > maxInteger || length + 3 >= header.size())
            return false;
        header[length++] = ':';
        header[length++] = key;
        header[length++] = '=';
        const auto converted = std::to_chars(header.data() + length, header.data() + header.size(), *value);
        if (converted.ec != std::errc {})
            return false;
        length = static_cast<std::size_t>(converted.ptr - header.data());
        return true;
    };
    if (!field('x', metadata.x) || !field('y', metadata.y) || !field('X', metadata.pixelX) || !field('Y', metadata.pixelY)
        || !field('o', metadata.operation) || !field('i', metadata.identity)
        || (metadata.hasMore && !field('m', metadata.hasMoreChunks ? 1 : 0)) || !append(";"))
        return std::unexpected(Error::Invalid);
    if (output.size() < length + 2 || payload.size() > output.size() - length - 2)
        return std::unexpected(Error::TooLarge);
    std::memcpy(output.data(), header.data(), length);
    if (!payload.empty())
        std::memcpy(output.data() + length, payload.data(), payload.size());
    length += payload.size();
    output[length++] = '\033';
    output[length++] = '\\';
    return length;
}

std::string_view errorName(Error error)
{
    switch (error) {
    case Error::Invalid:
        return "EINVAL";
    case Error::TooLarge:
        return "EFBIG";
    case Error::NoMemory:
        return "ENOMEM";
    case Error::NotPermitted:
        return "EPERM";
    case Error::NotFound:
        return "ENOENT";
    case Error::TooManyFiles:
        return "EMFILE";
    case Error::InputOutput:
        return "EIO";
    case Error::Unknown:
        return "EUNKNOWN";
    }
    return "EUNKNOWN";
}

std::expected<std::size_t, Error> countMIMETypes(std::string_view list)
{
    if (list.size() > maxMIMEListBytes)
        return std::unexpected(Error::TooLarge);
    std::size_t count = 0;
    while (!list.empty()) {
        const auto space = list.find(' ');
        const auto mime = list.substr(0, space);
        if (mime.empty() || mime.size() > maxMIMEBytes || mime.find('/') == std::string_view::npos)
            return std::unexpected(Error::Invalid);
        for (const auto character : mime) {
            if (!isMIMECharacter(static_cast<unsigned char>(character)))
                return std::unexpected(Error::Invalid);
        }
        if (++count > maxMIMETypes)
            return std::unexpected(Error::TooLarge);
        if (space == std::string_view::npos)
            break;
        list.remove_prefix(space + 1);
        if (list.empty())
            return std::unexpected(Error::Invalid);
    }
    return count;
}

std::optional<std::string_view> mimeAt(std::string_view list, std::size_t index)
{
    while (!list.empty()) {
        const auto space = list.find(' ');
        if (!index)
            return list.substr(0, space);
        if (space == std::string_view::npos)
            return std::nullopt;
        list.remove_prefix(space + 1);
        --index;
    }
    return std::nullopt;
}

} // namespace Ubiquitty::DragDrop
