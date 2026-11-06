// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "SyncLink.h"
#include "TerminalFixture.h"

#include "Core/OSC/RichTextBlocks.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/RichText/Layout.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/HandoverQuiesce.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "PAL/TerminalClients.h"

#include <array>
#include <limits>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::RichText;
using namespace Ubiquitty::Protocol::Sync;

namespace {

TypographyFacts catalogFacts(FontCatalogScope scope = FontCatalogScope::System)
{
    TypographyFacts facts;
    facts.cellWidth = 8 * 64;
    facts.cellHeight = 16 * 64;
    facts.rendererRevision = 7;
    facts.rendersBlocks = 1;
    facts.fontCatalogScope = scope;
    return facts;
}

struct Catalog {
    std::vector<std::string> families { "Alpha", "Beta;{}\\", "字体" };
    unsigned calls = 0;
    FontCatalogStatus status = FontCatalogStatus::Ready;

    FontCatalogProvider provider()
    {
        return { [](void* context, const FontCatalogQuery* query, const TypographyFacts* facts, FontCatalogPage* page) {
                    auto& catalog = *static_cast<Catalog*>(context);
                    ++catalog.calls;
                    page->status = catalog.status;
                    page->scope = facts->fontCatalogScope;
                    page->offset = query->offset;
                    page->rendererRevision = query->rendererRevision;
                    if (page->status != FontCatalogStatus::Ready)
                        return true;
                    if (query->offset > catalog.families.size()) {
                        page->status = FontCatalogStatus::Invalid;
                        return true;
                    }
                    page->total = static_cast<std::uint32_t>(catalog.families.size());
                    for (std::size_t index = query->offset; index < catalog.families.size() && page->families.size() < query->count; ++index) {
                        if (!page->appendFamily(catalog.families[index]))
                            break;
                    }
                    return true;
                },
            this };
    }
};

class CatalogClock final : public TerminalClock {
public:
    Ticks now() override { return instant; }
    Ticks instant;
};

class DeniedCatalog final : public CapabilityPolicy {
public:
    CapabilityDecision check(Capability capability, CapabilityOrigin, std::string_view) override
    {
        return capability == Capability::TextMeasureRead ? CapabilityDecision::Deny : CapabilityDecision::Allow;
    }
};

std::string drainCatalog(Terminal& terminal)
{
    std::array<char, maxFontCatalogPageBytes * 2 + 2048> bytes {};
    const auto length = UbiquittyTest::drain(terminal, bytes);
    return { bytes.data(), static_cast<std::size_t>(std::max(length, 0)) };
}

void ask(Terminal& terminal, std::string_view keys)
{
    UbiquittyTest::feed(terminal, "\x1b]7404;" + std::string(keys) + "\x1b\\");
}

MeasureRequest catalogRequest(std::uint32_t token, std::uint32_t offset = 0, std::uint16_t count = 2)
{
    MeasureRequest request;
    request.token = token;
    request.id = 19;
    request.rendererRevision = 7;
    request.fontCatalog = FontCatalogQuery { offset, count, 7 };
    return request;
}

bool decodePage(const Measurement& measured)
{
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writeMeasurement(writer, measured);
    CHECK(bytes.size() <= maxRichTextFontCatalogBytes);
    WireReader reader(bytes);
    Measurement decoded;
    return readMeasurement(reader, decoded) && reader.isAtEnd();
}

} // namespace

TEST(catalogFamiliesAreExactBoundedUTF8Identifiers)
{
    for (const auto name : { "Alpha", "字体", "A;{}\\", "A B" })
        CHECK(isFontCatalogFamily(name));
    for (const auto name : { "", " Alpha", "Alpha ", "A\nB", "A\x1b]B", "A\x7f", "A\xc2\x80", "A\xc2\x9f", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80" })
        CHECK(!isFontCatalogFamily(name));
    CHECK(isFontCatalogFamily(std::string(maxFamilyNameBytes, 'A')));
    CHECK(!isFontCatalogFamily(std::string(maxFamilyNameBytes + 1, 'A')));
    FontCatalogPage page;
    for (unsigned index = 0; index < maxFontCatalogPageFamilies; ++index)
        REQUIRE(page.appendFamily("A"));
    CHECK(!page.appendFamily("A"));
    page.families.clear();
    std::size_t bytes = 0;
    while (page.appendFamily(std::string(maxFamilyNameBytes, 'A')))
        bytes += maxFamilyNameBytes;
    CHECK(bytes <= maxFontCatalogPageBytes);
    CHECK(bytes + maxFamilyNameBytes > maxFontCatalogPageBytes);
}

TEST(publishedFaceNamesUseTheSameUTF8IdentifierBoundaryAsCatalogs)
{
    for (const std::string_view name : { "", "字体", "A;{}\\", "Leading ", "A\x1b", "A\xc2\x85", "\xc0\xaf" }) {
        auto facts = catalogFacts();
        facts.faceCount = 1;
        facts.faces[0].lineHeight = 2048;
        facts.faces[0].nameLength = static_cast<std::uint8_t>(name.size());
        std::ranges::copy(name, facts.faces[0].name.begin());
        const bool valid = name.empty() || isFontCatalogFamily(name);
        CHECK(isAdoptable(facts) == valid);
        std::vector<std::uint8_t> body;
        encode(body, facts);
        CHECK(decodeTypographyFacts(body).has_value() == valid);
    }
}

TEST(fontCatalogPagesAreEpochPinnedAndEncodeNamesWithoutTerminalControls)
{
    Terminal terminal;
    resize(terminal, 24, 80);
    Catalog catalog;
    terminal.typography.fontCatalog = catalog.provider();
    REQUIRE(adoptTypography(terminal, catalogFacts()));
    ask(terminal, "a=f:i=19:n=2");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=f:i=19:v=3:E=1:F=2:b=0:n=2:t=3:s=ok;416c706861,426574613b7b7d5c\x1b\\");
    ask(terminal, "a=f:i=20:b=2:n=2:E=1");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=f:i=20:v=3:E=1:F=2:b=2:n=1:t=3:s=ok;e5ad97e4bd93\x1b\\");
    ask(terminal, "a=f:i=21:b=3:E=1");
    CHECK(drainCatalog(terminal).find(":b=3:n=0:t=3:s=ok;") != std::string::npos);
    ask(terminal, "a=f:i=22:b=4:E=1");
    CHECK(drainCatalog(terminal).find(":b=4:n=0:t=0:s=invalid;") != std::string::npos);
    auto changed = catalogFacts();
    ++changed.rendererRevision;
    REQUIRE(adoptTypography(terminal, changed));
    ask(terminal, "a=f:i=23:b=2:E=1:e=1");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=23:v=3:s=stale\x1b\\");
    CHECK_EQ(catalog.calls, 4U);
}

TEST(catalogDiscoveryDistinguishesUnavailableExposedAndSystem)
{
    Terminal terminal;
    resize(terminal, 24, 80);
    ask(terminal, "a=f:i=1");
    CHECK(drainCatalog(terminal).find(":F=0:b=0:n=0:t=0:s=unavailable;") != std::string::npos);
    Catalog catalog;
    catalog.families.clear();
    terminal.typography.fontCatalog = catalog.provider();
    for (const auto scope : { FontCatalogScope::Exposed, FontCatalogScope::System }) {
        REQUIRE(adoptTypography(terminal, catalogFacts(scope)));
        ask(terminal, "a=q:i=2:w=0:h=0");
        const auto discovery = drainCatalog(terminal);
        CHECK(discovery.find(":F=" + std::to_string(std::to_underlying(scope)) + ':') != std::string::npos);
        CHECK(discovery.find("catalog-count=255,catalog-bytes=16212") != std::string::npos);
        ask(terminal, "a=f:i=3");
        CHECK(drainCatalog(terminal).find(":n=0:t=0:s=ok;") != std::string::npos);
    }
    catalog.status = FontCatalogStatus::Denied;
    ask(terminal, "a=f:i=4");
    CHECK(drainCatalog(terminal).find(":n=0:t=0:s=denied;") != std::string::npos);
    DeniedCatalog policy;
    terminal.capabilityPolicy = &policy;
    const auto calls = catalog.calls;
    ask(terminal, "a=f:i=5:e=1");
    CHECK_EQ(catalog.calls, calls);
    CHECK(drainCatalog(terminal).empty());
    terminal.capabilityPolicy = nullptr;
}

TEST(fontCatalogGrammarRejectsAmbiguousContinuationAndIrrelevantKeys)
{
    for (const auto keys : { "a=f", "a=f:i=1:b=1", "a=f:i=1:n=0", "a=f:i=1:n=256", "a=f:i=1:w=0", "a=f:i=1:G=0", "a=f:i=1:N=1", "a=f:i=1:b=0:b=1", "a=q:i=1:w=0:h=0:n=1", "a=f:i=1:b=4294967296:E=1" })
        CHECK(!OSC::RichTextBlocks::parseKeys(keys));
    CHECK(OSC::RichTextBlocks::parseKeys("a=f:i=0:b=0:n=255"));
    CHECK(OSC::RichTextBlocks::parseKeys("a=f:i=1:b=4294967295:E=4294967295"));
    Terminal terminal;
    resize(terminal, 24, 80);
    ask(terminal, "a=f:i=1:e=1;ignored");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=1:v=3:s=invalid\x1b\\");
}

TEST(completedFontPagesSurviveOutputPressureWithoutRepeatingTheProvider)
{
    Terminal terminal;
    resize(terminal, 24, 80);
    Catalog catalog;
    terminal.typography.fontCatalog = catalog.provider();
    REQUIRE(adoptTypography(terminal, catalogFacts()));
    const std::string pressure(VT::maxBulkOutputBytes, 'x');
    REQUIRE(terminal.bulkOutput.append(pressure, VT::maxBulkOutputBytes));
    ask(terminal, "a=f:i=1:n=2:e=1");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    REQUIRE(terminal.richTextMeasurements.front().completion);
    CHECK_EQ(terminal.bulkOutput.length, VT::maxBulkOutputBytes);
    OSC::RichTextBlocks::expireMeasurements(terminal, Ticks(maxMeasurementWaitTicks * 2));
    terminal.bulkOutput.reset();
    OSC::RichTextBlocks::publishMeasurements(terminal);
    CHECK(drainCatalog(terminal).find(":n=2:t=3:s=ok;") != std::string::npos);
    CHECK_EQ(catalog.calls, 1U);
    CHECK(terminal.richTextMeasurements.empty());
}

TEST(fontCatalogLongNamesPageAtTheComposedOSCPacketBound)
{
    Terminal terminal;
    resize(terminal, 24, 80);
    Catalog catalog;
    catalog.families.clear();
    for (unsigned index = 100; index < 200; ++index) {
        auto family = std::to_string(index);
        family.resize(maxFamilyNameBytes, 'A');
        catalog.families.push_back(std::move(family));
    }
    terminal.typography.fontCatalog = catalog.provider();
    REQUIRE(adoptTypography(terminal, catalogFacts()));
    ask(terminal, "a=f:i=1:n=255");
    const auto first = drainCatalog(terminal);
    const auto count = maxFontCatalogPageBytes / maxFamilyNameBytes;
    CHECK(first.size() < fontCatalogPacketBytes);
    CHECK(first.find(":b=0:n=" + std::to_string(count) + ":t=100:s=ok;") != std::string::npos);
    ask(terminal, "a=f:i=2:n=255:b=" + std::to_string(count) + ":E=1");
    const auto second = drainCatalog(terminal);
    CHECK(second.size() < fontCatalogPacketBytes);
    CHECK(second.find(":n=" + std::to_string(100 - count) + ":t=100:s=ok;") != std::string::npos);
    CHECK_EQ(catalog.calls, 2U);
}

TEST(fontCatalogAdmissionAndRevokedReplyPermissionReleaseCredits)
{
    CatalogClock clock;
    Terminal terminal;
    resize(terminal, 24, 80);
    terminal.tickClock = &clock;
    REQUIRE(adoptTypography(terminal, catalogFacts()));
    for (unsigned index = 0; index < maxPendingMeasurements; ++index)
        ask(terminal, "a=f:i=" + std::to_string(index) + ":e=1");
    REQUIRE(terminal.richTextMeasurements.size() == maxPendingMeasurements);
    ask(terminal, "a=f:i=100:e=1");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=100:v=3:s=busy\x1b\\");
    OSC::RichTextBlocks::expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(!drainCatalog(terminal).empty());

    Catalog catalog;
    terminal.typography.fontCatalog = catalog.provider();
    const std::string pressure(VT::maxBulkOutputBytes, 'x');
    REQUIRE(terminal.bulkOutput.append(pressure, VT::maxBulkOutputBytes));
    ask(terminal, "a=f:i=100:e=1");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    DeniedCatalog policy;
    terminal.capabilityPolicy = &policy;
    terminal.bulkOutput.reset();
    OSC::RichTextBlocks::publishMeasurements(terminal);
    CHECK(terminal.richTextMeasurements.empty());
    CHECK(drainCatalog(terminal).empty());
    terminal.capabilityPolicy = nullptr;
}

TEST(fontPagesShareMeasurementCancellationDuplicatesAndDeadlines)
{
    CatalogClock clock;
    Terminal terminal;
    resize(terminal, 24, 80);
    terminal.tickClock = &clock;
    auto facts = catalogFacts();
    facts.shaping = 1;
    REQUIRE(adoptTypography(terminal, facts));
    ask(terminal, "a=f:i=9:e=1");
    REQUIRE(terminal.richTextMeasurements.size() == 1);
    ask(terminal, "a=q:i=9:e=1:w=0:h=0;{\\rtf1 waiting}");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=9:v=3:s=duplicate\x1b\\");
    CHECK(terminal.richTextMeasurements.empty());
    ask(terminal, "a=f:i=9:e=1");
    ask(terminal, "a=c:i=9:e=1");
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=9:v=3:s=cancelled\x1b\\");
    ask(terminal, "a=f:i=9:e=1");
    OSC::RichTextBlocks::expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=9:v=3:s=timeout\x1b\\");
    ask(terminal, "a=f:i=9:e=1");
    ++facts.rendererRevision;
    REQUIRE(adoptTypography(terminal, facts));
    CHECK(drainCatalog(terminal) == "\x1b]7404;a=e:i=9:v=3:s=stale\x1b\\");
}

TEST(localFontAndMeasurementCallbacksCannotReturnSuccessAfterTheirDeadline)
{
    for (const bool isCatalog : { false, true }) {
        for (const auto elapsed : { maxMeasurementWaitTicks - 1, maxMeasurementWaitTicks }) {
            struct Callback {
                CatalogClock clock;
                std::uint32_t elapsed;
            } callback { {}, elapsed };
            Terminal terminal;
            resize(terminal, 24, 80);
            terminal.tickClock = &callback.clock;
            auto facts = catalogFacts();
            facts.shaping = 1;
            REQUIRE(adoptTypography(terminal, facts));
            terminal.typography.fontCatalog = { [](void* context, const FontCatalogQuery* query, const TypographyFacts* facts, FontCatalogPage* page) {
                                                   auto& callback = *static_cast<Callback*>(context);
                                                   callback.clock.instant = callback.clock.instant + callback.elapsed;
                                                   page->offset = query->offset;
                                                   page->rendererRevision = query->rendererRevision;
                                                   page->scope = facts->fontCatalogScope;
                                                   page->status = FontCatalogStatus::Ready;
                                                   return true;
                                               },
                &callback };
            terminal.typography.shaper = { [](void* context, const Document*, const ShapeRequest*, const TypographyFacts*, ShapedText*) {
                                              auto& callback = *static_cast<Callback*>(context);
                                              callback.clock.instant = callback.clock.instant + callback.elapsed;
                                              return false;
                                          },
                &callback };
            ask(terminal, isCatalog ? "a=f:i=1:e=1" : "a=q:i=1:e=1:w=1:h=1;{\\rtf1 a}");
            const auto reply = drainCatalog(terminal);
            if (elapsed == maxMeasurementWaitTicks)
                CHECK(reply == "\x1b]7404;a=e:i=1:v=3:s=timeout\x1b\\");
            else
                CHECK(reply.starts_with(isCatalog ? "\x1b]7404;a=f:i=1:" : "\x1b]7404;a=r:i=1:"));
            CHECK(terminal.richTextMeasurements.empty());
        }
    }
}

TEST(fontCatalogPageDecoderRejectsHostileNamesCountsAndGenerations)
{
    Catalog catalog;
    PublishedTypography typography;
    typography.facts = std::make_shared<const TypographyFacts>(catalogFacts());
    typography.fontCatalog = catalog.provider();
    const FontCatalogQuery query { 0, 2, 7 };
    Measurement measured;
    measured.rendererRevision = 7;
    measured.fontCatalog = queryFontCatalog(query, typography);
    REQUIRE(decodePage(measured));
    auto& page = *measured.fontCatalog;
    page.families[0].name[0] = '\x1b';
    CHECK(!decodePage(measured));
    page = queryFontCatalog(query, typography);
    page.families[1] = page.families[0];
    CHECK(!decodePage(measured));
    page = queryFontCatalog(query, typography);
    page.total = 1;
    CHECK(!decodePage(measured));
    page = queryFontCatalog(query, typography);
    page.families.clear();
    CHECK(!decodePage(measured));
    page = queryFontCatalog(query, typography);
    page.scope = static_cast<FontCatalogScope>(255);
    CHECK(!decodePage(measured));
    page = queryFontCatalog(query, typography);
    page.status = FontCatalogStatus::Denied;
    CHECK(!decodePage(measured));
    page = queryFontCatalog(query, typography);
    ++page.rendererRevision;
    CHECK(!isFontCatalogPage(query, page));
    page = queryFontCatalog(query, typography);
    CHECK(!isFontCatalogPage({ 0, 1, 7 }, page));
    CHECK(!isFontCatalogPage({ 1, 2, 7 }, page));
}

TEST(twoSyncPanesUseTheirOwnFontCatalogEvenWithoutNativeShaping)
{
    UbiquittyTest::SyncLink link(0);
    const auto first = link.addPane(6, 30);
    const auto second = link.addPane(6, 30);
    Catalog firstCatalog;
    Catalog secondCatalog;
    secondCatalog.families = { "Other OS" };
    for (const auto handle : { first, second }) {
        const auto facts = catalogFacts(handle == first ? FontCatalogScope::System : FontCatalogScope::Exposed);
        link.client().publishTypography(handle, facts);
        auto* terminal = link.client().paneTerminal(handle);
        REQUIRE(terminal);
        REQUIRE(adoptTypography(*terminal, facts));
        terminal->typography.fontCatalog = (handle == first ? firstCatalog : secondCatalog).provider();
    }
    link.step();
    REQUIRE(link.host().requestRichText(first, catalogRequest(1), link.now()));
    REQUIRE(link.host().requestRichText(second, catalogRequest(2), link.now()));
    link.step();
    link.step();
    const auto replies = link.host().takeRichTextReplies();
    REQUIRE(replies.size() == 2);
    for (const auto& reply : replies) {
        REQUIRE(reply.measurement.fontCatalog);
        const auto& page = *reply.measurement.fontCatalog;
        CHECK_EQ(page.rendererRevision, std::uint64_t { 7 });
        CHECK(page.families.front().familyName() == (reply.handle == first ? "Alpha" : "Other OS"));
        CHECK(page.scope == (reply.handle == first ? FontCatalogScope::System : FontCatalogScope::Exposed));
    }
    CHECK_EQ(firstCatalog.calls, 1U);
    CHECK_EQ(secondCatalog.calls, 1U);
}

TEST(syncFontCatalogRequestsRejectStaleLocalGenerationAndCancelledReplies)
{
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = catalogFacts();
    link.client().publishTypography(handle, facts);
    link.step();
    auto* terminal = link.client().paneTerminal(handle);
    REQUIRE(terminal);
    REQUIRE(adoptTypography(*terminal, facts));
    Catalog catalog;
    terminal->typography.fontCatalog = catalog.provider();
    REQUIRE(link.host().requestRichText(handle, catalogRequest(1), link.now()));
    ++facts.rendererRevision;
    REQUIRE(adoptTypography(*terminal, facts));
    link.step();
    link.step();
    CHECK_EQ(catalog.calls, 0U);
    CHECK(link.host().takeRichTextReplies().empty());
    REQUIRE(link.host().typography(handle));
    CHECK_EQ(link.host().typography(handle)->rendererRevision, facts.rendererRevision);
    REQUIRE(link.host().requestRichText(handle, catalogRequest(2), link.now()));
    link.host().cancelRichText(handle, 2);
    link.step();
    link.step();
    CHECK(link.host().takeRichTextReplies().empty());
    CHECK(link.host().state() == ConnectionState::Attached);
}

TEST(syncFontCatalogAssemblyHasItsOwnDerivedByteEnvelope)
{
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    link.client().publishTypography(handle, catalogFacts());
    link.step();
    const auto request = catalogRequest(1);
    REQUIRE(link.host().requestRichText(handle, request, link.now()));
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writer.u32(request.token);
    writer.u32(static_cast<std::uint32_t>(maxRichTextFontCatalogBytes + 1));
    writer.u32(0);
    writer.u8(0);
    link.sendToHost(FrameKind::RichTextMeasured, bytes, handle);
    CHECK(link.host().state() != ConnectionState::Attached);
    CHECK(link.host().takeRichTextReplies().empty());
}

TEST(syncFontCatalogRepliesMustMatchRequestedModeCountScopeOffsetAndRevision)
{
    for (unsigned variation = 0; variation < 5; ++variation) {
        UbiquittyTest::SyncLink link(0);
        const auto handle = link.addPane(6, 30);
        link.client().publishTypography(handle, catalogFacts());
        link.step();
        const auto request = catalogRequest(1, 0, 1);
        REQUIRE(link.host().requestRichText(handle, request, link.now()));
        Measurement measured;
        measured.rendererRevision = 7;
        measured.fontCatalog = FontCatalogPage {};
        auto& page = *measured.fontCatalog;
        page.status = FontCatalogStatus::Ready;
        page.scope = FontCatalogScope::System;
        page.total = 3;
        page.rendererRevision = 7;
        REQUIRE(page.appendFamily("Alpha"));
        switch (variation) {
        case 0:
            measured.fontCatalog.reset();
            measured.cellWidth = measured.cellHeight = 64;
            break;
        case 1:
            REQUIRE(page.appendFamily("Beta"));
            break;
        case 2:
            page.scope = FontCatalogScope::Exposed;
            break;
        case 3:
            page.offset = 1;
            break;
        case 4:
            ++measured.rendererRevision;
            break;
        }
        std::vector<std::uint8_t> body;
        WireWriter bodyWriter(body);
        writeMeasurement(bodyWriter, measured);
        std::vector<std::uint8_t> chunk;
        WireWriter chunkWriter(chunk);
        chunkWriter.u32(request.token);
        chunkWriter.u32(static_cast<std::uint32_t>(body.size()));
        chunkWriter.u32(0);
        chunkWriter.raw(body);
        link.sendToHost(FrameKind::RichTextMeasured, chunk, handle);
        CHECK(link.host().state() != ConnectionState::Attached);
        CHECK(link.host().takeRichTextReplies().empty());
    }
}

TEST(syncFontCatalogRequestModeAndCountHaveStrictWireDomains)
{
    const auto request = catalogRequest(1, std::numeric_limits<std::uint32_t>::max(), 255);
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writeMeasureRequest(writer, request);
    REQUIRE(bytes.size() == 20);
    for (unsigned variation = 0; variation < 5; ++variation) {
        auto input = bytes;
        switch (variation) {
        case 0:
            input[14] = 2;
            break;
        case 1:
            input[19] = 0;
            break;
        case 2:
            input[0] = 0;
            break;
        case 3:
            std::fill(input.begin() + 4, input.begin() + 12, 0);
            break;
        case 4:
            input.pop_back();
            break;
        }
        WireReader reader(input);
        MeasureRequest decoded;
        CHECK(!readMeasureRequest(reader, decoded));
    }
    WireReader reader(bytes);
    MeasureRequest decoded;
    REQUIRE(readMeasureRequest(reader, decoded));
    REQUIRE(decoded.fontCatalog);
    CHECK_EQ(decoded.fontCatalog->offset, std::numeric_limits<std::uint32_t>::max());
    CHECK_EQ(decoded.fontCatalog->count, 255);
}

TEST(handoverCannotDropPendingOrBackpressuredFontAndMeasurementOutcomes)
{
    for (const bool isCatalog : { false, true }) {
        for (const bool isCompleted : { false, true }) {
            CatalogClock clock;
            Terminal terminal;
            resize(terminal, 24, 80);
            terminal.tickClock = &clock;
            auto facts = catalogFacts();
            facts.shaping = 1;
            REQUIRE(adoptTypography(terminal, facts));
            ask(terminal, isCatalog ? "a=f:i=1:e=1" : "a=q:i=1:e=1:w=1:h=1;{\\rtf1 a}");
            REQUIRE(terminal.richTextMeasurements.size() == 1);
            if (isCompleted) {
                const auto& pending = terminal.richTextMeasurements.front();
                Measurement measured;
                measured.rendererRevision = facts.rendererRevision;
                if (isCatalog) {
                    Catalog catalog;
                    PublishedTypography typography;
                    typography.facts = terminal.typography.facts;
                    typography.fontCatalog = catalog.provider();
                    measured.fontCatalog = queryFontCatalog(*pending.request.fontCatalog, typography);
                } else
                    measured = measure(pending.request.document, pending.request.shape, terminal.typography);
                REQUIRE(OSC::RichTextBlocks::completeMeasurement(terminal, pending.request.token, std::move(measured)));
                const std::string pressure(VT::maxBulkOutputBytes, 'x');
                REQUIRE(terminal.bulkOutput.append(pressure, VT::maxBulkOutputBytes));
                OSC::RichTextBlocks::publishMeasurements(terminal);
                REQUIRE(terminal.richTextMeasurements.size() == 1);
            }
            CHECK(terminalHandoverBlockers(terminal).contains(HandoverBlocker::RichTextMeasurement));
            Runtime::HandoverQuiesce quiesce({ 100, 100, 1 }, 0);
            quiesce.notePass(terminal, Runtime::FrameClass::Settled, 1);
            CHECK(quiesce.phase() != Runtime::QuiescePhase::Ready);
            constexpr std::size_t maxHostStateBytes = 8 * 1024 * 1024;
            const auto refused = encodeHandoverHostState(terminal, maxHostStateBytes);
            REQUIRE(!refused);
            CHECK(refused.error() == HandoverBodyError::NotAtGround);
            if (isCompleted) {
                terminal.bulkOutput.reset();
                OSC::RichTextBlocks::publishMeasurements(terminal);
            } else
                OSC::RichTextBlocks::expireMeasurements(terminal, clock.instant + maxMeasurementWaitTicks);
            REQUIRE(terminal.richTextMeasurements.empty());
            CHECK(!terminalHandoverBlockers(terminal).contains(HandoverBlocker::RichTextMeasurement));
            quiesce.notePass(terminal, Runtime::FrameClass::Settled, 2);
            CHECK(quiesce.phase() == Runtime::QuiescePhase::Ready);
            const auto encoded = encodeHandoverHostState(terminal, maxHostStateBytes);
            REQUIRE(encoded);
            Terminal successor;
            REQUIRE(restoreHandoverHostState(encoded->view(), successor, maxHostStateBytes));
            const auto reply = drainCatalog(successor);
            CHECK(reply == drainCatalog(terminal));
            CHECK(reply.starts_with(isCompleted ? (isCatalog ? "\x1b]7404;a=f:i=1:" : "\x1b]7404;a=r:i=1:")
                                                : "\x1b]7404;a=e:i=1:v=3:s=timeout"));
            CHECK(!successor.typography.fontCatalog.function);
            CHECK(!successor.typography.shaper.function);
        }
    }
}

UBIQUITTY_TEST_MAIN()
