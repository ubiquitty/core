// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sync_rich_text.cpp — rich text blocks over Sync (Docs/rich-text.md,
// "Synchronization"): a checkpoint carries every block, an update carries a placement
// and defines only the documents its attachment lacks, and the loopback
// oracle compares every block after every frame.

#include "Core/RichText/RTFReader.h"
#include "LoopbackPaneLink.h"
#include "SyncLink.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/RichText/Blocks.h"
#include "Core/RichText/Drawing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Protocol::Sync::ApplyOutcome;
using Ubiquitty::Protocol::Sync::AttachmentBaseline;
using Ubiquitty::Protocol::Sync::ScreenPublisher;
using Ubiquitty::Protocol::Sync::ScreenReplica;
using Ubiquitty::Protocol::Sync::UpdateOutcome;

namespace {

std::unique_ptr<Ubiquitty::Terminal> makeTerminal(int rows, int columns)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, rows, columns);
    return terminal;
}

std::string place(std::string_view keys, std::string_view body)
{
    return "\x1b]7404;" + std::string(keys) + ";" + std::string(body) + "\x1b\\";
}

const std::string paragraph = "{\\rtf1{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\colortbl;\\red0\\green102\\blue204;}"
                              "\\f0\\fs36\\b\\cf1 Release notes\\par\\fs24\\b0\\cf0 The build signs every artefact "
                              "and verifies the chain before upload.}";

Ubiquitty::RichText::TypographyFacts phoneFacts();

} // namespace

TEST(aLateClientReplacesThePreviousPicturesRichTextLayoutAtCheckpoint)
{
    auto host = makeTerminal(8, 40);
    ScreenPublisher publisher;
    AttachmentBaseline earlyBaseline;
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.capture(*host));
    REQUIRE(publisher.checkpoint(*host, 1, body, earlyBaseline));
    auto early = makeTerminal(8, 40);
    REQUIRE(Ubiquitty::RichText::adoptTypography(*early, phoneFacts()));
    ScreenReplica earlyReplica;
    REQUIRE(applyCheckpoint(*early, body, earlyReplica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, place("w=30:h=2:i=1", "{\\rtf1 same layout}"));
    body.clear();
    REQUIRE(publisher.capture(*host));
    REQUIRE(publisher.rowUpdate(*host, earlyBaseline, body) == UpdateOutcome::Encoded);
    REQUIRE(applyRowUpdate(*early, body, earlyReplica) == ApplyOutcome::Applied);

    // The late client's terminal held a previous picture. Its first block
    // reused the host block's serial, and its layout and drawing were cached.
    auto late = makeTerminal(8, 40);
    REQUIRE(Ubiquitty::RichText::adoptTypography(*late, phoneFacts()));
    UbiquittyTest::feed(*late, place("w=30:h=2:i=2", "{\\rtf1 x}"));
    const auto hostBlocks = host->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    const auto previousBlocks = late->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    REQUIRE(hostBlocks.size() == 1 && previousBlocks.size() == 1);
    REQUIRE(hostBlocks.front().serial == previousBlocks.front().serial);
    const auto previousDrawing = late->richTextLayouts.drawingOf(previousBlocks.front(), late->typography);
    REQUIRE(previousDrawing);
    CHECK_EQ(previousDrawing->text().size(), std::size_t { 1 });

    AttachmentBaseline lateBaseline;
    body.clear();
    REQUIRE(publisher.checkpoint(*host, 2, body, lateBaseline));
    ScreenReplica lateReplica;
    REQUIRE(applyCheckpoint(*late, body, lateReplica) == ApplyOutcome::Applied);
    const auto earlyBlocks = early->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    const auto lateBlocks = late->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    REQUIRE(earlyBlocks.size() == 1 && lateBlocks.size() == 1);
    CHECK(earlyBlocks.front().document == lateBlocks.front().document);
    const auto earlyDrawing = early->richTextLayouts.drawingOf(earlyBlocks.front(), early->typography);
    const auto lateDrawing = late->richTextLayouts.drawingOf(lateBlocks.front(), late->typography);
    REQUIRE(earlyDrawing && lateDrawing);
    CHECK(std::ranges::equal(earlyDrawing->text(), lateDrawing->text()));
    CHECK_EQ(earlyDrawing->clusters().size(), lateDrawing->clusters().size());
    CHECK_EQ(Ubiquitty::RichText::layoutOf(*early, earlyBlocks.front()).copyableEnd(), Ubiquitty::RichText::layoutOf(*late, lateBlocks.front()).copyableEnd());
}

TEST(aCheckpointCarriesEveryBlockOfBothScreensAndTheScrollback)
{
    auto host = makeTerminal(8, 40);
    UbiquittyTest::feed(*host, place("w=30:h=3:i=1", paragraph));
    UbiquittyTest::feed(*host, "\x1b[8;1H\n\n\n\n");
    UbiquittyTest::feed(*host, place("w=10:h=1:i=2", "{\\rtf1\\i on screen}"));
    UbiquittyTest::feed(*host, "\x1b[?1049h");
    UbiquittyTest::feed(*host, place("w=12:h=2:r=0,0,5,1,9,1,3,1", "{\\rtf1 alternate}"));
    CHECK_EQ(host->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 2 });
    CHECK(host->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary)[0].row < 0);

    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(8, 40);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    CHECK(client->richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).front().visibleRects == host->richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).front().visibleRects);
    const std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
}

// A paragraph's spacing and indents, and a line break that keeps it, reach
// the replica with the document.
TEST(aCheckpointCarriesParagraphSpacingAndIndents)
{
    auto host = makeTerminal(8, 40);
    UbiquittyTest::feed(*host, place("w=30:h=4:i=1", "{\\rtf1\\sl-360\\sb120\\sa60\\li720\\ri240\\fi-360 a\\line b\\par\\pard\\sl480\\slmult1 c}"));
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(8, 40);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const auto& hostBlocks = host->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    const auto& clientBlocks = client->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    REQUIRE(hostBlocks.size() == 1 && clientBlocks.size() == 1);
    CHECK(clientBlocks.front().document == hostBlocks.front().document);
    CHECK_EQ(clientBlocks.front().document.runs.front().paragraph.firstIndent, -360);
    CHECK_EQ(clientBlocks.front().document.text[1], Ubiquitty::RichText::lineSeparator);
}

// A block's padding and vertical placement reach the replica with it.
TEST(aCheckpointCarriesPaddingAndVerticalPlacement)
{
    auto host = makeTerminal(8, 40);
    UbiquittyTest::feed(*host, place("w=30:h=4:i=1:p=1,2,3,4:y=b", "{\\rtf1 padded}"));
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(8, 40);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
    const auto& blocks = client->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary);
    REQUIRE(blocks.size() == 1);
    CHECK(blocks.front().padding == (Ubiquitty::RichText::Insets { .top = 1, .right = 2, .bottom = 3, .left = 4 }));
    CHECK(blocks.front().verticalPlacement == Ubiquitty::RichText::VerticalPlacement::Bottom);
}

TEST(anUpdateDefinesADocumentOnceAndMovesItsPlacementAfter)
{
    auto host = makeTerminal(10, 50);
    UbiquittyTest::feed(*host, place("w=40:h=3:i=1", paragraph));
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(10, 50);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    // A second block: its document travels in this update.
    UbiquittyTest::feed(*host, place("w=20:h=2:i=2", "{\\rtf1\\b second}"));
    body.clear();
    CHECK(publisher.capture(*host));
    CHECK(publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    std::string difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    const std::size_t definingUpdate = body.size();

    // A scroll moves both blocks; the update carries placements, not documents.
    UbiquittyTest::feed(*host, "\x1b[10;1H\n\n");
    body.clear();
    CHECK(publisher.capture(*host));
    CHECK(publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    const std::string paragraphText = "The build signs every artefact";
    const std::string bodyBytes(body.begin(), body.end());
    CHECK_MSG(bodyBytes.find("Helvetica") == std::string::npos, "a held document was defined again");
    CHECK(definingUpdate > 0);

    // Removing a block moves the placements only.
    UbiquittyTest::feed(*host, "\x1b]7404;a=d:i=1\x1b\\");
    body.clear();
    CHECK(publisher.capture(*host));
    CHECK(publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Applied);
    difference = UbiquittyTest::firstDisagreement(*host, *client);
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK_EQ(client->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 1 });
}

TEST(aPlacementNamingAnUndefinedDocumentIsRefused)
{
    auto host = makeTerminal(6, 30);
    UbiquittyTest::feed(*host, place("w=10:h=1", "{\\rtf1 one}"));
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(*host) && publisher.checkpoint(*host, 1, body, baseline));
    auto client = makeTerminal(6, 30);
    ScreenReplica replica;
    CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);

    UbiquittyTest::feed(*host, place("w=10:h=1", "{\\rtf1 two}"));
    // The baseline claims a document it never received.
    baseline.richTextSerials.push_back(host->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).back().serial);
    std::ranges::sort(baseline.richTextSerials);
    body.clear();
    CHECK(publisher.capture(*host));
    CHECK(publisher.rowUpdate(*host, baseline, body) == UpdateOutcome::Encoded);
    CHECK(applyRowUpdate(*client, body, replica) == ApplyOutcome::Malformed);
    CHECK_EQ(client->richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 1 });
}

TEST(syncDocumentsCannotBypassTheUnicodeProfileThroughCanonicalFields)
{
    using namespace Ubiquitty::Protocol::Sync;
    constexpr std::string_view source = R"({\rtf1{\fonttbl{\f0 Family;}}\f0 x})";
    const auto parsed = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(parsed);
    const auto checkRefusal = [](const Ubiquitty::RichText::Document& document) {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writeDocument(writer, document);
        Ubiquitty::RichText::Document decoded;
        WireReader allocated(bytes);
        CHECK(!readDocument(allocated, decoded));
        CHECK(!allocated.ok());
        WireReader planned(bytes);
        CHECK(!readDocumentView(planned));
        CHECK(!planned.ok());
    };
    for (const auto codePoint : { 0u, 1u, 11u, 13u, 27u, 127u, 133u, 159u }) {
        auto document = *parsed;
        document.text[0] = codePoint;
        checkRefusal(document);
    }
    for (const std::string_view family : { "\xC3", "\xED\xA0\x80", "A\nB", "A\tB", "\x7F", "\xC2\x85", " leading", "trailing " }) {
        auto document = *parsed;
        auto& font = document.fonts[0];
        font.nameLength = static_cast<std::uint8_t>(family.size());
        std::ranges::copy(family, font.name.begin());
        checkRefusal(document);
    }
    constexpr std::string_view unicode = "{\\rtf1{\\fonttbl{\\f0 家族\\;\\{Display\\}\\\\;}}\\f0 café\\tab é\\line 😀\\par 終}";
    const auto valid = Ubiquitty::RichText::readRTF({ reinterpret_cast<const std::uint8_t*>(unicode.data()), unicode.size() });
    REQUIRE(valid);
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writeDocument(writer, *valid);
    Ubiquitty::RichText::Document decoded;
    WireReader allocated(bytes);
    REQUIRE(readDocument(allocated, decoded));
    CHECK(allocated.isAtEnd());
    CHECK(decoded == *valid);
    WireReader planned(bytes);
    CHECK(readDocumentView(planned));
    CHECK(planned.isAtEnd());
}

TEST(theLoopbackOracleHoldsAcrossPlacementsScrollsAndRemovals)
{
    // Timed reads, so the coherence rule publishes as the daemon would; the
    // oracle compares every block after every frame.
    std::vector<std::string> chunks;
    chunks.push_back(place("w=50:h=3:i=1", paragraph));
    for (int index = 0; index < 20; ++index) {
        chunks.push_back("line " + std::to_string(index) + "\r\n");
        if (index % 5 == 0)
            chunks.push_back(place("w=20:h=2:i=" + std::to_string(index + 2), "{\\rtf1\\b block " + std::to_string(index) + "}"));
    }
    chunks.push_back("\x1b]7404;a=d:i=1\x1b\\");
    chunks.push_back("\x1b[?1049h");
    chunks.push_back(place("w=10:h=1", "{\\rtf1 alternate}"));
    std::vector<UbiquittyTest::PTYRead> reads;
    for (std::size_t index = 0; index < chunks.size(); ++index)
        reads.push_back({ .atMicroseconds = 1000 + index * 40'000, .bytes = chunks[index] });

    UbiquittyTest::LoopbackPaneLink link({ .rows = 12, .columns = 60 });
    link.replay(reads);
    CHECK(link.frames().size() >= chunks.size() / 2);
    CHECK_EQ(link.host().richTextBlocks.blocks(Ubiquitty::ScreenMode::Alternate).size(), std::size_t { 1 });
    CHECK(link.host().richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size() >= 3);
    CHECK_MSG(link.failure().empty(), link.failure().c_str());
}

namespace {

Ubiquitty::RichText::TypographyFacts phoneFacts()
{
    Ubiquitty::RichText::TypographyFacts facts;
    facts.cellWidth = 7 * 64;
    facts.cellHeight = 15 * 64;
    facts.faceCount = 1;
    Ubiquitty::RichText::FaceMetrics& face = facts.faces[0];
    face.lineHeight = 2400;
    face.ascent = 1900;
    face.descent = 500;
    face.averageAdvance = 1100;
    face.advances.fill(1100);
    return facts;
}

} // namespace

TEST(theTypographyTableRoundTripsAndATableThatIsNotWellFormedIsRefused)
{
    auto facts = phoneFacts();
    facts.rendererRevision = 0x123456789abcdef0ULL;
    facts.rendersBlocks = 1;
    facts.proportionalSize = 14 * 64;
    facts.monospaceSize = 11 * 64;
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, facts);
    CHECK(body.size() < Ubiquitty::Protocol::Sync::maxFrameBodyBytes);
    const auto decoded = Ubiquitty::Protocol::Sync::decodeTypographyFacts(body);
    CHECK(decoded && *decoded == facts);
    // Sixteen faces, the most a table holds, still fit one frame.
    Ubiquitty::RichText::TypographyFacts full = facts;
    full.faceCount = Ubiquitty::RichText::maxFaces;
    for (Ubiquitty::RichText::FaceMetrics& face : full.faces) {
        face = facts.faces[0];
        face.nameLength = Ubiquitty::RichText::maxFamilyNameBytes;
        face.name.fill('N');
    }
    body.clear();
    Ubiquitty::Protocol::Sync::encode(body, full);
    CHECK(body.size() <= Ubiquitty::Protocol::Sync::maxFrameBodyBytes);
    CHECK(Ubiquitty::Protocol::Sync::decodeTypographyFacts(body).has_value());
    Ubiquitty::RichText::TypographyFacts broken = facts;
    broken.faces[0].lineHeight = 0;
    body.clear();
    Ubiquitty::Protocol::Sync::encode(body, broken);
    CHECK(!Ubiquitty::Protocol::Sync::decodeTypographyFacts(body).has_value());
    broken = facts;
    broken.rendererRevision = 0;
    body.clear();
    Ubiquitty::Protocol::Sync::encode(body, broken);
    CHECK(!Ubiquitty::Protocol::Sync::decodeTypographyFacts(body));
    broken = facts;
    broken.rendersBlocks = 2;
    body.clear();
    Ubiquitty::Protocol::Sync::encode(body, broken);
    CHECK(!Ubiquitty::Protocol::Sync::decodeTypographyFacts(body));
    // A body cut short is refused.
    body.clear();
    Ubiquitty::Protocol::Sync::encode(body, facts);
    body.pop_back();
    CHECK(!Ubiquitty::Protocol::Sync::decodeTypographyFacts(body).has_value());
}

TEST(aClientPublishesItsTypographyToTheHostOnce)
{
    const std::string text = "ready\r\n";
    std::vector<UbiquittyTest::PTYRead> reads = { { .atMicroseconds = 1000, .bytes = text } };
    UbiquittyTest::LoopbackPaneLink link({ .rows = 6, .columns = 30 });
    link.replay(reads);
    CHECK(!link.hostEnd().typography(link.handle()));
    link.publishClientTypography(phoneFacts());
    CHECK(link.hostEnd().typography(link.handle()) && *link.hostEnd().typography(link.handle()) == phoneFacts());
    const auto firstPublication = link.hostEnd().typography(link.handle());
    // An equal table is not sent again.
    link.publishClientTypography(phoneFacts());
    CHECK(link.hostEnd().typography(link.handle()) == firstPublication);
    Ubiquitty::RichText::TypographyFacts wider = phoneFacts();
    wider.cellWidth = 8 * 64;
    link.publishClientTypography(wider);
    CHECK(link.hostEnd().typography(link.handle()) != firstPublication);

    // The host measures in the client's table once the pane adopts it, as the
    // daemon does for every session the client views.
    auto host = makeTerminal(6, 30);
    Ubiquitty::RichText::adoptTypography(*host, link.hostEnd().typography(link.handle()));
    CHECK(host->typography.facts == link.hostEnd().typography(link.handle()));
    UbiquittyTest::feed(*host, "\x1b]7404;a=q:w=10:h=1;{\\rtf1\\fs20 aaaa}\x1b\\");
    std::array<char, 2048> reply {};
    const int length = UbiquittyTest::drain(*host, reply);
    const std::string answered(reply.data(), static_cast<std::size_t>(std::max(length, 0)));
    // Four letters of 1100/2048 em at ten points: 21.5 points, three eight-point cells.
    CHECK_MSG(answered.starts_with("\x1b]7404;a=r:i=0:f=1:w=3:h=1:n=4:c=14:l=1:m=3:x=1:v=3:W=512:H=960:B=0:S=0:E=1:L=750:A=594:Q=0:"), answered.c_str());
    CHECK(answered.ends_with(":R=4\x1b\\"));
}

TEST(aMeasurementVisitsTheFontOwningClientAndReturnsBoundedGeometry)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = phoneFacts();
    facts.shaping = 1;
    facts.baseline = 12 * 64;
    facts.proportionalSize = 12 * 64;
    link.client().publishTypography(handle, facts);
    link.step();
    Terminal* replica = link.client().paneTerminal(handle);
    REQUIRE(replica);
    CHECK(adoptTypography(*replica, facts));
    unsigned calls = 0;
    replica->typography.shaper = { [](void* context, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                                      ++*static_cast<unsigned*>(context);
                                      const auto end = static_cast<std::uint32_t>(document->text.size());
                                      output->appendLine({ 0, end, 0, static_cast<std::uint32_t>(facts->cellHeight) * 64, static_cast<std::uint32_t>(facts->baseline) * 64, 0, 4096 });
                                      output->appendSpan({ 0, end, 0, 0, 4096 });
                                      return true;
                                  },
        &calls };
    const std::string body = "{\\rtf1 कर्म العربية}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    REQUIRE(document);
    MeasureRequest request { 71, 19, { 20, 10, Overflow::Wrap, Alignment::Left, LineMode::Terminal }, *document };
    CHECK(link.host().requestRichText(handle, request, 0));
    link.step();
    link.step();
    const auto replies = link.host().takeRichTextReplies();
    REQUIRE(replies.size() == 1);
    CHECK_EQ(replies[0].handle, handle);
    CHECK_EQ(replies[0].token, request.token);
    CHECK(replies[0].measurement.isShaped);
    CHECK_EQ(replies[0].measurement.height, 1u);
    CHECK_EQ(replies[0].measurement.lineEnds.back(), document->text.size());
    CHECK_EQ(calls, 1u);
    // A late response to a cancelled request cannot answer a later program.
    ++request.token;
    CHECK(link.host().requestRichText(handle, request, 0));
    link.host().cancelRichText(handle, request.token);
    link.step();
    link.step();
    CHECK(link.host().takeRichTextReplies().empty());
    CHECK(link.host().state() == Protocol::Sync::ConnectionState::Attached);
    replica->typography.shaper = {};
}

// An open width crosses to the font-owning client as zero, which measures the
// body unwrapped there and resolves the width to the reply's. The widest word
// crosses back with the measurement.
TEST(anOpenWidthIsMeasuredByTheFontOwningClient)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = phoneFacts();
    facts.shaping = 1;
    facts.baseline = 12 * 64;
    facts.proportionalSize = 12 * 64;
    link.client().publishTypography(handle, facts);
    link.step();
    Terminal* replica = link.client().paneTerminal(handle);
    REQUIRE(replica);
    CHECK(adoptTypography(*replica, facts));
    // Every character four points wide, on one line.
    std::vector<std::uint16_t> widths;
    replica->typography.shaper = { [](void* context, const Document* document, const ShapeRequest* request, const TypographyFacts* facts, ShapedText* output) {
                                      static_cast<std::vector<std::uint16_t>*>(context)->push_back(request->width);
                                      const auto end = static_cast<std::uint32_t>(document->text.size());
                                      constexpr std::int32_t step = 4 * 4096;
                                      output->appendLine({ 0, end, 0, static_cast<std::uint32_t>(facts->cellHeight) * 64, static_cast<std::uint32_t>(facts->baseline) * 64, 0, end * step });
                                      for (std::uint32_t index = 0; index < end; ++index)
                                          output->appendSpan({ index, index + 1, 0, static_cast<std::int32_t>(index) * step, static_cast<std::int32_t>(index + 1) * step });
                                      return true;
                                  },
        &widths };
    const std::string body = "{\\rtf1 aaa bbbbb}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    REQUIRE(document);
    MeasureRequest request { 72, 20, { 0, 1, Overflow::Wrap, Alignment::Left, LineMode::Natural }, *document };
    CHECK(link.host().requestRichText(handle, request, 0));
    link.step();
    link.step();
    const auto replies = link.host().takeRichTextReplies();
    REQUIRE(replies.size() == 1);
    // Nine characters of four points: 36 points, six cells of seven. The
    // widest word, five characters, is twenty points: three cells.
    CHECK_EQ(replies[0].measurement.width, 6u);
    CHECK_EQ(replies[0].measurement.minWidth, 3u);
    REQUIRE(widths.size() == 2);
    CHECK_EQ(widths[0], 1024);
    CHECK_EQ(widths[1], 6);
    CHECK(link.host().state() == Protocol::Sync::ConnectionState::Attached);
    replica->typography.shaper = {};
}

TEST(nativeBlockGeometryCrossesMultipleBoundedFrames)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = phoneFacts();
    facts.shaping = 1;
    facts.baseline = 12 * 64;
    facts.proportionalSize = 12 * 64;
    link.client().publishTypography(handle, facts);
    link.step();
    Terminal* replica = link.client().paneTerminal(handle);
    REQUIRE(replica);
    REQUIRE(adoptTypography(*replica, facts));
    replica->typography.shaper = { [](void*, const Document* document, const ShapeRequest*, const TypographyFacts* facts, ShapedText* output) {
                                      const auto height = static_cast<std::uint32_t>(facts->cellHeight) * 64;
                                      const auto baseline = static_cast<std::uint32_t>(facts->baseline) * 64;
                                      for (std::uint32_t index = 0; index < document->text.size(); ++index) {
                                          output->appendLine({ index, index + 1, index * height, height, baseline, 0, 4096 });
                                          output->appendSpan({ index, index + 1, index, 0, 4096 });
                                      }
                                      return true;
                                  },
        nullptr };
    const std::string body = "{\\rtf1 " + std::string(maxCodePoints, 'a') + "}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    REQUIRE(document);
    MeasureRequest request { 72, 0, { 20, 5000, Overflow::Wrap, Alignment::Left, LineMode::Terminal }, *document, true };
    REQUIRE(link.host().requestRichText(handle, request, 0));
    link.step();
    link.step();
    auto replies = link.host().takeRichTextReplies();
    REQUIRE(replies.size() == 1);
    CHECK_EQ(replies[0].measurement.geometry.lines.size(), static_cast<std::size_t>(maxCodePoints));
    CHECK_EQ(replies[0].measurement.geometry.spans.back().end, static_cast<std::uint32_t>(maxCodePoints));
    CHECK(link.host().state() == Protocol::Sync::ConnectionState::Attached);
    replica->typography.shaper = {};
}

TEST(nativeMeasurementRejectsOversizedOrOutOfOrderChunks)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    using namespace Ubiquitty::Protocol::Sync;
    for (const auto& malformed : { std::pair { maxRichTextMeasurementBytes + 1, std::size_t { 0 } }, std::pair { std::size_t { 20 }, std::size_t { 1 } } }) {
        UbiquittyTest::SyncLink link(0);
        const auto handle = link.addPane(6, 30);
        auto facts = phoneFacts();
        facts.shaping = 1;
        link.client().publishTypography(handle, facts);
        link.step();
        const std::string body = "{\\rtf1 test}";
        auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
        REQUIRE(document);
        MeasureRequest request { 73, 0, { 20, 10, Overflow::Wrap, Alignment::Left, LineMode::Terminal }, *document, true };
        REQUIRE(link.host().requestRichText(handle, request, 0));
        std::vector<std::uint8_t> chunk;
        WireWriter writer(chunk);
        writer.u32(request.token);
        writer.u32(static_cast<std::uint32_t>(malformed.first));
        writer.u32(static_cast<std::uint32_t>(malformed.second));
        writer.u8(0);
        link.sendToHost(FrameKind::RichTextMeasured, chunk, handle);
        CHECK(link.host().state() != ConnectionState::Attached);
        CHECK(link.host().takeRichTextReplies().empty());
    }
}

TEST(malformedClipRectanglesNeverEnterAReplica)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Protocol::Sync;
    auto terminal = makeTerminal(6, 30);
    UbiquittyTest::feed(*terminal, place("w=10:h=3:r=0,0,5,1", "{\\rtf1 clipped}"));
    const auto original = terminal->richTextBlocks.blocks(ScreenMode::Primary).front();
    for (const auto& rectangles : {
             std::vector<RichText::VisibleRect> { { 0, 0, 0, 1 } },
             std::vector<RichText::VisibleRect> { { 9, 0, 2, 1 } },
             std::vector<RichText::VisibleRect> { { 0, 2, 1, 2 } },
             std::vector<RichText::VisibleRect> { { 0, 0, 5, 2 }, { 3, 1, 5, 2 } },
             std::vector<RichText::VisibleRect>(RichText::maxVisibleRects + 1, { 0, 0, 1, 1 }) }) {
        auto invalid = original;
        invalid.visibleRects = rectangles;
        RichText::BlockStore source;
        source.restore(ScreenMode::Primary, std::move(invalid));
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writeRichTextBlock(writer, source);
        RichText::BlockStore decoded;
        CHECK(!readRichTextBlock(bytes, 6, 30, {}, terminal->richTextBlocks, decoded));
        CHECK(decoded.isEmpty());
    }
}

TEST(aCheckpointCannotInstallDuplicateApplicationIDsOnOneScreen)
{
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        auto host = makeTerminal(6, 30);
        if (screen == Ubiquitty::ScreenMode::Alternate)
            UbiquittyTest::feed(*host, "\x1b[?1049h");
        UbiquittyTest::feed(*host, place("w=10:h=1:i=7:D=11:M=1", "{\\rtf1 first}"));
        UbiquittyTest::feed(*host, place("w=10:h=1:i=8:D=12:M=1", "{\\rtf1 second}"));
        auto blocks = host->richTextBlocks.mutableBlocks(screen);
        REQUIRE(blocks.size() == 2);
        // Craft a validly hashed snapshot normal placement can never emit:
        // one screen gives the same application identity to two sources.
        blocks[1].id = blocks[0].id;
        ScreenPublisher publisher;
        AttachmentBaseline baseline;
        std::vector<std::uint8_t> body;
        REQUIRE(publisher.capture(*host));
        REQUIRE(publisher.checkpoint(*host, 1, body, baseline));
        auto client = makeTerminal(6, 30);
        UbiquittyTest::feed(*client, place("w=10:h=1:i=99", "{\\rtf1 preserved}"));
        const auto retained = client->richTextBlocks;
        const auto cursor = client->screens.active->cursorY;
        ScreenReplica replica;
        CHECK(applyCheckpoint(*client, body, replica) == ApplyOutcome::Malformed);
        CHECK(client->richTextBlocks == retained);
        CHECK_EQ(client->screens.active->cursorY, cursor);
        CHECK(!client->screens.isUsingAlternateScreen);
    }
}

TEST(applicationIDsAreScopedToOneScreenAndZeroRemainsAnonymous)
{
    for (const auto id : { 0, 7 }) {
        auto host = makeTerminal(6, 30);
        const int count = id ? 1 : 2;
        for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
            if (screen == Ubiquitty::ScreenMode::Alternate)
                UbiquittyTest::feed(*host, "\x1b[?1049h");
            for (int index = 0; index < count; ++index)
                UbiquittyTest::feed(*host, place("w=10:h=1:i=" + std::to_string(id), "{\\rtf1 retained}"));
            REQUIRE(host->richTextBlocks.blocks(screen).size() == static_cast<std::size_t>(count));
        }
        ScreenPublisher publisher;
        AttachmentBaseline baseline;
        std::vector<std::uint8_t> body;
        REQUIRE(publisher.capture(*host));
        REQUIRE(publisher.checkpoint(*host, 1, body, baseline));
        auto client = makeTerminal(6, 30);
        ScreenReplica replica;
        REQUIRE(applyCheckpoint(*client, body, replica) == ApplyOutcome::Applied);
        CHECK(client->richTextBlocks == host->richTextBlocks);
    }
}

TEST(nativeElisionGeometryRoundTripsWithBoundedValidation)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Protocol::Sync;
    RichText::Measurement value;
    value.isShaped = true;
    value.cellWidth = 512;
    value.cellHeight = 1024;
    value.lineEnds = { 2, 4 };
    value.geometry.lines = { { 0, 2, 0, 65536, 49152, 0, 8192 }, { 2, 4, 65536, 65536, 49152, 0, 8192 } };
    value.geometry.spans = { { 0, 1, 0, 0, 4096 }, { 1, 2, 0, 4096, 8192 }, { 2, 3, 1, 0, 4096 }, { 3, 4, 1, 4096, 8192 } };
    value.geometry.elisionEnd = 1;
    value.geometry.elision = std::make_shared<RichText::ShapedText>();
    value.geometry.elision->lines = { value.geometry.lines.front() };
    value.geometry.elision->spans = { value.geometry.spans[0], value.geometry.spans[1] };
    const auto roundTrip = [&] {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writeMeasurement(writer, value);
        WireReader reader(bytes);
        RichText::Measurement decoded;
        return readMeasurement(reader, decoded) && reader.isAtEnd();
    };
    CHECK(roundTrip());
    value.geometry.elisionEnd = 4;
    CHECK(!roundTrip());
    value.geometry.elisionEnd = 1;
    value.geometry.elision->spans.back().end = 3;
    CHECK(!roundTrip());
    value.geometry.elision->spans.back().end = 2;
    value.geometry.elision->spans.back().leading = -1;
    CHECK(!roundTrip());
}

TEST(twoPanesPublishIndependentTypographyAndRendererRevisions)
{
    UbiquittyTest::SyncLink link(0);
    const auto first = link.addPane(6, 30);
    const auto second = link.addPane(6, 30);
    auto firstFacts = phoneFacts();
    auto secondFacts = phoneFacts();
    secondFacts.cellWidth *= 2;
    secondFacts.rendererRevision = 7;
    link.client().publishTypography(first, firstFacts);
    link.client().publishTypography(second, secondFacts);
    link.step();
    REQUIRE(link.host().typography(first));
    REQUIRE(link.host().typography(second));
    CHECK(*link.host().typography(first) == firstFacts);
    CHECK(*link.host().typography(second) == secondFacts);
    const auto held = link.host().typography(second);
    ++firstFacts.rendererRevision;
    link.client().publishTypography(first, firstFacts);
    link.step();
    CHECK(*link.host().typography(first) == firstFacts);
    CHECK(link.host().typography(second) == held);
    link.unbind(first);
    link.step();
    CHECK(!link.host().typography(first));
    CHECK(link.host().typography(second) == held);
}

TEST(aNativeRequestCannotMeasureUnderANewerLocalRenderer)
{
    using namespace Ubiquitty::RichText;
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = phoneFacts();
    facts.shaping = 1;
    facts.rendersBlocks = 1;
    link.client().publishTypography(handle, facts);
    link.step();
    auto* replica = link.client().paneTerminal(handle);
    REQUIRE(replica);
    REQUIRE(adoptTypography(*replica, facts));
    unsigned calls = 0;
    replica->typography.shaper = { [](void* context, const Document*, const ShapeRequest*, const TypographyFacts*, ShapedText*) {
                                      ++*static_cast<unsigned*>(context);
                                      return false;
                                  },
        &calls };
    const std::string source = "{\\rtf1 versioned}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(document);
    MeasureRequest request { 90, 8, { 20, 10 }, *document };
    REQUIRE(link.host().requestRichText(handle, request, link.now()));
    // The local renderer changes before the request crosses the link. Its
    // dense metrics remain identical, but its native fallback may differ.
    ++facts.rendererRevision;
    REQUIRE(adoptTypography(*replica, facts));
    link.step();
    link.step();
    CHECK_EQ(calls, 0U);
    CHECK(link.host().takeRichTextReplies().empty());
    REQUIRE(link.host().typography(handle));
    CHECK_EQ(link.host().typography(handle)->rendererRevision, facts.rendererRevision);
    CHECK(link.host().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
    replica->typography.shaper = {};
}

TEST(typographyFramesRequireABoundPaneAndNonzeroRevision)
{
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, phoneFacts());
    std::vector<std::uint8_t> invalid;
    CHECK(!Ubiquitty::Protocol::Sync::appendFrame(invalid, Ubiquitty::Protocol::Sync::FrameKind::TypographyFacts, 0, body));
    link.sendToHost(Ubiquitty::Protocol::Sync::FrameKind::TypographyFacts, body, handle + 100);
    CHECK(link.host().state() == Ubiquitty::Protocol::Sync::ConnectionState::Closed);
}

TEST(aTypographyFrameCrossingAnUnbindLeavesTheConnectionOpen)
{
    UbiquittyTest::SyncLink link(0);
    const auto unbound = link.addPane(6, 30);
    const auto kept = link.addPane(6, 30);
    const auto facts = phoneFacts();
    link.client().publishTypography(kept, facts);
    link.step();
    REQUIRE(link.host().typography(kept));
    // The host lets one pane go. Before the client reads that PaneUnbind, a
    // renderer change publishes typography on the handle it still holds.
    link.unbind(unbound);
    std::vector<std::uint8_t> body;
    Ubiquitty::Protocol::Sync::encode(body, phoneFacts());
    link.sendToHost(Ubiquitty::Protocol::Sync::FrameKind::TypographyFacts, body, unbound);
    CHECK(link.host().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
    CHECK(!link.host().typography(unbound));
    REQUIRE(link.host().typography(kept));
    CHECK(*link.host().typography(kept) == facts);
}

TEST(nativeMeasurementDeadlineIsEnforcedBeforeReadingAnyReplyChunk)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    using namespace Ubiquitty::Protocol::Sync;
    for (const auto elapsed : { std::uint64_t { 99 }, std::uint64_t { 100 } }) {
        UbiquittyTest::SyncLink link(0);
        const auto handle = link.addPane(6, 30);
        auto facts = phoneFacts();
        facts.shaping = 1;
        link.client().publishTypography(handle, facts);
        link.step();
        const std::string source = "{\\rtf1 exact}";
        auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
        REQUIRE(document);
        MeasureRequest request { 107, 11, { 20, 10 }, *document };
        REQUIRE(link.host().requestRichText(handle, request, link.now(), 100));
        CHECK(!link.host().requestRichText(handle, request, link.now(), 100));
        const auto due = link.host().nextServiceMicroseconds(link.now());
        REQUIRE(due);
        CHECK(*due <= link.now() + 100);
        Measurement measured;
        measured.width = 1;
        measured.height = 1;
        measured.shown = 5;
        measured.cellWidth = facts.cellWidth;
        measured.cellHeight = facts.cellHeight;
        measured.isFitting = true;
        measured.lineEnds = { 5 };
        measured.rendererRevision = facts.rendererRevision;
        std::vector<std::uint8_t> body;
        WireWriter bodyWriter(body);
        writeMeasurement(bodyWriter, measured);
        std::vector<std::uint8_t> chunk;
        WireWriter writer(chunk);
        writer.u32(request.token);
        writer.u32(static_cast<std::uint32_t>(body.size()));
        writer.u32(0);
        writer.raw(body);
        // No service pass runs between advancing the clock and receiving bytes.
        link.advance(elapsed);
        link.sendToHost(FrameKind::RichTextMeasured, chunk, handle);
        const auto replies = link.host().takeRichTextReplies();
        CHECK_EQ(replies.size(), elapsed == 99 ? std::size_t { 1 } : std::size_t { 0 });
        CHECK(link.host().state() == ConnectionState::Attached);
    }
}

TEST(cancellationAndTerminalReconciliationRetireAlreadyDecodedReplies)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::RichText;
    using namespace Ubiquitty::Protocol::Sync;
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = phoneFacts();
    facts.shaping = 1;
    link.client().publishTypography(handle, facts);
    link.step();
    const std::string source = "{\\rtf1 exact}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(document);
    MeasureRequest request { 110, 11, { 20, 10 }, *document };
    Measurement measured;
    measured.width = 1;
    measured.height = 1;
    measured.shown = 5;
    measured.cellWidth = facts.cellWidth;
    measured.cellHeight = facts.cellHeight;
    measured.isFitting = true;
    measured.lineEnds = { 5 };
    measured.rendererRevision = facts.rendererRevision;
    const auto receive = [&](std::uint32_t token) {
        std::vector<std::uint8_t> body;
        WireWriter bodyWriter(body);
        writeMeasurement(bodyWriter, measured);
        std::vector<std::uint8_t> chunk;
        WireWriter writer(chunk);
        writer.u32(token);
        writer.u32(static_cast<std::uint32_t>(body.size()));
        writer.u32(0);
        writer.raw(body);
        link.sendToHost(FrameKind::RichTextMeasured, chunk, handle);
    };
    REQUIRE(link.host().requestRichText(handle, request, link.now()));
    receive(request.token);
    CHECK(!link.host().requestRichText(handle, request, link.now()));
    link.host().cancelRichText(handle, request.token);
    CHECK(link.host().takeRichTextReplies().empty());
    ++request.token;
    REQUIRE(link.host().requestRichText(handle, request, link.now()));
    receive(request.token);
    link.host().retainRichTextRequests(handle, {});
    CHECK(link.host().takeRichTextReplies().empty());
    ++request.token;
    REQUIRE(link.host().requestRichText(handle, request, link.now()));
    link.host().retainRichTextRequests(handle, {});
    receive(request.token);
    CHECK(link.host().takeRichTextReplies().empty());
    CHECK(link.host().state() == ConnectionState::Attached);
}

TEST(nativeMeasurementAdmissionRejectsExpiredOverflowingAndUnboundedWaits)
{
    using namespace Ubiquitty::RichText;
    UbiquittyTest::SyncLink link(0);
    const auto handle = link.addPane(6, 30);
    auto facts = phoneFacts();
    facts.shaping = 1;
    link.client().publishTypography(handle, facts);
    link.step();
    const std::string source = "{\\rtf1 bounded}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(document);
    MeasureRequest request { 112, 11, { 20, 10 }, *document };
    CHECK(!link.host().requestRichText(handle, request, link.now(), 0));
    CHECK(!link.host().requestRichText(handle, request, link.now(), 2'000'001));
    CHECK(!link.host().requestRichText(handle, request, std::numeric_limits<std::uint64_t>::max(), 1));
    CHECK(link.host().requestRichText(handle, request, link.now(), 1));
}

TEST(retentionPressureReplicatesHistoryRetirementAndRemainingDocuments)
{
    using namespace Ubiquitty::RichText;
    const std::string source = "{\\rtf1 " + std::string(1000, 'x') + "}";
    const auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(document);
    UbiquittyTest::LoopbackPaneLink link({ .rows = 6, .columns = 40 });
    REQUIRE(link.host().richTextBlocks.setStorageLimit(2 * (sizeof(Block) + document->storageBytes())));
    const std::array<std::string, 3> output {
        place("w=8:h=1:C=1:i=1", source) + "\x1b[2;1Hkeep\x1b[6;1H\n\n\x1b[1;1H",
        place("w=8:h=1:C=1:i=2", source) + "\x1b[6;1H\n\n\x1b[4;20HLIVE\x1b[1;1H",
        place("w=8:h=1:C=1:i=3", source),
    };
    const std::array<UbiquittyTest::PTYRead, 3> reads { {
        { .atMicroseconds = 1000, .bytes = output[0] },
        { .atMicroseconds = 41000, .bytes = output[1] },
        { .atMicroseconds = 81000, .bytes = output[2] },
    } };
    link.replay(reads);
    CHECK_MSG(link.failure().empty(), link.failure().c_str());
    CHECK_EQ(link.host().scrollbackCount, 3);
    CHECK_EQ(link.host().richTextBlocks.blocks(Ubiquitty::ScreenMode::Primary).size(), std::size_t { 2 });
    CHECK(link.host().richTextBlocks == link.client().richTextBlocks);

    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> bytes;
    REQUIRE(publisher.capture(link.host()));
    REQUIRE(publisher.checkpoint(link.host(), 1, bytes, baseline));
    auto late = makeTerminal(6, 40);
    ScreenReplica replica;
    REQUIRE(applyCheckpoint(*late, bytes, replica) == ApplyOutcome::Applied);
    CHECK(late->richTextBlocks == link.host().richTextBlocks);
    CHECK_EQ(late->richTextBlocks.storageLimit(), maxRetainedBytes);
}

TEST(nativeRequestRevisionAndSourceAnchorAreValidatedAsOneContext)
{
    using namespace Ubiquitty::RichText;
    using namespace Ubiquitty::Protocol::Sync;
    const std::string source = "{\\rtf1 anchor}";
    auto document = readRTF({ reinterpret_cast<const std::uint8_t*>(source.data()), source.size() });
    REQUIRE(document);
    MeasureRequest request { 120, 12, { 20, 10, Overflow::Wrap, Alignment::Left, LineMode::Terminal }, *document };
    request.rendererRevision = std::numeric_limits<std::uint64_t>::max();
    request.shape.viewportAnchor = static_cast<std::uint16_t>(document->text.size());
    const auto roundTrip = [&] {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writeMeasureRequest(writer, request);
        WireReader reader(bytes);
        MeasureRequest decoded;
        return readMeasureRequest(reader, decoded) && reader.isAtEnd()
            && decoded.rendererRevision == request.rendererRevision && decoded.shape.viewportAnchor == request.shape.viewportAnchor;
    };
    CHECK(roundTrip());
    request.rendererRevision = 0;
    CHECK(!roundTrip());
    request.rendererRevision = 1;
    ++request.shape.viewportAnchor;
    CHECK(!roundTrip());
    request.shape.viewportAnchor = 1;
    request.shape.lineMode = LineMode::Natural;
    CHECK(!roundTrip());
    request.shape.lineMode = LineMode::Terminal;
    request.shape.height = 1025;
    CHECK(!roundTrip());
    request.shape.height = 1024;
    CHECK(roundTrip());
}

TEST(nativeGeometryUsesTheRepresentableSignedCoordinateRange)
{
    using namespace Ubiquitty::RichText;
    using namespace Ubiquitty::Protocol::Sync;
    Measurement measured;
    measured.cellWidth = 512;
    measured.cellHeight = 1024;
    measured.isShaped = true;
    measured.lineEnds = { 1 };
    constexpr auto edge = std::numeric_limits<std::int32_t>::max() - 1;
    measured.geometry.lines = { { 0, 1, 0, 65536, 49152, 0, static_cast<std::uint32_t>(edge) } };
    measured.geometry.spans = { { 0, 1, 0, 0, edge } };
    const auto roundTrip = [&] {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writeMeasurement(writer, measured);
        WireReader reader(bytes);
        Measurement decoded;
        return readMeasurement(reader, decoded) && reader.isAtEnd();
    };
    CHECK(roundTrip());
    ++measured.geometry.spans.front().trailing;
    CHECK(!roundTrip());
    measured.geometry.spans.front().trailing = -1;
    CHECK(!roundTrip());
}

UBIQUITTY_TEST_MAIN()
