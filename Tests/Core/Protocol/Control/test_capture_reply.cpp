// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Accessibility/TextProjection.h"
#include "Core/CellGrid.h"
#include "Core/Protocol/Control/CaptureReply.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/RichText/Blocks.h"
#include "Core/Terminal.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include <limits>
#include <memory>
#include <string>

namespace {

using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Protocol::Control::CaptureReplyStatus;
using Ubiquitty::Protocol::Control::CaptureTarget;
using Ubiquitty::Protocol::Control::decodeCaptureReply;

Value json(const char* text)
{
    bool isParsed = false;
    return Ubiquitty::Codec::JSON::parse(text, isParsed);
}

Value response(std::string text, bool isComplete = true) // bool-ok: serialized completeness data.
{
    Value result = Value::object();
    result.set("pane", Value::number(7));
    result.set("rows", Value::number(2));
    result.set("columns", Value::number(8));
    result.set("complete", Value::boolean(isComplete));
    result.set("text", Value::string(std::move(text)));
    Value answer = Value::object();
    answer.set("jsonrpc", Value::string("2.0"));
    answer.set("id", Value::string("capture.2"));
    answer.set("result", std::move(result));
    return answer;
}

constexpr CaptureTarget target { .pane = 7, .rows = 2, .columns = 8 };

} // namespace

TEST(capturePreservesUnicodeBlankRowsAndEmbeddedLFWithoutCountingThem)
{
    for (const std::string& text : { std::string("\n\n"), std::string("e\xcc\x81  \xf0\x9f\x98\x80\nend\n"), std::string("a\nb\nlast\n") }) {
        const auto decoded = decodeCaptureReply(response(text), "capture.2", target);
        CHECK(decoded.status == CaptureReplyStatus::Complete);
        CHECK(decoded.text == text.substr(0, text.size() - 1));
    }
    const auto empty = decodeCaptureReply(json(R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":1,"columns":1,"complete":true,"text":"\n"}})"), "capture.2", { .pane = 7, .rows = 1, .columns = 1 });
    CHECK(empty.status == CaptureReplyStatus::Complete && empty.text.empty());
}

TEST(captureNeverReturnsPartialOrRefusedTextAsSuccess)
{
    const auto partial = decodeCaptureReply(response("partial\n", false), "capture.2", target);
    CHECK(partial.status == CaptureReplyStatus::Incomplete && partial.text.empty());
    const auto denied = decodeCaptureReply(json(R"({"jsonrpc":"2.0","id":"capture.2","error":{"code":-32002,"message":"pane.capture needs region","data":{"kind":"forbidden"}}})"), "capture.2", target);
    CHECK(denied.status == CaptureReplyStatus::Refused && denied.text.empty());
    CHECK(denied.errorCode == -32002 && denied.reason == "pane.capture needs region");
    const auto late = decodeCaptureReply(response("old\n\n"), "capture.3", target);
    CHECK(late.status == CaptureReplyStatus::Unrelated && late.text.empty());
}

TEST(captureRejectsWrongTargetMalformedMetadataAndAmbiguousEnvelopes)
{
    for (const char* invalid : {
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":2,"columns":8,"complete":true,"text":"x\n"},"error":{"code":-32002,"message":"no"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","id":"capture.2","result":{"pane":7,"rows":2,"columns":8,"complete":true,"text":"x\n"}})",
             R"({"jsonrpc":"1.0","id":"capture.2","result":{"pane":7,"rows":2,"columns":8,"complete":true,"text":"x\n"}})",
             R"({"jsonrpc":"2.0","id":2,"result":{"pane":7,"rows":2,"columns":8,"complete":true,"text":"x\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"text":"old schema\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":8,"rows":2,"columns":8,"complete":true,"text":"wrong pane\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":3,"columns":8,"complete":true,"text":"wrong rows\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":2,"columns":7,"complete":true,"text":"wrong columns\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":2.5,"columns":8,"complete":true,"text":"fraction\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":2,"columns":8,"complete":1,"text":"not boolean\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":2,"columns":8,"complete":true,"text":null}})",
             R"({"jsonrpc":"2.0","id":"capture.2","result":{"pane":7,"rows":2,"columns":8,"complete":true,"text":"x\n","text":"duplicate\n"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","error":{"code":-32002.5,"message":"fractional code"}})",
             R"({"jsonrpc":"2.0","id":"capture.2","error":{"code":-32002,"message":false}})" }) {
        const auto decoded = decodeCaptureReply(json(invalid), "capture.2", target);
        CHECK_MSG(decoded.status == CaptureReplyStatus::Invalid && decoded.text.empty(), invalid);
    }
    for (const std::string& text : { std::string(), std::string("no final separator"), std::string("\xff\n", 2), std::string(200, 'x') + '\n' }) {
        const auto decoded = decodeCaptureReply(response(text), "capture.2", target);
        CHECK(decoded.status == CaptureReplyStatus::Invalid && decoded.text.empty());
    }
    CHECK(decodeCaptureReply(response("\n\n"), "capture.2", { .pane = 0, .rows = 2, .columns = 8 }).status == CaptureReplyStatus::Invalid);
    CHECK(decodeCaptureReply(response("\n\n"), "capture.2", { .pane = 7, .rows = 1025, .columns = 8 }).status == CaptureReplyStatus::Invalid);
}

TEST(captureRangesRequireExactSourceExtentAndCompletion)
{
    using Ubiquitty::Protocol::Control::CaptureSource;
    const auto range = json(R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"rows","first":0,"last":1,"totalRows":2,"columns":8,"complete":true,"text":"one\ntwo\n"}})");
    const CaptureTarget request { .pane = 7, .request = { .source = CaptureSource::Rows, .start = -1, .end = 100 } };
    const auto decoded = decodeCaptureReply(range, "range", request);
    CHECK(decoded.status == CaptureReplyStatus::Complete && decoded.text == "one\ntwo\n");
    for (const char* invalid : {
             R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"rows","first":1,"last":1,"totalRows":2,"columns":8,"complete":true,"text":"two\n"}})",
             R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"rows","first":0,"last":0,"totalRows":2,"columns":8,"complete":true,"text":"one\n"}})",
             R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"rows","first":0,"last":2,"totalRows":2,"columns":8,"complete":true,"text":"one\n"}})",
             R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"tail","first":0,"last":1,"totalRows":2,"columns":8,"complete":true,"text":"one\ntwo\n"}})",
             R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"rows","first":0,"last":1,"totalRows":2.5,"columns":8,"complete":true,"text":"one\ntwo\n"}})" }) {
        const auto refused = decodeCaptureReply(json(invalid), "range", request);
        CHECK(refused.status == CaptureReplyStatus::Invalid && refused.text.empty());
    }
    const auto incomplete = decodeCaptureReply(json(R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"rows","first":0,"last":0,"totalRows":2,"columns":8,"complete":false,"text":"one\n"}})"), "range", request);
    CHECK(incomplete.status == CaptureReplyStatus::Incomplete && incomplete.text.empty());
}

TEST(captureTailAndCommandKeepFinalNewlineWithoutReplicaGeometry)
{
    using Ubiquitty::Protocol::Control::CaptureSource;
    const CaptureTarget tail { .pane = 7, .request = { .source = CaptureSource::Tail, .lines = 1 } };
    const auto answer = json(R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"tail","first":9,"last":9,"totalRows":10,"columns":8,"complete":true,"text":"last\n"}})");
    CHECK(decodeCaptureReply(answer, "range", tail).text == "last\n");
    const CaptureTarget wrongCount { .pane = 7, .request = { .source = CaptureSource::Tail, .lines = 2 } };
    CHECK(decodeCaptureReply(answer, "range", wrongCount).status == CaptureReplyStatus::Invalid);
    const CaptureTarget command { .pane = 7, .request = { .source = CaptureSource::LastCommand } };
    const auto output = decodeCaptureReply(json(R"({"jsonrpc":"2.0","id":"range","result":{"pane":7,"source":"last-command","first":3,"last":4,"totalRows":10,"columns":8,"complete":true,"text":"out\n\n"}})"), "range", command);
    CHECK(output.status == CaptureReplyStatus::Complete && output.text == "out\n\n");
}

TEST(screenCaptureIncludesUnicodeOverlaysAndUsesTheSameProjectionAsAccessibility)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 40);
    UbiquittyTest::feed(terminal, "grid label\x1b[2;3H\x1b]7404;i=7:w=20:h=1:g=1:P=1:C=1;{\\rtf1 I’m office العربية}\x1b\\");
    const auto result = Ubiquitty::Protocol::Control::captureScreen(terminal, 7);
    const auto projection = Ubiquitty::Accessibility::projectVisibleText(terminal, 0);
    CHECK(result.stringOr("text") == projection.text + '\n');
    CHECK(projection.text.find("grid label") != std::string::npos);
    CHECK(projection.text.find("I’m office العربية") != std::string::npos);
    const auto* richText = result.find("richText");
    REQUIRE(richText);
    REQUIRE(richText->find("blocks")->items().size() == 1);
    CHECK(richText->find("blocks")->items()[0].stringOr("text") == "I’m office العربية");
    const Value id = Value::string("capture.2");
    const auto decoded = decodeCaptureReply(Ubiquitty::Protocol::Control::rpcResult(&id, result), "capture.2", { .pane = 7, .rows = 4, .columns = 40 });
    CHECK(decoded.status == CaptureReplyStatus::Complete);
}

TEST(screenCaptureStopsAfterTheWholeRowThatReachesTheTextBound)
{
    // Each cell is two four-byte scalars, so a 1024-column row is 8 KiB and
    // its LF: the 128th row reaches 1 MiB. That row is returned whole, the
    // rows past it are omitted, and only the omission makes it incomplete.
    for (const int rows : { 128, 129 }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, rows, 1024);
        Ubiquitty::Cell filled {};
        filled.codePoint = 0x13000;
        filled.combining[0] = 0x1D185;
        filled.setCombiningLength(1);
        for (int row = 0; row < rows; ++row)
            terminal.grid->fillCellRun(Ubiquitty::Row(row), Ubiquitty::Column(0), filled, 1024);
        const auto result = Ubiquitty::Protocol::Control::captureScreen(terminal, 7);
        const auto projection = Ubiquitty::Accessibility::projectVisibleText(terminal, 0);
        const std::size_t rowBytes = 1024u * 8u + 1u;
        CHECK(result.boolOr("complete") == (rows == 128));
        CHECK(result.stringOr("text").size() == 128u * rowBytes);
        CHECK(result.stringOr("text") == projection.text.substr(0, 128u * rowBytes - 1) + '\n');
    }
}

TEST(captureProjectsOnlyTheNativeViewportAndRejectsStaleGeometry)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 4, 20);
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.baseline = 12 * 64;
    facts.proportionalSize = 12 * 64;
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;i=4:D=4294967295:w=10:h=1:g=1:P=1:C=1:v=7;{\\rtf1 hidden\\line office\\line end}\x1b\\");
    requestBlockMeasurements(terminal);
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    const auto pending = terminal.richTextMeasurements.front();
    Measurement measured;
    measured.isShaped = true;
    measured.geometry.lines = { { 0, 7, 0, 65536, 49152, 0, 32768 }, { 7, 14, 65536, 65536, 49152, 0, 32768 }, { 14, 17, 131072, 65536, 49152, 0, 16384 } };
    measured.geometry.spans = { { 0, 7, 0, 0, 32768 }, { 7, 14, 1, 0, 32768 }, { 14, 17, 2, 0, 16384 } };
    REQUIRE(adoptBlockMeasurement(terminal, pending, measured));
    auto malformed = measured;
    malformed.geometry.spans.front().end = 1000;
    CHECK(!adoptBlockMeasurement(terminal, pending, std::move(malformed)));
    const auto snapshot = Protocol::Control::captureScreen(terminal, 7);
    CHECK(snapshot.stringOr("text").find("office") != std::string::npos);
    CHECK(snapshot.stringOr("text").find("hidden") == std::string::npos);
    CHECK(snapshot.stringOr("text").find("end") == std::string::npos);
    const auto& block = snapshot.find("richText")->find("blocks")->items()[0];
    CHECK_EQ(block.numberOr("documentRevision"), 4294967295.0);
    CHECK_EQ(block.numberOr("sourceAnchor"), 7.0);
    CHECK_EQ(block.numberOr("viewportTop"), 1.0);
    CHECK(block.find("shaped")->asBool());
    CHECK(block.find("visible")->items()[0].find("first")->asNumber() == 7);
    const Value id = Value::string("capture.2");
    auto decoded = decodeCaptureReply(Protocol::Control::rpcResult(&id, snapshot), "capture.2", { .pane = 7, .rows = 4, .columns = 20 });
    CHECK(decoded.status == CaptureReplyStatus::Complete);
    CHECK(decoded.screen.isObject());
    ++terminal.typography.epoch;
    CHECK(!adoptBlockMeasurement(terminal, pending, measured));
    decoded = decodeCaptureReply(Protocol::Control::rpcResult(&id, Protocol::Control::captureScreen(terminal, 7)), "capture.2", { .pane = 7, .rows = 4, .columns = 20 });
    CHECK(decoded.status == CaptureReplyStatus::Pending && decoded.text.empty());
    UbiquittyTest::feed(terminal, "\x1b[2J");
    CHECK(!adoptBlockMeasurement(terminal, pending, measured));
    CHECK(Protocol::Control::captureScreen(terminal, 7).find("richText")->find("blocks")->items().empty());
}

TEST(remoteEllipsisCaptureWaitsForTheNativeSourceCut)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 2, 20);
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.baseline = 12 * 64;
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    UbiquittyTest::feed(terminal, "\x1b]7404;i=4:w=10:h=1:g=1:o=e:P=1:C=1;{\\rtf1 abcdefgh}\x1b\\");
    const Value id = Value::string("capture.2");
    const auto waiting = Protocol::Control::captureScreen(terminal, 7);
    CHECK(!waiting.boolOr("complete"));
    CHECK(waiting.find("richText")->boolOr("layoutPending"));
    CHECK(waiting.find("richText")->find("blocks")->items()[0].find("visible")->items().empty());
    const auto pendingReply = decodeCaptureReply(Protocol::Control::rpcResult(&id, waiting), "capture.2", { .pane = 7, .rows = 2, .columns = 20 });
    CHECK(pendingReply.status == CaptureReplyStatus::Pending && pendingReply.text.empty());
    requestBlockMeasurements(terminal);
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    const auto& pending = terminal.richTextMeasurements.front();
    CHECK(pending.request.shape.overflow == Overflow::Ellipsis);
    Measurement measured;
    measured.isShaped = true;
    measured.geometry.lines = { { 0, 4, 0, 65536, 49152, 0, 16384 }, { 4, 8, 65536, 65536, 49152, 0, 16384 } };
    for (std::uint32_t index = 0; index < 8; ++index)
        measured.geometry.spans.push_back({ index, index + 1, index / 4, static_cast<std::int32_t>((index % 4) * 4096), static_cast<std::int32_t>((index % 4 + 1) * 4096) });
    measured.geometry.elisionEnd = 3;
    measured.geometry.elision = std::make_shared<ShapedText>();
    measured.geometry.elision->lines = { measured.geometry.lines.front() };
    measured.geometry.elision->spans.assign(measured.geometry.spans.begin(), measured.geometry.spans.begin() + 4);
    REQUIRE(adoptBlockMeasurement(terminal, pending, std::move(measured)));
    const auto ready = Protocol::Control::captureScreen(terminal, 7);
    CHECK(ready.boolOr("complete"));
    CHECK(!ready.find("richText")->boolOr("layoutPending"));
    const auto& block = ready.find("richText")->find("blocks")->items()[0];
    CHECK(block.boolOr("shaped"));
    CHECK(block.stringOr("text") == "abcdefgh");
    REQUIRE(block.find("visible")->items().size() == 1);
    CHECK_EQ(block.find("visible")->items()[0].numberOr("end"), 3.0);
    CHECK(ready.stringOr("text") == "abc\n\n");
    const auto readyReply = decodeCaptureReply(Protocol::Control::rpcResult(&id, ready), "capture.2", { .pane = 7, .rows = 2, .columns = 20 });
    CHECK(readyReply.status == CaptureReplyStatus::Complete);
    CHECK(readyReply.text == "abc\n");
}

TEST(richTextCapturePreservesFontIntentAndAccessibilityCaretUnits)
{
    using namespace Ubiquitty;
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 2, 40);
    UbiquittyTest::feed(terminal, "\x1b]7404;i=8:w=30:h=1:g=1:P=1:C=1:k=4;{\\rtf1{\\fonttbl{\\f0\\fswiss ;}{\\f1\\fmodern ;}}\\f0 😀 {\\f1 code}   }\x1b\\");
    auto projection = Accessibility::projectVisibleText(terminal, 0);
    CHECK(Accessibility::caretInProjection(projection, terminal, 0).location == 5);
    const auto snapshot = Protocol::Control::captureScreen(terminal, 7);
    const auto& runs = snapshot.find("richText")->find("blocks")->items()[0].find("runs")->items();
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].stringOr("familyClass") == "sans-serif");
    CHECK(runs[1].stringOr("familyClass") == "monospace");
    CHECK(runs[2].stringOr("familyClass") == "sans-serif");
    UbiquittyTest::feed(terminal, "\x1b]7404;i=8:w=30:h=1:g=1:P=1:C=1:k=9;{\\rtf1 😀 code   }\x1b\\");
    projection = Accessibility::projectVisibleText(terminal, 0);
    CHECK(Accessibility::caretInProjection(projection, terminal, 0).location == projection.lineRanges[0].length);
}

TEST(captureDistinguishesTheSharedSourceAnchorFromEachRenderersResolvedRow)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    for (std::uint32_t perLine : { 2, 4 }) {
        Terminal terminal;
        hardReset(terminal);
        resize(terminal, 4, 20);
        TypographyFacts facts;
        facts.cellWidth = 8 * 64;
        facts.cellHeight = 16 * 64;
        facts.baseline = 12 * 64;
        facts.shaping = 1;
        facts.rendererRevision = perLine;
        REQUIRE(adoptTypography(terminal, facts));
        terminal.typography.shaper = { [](void* context, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                                          const auto perLine = *static_cast<const std::uint32_t*>(context);
                                          const auto height = static_cast<std::uint32_t>(facts->cellHeight) * 64;
                                          const auto baseline = static_cast<std::uint32_t>(facts->baseline) * 64;
                                          const auto count = static_cast<std::uint32_t>(document->text.size());
                                          for (std::uint32_t first = 0; first < count; first += perLine) {
                                              const auto end = std::min(count, first + perLine);
                                              const auto line = first / perLine;
                                              output->appendLine({ first, end, line * height, height, baseline, 0, (end - first) * 4096 });
                                              for (auto index = first; index < end; ++index)
                                                  output->appendSpan({ index, index + 1, line, static_cast<std::int32_t>((index - first) * 4096), static_cast<std::int32_t>((index - first + 1) * 4096) });
                                          }
                                          return true;
                                      },
            &perLine };
        UbiquittyTest::feed(terminal, "\x1b]7404;i=4:D=99:w=10:h=1:g=1:P=1:C=1:v=7;{\\rtf1 abcdefghi}\x1b\\");
        const auto snapshot = Protocol::Control::captureScreen(terminal, 7);
        const auto& block = snapshot.find("richText")->find("blocks")->items()[0];
        CHECK(block.boolOr("shaped"));
        CHECK_EQ(block.numberOr("sourceAnchor"), 7.0);
        CHECK_EQ(block.numberOr("documentRevision"), 99.0);
        CHECK_EQ(block.numberOr("viewportTop"), static_cast<double>(7 / perLine));
        const auto& first = block.find("visible")->items()[0];
        CHECK_EQ(first.numberOr("first"), static_cast<double>((7 / perLine) * perLine));
        CHECK_EQ(first.numberOr("row"), 0.0);
        const Value id = Value::string("capture.2");
        CHECK(decodeCaptureReply(Protocol::Control::rpcResult(&id, snapshot), "capture.2", { .pane = 7, .rows = 4, .columns = 20 }).status == CaptureReplyStatus::Complete);
        terminal.typography.shaper = {};
    }
}

TEST(captureAcceptsTheNativeSignedCoordinateEnvelope)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 2, 20);
    TypographyFacts facts;
    facts.cellWidth = maxCellSize;
    facts.cellHeight = maxCellSize;
    facts.baseline = maxCellSize;
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    terminal.typography.shaper = { [](void*, const Document*, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                                      constexpr auto edge = std::numeric_limits<std::int32_t>::max() - 1;
                                      const auto height = static_cast<std::uint32_t>(facts->cellHeight) * 64;
                                      output->appendLine({ 0, 1, 0, height, height, 0, edge });
                                      output->appendSpan({ 0, 1, 0, 0, edge });
                                      return true;
                                  },
        nullptr };
    UbiquittyTest::feed(terminal, "\x1b]7404;i=1:w=10:h=1:g=1:P=1:C=1;{\\rtf1 x}\x1b\\");
    const auto snapshot = Protocol::Control::captureScreen(terminal, 7);
    const auto& block = snapshot.find("richText")->find("blocks")->items()[0];
    REQUIRE(block.boolOr("shaped"));
    CHECK_EQ(block.find("lines")->items()[0].numberOr("width"), (std::numeric_limits<std::int32_t>::max() - 1) / 4096.0);
    const Value id = Value::string("capture.2");
    CHECK(decodeCaptureReply(Protocol::Control::rpcResult(&id, snapshot), "capture.2", { .pane = 7, .rows = 2, .columns = 20 }).status == CaptureReplyStatus::Complete);
    terminal.typography.shaper = {};
}

TEST(captureGeometrySchemaUsesItsActualNumericRepresentations)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Protocol::Control;
    const auto schema = parseRegistryJSON(findSyncMethod("pane.capture")->resultSchema);
    const auto& richText = *schema.find("properties")->find("richText")->find("properties");
    const auto& block = *richText.find("blocks")->find("items")->find("properties");
    const auto maximum = std::numeric_limits<std::int32_t>::max() / 4096.0;
    for (const auto collection : { "lines", "graphemes" }) {
        const auto& properties = *block.find(collection)->find("items")->find("properties");
        for (const auto& [name, field] : properties.members()) {
            if (field.stringOr("type") != "number")
                continue;
            CHECK_EQ(field.numberOr("maximum"), maximum);
            CHECK(!validate(Value::number(maximum), field));
            CHECK(validate(Value::number(maximum + 1.0 / 4096), field).has_value());
        }
    }
    for (const auto name : { "cellWidth", "cellHeight" }) {
        const auto& field = *richText.find(name);
        CHECK_EQ(field.numberOr("maximum"), RichText::maxCellSize / 64.0);
        CHECK(!validate(Value::number(RichText::maxCellSize / 64.0), field));
        CHECK(validate(Value::number((RichText::maxCellSize + 1) / 64.0), field).has_value());
    }
    CHECK(!validate(Value::number(RichText::maxCodePoints), *block.find("sourceAnchor")));
    CHECK(validate(Value::number(RichText::maxCodePoints + 1), *block.find("sourceAnchor")).has_value());
    CHECK(!validate(Value::number(std::numeric_limits<std::uint32_t>::max()), *block.find("documentRevision")));
    CHECK(validate(Value::number(4294967296.0), *block.find("documentRevision")).has_value());
}

TEST(captureTextAllowanceFollowsTheReturnedDocuments)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 36, 2);
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.baseline = 12 * 64;
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    terminal.typography.shaper = { [](void*, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                                      const auto end = static_cast<std::uint32_t>(document->text.size());
                                      output->appendLine({ 0, end, 0, static_cast<std::uint32_t>(facts->cellHeight) * 64, static_cast<std::uint32_t>(facts->baseline) * 64, 0, 4096 });
                                      output->appendSpan({ 0, end, 0, 0, 4096 });
                                      return true;
                                  },
        nullptr };
    for (int row = 1; row <= terminal.rows; ++row)
        UbiquittyTest::feed(terminal, "\x1b[" + std::to_string(row) + ";1H\x1b]7404;i=" + std::to_string(row) + ":w=1:h=1:g=1:P=1:C=1;{\\rtf1 " + std::string(maxCodePoints, 'a') + "}\x1b\\");
    REQUIRE(terminal.richTextBlocks.blocks(ScreenMode::Primary).size() == 36);
    const auto snapshot = Protocol::Control::captureScreen(terminal, 7);
    CHECK(snapshot.stringOr("text").size() > 65536);
    const Value id = Value::string("capture.2");
    const auto decoded = decodeCaptureReply(Protocol::Control::rpcResult(&id, snapshot), "capture.2", { .pane = 7, .rows = 36, .columns = 2 });
    CHECK(decoded.status == CaptureReplyStatus::Complete);
    CHECK_EQ(decoded.text.size(), static_cast<std::size_t>(36 * (maxCodePoints + 1) - 1));
    terminal.typography.shaper = {};
}

UBIQUITTY_TEST_MAIN()
