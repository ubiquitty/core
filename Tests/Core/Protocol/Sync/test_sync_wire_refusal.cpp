// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sync_wire_refusal.cpp — complete current bodies or explicit refusal.
//
// Every proper-prefix sweep reaches each field boundary, not merely a few
// short inputs. Calls and Results are checked through both the canonical
// body codec and their correlated application decoder: accepting UTF-8 alone
// is not acceptance of a truncated JSON request or file owner. Descriptor
// handoff remains an explicitly local payload, not a remote Sync message.

#include "Core/Codec/JSON.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Control/FileRead.h"
#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
namespace Control = Ubiquitty::Protocol::Control;
namespace Sync = Ubiquitty::Protocol::Sync;
using Codec::JSON::Value;

namespace {

// Every shorter body must be refused by the complete current decode path.
template<typename Decode>
void everyProperPrefixIsRefused(const char* what, std::span<const std::uint8_t> body, Decode decode)
{
    for (std::size_t n = 0; n < body.size(); ++n) {
        char message[160];
        std::snprintf(message, sizeof(message), "%s accepted a %zu-byte prefix of a %zu-byte body", what, n, body.size());
        CHECK_MSG(!decode(body.first(n)), message);
    }
}

std::vector<std::uint8_t> callBodyOf(std::string text)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Call { .text = std::move(text) });
    return body;
}

std::optional<Control::DecodedRequest> readCall(std::span<const std::uint8_t> body)
{
    const auto call = Sync::decodeCall(body);
    if (!call)
        return std::nullopt;
    auto request = Sync::decodeCallRequest(call->text);
    if (request.outcome != Control::RequestOutcome::Dispatch)
        return std::nullopt;
    return request;
}

std::vector<std::uint8_t> resultBodyOf(const Value& answer)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Result { .transfer = 0, .text = Codec::JSON::serialize(answer) });
    return body;
}

std::optional<Value> readInlineAnswer(std::span<const std::uint8_t> body)
{
    const auto result = Sync::decodeResult(body);
    if (!result || result->transfer)
        return std::nullopt;
    bool isJSON = false;
    auto answer = Codec::JSON::parse(result->text, isJSON);
    return isJSON ? std::optional { std::move(answer) } : std::nullopt;
}

Value fileAnswer(std::string_view requestID = "file.11", std::uint32_t pane = 7,
    std::string_view path = "/tmp/f", std::size_t bytes = 5, std::string_view data = "AQIDBAU=")
{
    auto result = Value::object();
    result.set("pane", Value::number(pane));
    result.set("path", Value::string(std::string(path)));
    result.set("status", Value::string("ok"));
    result.set("bytes", Value::number(static_cast<double>(bytes)));
    result.set("data", Value::string(std::string(data)));
    const auto id = Value::string(std::string(requestID));
    return Control::rpcResult(&id, std::move(result));
}

std::expected<Control::FileReadReply, Control::FileReadFailure> readFileAnswer(std::span<const std::uint8_t> body)
{
    const auto answer = readInlineAnswer(body);
    if (!answer)
        return std::unexpected(Control::FileReadFailure::Invalid);
    return Control::decodeFileReadReply(*answer, "file.11", 7, "/tmp/f");
}

} // namespace

TEST(aRefusedDecodeNeverPopulatesTheBulkField)
{
    const auto body = resultBodyOf(fileAnswer());
    const auto good = readFileAnswer(body);
    REQUIRE(good);
    CHECK_EQ(good->length, std::size_t { 5 });
    CHECK(good->storage != nullptr);

    // FileReadReply is move-only and exists only on successful validation.
    // Every truncated Result/JSON response must return an error, not expose
    // a partial file owner to the consumer.
    everyProperPrefixIsRefused("file.read owner", body, [](auto prefix) {
        const auto decoded = readFileAnswer(prefix);
        if (decoded) {
            CHECK(decoded->storage == nullptr);
            CHECK_EQ(decoded->length, std::size_t { 0 });
        }
        return decoded.has_value();
    });
}

TEST(reclaimSessionRoundTripsAndRefusesEveryShortBody)
{
    const Control::ReclaimRequest sent { .handle = 0xDEADBEEF, .daemonInstanceID = 17 };
    const auto body = callBodyOf(Control::reclaimRequest(sent, "reclaim.7"));
    const auto request = readCall(body);
    REQUIRE(request);
    CHECK(request->method == "pane.reclaim");
    CHECK(request->id.isString() && request->id.asString() == "reclaim.7");
    const auto got = Control::readReclaimRequest(request->params);
    REQUIRE(got);
    CHECK_EQ(got->handle, sent.handle);
    CHECK_EQ(got->daemonInstanceID, sent.daemonInstanceID);
    everyProperPrefixIsRefused("pane.reclaim Call", body, [](auto prefix) {
        const auto decoded = readCall(prefix);
        return decoded && Control::readReclaimRequest(decoded->params).has_value();
    });

    for (const auto& incarnation : { "", "0", "017", "-17", "17x", "18446744073709551616" }) {
        auto parameters = Value::object();
        parameters.set("pane", Value::number(sent.handle));
        parameters.set("daemonInstance", Value::string(incarnation));
        CHECK(!Control::readReclaimRequest(parameters));
    }
    auto unknown = request->params;
    unknown.set("session", Value::number(7));
    CHECK(!Control::readReclaimRequest(unknown));
    CHECK(Control::reclaimRequest({ .handle = 0, .daemonInstanceID = 17 }, "reclaim.7").empty());
    CHECK(Control::reclaimRequest(sent, {}).empty());
}

TEST(reclaimAckRoundTripsAndRefusesEveryShortBody)
{
    const Control::ReclaimTarget sent { .handle = 0x01020304, .sessionID = 9, .daemonInstanceID = 17, .hostInstanceID = { 19, 23 }, .attachmentID = { 29, 31 }, .paneID = { 37, 41 } };
    const auto callID = Value::string("reclaim.7");
    const auto body = resultBodyOf(Control::reclaimReleasedResult(&callID, sent));
    const auto answer = readInlineAnswer(body);
    REQUIRE(answer);
    CHECK(Control::isReclaimReleasedResult(*answer, "reclaim.7", sent));
    everyProperPrefixIsRefused("pane.reclaim Result", body, [&](auto prefix) {
        const auto decoded = readInlineAnswer(prefix);
        return decoded && Control::isReclaimReleasedResult(*decoded, "reclaim.7", sent);
    });

    CHECK(!Control::isReclaimReleasedResult(*answer, "reclaim.other", sent));
    for (int changed = 0; changed < 6; ++changed) {
        auto other = sent;
        switch (changed) {
        case 0:
            ++other.handle;
            break;
        case 1:
            ++other.sessionID;
            break;
        case 2:
            ++other.daemonInstanceID;
            break;
        case 3:
            ++other.hostInstanceID.high;
            break;
        case 4:
            ++other.attachmentID.high;
            break;
        case 5:
            ++other.paneID.high;
            break;
        }
        CHECK(!Control::isReclaimReleasedResult(*answer, "reclaim.7", other));
    }
}

TEST(fileRequestRoundTripsAndRefusesEveryShortBody)
{
    const auto body = callBodyOf(Control::fileReadRequest(7, "/etc/hosts", "file.7"));
    const auto got = readCall(body);
    REQUIRE(got);
    CHECK(got->method == "file.read");
    CHECK(got->id.isString() && got->id.asString() == "file.7");
    CHECK_EQ(got->params.numberOr("pane"), 7);
    CHECK(got->params.stringOr("path") == "/etc/hosts");
    everyProperPrefixIsRefused("file.read Call", body, [](auto prefix) {
        return readCall(prefix).has_value();
    });

    CHECK(Control::fileReadRequest(0, "/etc/hosts", "file.7").empty());
    CHECK(Control::fileReadRequest(7, {}, "file.7").empty());
    CHECK(Control::fileReadRequest(7, "/etc/hosts", {}).empty());
    CHECK(Control::fileReadRequest(7, std::string_view("/tmp/f\0suffix", 13), "file.7").empty());
    const std::string atLimit(Control::maxReadFilePathBytes, 'p');
    const auto large = Control::fileReadRequest(7, atLimit, "file.7");
    CHECK(!large.empty());
    CHECK(Sync::callSizeRefusal(large.size()));
    CHECK(Control::fileReadRequest(7, atLimit + "p", "file.7").empty());
}

TEST(fileDataRoundTripsAndRefusesEveryShortBody)
{
    const auto body = resultBodyOf(fileAnswer());
    const auto got = readFileAnswer(body);
    REQUIRE(got);
    const std::vector<std::uint8_t> expected { 1, 2, 3, 4, 5 };
    CHECK(got->status == Control::FileReadStatus::OK);
    CHECK(got->path == "/tmp/f");
    CHECK(std::ranges::equal(got->data(), expected));
    everyProperPrefixIsRefused("file.read Result", body, [](auto prefix) {
        return readFileAnswer(prefix).has_value();
    });

    CHECK(!readFileAnswer(resultBodyOf(fileAnswer("file.other"))));
    CHECK(!readFileAnswer(resultBodyOf(fileAnswer("file.11", 8))));
    CHECK(!readFileAnswer(resultBodyOf(fileAnswer("file.11", 7, "/other"))));
    CHECK(!readFileAnswer(resultBodyOf(fileAnswer("file.11", 7, "/tmp/f", 6))));
    CHECK(!readFileAnswer(resultBodyOf(fileAnswer("file.11", 7, "/tmp/f", 5, "*QIDBAU="))));
    CHECK(!readFileAnswer(resultBodyOf(fileAnswer("file.11", 7, "/tmp/f", Control::maxReadFileBytes + 1))));
}

TEST(titlePartsRoundTripsAndRefusesEveryShortBody)
{
    Protocol::Sync::PaneFacts sent;
    sent.revision = 7;
    sent.currentDirectory = "/home/x";
    sent.processChain = "vim ◂ -zsh";
    sent.processChainWithArguments = "vim notes.txt ◂ -zsh";
    sent.foreground.name = "vim";
    sent.shellCommandName = "zsh";
    sent.terminalName = "ttys003";
    std::vector<std::uint8_t> frame;
    Protocol::Sync::encodePaneFacts(frame, sent);

    const auto gotDecoded = Protocol::Sync::decodePaneFacts(frame);
    CHECK(gotDecoded.has_value());
    CHECK(gotDecoded && *gotDecoded == sent);

    // The record is whole: every proper prefix cuts a field it carries.
    for (std::size_t length = 0; length < frame.size(); ++length)
        CHECK(!Protocol::Sync::decodePaneFacts(std::span { frame }.first(length)));
}

TEST(titlePartsCarriesAnEmptyRecordAsEmpty)
{
    // The empty record is the one the prefix sweep cannot reach, because it is
    // the SHORTEST valid typed body, with empty fields. Empty fields
    // decode as empty, not as whatever the reader held before.
    Protocol::Sync::PaneFacts sent;
    std::vector<std::uint8_t> frame;
    Protocol::Sync::encodePaneFacts(frame, sent);

    Protocol::Sync::PaneFacts got;
    got.currentDirectory = "stale";
    got.processChain = "stale";
    const auto gotDecoded = Protocol::Sync::decodePaneFacts(frame);
    CHECK(gotDecoded.has_value());
    got = gotDecoded.value_or(got);
    CHECK(got == sent);
}

// Listing uses the canonical workspace structure and per-pane facts rather
// than a second session-list grammar. Every short structure is refused, as
// is a count its complete variable-length record cannot honour.
TEST(sessionListRefusesEveryShortBodyAndACountItCannotHonour)
{
    Session::TreeWorkspace sent;
    sent.id = 17;
    sent.revision = 7;
    sent.name = "work";
    Session::TreeWindow window;
    window.id = Session::Layout::SurfaceID { 2 };
    Session::TreeTab tab;
    tab.id = Session::Layout::TabID { 3 };
    tab.hostFocusedPane = 1;
    tab.split.rows = 40;
    tab.split.columns = 200;
    const Sync::TopologyNode first { .paneID = { 19, 23 }, .rows = 24, .columns = 80, .structuralGeneration = 5 };
    const Sync::TopologyNode second { .paneID = { 29, 31 }, .rows = 40, .columns = 120, .offsetColumns = 80, .structuralGeneration = 6 };
    tab.split.children = { first, second };
    Session::TreePane firstPane;
    firstPane.id = Session::Layout::PaneID { 11 };
    firstPane.sessionID = 0x11111111;
    firstPane.label = "first";
    Session::TreePane secondPane;
    secondPane.id = Session::Layout::PaneID { 12 };
    secondPane.sessionID = 0x22222222;
    secondPane.label = "second";
    tab.panes = { firstPane, secondPane };
    window.tabs.push_back(tab);
    sent.windows.push_back(window);
    std::vector<std::uint8_t> body;
    REQUIRE(Session::encodeWorkspaceStructure(body, sent));
    const auto decoded = Session::decodeWorkspaceStructure(body);
    REQUIRE(decoded);
    CHECK_EQ(decoded->id, sent.id);
    CHECK_EQ(decoded->revision, sent.revision);
    REQUIRE(decoded->windows.size() == 1);
    REQUIRE(decoded->windows[0].tabs.size() == 1);
    const auto& got = decoded->windows[0].tabs[0];
    REQUIRE(got.panes.size() == 2);
    CHECK_EQ(got.hostFocusedPane, std::size_t { 1 });
    CHECK_EQ(got.panes[1].sessionID, secondPane.sessionID);
    CHECK(got.panes[1].label == "second");
    CHECK(got.split == tab.split);
    everyProperPrefixIsRefused("workspace structure", body, [](auto prefix) {
        return Session::decodeWorkspaceStructure(prefix).has_value();
    });

    // Pane content is not duplicated into the structure. These are the
    // canonical records listing publishes beside its corresponding leaves.
    std::vector<Sync::PaneFacts> records(2);
    records[0].revision = 1;
    records[0].columns = 80;
    records[0].rows = 24;
    records[0].programTitle = "first";
    records[0].currentDirectory = "/tmp";
    records[0].foreground.name = "zsh";
    records[1].revision = 2;
    records[1].columns = 120;
    records[1].rows = 40;
    records[1].programTitle = "second";
    records[1].currentDirectory = "/var";
    records[1].foreground.name = "vim";
    for (const auto& record : records) {
        std::vector<std::uint8_t> factsBody;
        Sync::encodePaneFacts(factsBody, record);
        const auto facts = Sync::decodePaneFacts(factsBody);
        REQUIRE(facts);
        CHECK(*facts == record);
        everyProperPrefixIsRefused("listed pane facts", factsBody, [](auto prefix) {
            return Sync::decodePaneFacts(prefix).has_value();
        });
    }

    // Locate the real field with the primitive reader rather than at fixed
    // byte offsets.
    Sync::WireReader reader(body);
    CHECK_EQ(reader.u64(), sent.revision);
    (void)reader.u8(); // state
    (void)reader.u8(); // flags
    CHECK_EQ(reader.u32(), sent.id);
    CHECK(reader.utf8(Session::maxWorkspaceNameBytes) == sent.name);
    CHECK(reader.utf8(Session::Identity::maxLabelLength).empty());
    CHECK_EQ(reader.varint(), 0u); // tags
    const std::size_t countOffset = reader.offset();
    CHECK_EQ(reader.varint(), 1u);
    REQUIRE(reader.ok() && reader.offset() == countOffset + 1);
    std::vector<std::uint8_t> lying = body;
    lying[countOffset] = 2;
    CHECK_MSG(!Session::decodeWorkspaceStructure(lying), "two windows cannot be read from a complete one-window body");
    std::vector<std::uint8_t> pastLimit(body.begin(), body.begin() + countOffset);
    Sync::WireWriter writer(pastLimit);
    writer.varint(static_cast<std::uint64_t>(Session::Layout::maxSurfaces) + 1);
    writer.varint(0);
    CHECK_MSG(!Session::decodeWorkspaceStructure(pastLimit), "an oversized window count is refused before reading its elements");
}

UBIQUITTY_TEST_MAIN()
