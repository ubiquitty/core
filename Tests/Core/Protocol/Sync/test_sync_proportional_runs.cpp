// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sync_proportional_runs.cpp — an OSC 66 proportional run in a pane a
// host runs and a client draws (Docs/proportional-text.md): the run reaches
// the client's replica with the pen it was printed under, at the cells the
// host placed it in, and the host answers a measure in the face the client
// draws it in.

#include "TerminalFixture.h"

#include "Core/Cell.h"
#include "Core/CellPen.h"
#include "Core/OSC/PublishedFaces.h"
#include "Core/OSC/TextSizing.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TextSizeTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Sync;

namespace {

std::unique_ptr<Terminal> makeDrawingTerminal()
{
    auto t = std::make_unique<Terminal>();
    hardReset(*t);
    resize(*t, 20, 40);
    t->drawsSizedText = true;
    t->drawsProportionalText = true;
    return t;
}

// A host and a client that draws runs, the client's checkpoint applied.
struct Pair {
    std::unique_ptr<Terminal> host = makeDrawingTerminal();
    std::unique_ptr<Terminal> client = makeDrawingTerminal();
    ScreenPublisher publisher;
    AttachmentBaseline attachment;
    ScreenReplica replica;
    std::uint64_t generation = 0;

    ApplyOutcome checkpoint()
    {
        std::vector<std::uint8_t> body;
        if (!publisher.capture(*host) || !publisher.checkpoint(*host, ++generation, body, attachment))
            return ApplyOutcome::Malformed;
        return applyCheckpoint(*client, body, replica);
    }

    /// Brings the replica to the host as a live host does: by an update, or
    /// by a checkpoint where the publisher can build no update.
    ApplyOutcome bring()
    {
        std::vector<std::uint8_t> body;
        if (!publisher.capture(*host))
            return ApplyOutcome::Malformed;
        if (publisher.rowUpdate(*host, attachment, body) == UpdateOutcome::Encoded)
            return applyRowUpdate(*client, body, replica);
        body.clear();
        if (!publisher.checkpoint(*host, ++generation, body, attachment))
            return ApplyOutcome::Malformed;
        return applyCheckpoint(*client, body, replica);
    }
};

/// What SGR sets in a pen: everything a renderer draws a cell's text with,
/// and nothing the claim stamps (the wide pair, the spec id, the band).
struct Rendition {
    std::uint16_t attributes = 0;
    std::uint16_t foreground = 0;
    std::uint16_t background = 0;
    std::uint16_t underline = 0;
    std::array<std::uint8_t, 9> rgb {};
    std::uint8_t underlineStyle = 0;
    std::uint8_t fontStyle = 0;
    unsigned weight = 0;

    friend bool operator==(const Rendition&, const Rendition&) = default;
};

Rendition renditionOf(const Cell& cell)
{
    const Pen pen = cellPen(cell);
    VT::AttributeSet attributes = pen.attributes;
    attributes.remove(VT::Attribute::Wide);
    attributes.remove(VT::Attribute::WideContinuation);
    return {
        .attributes = attributes.toRaw(),
        .foreground = pen.foregroundIndex,
        .background = pen.backgroundIndex,
        .underline = static_cast<std::uint16_t>(pen.underlineIndex),
        .rgb = { pen.foregroundRed, pen.foregroundGreen, pen.foregroundBlue, pen.backgroundRed, pen.backgroundGreen, pen.backgroundBlue, pen.underlineRed, pen.underlineGreen, pen.underlineBlue },
        .underlineStyle = pen.underlineStyle,
        .fontStyle = penFontStyle(&pen),
        .weight = static_cast<unsigned>(pen.extendedAttributes.weight()),
    };
}

/// Every SGR a run takes from its pen, one per row: a run under it, then one
/// grid letter under the same SGR, the rendition the run must match.
constexpr std::array<std::string_view, 16> renditions {
    "1",
    "3",
    "1;3",
    "2",
    "1:3",
    "4",
    "4:2",
    "4:3;58;5;27",
    "4:4;58;2;10;20;30",
    "4:5",
    "9",
    "53",
    "7",
    "38;5;160",
    "38;2;1;2;3;48;2;4;5;6",
    "44;97",
};

/// Prints rows `first` up to `end` of ``renditions``, each on its own row.
void printRenditions(Terminal& t, std::size_t first = 0, std::size_t end = renditions.size())
{
    std::string bytes;
    for (std::size_t row = first; row < end; ++row) {
        bytes += "\x1b[" + std::to_string(row + 1) + ";1H\x1b[" + std::string(renditions[row]) + "m";
        bytes += "\x1b]66;p=1:c=6;Title\x1b\\X\x1b[0m";
    }
    UbiquittyTest::feed(t, bytes);
}

/// Checks rows `first` up to `end` of `t`: the run's anchor at column 0 and
/// its continuations carry the rendition of the grid letter after them.
void checkRenditions(const Terminal& t, std::string_view where, std::size_t first = 0, std::size_t end = renditions.size())
{
    for (std::size_t row = first; row < end; ++row) {
        const Cell* anchor = Screen::visibleCell(&t, 0, static_cast<int>(row), 0);
        const Cell* tail = Screen::visibleCell(&t, 0, static_cast<int>(row), 5);
        const Cell* letter = Screen::visibleCell(&t, 0, static_cast<int>(row), 6);
        REQUIRE(anchor && tail && letter);
        const std::string label = std::string(where) + ", SGR " + std::string(renditions[row]);
        CHECK_MSG(isProportionalRunAnchor(&t, *anchor), (label + ": the run is placed").c_str());
        CHECK_MSG(letter->codePoint == 'X', (label + ": the grid letter follows the claim").c_str());
        CHECK_MSG(renditionOf(*anchor) == renditionOf(*letter), (label + ": the anchor carries the grid's rendition").c_str());
        CHECK_MSG(renditionOf(*tail) == renditionOf(*letter), (label + ": a continuation carries it too").c_str());
    }
}

} // namespace

// A run printed under SGR carries that SGR on its cells, as grid text does,
// on the host and on a client's replica, whether the replica took the run in
// its first checkpoint or after it held the screen: the renderer reads a
// run's weight, slant, rules and colours from its anchor's pen, so a replica
// that dropped any of them would draw the run plainer than the grid text
// beside it.
TEST(aRunKeepsEveryRenditionOnTheHostAndOnTheReplica)
{
    {
        Pair pair;
        printRenditions(*pair.host);
        checkRenditions(*pair.host, "the host");
        CHECK(pair.checkpoint() == ApplyOutcome::Applied);
        checkRenditions(*pair.client, "a first checkpoint");
    }
    {
        Pair pair;
        CHECK(pair.checkpoint() == ApplyOutcome::Applied);
        for (std::size_t row = 0; row < renditions.size(); ++row) {
            printRenditions(*pair.host, row, row + 1);
            CHECK(pair.bring() == ApplyOutcome::Applied);
            checkRenditions(*pair.client, "a replica that held the screen", row, row + 1);
        }
    }
}

namespace {

/// The column of the last `│` on `row`, or -1.
int borderColumn(const Terminal& t, int row)
{
    int found = -1;
    for (int column = 0; column < t.columns; ++column) {
        const Cell* at = Screen::visibleCell(&t, 0, row, column);
        if (at && at->codePoint == 0x2502)
            found = column;
    }
    return found;
}

} // namespace

// A right-aligned run advances by its claim, not by its text, and the replica
// holds the cells where the host put them: the border after it stands `c`
// cells on, whether the run was stored as a run or, with every spec slot
// taken, set in the grid face inside its claim.
TEST(anAlignedRunAdvancesByItsClaimOnTheHostAndOnTheReplica)
{
    for (const bool isTableFull : { false, true }) {
        Pair pair;
        CHECK(pair.checkpoint() == ApplyOutcome::Applied);
        if (isTableFull) {
            for (int cells = 1; cells <= textSizeTableCapacity; ++cells)
                UbiquittyTest::feed(*pair.host, "\x1b[" + std::to_string(cells) + ";1H\x1b]66;p=1:c=" + std::to_string(cells) + ";x\x1b\\");
        }
        UbiquittyTest::feed(*pair.host, "\x1b[17;1H\xe2\x94\x82 \x1b]66;p=1:c=17:h=1;Age\x1b\\ \xe2\x94\x82");
        const std::string label = isTableFull ? "with every spec slot taken" : "stored as a run";
        CHECK_MSG(borderColumn(*pair.host, 16) == 20, (label + ": the host's border stands the claim on").c_str());
        CHECK(pair.bring() == ApplyOutcome::Applied);
        CHECK_MSG(borderColumn(*pair.client, 16) == 20, (label + ": the replica's border stands where the host's does").c_str());
        const Cell* first = Screen::visibleCell(pair.client.get(), 0, 16, 2);
        REQUIRE(first);
        CHECK_MSG(isProportionalRunAnchor(pair.client.get(), *first) != isTableFull, (label + ": the claim's first cell").c_str());
        if (isTableFull) {
            const Cell* letter = Screen::visibleCell(pair.client.get(), 0, 16, 16);
            CHECK_MSG(letter && letter->codePoint == 'A', "the grid letters are set right in the claim");
        }
    }
}

namespace {

/// A client's measurer whose letters are wider than its cell, as a text face
/// beside a monospace grid is: every code point one glyph of `advance`
/// pixels, a pixel more in bold, in eight-pixel cells.
struct WideFaceMeasurer final : TextMeasureClient {
    explicit WideFaceMeasurer(float letter)
        : advance(letter)
    {
    }
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, ProportionalRunStyle style, std::span<ProportionalShapedGlyph> glyphsOut) override
    {
        const std::size_t count = std::min(codePoints.size(), glyphsOut.size());
        for (std::size_t index = 0; index < count; ++index) {
            glyphsOut[index] = { .glyph = codePoints[index],
                .advanceInPixels = advance + ((style.faceBits & 1) != 0 ? 1.0f : 0.0f),
                .cluster = static_cast<std::uint16_t>(index) };
        }
        return static_cast<int>(count);
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
    float advance;
};

struct ScriptWeightMeasurer final : TextMeasureClient {
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, ProportionalRunStyle style, std::span<ProportionalShapedGlyph> out) override
    {
        const std::size_t count = std::min(codePoints.size(), out.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto codePoint = codePoints[index];
            float advance = 9.0f;
            if (codePoint == 'W' || codePoint == 'M')
                advance = 15.0f;
            if (codePoint == U'Ж' || codePoint == U'Ш' || codePoint == U'Щ')
                advance = 17.0f;
            if (codePoint == U'Ω' || codePoint == U'Ψ')
                advance = 16.0f;
            if (codePoint == U'日' || codePoint == U'🚀')
                advance = 17.0f;
            if (style.faceBits & 1)
                advance += 2.0f;
            advance += static_cast<float>(style.weight) * 0.5f;
            out[index] = { .glyph = codePoint, .advanceInPixels = advance, .cluster = static_cast<std::uint16_t>(index) };
        }
        return static_cast<int>(count);
    }
    float proportionalCellAdvanceInPixels() override { return 8.0f; }
};

int realFaceCells(ScriptWeightMeasurer& measurer, std::span<const std::uint32_t> codePoints, ProportionalRunStyle style)
{
    std::array<ProportionalShapedGlyph, 16> glyphs {};
    const int count = measurer.shapeProportionalRun(codePoints, style, glyphs);
    double advance = 0.0;
    for (int index = 0; index < count; ++index)
        advance += glyphs[static_cast<std::size_t>(index)].advanceInPixels;
    return static_cast<int>(std::ceil(advance / measurer.proportionalCellAdvanceInPixels()));
}

/// The reply a terminal gives a measure of `text`, under `sgr`.
std::string measureReply(Terminal& t, std::string_view text, std::string_view sgr = "0")
{
    UbiquittyTest::feed(t, "\x1b[" + std::string(sgr) + "m\x1b]66;p=1:q=1:i=1;" + std::string(text) + "\x1b\\\x1b[0m");
    std::array<char, 128> reply {};
    const int length = UbiquittyTest::drain(t, reply);
    return std::string(reply.data(), static_cast<std::size_t>(std::max(length, 0)));
}

/// The faces a client's measurer publishes, as the host decodes them off the wire.
std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts> publishedFaces(TextMeasureClient& measurer)
{
    OSC::TextSizing::ProportionalFaceFacts faces;
    if (!OSC::TextSizing::measureProportionalFaces(measurer, faces))
        return nullptr;
    std::vector<std::uint8_t> body;
    encode(body, faces);
    std::optional<OSC::TextSizing::ProportionalFaceFacts> decoded = decodeProportionalFaces(body);
    return decoded ? std::make_shared<const OSC::TextSizing::ProportionalFaceFacts>(*decoded) : nullptr;
}

constexpr std::array<std::string_view, 4> labels { "Name", "Now", "Deployment Monitor", "Latency (p95)" };

} // namespace

// A pane the host runs is answered in the face its client draws runs in: the
// client measures its faces with its own measurer and publishes them, and the
// host's measure of each label is the client's local one, in the regular and
// the bold face. The host draws nothing, so without the client's faces it
// answers in the grid's width, where letters wider than a cell clip: `Name`
// in four cells is drawn `Nam`.
TEST(aHostedPaneAnswersAMeasureInTheFaceItsClientDraws)
{
    Pair pair;
    WideFaceMeasurer clientFace(10.4f);
    pair.client->textMeasureClient = CheckedPtr<TextMeasureClient>(&clientFace);
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(&hostFaces);
    CHECK_MSG(measureReply(*pair.host, "Name") == "\x1b]66;i=1:c=4\x1b\\", "premise: with no client faces the host answers the grid width");

    const std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts> faces = publishedFaces(clientFace);
    REQUIRE(faces);
    hostFaces.setFaces(std::span(&faces, 1), pair.host->ambiguousWidth);
    for (const std::string_view label : labels) {
        for (const std::string_view sgr : { std::string_view("0"), std::string_view("1") }) {
            const std::string local = measureReply(*pair.client, label, sgr);
            const std::string hosted = measureReply(*pair.host, label, sgr);
            CHECK_MSG(!local.empty() && hosted == local, (std::string(label) + " under SGR " + std::string(sgr) + ": host " + hosted.substr(1) + ", client " + local.substr(1)).c_str());
        }
    }
    CHECK_MSG(measureReply(*pair.host, "Name") == "\x1b]66;i=1:c=6\x1b\\", "four 10.4-pixel letters in 8-pixel cells take six");
    pair.client->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
}

// Two clients with different faces viewing one pane: each run is answered in
// the wider, so a program that lays out by the answer clips on neither, and
// the narrower client draws the slack.
TEST(aPaneTwoClientsDrawIsAnsweredInTheWiderFace)
{
    Pair pair;
    WideFaceMeasurer narrow(9.0f);
    WideFaceMeasurer wide(13.0f);
    const std::array faces { publishedFaces(narrow), publishedFaces(wide), publishedFaces(narrow) };
    REQUIRE(faces[0] && faces[1]);
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    hostFaces.setFaces(faces, pair.host->ambiguousWidth);
    CHECK_MSG(hostFaces.sourceCount() == 2, "a repeated client's faces count once");
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(&hostFaces);
    // Four letters: 36 pixels in the narrow face, five cells; 52 in the wide, seven.
    CHECK_MSG(measureReply(*pair.host, "Name") == "\x1b]66;i=1:c=7\x1b\\", measureReply(*pair.host, "Name").substr(1).c_str());
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
}

// A host counts in published advances and bounds an untabled wide glyph by
// the wider of two cells and the face's widest tabled glyph.
TEST(publishedFacesCountEveryCodePointAndRefuseAFaceWithNoCell)
{
    WideFaceMeasurer clientFace(10.4f);
    const std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts> faces = publishedFaces(clientFace);
    REQUIRE(faces);
    CHECK_EQ(faces->cellAdvance, 8u * 64u);
    CHECK_EQ(static_cast<unsigned>(faces->advances[0]['N' - OSC::TextSizing::firstFacedCodePoint]), 666u);
    CHECK_EQ(static_cast<unsigned>(faces->untabledAdvances[1]), 730u);
    const auto cells = [&](std::u32string_view text) {
        const std::vector<std::uint32_t> codePoints(text.begin(), text.end());
        return OSC::TextSizing::tabledRunCells(*faces, codePoints, {}, Unicode::Width::AmbiguousWidth::Narrow);
    };
    // Eight letters of 666/64 pixels: 83.25 pixels, eleven cells.
    CHECK_EQ(cells(U"abcdefgh"), 11);
    // Cyrillic is tabled; a CJK glyph beyond the table remains bounded.
    CHECK_EQ(cells(U"\u0416\u0418"), 3);
    CHECK_EQ(cells(U"\u65E5"), 2);
    CHECK_EQ(cells(U"e\u0301\u0301"), 4);

    OSC::TextSizing::ProportionalFaceFacts cellless = *faces;
    cellless.cellAdvance = 0;
    std::vector<std::uint8_t> body;
    encode(body, cellless);
    CHECK_MSG(!decodeProportionalFaces(body), "faces with no cell are refused");
    body.clear();
    encode(body, *faces);
    static_assert(OSC::TextSizing::publishedFaceBodyBytes == 12'876);
    CHECK_EQ(body.size(), OSC::TextSizing::publishedFaceBodyBytes);
    CHECK(body.size() <= maxFrameBodyBytes);
    body.push_back(0);
    CHECK_MSG(!decodeProportionalFaces(body), "a body with a byte past the faces is refused");

    // A face cannot stretch a run past eight cells a code point, nor count in
    // a cell under a pixel, so no client can make every answer the widest.
    OSC::TextSizing::ProportionalFaceFacts tinyCell = *faces;
    tinyCell.cellAdvance = 1;
    CHECK_MSG(!OSC::TextSizing::isAdoptable(tinyCell), "a cell under a pixel is refused");
    OSC::TextSizing::ProportionalFaceFacts stretched = *faces;
    stretched.advances[0]['N' - OSC::TextSizing::firstFacedCodePoint] = static_cast<std::uint16_t>(stretched.cellAdvance * OSC::TextSizing::maxFacedAdvanceCells + 1);
    CHECK_MSG(!OSC::TextSizing::isAdoptable(stretched), "an advance wider than eight cells is refused");
    body.clear();
    encode(body, stretched);
    CHECK_MSG(!decodeProportionalFaces(body), "and is refused off the wire");
    OSC::TextSizing::ProportionalFaceFacts stretchedWeight = *faces;
    stretchedWeight.weightedAdvances[0]['N' - OSC::TextSizing::firstFacedCodePoint] = 129;
    CHECK_MSG(!OSC::TextSizing::isAdoptable(stretchedWeight), "a weighted advance over eight cells is refused");
    body.clear();
    encode(body, stretchedWeight);
    CHECK_MSG(!decodeProportionalFaces(body), "an excessive weighted advance is refused off the wire");
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    const std::array offered { std::make_shared<const OSC::TextSizing::ProportionalFaceFacts>(stretched) };
    hostFaces.setFaces(offered, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_MSG(hostFaces.sourceCount() == 0, "a host counts in no face it cannot adopt");
}

TEST(publishedBoldAndVariableWeightsBoundTheRealFace)
{
    ScriptWeightMeasurer measurer;
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    constexpr std::array<std::uint32_t, 2> wideLatin { 'W', 'M' };
    for (const ProportionalRunStyle style : { ProportionalRunStyle { 1, 0 }, ProportionalRunStyle { 0, 7 }, ProportionalRunStyle { 1, 3 } }) {
        const int real = realFaceCells(measurer, wideLatin, style);
        const int published = OSC::TextSizing::tabledRunCells(*faces, wideLatin, style, Unicode::Width::AmbiguousWidth::Narrow);
        CHECK_MSG(published >= real, "a bold or variable-weight run needs at least its drawn advance");
    }
}

TEST(publishedCyrillicAdvancesBoundWideLetters)
{
    ScriptWeightMeasurer measurer;
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    constexpr std::array<std::uint32_t, 3> cyrillic { U'Ж', U'Ш', U'Щ' };
    constexpr ProportionalRunStyle style { 1, 5 };
    const int real = realFaceCells(measurer, cyrillic, style);
    const int published = OSC::TextSizing::tabledRunCells(*faces, cyrillic, style, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_MSG(published >= real, "a Cyrillic run needs at least the face's real advance");
    constexpr std::array<std::uint32_t, 2> greek { U'Ω', U'Ψ' };
    CHECK(OSC::TextSizing::tabledRunCells(*faces, greek, style, Unicode::Width::AmbiguousWidth::Narrow)
        >= realFaceCells(measurer, greek, style));
}

TEST(publishedCJKRunPastTheTableHasABoundedClaim)
{
    ScriptWeightMeasurer measurer;
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    for (const ProportionalRunStyle style : { ProportionalRunStyle { 0, 0 }, ProportionalRunStyle { 1, 5 } }) {
        constexpr std::array<std::uint32_t, 2> cjk { 'W', U'日' };
        const int real = realFaceCells(measurer, cjk, style);
        const int published = OSC::TextSizing::tabledRunCells(*faces, cjk, style, Unicode::Width::AmbiguousWidth::Narrow);
        CHECK_MSG(published >= real, "an untabled CJK glyph needs its drawn advance");
        CHECK_MSG(published < OSC::TextSizing::maxProportionalRunCells, "one CJK glyph cannot claim the whole row");
    }
}

TEST(publishedEmojiRunPastTheTableHasABoundedClaim)
{
    ScriptWeightMeasurer measurer;
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    for (const ProportionalRunStyle style : { ProportionalRunStyle { 0, 0 }, ProportionalRunStyle { 1, 5 } }) {
        constexpr std::array<std::uint32_t, 2> emoji { 'W', U'🚀' };
        const int real = realFaceCells(measurer, emoji, style);
        const int published = OSC::TextSizing::tabledRunCells(*faces, emoji, style, Unicode::Width::AmbiguousWidth::Narrow);
        CHECK_MSG(published >= real, "an untabled emoji needs its drawn advance");
        CHECK_MSG(published < OSC::TextSizing::maxProportionalRunCells, "one emoji cannot claim the whole row");
    }
}

TEST(publishedNarrowRunPastTheTableUsesTheWidestFacedAdvance)
{
    ScriptWeightMeasurer measurer;
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    constexpr std::array<std::uint32_t, 2> narrow { 'W', U'Ḃ' };
    const ProportionalRunStyle style { 0, 0 };
    const int real = realFaceCells(measurer, narrow, style);
    const int published = OSC::TextSizing::tabledRunCells(*faces, narrow, style, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_MSG(published >= real, "an untabled narrow glyph needs its drawn advance");
    CHECK_MSG(published < OSC::TextSizing::maxProportionalRunCells, "one narrow glyph cannot claim the whole row");
}

// The host counts every offer's glyph and weight, including a ninth face
// whose regular Latin is narrow but whose bold Cyrillic is widest.
TEST(aHostCountsEveryOfferedFaceWhateverTheirOrder)
{
    std::vector<std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts>> narrowCopies;
    WideFaceMeasurer narrow(9.0f);
    WideFaceMeasurer wide(13.0f);
    const auto narrowFaces = publishedFaces(narrow);
    const auto wideFaces = publishedFaces(wide);
    REQUIRE(narrowFaces && wideFaces);
    for (int copy = 0; copy < 8; ++copy)
        narrowCopies.push_back(std::make_shared<const OSC::TextSizing::ProportionalFaceFacts>(*narrowFaces));
    narrowCopies.push_back(wideFaces);
    const std::vector<std::uint32_t> name { 'N', 'a', 'm', 'e' };
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    hostFaces.setFaces(narrowCopies, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_EQ(static_cast<int>(hostFaces.sourceCount()), 2);
    CHECK_EQ(hostFaces.proportionalRunCells(name, {}), 7);

    // Nine distinct faces, offered both ways round. The ninth's regular
    // Latin is narrow; its bold Cyrillic is wider than any prior offer.
    std::vector<std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts>> distinct;
    for (int step = 0; step < 8; ++step) {
        WideFaceMeasurer face(9.0f + 0.1f * static_cast<float>(step));
        distinct.push_back(publishedFaces(face));
        REQUIRE(distinct.back());
    }
    auto ninth = std::make_shared<OSC::TextSizing::ProportionalFaceFacts>(*narrowFaces);
    const auto zhe = OSC::TextSizing::facedCodePointIndex(U'Ж');
    REQUIRE(zhe);
    ninth->weightedAdvances[1][*zhe] = 8 * OSC::TextSizing::weightedAdvanceFractionsPerCell;
    distinct.push_back(ninth);
    OSC::TextSizing::PublishedFacesMeasurer forward;
    forward.setFaces(distinct, Unicode::Width::AmbiguousWidth::Narrow);
    std::vector<std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts>> reversed(distinct.rbegin(), distinct.rend());
    OSC::TextSizing::PublishedFacesMeasurer backward;
    backward.setFaces(reversed, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_EQ(forward.sourceCount(), 9u);
    CHECK_EQ(backward.sourceCount(), 9u);
    const std::array<std::uint32_t, 1> cyrillic { U'Ж' };
    const ProportionalRunStyle bold { 1, 1 };
    const int ninthCells = OSC::TextSizing::tabledRunCells(*ninth, cyrillic, bold, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_MSG(forward.proportionalRunCells(cyrillic, bold) >= ninthCells, "the ninth face's bold Cyrillic dominates");
    CHECK_EQ(backward.proportionalRunCells(cyrillic, bold), forward.proportionalRunCells(cyrillic, bold));
    distinct.pop_back();
    forward.setFaces(distinct, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_MSG(forward.proportionalRunCells(cyrillic, bold) < ninthCells, "removing the wide offer rebuilds the envelope");

    // The 65th offer does not expand a pane's bounded face set.
    std::vector<std::shared_ptr<const OSC::TextSizing::ProportionalFaceFacts>> overLimit(65, narrowFaces);
    overLimit.back() = wideFaces;
    hostFaces.setFaces(overLimit, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_EQ(hostFaces.sourceCount(), 1u);
    CHECK_EQ(hostFaces.proportionalRunCells(name, {}), 5);

    // An offer with no face left: the grid's width answers.
    hostFaces.setFaces({}, Unicode::Width::AmbiguousWidth::Narrow);
    CHECK_EQ(static_cast<int>(hostFaces.sourceCount()), 0);
    CHECK_EQ(hostFaces.proportionalRunCells(name, {}), 0);
}

TEST(aGridOnlyClientOffersNoFaceToTheHost)
{
    WideFaceMeasurer measurer(13.0f);
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    Pair pair;
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(&hostFaces);
    for (const bool drawsRuns : { false, true }) {
        pair.client->drawsProportionalText = drawsRuns;
        if (OSC::TextSizing::mayOfferFaceFacts(*pair.client))
            hostFaces.setFaces(std::span(&faces, 1), pair.host->ambiguousWidth);
        else
            hostFaces.setFaces({}, pair.host->ambiguousWidth);
        CHECK_EQ(hostFaces.sourceCount(), drawsRuns ? 1u : 0u);
        CHECK(measureReply(*pair.host, "Name") == (drawsRuns ? "\x1b]66;i=1:c=7\x1b\\" : "\x1b]66;i=1:c=4\x1b\\"));
    }
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
}

TEST(aHostAnswersAnOwedQueryWhenFacesArriveInReplyOrder)
{
    Pair pair;
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(&hostFaces);
    hostFaces.notePublicationState(OSC::TextSizing::PublishedFacesMeasurer::FacePublicationState::Owed, 1'000);
    CHECK(measureReply(*pair.host, "Name").empty());
    Ubiquitty::respond(*pair.host, Capability::None, "later");
    std::array<char, 128> output {};
    CHECK_EQ(UbiquittyTest::drain(*pair.host, output), 0);

    WideFaceMeasurer measurer(13.0f);
    const auto faces = publishedFaces(measurer);
    REQUIRE(faces);
    hostFaces.setFaces(std::span(&faces, 1), pair.host->ambiguousWidth);
    hostFaces.notePublicationState(OSC::TextSizing::PublishedFacesMeasurer::FacePublicationState::Ready, 1'120);
    CHECK(hostFaces.releaseHeld(*pair.host, 1'120));
    const int length = UbiquittyTest::drain(*pair.host, output);
    CHECK(std::string(output.data(), static_cast<std::size_t>(length)) == "\x1b]66;i=1:c=7\x1b\\later");
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
}

TEST(aHostAnswersAnOwedQueryInGridWidthAtTheDeadline)
{
    Pair pair;
    OSC::TextSizing::PublishedFacesMeasurer hostFaces;
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(&hostFaces);
    hostFaces.notePublicationState(OSC::TextSizing::PublishedFacesMeasurer::FacePublicationState::Owed, 1'000);
    CHECK(measureReply(*pair.host, "Name").empty());
    CHECK_EQ(hostFaces.heldDeadlineMilliseconds(), 1'250u);
    CHECK(!hostFaces.releaseHeld(*pair.host, 1'249));
    CHECK(hostFaces.releaseHeld(*pair.host, 1'250));
    std::array<char, 128> output {};
    const int length = UbiquittyTest::drain(*pair.host, output);
    CHECK(std::string(output.data(), static_cast<std::size_t>(length)) == "\x1b]66;i=1:c=4\x1b\\");
    CHECK(measureReply(*pair.host, "Name") == "\x1b]66;i=1:c=4\x1b\\");
    pair.host->textMeasureClient = CheckedPtr<TextMeasureClient>(nullptr);
}

UBIQUITTY_TEST_MAIN()
