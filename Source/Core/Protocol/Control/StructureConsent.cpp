// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/StructureConsent.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/NameLimits.h"

#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

namespace JSON = Ubiquitty::Codec::JSON;

constexpr std::size_t maxMethodBytes = 32;
constexpr std::size_t maxAgentLabelBytes = 256;
constexpr std::size_t maxPeerProcessBytes = 1024;

bool textIsValid(std::string_view value, std::size_t maximum)
{
    return value.size() <= maximum && value.find('\0') == std::string_view::npos
        && isValidUTF8({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
}

/// Whether the parameters are one JSON object the method's schema accepts.
bool parametersAreValid(std::string_view method, const std::string& parameters)
{
    if (parameters.size() > maxStructureConsentParametersBytes)
        return false;
    bool isRead = false;
    const JSON::Value operation = JSON::parse(parameters, isRead);
    const Control::MethodDescriptor* descriptor = Control::findSyncMethod(method);
    return isRead && operation.isObject() && descriptor
        && !Control::validate(operation, Control::parseRegistryJSON(descriptor->parametersSchema));
}

bool isValid(const StructureConsent& request)
{
    return request.requestID && request.sessionID && request.workspaceID && request.revision
        && isStructureMethod(request.method) && textIsValid(request.method, maxMethodBytes)
        && textIsValid(request.workspaceName, Session::maxWorkspaceNameBytes)
        && textIsValid(request.agentLabel, maxAgentLabelBytes) && textIsValid(request.peerProcess, maxPeerProcessBytes)
        && (request.peerProcessID || request.peerProcess.empty())
        && textIsValid(request.parameters, maxStructureConsentParametersBytes);
}

} // namespace

std::optional<std::vector<std::uint8_t>> structureConsentNotify(const StructureConsent& request)
{
    if (!isValid(request) || !parametersAreValid(request.method, request.parameters))
        return std::nullopt;
    // Measured before any byte is copied.
    const std::size_t bytes = varintBytes(std::to_underlying(NotifyKind::StructureConsent)) + 4 + 4 + 4 + 8 + 4
        + utf8Bytes(request.workspaceName) + utf8Bytes(request.method) + utf8Bytes(request.parameters)
        + utf8Bytes(request.agentLabel) + utf8Bytes(request.peerProcess);
    if (bytes > maxFrameBodyBytes)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    body.reserve(bytes);
    WireWriter writer(body);
    writer.varint(std::to_underlying(NotifyKind::StructureConsent));
    writer.u32(request.requestID);
    writer.u32(request.sessionID);
    writer.u32(request.workspaceID);
    writer.u64(request.revision);
    writer.utf8(request.workspaceName);
    writer.utf8(request.method);
    writer.utf8(request.parameters);
    writer.utf8(request.agentLabel);
    writer.utf8(request.peerProcess);
    writer.u32(request.peerProcessID);
    return body;
}

std::optional<StructureConsent> decodeStructureConsent(std::span<const std::uint8_t> record)
{
    WireReader reader(record);
    StructureConsent request;
    request.requestID = reader.u32();
    request.sessionID = reader.u32();
    request.workspaceID = reader.u32();
    request.revision = reader.u64();
    const std::string_view workspaceName = reader.utf8(Session::maxWorkspaceNameBytes);
    const std::string_view method = reader.utf8(maxMethodBytes);
    const std::string_view parameters = reader.utf8(maxStructureConsentParametersBytes);
    const std::string_view agentLabel = reader.utf8(maxAgentLabelBytes);
    const std::string_view peerProcess = reader.utf8(maxPeerProcessBytes);
    request.peerProcessID = reader.u32();
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    request.workspaceName = workspaceName;
    request.method = method;
    request.parameters = parameters;
    request.agentLabel = agentLabel;
    request.peerProcess = peerProcess;
    if (!isValid(request) || !parametersAreValid(request.method, request.parameters))
        return std::nullopt;
    return request;
}

} // namespace Ubiquitty::Protocol::Sync
