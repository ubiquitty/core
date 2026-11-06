// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Diagnostics/EscapeDecode.h"
#include "Core/Diagnostics/SnapshotArguments.h"
#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/CursorPreferences.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/RenderStyle.h"
#include "Core/Preferences/SnapshotConfigurationFlags.h"
#include "Core/Rendering/BackgroundImage.h"
#include "Core/Rendering/BundledRichText.h"
#include "Core/Rendering/FontSelection.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/OverlaySpanGather.h"
#include "Core/Rendering/RenderPathMarker.h"
#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/RichText/Blocks.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Runtime/TriggerConfig.h"
#include "Core/Screen/Marks.h"
#include "Core/Search/SearchController.h"
#include "Core/Selection/Selection.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct Arguments {
    const char* snapshotPath = nullptr;
    const char* configPath = nullptr;
    const char* feedFile = nullptr;
    // The literal a search runs for after the feed, or null for no search. A
    // focused match is the terminal's own search state, which is what the
    // frame reads; the reference cannot draw the pill from anything else.
    const char* searchLiteral = nullptr;
    std::string feed;
    int columns = 80;
    // The backing scale every arm renders a comparison at. 2 because that is what
    // a Retina macOS surface reports, and an image compared across arms has to be
    // the same size before its pixels mean anything.
    double deviceScale = 2.0;
    int rows = 24;
    // The width the feed is written at, before the grid is resized to
    // `columns` and rewrapped; 0 writes it at `columns`.
    int reflowFromColumns = 0;
    int viewOffset = 0;
    bool shouldProbeReply = false;
    bool shouldClearToMark = false;
    bool shouldClearPreservingInput = false;
    bool drawsSizedText = false;
    // --select-rect r0,c0,r1,c1: a character selection over visible cells.
    std::optional<Ubiquitty::SelectionBounds> selection;
    // --select-points x0,y0,x1,y1: a plain drag between two points, in cells
    // across and rows down, run through the pointer ladder's caret gesture.
    std::optional<std::array<double, 4>> selectionPoints;
    // --unfocused: the frame of a view without keyboard focus.
    bool isUnfocused = false;
    // The flags that are configuration keys, written into the case's document.
    Ubiquitty::Preferences::SnapshotConfigurationFlags configurationFlags;
};

[[noreturn]] void fail(const char* message)
{
    Ubiquitty::PAL::logLine(stderr, "ubiquitty-snapshot: {}", message);
    std::exit(2);
}

bool readFile(const char* path, std::string& output)
{
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        output.append(buffer, count);
    return std::fclose(file) == 0;
}

bool writeFile(const char* path, const std::vector<std::uint8_t>& bytes)
{
    FILE* file = std::fopen(path, "wb");
    if (!file)
        return false;
    const bool didWriteAll = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && didWriteAll;
}

Arguments parseArguments(int argc, const char* argv[])
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        auto next = [&]() -> const char* { return index + 1 < argc ? argv[++index] : nullptr; };
        if (!std::strcmp(argument, "--snapshot"))
            arguments.snapshotPath = next();
        else if (!std::strcmp(argument, "--snapshot-backend")) {
            const char* backend = next();
            if (!backend || std::strcmp(backend, "portable"))
                fail("only the portable backend is available");
        } else if (!std::strcmp(argument, "--probe-reply"))
            arguments.shouldProbeReply = true;
        else if (!std::strcmp(argument, "--config"))
            arguments.configPath = next();
        else if (!std::strcmp(argument, "--search"))
            arguments.searchLiteral = next();
        else if (!std::strcmp(argument, "--feed-file"))
            arguments.feedFile = next();
        else if (!std::strcmp(argument, "--feed")) {
            const char* value = next();
            if (value)
                arguments.feed = Ubiquitty::Diagnostics::decodeEscapes(value);
        } else if (!std::strcmp(argument, "--device-scale")) {
            const char* value = next();
            if (value)
                arguments.deviceScale = std::atof(value);
        } else if (!std::strcmp(argument, "--columns")) {
            const char* value = next();
            if (value)
                arguments.columns = std::atoi(value);
        } else if (!std::strcmp(argument, "--rows")) {
            const char* value = next();
            if (value)
                arguments.rows = std::atoi(value);
        } else if (!std::strcmp(argument, "--reflow-from-columns")) {
            const char* value = next();
            if (value)
                arguments.reflowFromColumns = std::max(std::atoi(value), 0);
        } else if (!std::strcmp(argument, "--select-points")) {
            const char* value = next();
            std::array<double, 4> points {};
            if (!value || std::sscanf(value, "%lf,%lf,%lf,%lf", &points[0], &points[1], &points[2], &points[3]) != 4)
                fail("--select-points takes x0,y0,x1,y1");
            arguments.selectionPoints = points;
        } else if (!std::strcmp(argument, "--view-offset")) {
            const char* value = next();
            if (value)
                arguments.viewOffset = std::atoi(value);
        } else if (!std::strcmp(argument, "--clear-to-mark"))
            arguments.shouldClearToMark = true;
        else if (!std::strcmp(argument, "--clear-preserve-input"))
            arguments.shouldClearPreservingInput = true;
        else if (!std::strcmp(argument, "--draws-sized-text"))
            arguments.drawsSizedText = true;
        else if (!std::strcmp(argument, "--unfocused"))
            arguments.isUnfocused = true;
        else if (!std::strcmp(argument, "--select-rect")) {
            const char* value = next();
            Ubiquitty::SelectionBounds bounds {};
            if (!value || std::sscanf(value, "%d,%d,%d,%d", &bounds.row0, &bounds.column0, &bounds.row1, &bounds.column1) != 4)
                fail("--select-rect takes r0,c0,r1,c1");
            arguments.selection = bounds;
        } else {
            using Ubiquitty::Preferences::SnapshotFlagParse;
            const SnapshotFlagParse parsed = Ubiquitty::Preferences::parseSnapshotConfigurationFlag(argument, next, arguments.configurationFlags);
            if (parsed == SnapshotFlagParse::Malformed)
                fail("a configuration flag is missing its value or did not parse");
            if (parsed == SnapshotFlagParse::NotConfiguration)
                fail("unknown argument");
        }
    }
    if (!arguments.snapshotPath && !arguments.shouldProbeReply)
        fail("expected --snapshot or --probe-reply");
    if (arguments.snapshotPath && arguments.shouldProbeReply)
        fail("--snapshot and --probe-reply are mutually exclusive");
    if (arguments.feedFile && !readFile(arguments.feedFile, arguments.feed))
        fail("--feed-file could not be read");
    arguments.columns = std::max(arguments.columns, 2);
    arguments.rows = std::max(arguments.rows, 2);
    return arguments;
}

/// Answers a proportional run's measure in the face the frame draws it in, as
/// every frontend that draws runs does, so a query in the feed and a rewrap
/// after it agree with the frame.
struct UnifontMeasurer final : Ubiquitty::TextMeasureClient {
    UnifontMeasurer(Ubiquitty::Rendering::UnifontRasterizer& face, int cellAdvance)
        : m_face(face)
        , m_cellAdvance(cellAdvance)
    {
    }
    int shapeProportionalRun(std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style, std::span<Ubiquitty::ProportionalShapedGlyph> glyphsOut) override
    {
        return m_face.shapeProportionalRun(codePoints, static_cast<Ubiquitty::Rendering::FontFace>(style.faceBits), glyphsOut);
    }
    float proportionalCellAdvanceInPixels() override { return static_cast<float>(m_cellAdvance); }

private:
    Ubiquitty::Rendering::UnifontRasterizer& m_face;
    int m_cellAdvance;
};

void printReply(Ubiquitty::Terminal& terminal)
{
    std::string reply;
    char buffer[256];
    for (;;) {
        const int count = Ubiquitty::drainOutput(&terminal, buffer);
        if (count <= 0)
            break;
        reply.append(buffer, static_cast<std::size_t>(count));
    }
    std::fputs("REPLY-HEX ", stdout);
    for (const unsigned char byte : reply)
        Ubiquitty::PAL::logText(stdout, "{:02x}", byte);
    std::fputc('\n', stdout);
}

} // namespace

int main(int argc, const char* argv[])
{
    const Arguments arguments = parseArguments(argc, argv);
    // The case's document becomes the process's configuration, whole. A driver
    // that parsed it into a local would render the case's font at the case's
    // size under whatever the developer's own file says — `Preferences::values()`
    // is the machine's blob until something configures it — and report
    // success. A case naming no colour space gets the default table's, not the
    // one in a home directory. Strict: a misspelt key fails the case rather
    // than drawing with the key missing.
    std::string document;
    if (arguments.configPath && !readFile(arguments.configPath, document))
        fail("configuration could not be read");
    // The flags that state a key — `--tek-glow` is `[tek] glow_vectors` — are
    // lines of the document the fold reads rather than values beside it.
    std::optional<std::string> stated = Ubiquitty::Preferences::stateSnapshotConfigurationFlags(arguments.configurationFlags, std::move(document));
    if (!stated)
        fail("a configuration flag could not be written into the document");
    document = std::move(*stated);
    const Ubiquitty::Preferences::ConfigureResult configured = Ubiquitty::Preferences::configure(document, Ubiquitty::Preferences::ParseMode::Strict);
    if (!configured.isApplied) {
        for (const Ubiquitty::Preferences::SkippedKey& skipped : configured.diagnostics)
            Ubiquitty::PAL::logLine(stderr, "{}: {}", arguments.configPath, Ubiquitty::Preferences::diagnosticSentence(skipped));
        fail("configuration is invalid");
    }
    const Ubiquitty::Preferences::Values& configuration = Ubiquitty::Preferences::values();
    // The configured background image, decoded once: a case naming one that
    // does not load fails rather than drawing the window without it.
    Ubiquitty::Rendering::BackgroundImagePixels backgroundImage;
    if (const std::string path = configuration.backgroundImage.value_or(std::string {}); !path.empty()) {
        std::string bytes;
        if (!readFile(path.c_str(), bytes))
            fail("the configured background image could not be read");
        backgroundImage = Ubiquitty::Rendering::decodeBackgroundImage(std::span { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
        if (backgroundImage.isEmpty())
            fail("the configured background image did not decode");
    }
    const Ubiquitty::Rendering::RenderConfiguration render = Ubiquitty::Preferences::Effective::renderConfiguration(configuration);
    if (!render.fontFamily.isEmpty() && Ubiquitty::Rendering::selectFontFamily(render.fontFamily.view()).rasterizerKind != Ubiquitty::Rendering::FontRasterizerKind::EmbeddedBitmap)
        fail("portable snapshots require font.family = \"GNU Unifont\"");

    const double backingScale = arguments.deviceScale > 0.0 ? arguments.deviceScale : 1.0;
    // native-default: sized for the Unifont raster, not Core's point size.
    const int scale = Ubiquitty::Rendering::unifontScaleForRequestedSize(configuration.fontSize.value_or(10.0), backingScale);
    const int lineSpacing = Ubiquitty::Preferences::Effective::lineSpacingInPixels(render, backingScale);
    Ubiquitty::Rendering::UnifontRasterizer rasterizer(scale, lineSpacing,
        Ubiquitty::Preferences::Effective::characterSpacingInPixels(render, backingScale));
    const Ubiquitty::Rendering::CellMetrics metrics = rasterizer.cellMetrics();

    UnifontMeasurer measurer(rasterizer, metrics.cellWidth);
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    terminal->textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&measurer);
    Ubiquitty::resize(*terminal, arguments.rows, arguments.reflowFromColumns > 0 ? arguments.reflowFromColumns : arguments.columns);
    Ubiquitty::Rendering::TerminalSnapshotOptions options;
    Ubiquitty::Preferences::Effective::applyPageSettings(options.style, render, backingScale);
    const Ubiquitty::IntSize cell { Ubiquitty::Rendering::cellDimension(metrics.cellWidth), Ubiquitty::Rendering::cellDimension(metrics.cellHeight) };
    const Ubiquitty::IntSize textArea { cell.width * terminal->columns, cell.height * terminal->rows };
    const auto edge = [](float pixels) { return std::max(static_cast<int>(std::lround(pixels)), 0); };
    const auto& padding = options.style.paddingInPixels;
    Ubiquitty::publishPixelGeometry(*terminal,
        { cell, textArea,
            { textArea.width + edge(padding.left) + edge(padding.right),
                textArea.height + edge(padding.top) + edge(padding.bottom) } });
    // OSC 66 text sizing is gated on this per-terminal capability, which no
    // frontend sets: it draws unsized without it. The snapshot tool sets it from
    // a case flag so a sized-text golden holds the crop rather than plain text.
    // One flag declares both OSC 66 facts: every snapshot renderer draws
    // sized blocks and proportional runs alike.
    terminal->drawsSizedText = arguments.drawsSizedText;
    terminal->drawsProportionalText = arguments.drawsSizedText;
    // Rich text blocks are laid out from the faces this renderer draws them
    // in, published before the feed so a query in it is answered in them.
    Ubiquitty::Rendering::BundledRichText richText;
    if (!Ubiquitty::RichText::adoptTypography(*terminal, Ubiquitty::Rendering::BundledRichText::typographyFacts(metrics, scale, backingScale)))
        fail("the bundled faces' typography table was refused");

    // Core's rule for a headless render: the configured theme when one is
    // named, the palette a fresh Terminal is constructed with when none is —
    // never a theme this tool picked on its own.
    if (!Ubiquitty::Runtime::seedConfiguredTheme(*terminal, configuration))
        fail("configured theme was not found");

    // The heuristic behind the password badge is a per-terminal capability, and
    // a Terminal constructed here starts without it however the configuration
    // reads. Applied before the feed, because it decides what the feed's settled
    // prompt row means.
    Ubiquitty::setPasswordDetect(terminal.get(), Ubiquitty::Preferences::Effective::passwordDetect(configuration));
    // The `[cursor]` block is terminal state, not frame style: it is what
    // Cursor::resolve falls back to while no program has sent DECSCUSR, and a
    // Terminal constructed here starts on the struct's zero — a block —
    // whatever shape the configuration names. Seeded before the feed, which
    // may itself set a style that outranks it.
    Ubiquitty::Preferences::Effective::applyCursorPreferences(*terminal, configuration);
    // The configuration's trigger rules, before the feed so a rule fires on the
    // lines the feed commits — the route the macOS and Linux snapshot entries
    // take. The refusals are reported because a rule the parser rejected
    // renders identically to one nobody wrote, and a golden minted from that
    // frame would hold the absence forever.
    const std::string triggerRejections
        = Ubiquitty::Runtime::describeTriggerConfig(Ubiquitty::Runtime::applyTriggerConfig(*terminal, configuration));
    if (!triggerRejections.empty())
        Ubiquitty::PAL::logLine(stderr, "ubiquitty-snapshot: {}", triggerRejections);

    if (!arguments.feed.empty())
        Ubiquitty::processBytes(terminal.get(), arguments.feed);
    if (arguments.reflowFromColumns > 0) {
        Ubiquitty::resize(*terminal, arguments.rows, arguments.columns);
        const Ubiquitty::IntSize resizedArea { cell.width * terminal->columns, cell.height * terminal->rows };
        Ubiquitty::publishPixelGeometry(*terminal,
            { cell, resizedArea,
                { resizedArea.width + edge(padding.left) + edge(padding.right),
                    resizedArea.height + edge(padding.top) + edge(padding.bottom) } });
    }
    if (arguments.shouldProbeReply) {
        printReply(*terminal);
        return 0;
    }
    // A snapshot has no program to read what the terminal answered, so the
    // replies are printed for a case's stdout contract.
    if (const std::string replies = Ubiquitty::Diagnostics::drainRepliesForReport(*terminal); !replies.empty())
        Ubiquitty::PAL::logLine(stdout, "snapshot: replies {}", replies);
    if (arguments.shouldClearToMark)
        Ubiquitty::Screen::clearToMarkOrFull(terminal.get());
    else if (arguments.shouldClearPreservingInput)
        Ubiquitty::clearBufferPreserveInput(terminal.get());
    // The selection a drag over those cells leaves, resolved into spans by the
    // facade every frontend reads its selection through.
    Ubiquitty::Selection::Session selection;
    if (arguments.selection)
        selection.state = Ubiquitty::Selection::characterSelectionOverVisibleRows(*terminal, *arguments.selection);
    if (arguments.selectionPoints) {
        const std::array<double, 4>& points = *arguments.selectionPoints;
        Ubiquitty::Selection::selectBetweenPoints(selection, *terminal, points[0], points[1], points[2], points[3]);
    }

    // Automation waits on the same sliced search that publishes frontend highlights.
    if (arguments.searchLiteral && arguments.searchLiteral[0]) {
        const auto status = Ubiquitty::Search::searchScrollbackAndWait(*terminal,
            { .pattern = arguments.searchLiteral, .sensitivity = Ubiquitty::CaseSensitivity::Sensitive, .normalization = Ubiquitty::Screen::ScrollSearch::normalizationForm() });
        if (!status.isComplete || status.isPatternInvalid)
            fail("--search did not complete");
    }

    options.viewOffset = arguments.viewOffset;
    options.isFocused = !arguments.isUnfocused;
    std::vector<Ubiquitty::Rendering::SelectionRowSpan> selectionSpans(static_cast<std::size_t>(terminal->rows) * 2);
    const int selectionSpanCount = Ubiquitty::Selection::viewportSpans(selection, *terminal, arguments.viewOffset,
        Ubiquitty::Selection::ViewportSpans { selectionSpans.data(), selectionSpans.size() });
    if (selectionSpanCount > 0) {
        options.selectionSpans = selectionSpans.data();
        options.selectionSpanCount = selectionSpanCount;
    }
    std::array<Ubiquitty::RichText::BlockSelection, Ubiquitty::RichText::maxBlocksPerScreen> richTextSelections {};
    options.richTextSelections = richTextSelections;
    options.richTextSelectionCount = Ubiquitty::Selection::blockSelections(selection, *terminal, richTextSelections);
    Ubiquitty::Rendering::applyPointSpace(options, metrics.cellHeight, backingScale, render.lineSpacingPoints);
    std::vector<Ubiquitty::Rendering::SelectionRowSpan> searchMatchSpans;
    Ubiquitty::Rendering::collectSearchMatchRowSpans(*terminal, arguments.viewOffset, searchMatchSpans);
    std::vector<Ubiquitty::Rendering::SelectionRowSpan> otherSearchMatchSpans;
    Ubiquitty::Rendering::collectOtherSearchMatchRowSpans(*terminal, arguments.viewOffset, otherSearchMatchSpans);
    if (!searchMatchSpans.empty()) {
        options.searchMatchSpans = searchMatchSpans.data();
        options.searchMatchSpanCount = static_cast<int>(searchMatchSpans.size());
        const Ubiquitty::Rendering::OverlayColor highlight = Ubiquitty::Rendering::focusedSearchMatchColor();
        options.searchMatchHighlight = { highlight.red, highlight.green, highlight.blue, highlight.alpha };
    }
    if (!otherSearchMatchSpans.empty()) {
        options.otherSearchMatchSpans = otherSearchMatchSpans.data();
        options.otherSearchMatchSpanCount = static_cast<int>(otherSearchMatchSpans.size());
    }
    // The trigger tints, gathered from the terminal the way the frontends
    // gather them each frame: a rule that fired on the feed's lines is an
    // overlay span, and a frame captured without the gather draws the rule's
    // absence, however faithfully the rule itself was installed.
    std::vector<Ubiquitty::Rendering::SelectionRowSpan> triggerHighlightSpans;
    Ubiquitty::Rendering::collectTriggerHighlightRowSpans(*terminal, arguments.viewOffset, triggerHighlightSpans);
    if (!triggerHighlightSpans.empty()) {
        options.triggerHighlightSpans = triggerHighlightSpans.data();
        options.triggerHighlightSpanCount = static_cast<int>(triggerHighlightSpans.size());
    }
    // No view layer here: the badge that says the terminal is taking a
    // password has to be part of the frame or it does not exist.
    options.shouldDrawPasswordLock = true;
    options.style.unifontScale = scale;
    // The one conversion, not a copy of it. This tool renders the images the
    // other frontends are compared against, so a mapping of its own would make
    // every arm agree with a reference that agreed with none of them.
    Ubiquitty::Preferences::Effective::applyRuleSettings(options.style, render, backingScale);
    Ubiquitty::Preferences::Effective::applyCursorSettings(options, render);
    Ubiquitty::Preferences::Effective::applyLinkSettings(options, render, terminal.get());
    Ubiquitty::Preferences::Effective::applyTekSettings(options, render);
    Ubiquitty::Preferences::Effective::applyBackgroundImageSettings(options, render, &backgroundImage);
    options.tekDeviceScale = static_cast<float>(backingScale);
    // A still frame is drawn as a bell it was fed rings: the flash at its peak.
    if (terminal->bellPending)
        options.bellFlashAlpha = Ubiquitty::Preferences::Effective::bellFlashAlpha(render, 1, 1);
    options.style.shouldEnhanceTextContrast = render.shouldEnhanceTextContrast;
    options.style.backgroundOpacity = static_cast<float>(render.backgroundOpacity);
    const std::vector<std::uint8_t> png = Ubiquitty::Rendering::renderTerminalSnapshotPNG(*terminal, options, &richText);
    if (png.empty() || !writeFile(arguments.snapshotPath, png))
        fail("failed to render snapshot");
    Ubiquitty::PAL::logLine(stdout, "{}",
        Ubiquitty::Rendering::renderPathMarkerLine(Ubiquitty::Rendering::CellBuilder::Core,
            Ubiquitty::Rendering::FrameCompositor::Software,
            Ubiquitty::Rendering::FrameEncoder::Native));
    return 0;
}
