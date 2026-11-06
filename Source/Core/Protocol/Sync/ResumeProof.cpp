// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/ResumeProof.h"

#include "Core/Crypto/HMAC.h"
#include "Core/Protocol/Sync/Wire.h"

#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

void putID(WireWriter& writer, const ID128& value)
{
    writer.u64(value.low);
    writer.u64(value.high);
}

} // namespace

std::array<std::uint8_t, 32> resumeProofMAC(ID128 secret, std::span<const std::uint8_t, 32> exporter, ID128 attachmentID, ID128 heldHostInstanceID)
{
    std::vector<std::uint8_t> key;
    WireWriter keyWriter(key);
    putID(keyWriter, secret);
    std::vector<std::uint8_t> message(exporter.begin(), exporter.end());
    WireWriter messageWriter(message);
    putID(messageWriter, attachmentID);
    putID(messageWriter, heldHostInstanceID);
    return Crypto::hmacSHA256(key, message);
}

void encodeResumePreamble(std::vector<std::uint8_t>& output, const ResumeProof& proof)
{
    WireWriter writer(output);
    putID(writer, proof.attachmentID);
    for (const std::uint8_t byte : proof.mac)
        writer.u8(byte);
}

std::optional<ResumeProof> decodeResumePreamble(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != resumeProofBytes)
        return std::nullopt;
    WireReader reader(bytes);
    ResumeProof proof;
    proof.attachmentID.low = reader.u64();
    proof.attachmentID.high = reader.u64();
    for (std::uint8_t& byte : proof.mac)
        byte = reader.u8();
    if (!reader.ok())
        return std::nullopt;
    return proof;
}

void encodePreamble(std::vector<std::uint8_t>& output, const Preamble& preamble)
{
    if (const auto* token = std::get_if<TokenPreamble>(&preamble)) {
        output.push_back(std::to_underlying(PreambleKind::Token));
        output.push_back(static_cast<std::uint8_t>(token->token.size()));
        output.insert(output.end(), token->token.begin(), token->token.end());
        return;
    }
    output.push_back(std::to_underlying(PreambleKind::Resume));
    encodeResumePreamble(output, std::get<ResumeProof>(preamble));
}

std::vector<std::uint8_t> resumePreamble(ID128 secret, std::span<const std::uint8_t, 32> exporter, ID128 attachmentID, ID128 heldHostInstanceID)
{
    std::vector<std::uint8_t> preamble;
    encodePreamble(preamble, ResumeProof { .attachmentID = attachmentID, .mac = resumeProofMAC(secret, exporter, attachmentID, heldHostInstanceID) });
    return preamble;
}

std::optional<std::size_t> PreambleReader::wantedBytes() const
{
    if (m_bytes.empty())
        return 1;
    if (m_bytes[0] == std::to_underlying(PreambleKind::Resume))
        return 1 + resumeProofBytes;
    if (m_bytes[0] != std::to_underlying(PreambleKind::Token))
        return std::nullopt;
    if (m_bytes.size() < 2)
        return 2;
    const std::size_t tokenBytes = m_bytes[1];
    if (tokenBytes == 0 || tokenBytes > maxPreambleTokenBytes)
        return std::nullopt;
    return 2 + tokenBytes;
}

std::size_t PreambleReader::feed(std::span<const std::uint8_t> bytes)
{
    std::size_t taken = 0;
    while (m_status == Status::Incomplete) {
        const std::optional<std::size_t> wanted = wantedBytes();
        if (!wanted) {
            m_status = Status::Malformed;
            m_bytes.clear();
            break;
        }
        if (m_bytes.size() == *wanted) {
            m_status = Status::Complete;
            break;
        }
        if (taken == bytes.size())
            break;
        const std::size_t count = std::min(*wanted - m_bytes.size(), bytes.size() - taken);
        m_bytes.insert(m_bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(taken), bytes.begin() + static_cast<std::ptrdiff_t>(taken + count));
        taken += count;
    }
    return taken;
}

std::optional<Preamble> PreambleReader::take()
{
    if (m_status != Status::Complete)
        return std::nullopt;
    std::optional<Preamble> preamble;
    if (m_bytes[0] == std::to_underlying(PreambleKind::Token))
        preamble = TokenPreamble { .token = std::string(m_bytes.begin() + 2, m_bytes.end()) };
    else if (const std::optional<ResumeProof> proof = decodeResumePreamble(std::span { m_bytes }.subspan(1)))
        preamble = *proof;
    std::vector<std::uint8_t>().swap(m_bytes);
    return preamble;
}

} // namespace Ubiquitty::Protocol::Sync
