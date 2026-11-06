// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Control/HostAdministration.h"

#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Envelope.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <type_traits>
#include <utility>

namespace Ubiquitty::Protocol::Control {

namespace {

using Codec::JSON::Value;
using namespace Sync;

constexpr std::array<std::string_view, 5> administrationMethods {
    "host.management.list", "host.client.disconnect", "host.grant.revoke", "host.token.issue", "host.token.revoke"
};
constexpr std::array<std::string_view, 5> administrationStatuses { "success", "refused", "not-found", "invalid", "failed" };

bool readField(const Value& value, std::string& output)
{
    if (!value.isString() || value.asString().size() > maxAdministrationStringBytes)
        return false;
    output = value.asString();
    return true;
}

bool readField(const Value& value, std::uint32_t& output)
{
    if (!value.isNumber() || !(value.asNumber() >= 0 && value.asNumber() <= std::numeric_limits<std::uint32_t>::max()))
        return false;
    output = static_cast<std::uint32_t>(value.asNumber());
    return value.asNumber() == output;
}

bool readField(const Value& value, std::uint64_t& output)
{
    if (!value.isString() || value.asString().empty() || value.asString().size() > 20)
        return false;
    const auto& text = value.asString();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && std::to_string(output) == text;
}

bool readField(const Value& value, ScopeSet& output)
{
    std::uint32_t bits = 0;
    if (!readField(value, bits) || bits > 0x7f)
        return false;
    output = ScopeSet::fromRaw(static_cast<std::uint8_t>(bits));
    return true;
}

Value fieldValue(const std::string& value) { return Value::string(value); }
Value fieldValue(std::uint32_t value) { return Value::number(value); }
Value fieldValue(std::uint64_t value) { return Value::string(std::to_string(value)); }
Value fieldValue(ScopeSet value) { return Value::number(value.toRaw()); }

class FieldReader {
public:
    explicit FieldReader(const Value& value)
        : m_value(value)
    {
    }

    template<typename T>
    bool field(const char* name, T& output)
    {
        const Value* value = m_value.find(name);
        return value && readField(*value, output);
    }

private:
    const Value& m_value;
};

class FieldWriter {
public:
    template<typename T>
    bool field(const char* name, const T& input)
    {
        if constexpr (std::is_same_v<T, std::string>) {
            if (input.size() > maxAdministrationStringBytes)
                return false;
        } else if constexpr (std::is_same_v<T, ScopeSet>) {
            if (input.toRaw() > 0x7f)
                return false;
        }
        m_value.set(name, fieldValue(input));
        return true;
    }

    Value take() { return std::move(m_value); }

private:
    Value m_value = Value::object();
};

// Each record has one field map shared by the reader and writer.
template<typename Fields, typename Record>
bool recordFields(Fields& fields, Record& record)
{
    using T = std::remove_const_t<Record>;
    if constexpr (std::is_same_v<T, ManagedClient>) {
        return fields.field("client", record.clientID)
            && fields.field("session", record.sessionID)
            && fields.field("attachedUnix", record.attachedUnix)
            && fields.field("principal", record.principal)
            && fields.field("label", record.label)
            && fields.field("transport", record.transport)
            && fields.field("identity", record.identity);
    } else if constexpr (std::is_same_v<T, ManagedGrant>) {
        return fields.field("identifier", record.identifier)
            && fields.field("capability", record.capability)
            && fields.field("origin", record.origin)
            && fields.field("identity", record.identity)
            && fields.field("lifetime", record.lifetime)
            && fields.field("session", record.sessionID)
            && fields.field("grantedUnix", record.grantedUnix)
            && fields.field("grantedBy", record.grantedBy);
    } else {
        static_assert(std::is_same_v<T, ManagedToken>);
        return fields.field("fingerprint", record.fingerprint)
            && fields.field("label", record.label)
            && fields.field("scopes", record.scopes)
            && fields.field("expiryUnix", record.expiryUnix)
            && fields.field("createdUnix", record.createdUnix)
            && fields.field("lastUsedUnix", record.lastUsedUnix)
            && fields.field("lastUsedBy", record.lastUsedBy);
    }
}

template<typename T>
bool writeRecords(Value& output, const char* name, const std::vector<T>& records, std::size_t& remaining)
{
    if (records.size() > maxAdministrationRecords)
        return false;
    auto array = Value::array();
    for (const auto& record : records) {
        FieldWriter writer;
        if (!recordFields(writer, record))
            return false;
        auto value = writer.take();
        const std::size_t bytes = Codec::JSON::serialize(value).size() + 1;
        if (bytes > remaining)
            return false;
        remaining -= bytes;
        array.push(std::move(value));
    }
    output.set(name, std::move(array));
    return true;
}

template<typename T>
bool readRecords(const Value& input, const char* name, std::vector<T>& records)
{
    const auto* array = input.find(name);
    if (!array || !array->isArray() || array->items().size() > maxAdministrationRecords)
        return false;
    records.reserve(array->items().size());
    for (const auto& value : array->items()) {
        T record;
        FieldReader reader(value);
        if (!recordFields(reader, record))
            return false;
        records.push_back(std::move(record));
    }
    return true;
}

bool validIssue(const ManagementRequest& request)
{
    return !request.label.empty() && request.label.size() <= 128
        && std::ranges::none_of(request.label, [](unsigned char byte) { return byte < 32 || byte == 127; })
        && !request.scopes.isEmpty() && request.scopes.toRaw() <= 0x7f && request.expiryUnix;
}

bool selectors(FieldWriter& writer, const ManagementRequest& request)
{
    switch (request.operation) {
    case ManagementOperation::List:
        return true;
    case ManagementOperation::DisconnectClient:
        return request.clientID && writer.field("client", request.clientID);
    case ManagementOperation::RevokeGrant:
    case ManagementOperation::RevokeToken:
        return !request.identity.empty() && writer.field("identity", request.identity);
    case ManagementOperation::IssueToken:
        return validIssue(request) && writer.field("label", request.label)
            && writer.field("scopes", request.scopes) && writer.field("expiryUnix", request.expiryUnix);
    case ManagementOperation::TerminateSession:
        return false;
    }
    return false;
}

bool selectorsMatch(const Value& result, const ManagementRequest& request)
{
    FieldWriter writer;
    if (!selectors(writer, request))
        return false;
    const auto expectedFields = writer.take();
    for (const auto& [name, expected] : expectedFields.members()) {
        const auto* actual = result.find(name);
        if (!actual || Codec::JSON::serialize(*actual) != Codec::JSON::serialize(expected))
            return false;
    }
    return true;
}

} // namespace

std::string_view hostAdministrationMethod(Sync::ManagementOperation operation)
{
    const auto index = static_cast<std::size_t>(operation);
    return index < administrationMethods.size() ? administrationMethods[index] : std::string_view();
}

bool isValidHostAdministrationIntent(const Sync::ManagementRequest& request)
{
    FieldWriter writer;
    return !hostAdministrationMethod(request.operation).empty() && selectors(writer, request);
}

bool isHostAdministrationMethod(std::string_view method)
{
    return std::ranges::find(administrationMethods, method) != administrationMethods.end();
}

std::string hostAdministrationRequest(const Sync::ManagementRequest& request, std::string_view callID)
{
    const auto method = hostAdministrationMethod(request.operation);
    if (!request.requestID || !request.daemonInstanceID || callID.empty() || callID.size() > Sync::maxFrameBodyBytes || method.empty())
        return {};
    FieldWriter writer;
    if (!writer.field("daemonInstance", request.daemonInstanceID) || !selectors(writer, request))
        return {};
    auto call = Value::object();
    call.set("jsonrpc", Value::string("2.0"));
    call.set("id", Value::string(std::string(callID)));
    call.set("method", Value::string(std::string(method)));
    call.set("params", writer.take());
    auto encoded = Codec::JSON::serialize(call);
    if (encoded.size() > Sync::maxFrameBodyBytes)
        return {};
    return encoded;
}

std::optional<Sync::ManagementRequest> readHostAdministrationRequest(std::string_view method, const Value& parameters)
{
    const auto found = std::ranges::find(administrationMethods, method);
    if (found == administrationMethods.end() || !parameters.isObject())
        return std::nullopt;
    ManagementRequest request;
    request.operation = static_cast<ManagementOperation>(found - administrationMethods.begin());
    FieldReader reader(parameters);
    if (!reader.field("daemonInstance", request.daemonInstanceID) || !request.daemonInstanceID)
        return std::nullopt;
    switch (request.operation) {
    case ManagementOperation::List:
        break;
    case ManagementOperation::DisconnectClient:
        if (!reader.field("client", request.clientID) || !request.clientID)
            return std::nullopt;
        break;
    case ManagementOperation::RevokeGrant:
    case ManagementOperation::RevokeToken:
        if (!reader.field("identity", request.identity) || request.identity.empty())
            return std::nullopt;
        break;
    case ManagementOperation::IssueToken:
        if (!reader.field("label", request.label) || !reader.field("scopes", request.scopes)
            || !reader.field("expiryUnix", request.expiryUnix) || !validIssue(request))
            return std::nullopt;
        break;
    case ManagementOperation::TerminateSession:
        return std::nullopt;
    }
    return request;
}

std::optional<Value> hostAdministrationResponse(const Sync::ManagementRequest& request, const Sync::ManagementReply& reply, const Value* callID)
{
    const auto method = hostAdministrationMethod(request.operation);
    const auto status = static_cast<std::size_t>(reply.status);
    const bool isIssue = request.operation == ManagementOperation::IssueToken && reply.status == ManagementStatus::Success;
    if (method.empty() || !request.daemonInstanceID || reply.daemonInstanceID != request.daemonInstanceID
        || status >= administrationStatuses.size() || (!reply.issuedToken.empty() && !isIssue))
        return std::nullopt;
    FieldWriter writer;
    const auto methodName = std::string(method);
    const auto statusName = std::string(administrationStatuses[status]);
    if (!writer.field("method", methodName) || !writer.field("daemonInstance", reply.daemonInstanceID)
        || !writer.field("status", statusName) || !writer.field("detail", reply.detail) || !selectors(writer, request))
        return std::nullopt;
    auto result = writer.take();
    if (request.operation == ManagementOperation::List) {
        // The three names, punctuation and empty brackets fit in this fixed allowance.
        const std::size_t prefixBytes = Codec::JSON::serialize(rpcResult(callID, result)).size() + 64;
        if (prefixBytes > maxAdministrationResultBytes)
            return std::nullopt;
        std::size_t remaining = maxAdministrationResultBytes - prefixBytes;
        if (!writeRecords(result, "clients", reply.clients, remaining)
            || !writeRecords(result, "grants", reply.grants, remaining)
            || !writeRecords(result, "tokens", reply.tokens, remaining))
            return std::nullopt;
    } else if (!reply.clients.empty() || !reply.grants.empty() || !reply.tokens.empty())
        return std::nullopt;
    if (isIssue) {
        if (reply.issuedToken.empty() || reply.issuedToken.size() > maxAdministrationStringBytes)
            return std::nullopt;
        result.set("issuedToken", Value::string(reply.issuedToken));
    }
    auto response = rpcResult(callID, std::move(result));
    if (Codec::JSON::serialize(response).size() > maxAdministrationResultBytes)
        return std::nullopt;
    return response;
}

std::optional<std::size_t> hostAdministrationMutationAnswerBytes(const Sync::ManagementRequest& request, const Value* callID)
{
    if (request.operation == ManagementOperation::List || !isValidHostAdministrationIntent(request))
        return std::nullopt;
    ManagementReply bounded;
    bounded.daemonInstanceID = request.daemonInstanceID;
    // A control byte takes the longest JSON representation (six bytes).
    // Non-issue outcomes use the longest status; a successful issue's bounded
    // secret more than covers the two-byte difference from "not-found".
    bounded.status = request.operation == ManagementOperation::IssueToken ? ManagementStatus::Success : ManagementStatus::NotFound;
    bounded.detail.assign(maxAdministrationStringBytes, '\x01');
    if (request.operation == ManagementOperation::IssueToken)
        bounded.issuedToken.assign(maxAdministrationStringBytes, '\x01');
    const auto answer = hostAdministrationResponse(request, bounded, callID);
    return answer ? std::optional(Codec::JSON::serialize(*answer).size()) : std::nullopt;
}

bool hostAdministrationMutationHasCapacity(std::size_t answerBytes, std::size_t queuedBytes, std::size_t queueLimit)
{
    // An inline Result begins with its one-byte zero transfer varint. A
    // mutation whose worst answer needs bulk is Busy before it changes state:
    // revocation must not wait for a rate-limited transfer to finish.
    return answerBytes <= Sync::maxFrameBodyBytes - 1 && answerBytes <= queueLimit && queuedBytes <= queueLimit - answerBytes;
}

std::optional<Sync::ManagementReply> readHostAdministrationResult(std::string_view response, std::string_view callID, const Sync::ManagementRequest& request)
{
    if (response.size() > maxAdministrationResultBytes || callID.empty())
        return std::nullopt;
    bool isJSON = false;
    const auto answer = Codec::JSON::parse(std::string(response), isJSON);
    const auto* id = answer.find("id");
    if (!isJSON || !id || !id->isString() || id->asString() != callID)
        return std::nullopt;
    ManagementReply reply;
    reply.requestID = request.requestID;
    reply.daemonInstanceID = request.daemonInstanceID;
    reply.status = ManagementStatus::Failed;
    reply.detail = "The host returned an invalid management result";
    if (answer.stringOr("jsonrpc") != "2.0")
        return reply;
    const auto* result = answer.find("result");
    if (const auto* error = answer.find("error")) {
        if (!result && error->isObject()) {
            reply.status = ManagementStatus::Refused;
            const auto* message = error->find("message");
            if (message && message->isString() && message->asString().size() <= maxAdministrationStringBytes)
                reply.detail = message->asString();
        }
        return reply;
    }
    if (!result || !result->isObject() || result->stringOr("method") != hostAdministrationMethod(request.operation)
        || !selectorsMatch(*result, request))
        return reply;
    FieldReader reader(*result);
    std::uint64_t daemon = 0;
    std::string status;
    std::string detail;
    if (!reader.field("daemonInstance", daemon) || daemon != request.daemonInstanceID
        || !reader.field("status", status) || !reader.field("detail", detail))
        return reply;
    const auto found = std::ranges::find(administrationStatuses, status);
    if (found == administrationStatuses.end())
        return reply;
    reply.status = static_cast<ManagementStatus>(found - administrationStatuses.begin());
    reply.detail = std::move(detail);
    const bool isIssue = request.operation == ManagementOperation::IssueToken && reply.status == ManagementStatus::Success;
    if ((result->find("issuedToken") && !isIssue)
        || (request.operation == ManagementOperation::List
            && (!readRecords(*result, "clients", reply.clients)
                || !readRecords(*result, "grants", reply.grants) || !readRecords(*result, "tokens", reply.tokens)))
        || (isIssue && (!reader.field("issuedToken", reply.issuedToken) || reply.issuedToken.empty()))) {
        reply = {};
        reply.requestID = request.requestID;
        reply.daemonInstanceID = request.daemonInstanceID;
        reply.status = ManagementStatus::Failed;
        reply.detail = "The host returned an invalid management result";
    }
    return reply;
}

} // namespace Ubiquitty::Protocol::Control
