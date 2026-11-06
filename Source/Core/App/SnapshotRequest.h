// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SnapshotRequest.h — the macOS app's `--snapshot` command, as Core reads and
// renders it: the words it takes, the configuration lines its flags state, and
// the portable software frame its `--snapshot-backend portable` draws.
//
// Header-only, as SnapshotConfigurationFlags.h is, because one frontend reads
// it through Swift: the Swift snapshot runner parses through
// here, states the case's document through here, and hands the Metal frame to
// the surface renderer.
//

#include "Core/Diagnostics/EscapeDecode.h"
#include "Core/Diagnostics/SnapshotArguments.h"
#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/CursorPreferences.h"
#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/RenderStyle.h"
#include "Core/Preferences/SnapshotConfigurationFlags.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/PaddingGeometry.h"
#include "Core/Rendering/RenderPathMarker.h"
#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/Marks.h"
#include "Core/TerminalOperations.h"
#include "PAL/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::App {

/// The command line's words, named so Swift can make one.
using SnapshotWords = std::vector<std::string>;

/// What the command line asked of the snapshot command.
enum class SnapshotRequestParse : std::uint8_t {
    /// No `--snapshot`, or a `--hover-rect` that did not parse: the app
    /// launches as usual.
    NotRequested,
    /// A snapshot, with its arguments read.
    Requested,
    /// A `--snapshot-backend` this app does not have; the run ends with 2.
    UnsupportedBackend,
};

/// Reads the snapshot command's words.
///
/// - Parameters:
///   - words: The terminal's own command line, the program first.
///   - arguments: Receives what the words say.
///   - unsupportedBackend: Receives the backend word when the result is
///     ``SnapshotRequestParse/UnsupportedBackend``, `(missing)` when none
///     followed the flag.
/// - Returns: What the words asked.
[[nodiscard]] inline SnapshotRequestParse parseSnapshotRequest(const SnapshotWords& words, Diagnostics::SnapshotArguments& arguments, std::string& unsupportedBackend)
{
    const std::size_t count = words.size();
    for (std::size_t index = 1; index < count; ++index) {
        const std::string& word = words[index];
        const auto next = [&]() -> const std::string* { return index + 1 < count ? &words[++index] : nullptr; };
        const auto hexInto = [](const std::string* value, std::uint32_t& color) {
            if (!value)
                return false;
            const std::optional<std::uint32_t> parsed = Preferences::parseSnapshotHexColor(*value);
            if (parsed)
                color = *parsed;
            return parsed.has_value();
        };
        if (word == "--snapshot") {
            arguments.requested = true;
            const std::string* value = next();
            arguments.outPath = value ? *value : std::string();
        } else if (word == "--snapshot-backend") {
            const std::string* value = next();
            if (value && *value == "portable") {
                arguments.shouldUsePortableBackend = true;
            } else if (value && (*value == "swift" || *value == "macos")) {
                arguments.shouldUsePortableBackend = false;
            } else {
                unsupportedBackend = value ? *value : std::string("(missing)");
                return SnapshotRequestParse::UnsupportedBackend;
            }
        } else if (word == "--columns") {
            if (const std::string* value = next())
                arguments.columns = std::atoi(value->c_str());
        } else if (word == "--rows") {
            if (const std::string* value = next())
                arguments.rows = std::atoi(value->c_str());
        } else if (word == "--config") {
            const std::string* value = next();
            arguments.configPath = value ? *value : std::string();
        } else if (word == "--font") {
            const std::string* value = next();
            arguments.fontName = value ? *value : std::string();
        } else if (word == "--size") {
            if (const std::string* value = next())
                arguments.fontSize = std::atof(value->c_str());
        } else if (word == "--theme") {
            const std::string* value = next();
            arguments.themeName = value ? *value : std::string();
        } else if (word == "--feed") {
            if (const std::string* value = next()) {
                arguments.feed = Diagnostics::decodeEscapes(*value);
                arguments.haveFeed = true;
            }
        } else if (word == "--no-subpixel") {
            arguments.disableSubpixelText = true;
        } else if (word == "--hover-rect") {
            if (const std::string* value = next()) {
                // The id is optional: a regex or path link has none, and that
                // case must stay expressible because it is the one where a rule
                // keyed on the id would silently draw nothing.
                const int parsed = std::sscanf(value->c_str(), "%d,%d,%d,%d,%d", &arguments.hoverRow0, &arguments.hoverColumn0,
                    &arguments.hoverRow1, &arguments.hoverColumn1, &arguments.hoverLinkIdentifier);
                if (parsed < 4)
                    return SnapshotRequestParse::NotRequested;
                arguments.haveHoverRect = true;
            }
        } else if (word == "--also-feed") {
            if (const std::string* value = next())
                arguments.alsoFeeds.push_back(Diagnostics::decodeEscapes(*value));
        } else if (word == "--also-snapshot") {
            if (const std::string* value = next())
                arguments.alsoSnapshots.push_back(*value);
        } else if (word == "--feed-file") {
            const std::string* value = next();
            arguments.feedFile = value ? *value : std::string();
        } else if (word == "--scale") {
            if (const std::string* value = next())
                arguments.scale = std::atof(value->c_str());
        } else if (word == "--padding-x") {
            if (const std::string* value = next()) {
                arguments.paddingX = std::atof(value->c_str());
                arguments.havePaddingX = true;
            }
        } else if (word == "--padding-y") {
            if (const std::string* value = next()) {
                arguments.paddingY = std::atof(value->c_str());
                arguments.havePaddingY = true;
            }
        } else if (word == "--bg-opacity") {
            if (const std::string* value = next()) {
                arguments.backgroundOpacity = std::atof(value->c_str());
                arguments.haveBackgroundOpacity = true;
            }
        } else if (word == "--bg-image") {
            const std::string* value = next();
            arguments.backgroundImage = value ? *value : std::string();
        } else if (word == "--bg-image-opacity") {
            if (const std::string* value = next())
                arguments.backgroundImageOpacity = std::atof(value->c_str());
        } else if (word == "--bg-image-mode") {
            if (const std::string* value = next()) {
                constexpr std::array modes { "fill", "fit", "stretch", "tile", "center" };
                const auto found = std::ranges::find(modes, *value);
                if (found != modes.end())
                    arguments.backgroundImageMode = static_cast<int>(found - modes.begin());
                else
                    // Silently keeping the default renders a frame the caller
                    // cannot distinguish from the mode working; a numbered mode
                    // is the likeliest wrong spelling, and the words are short.
                    PAL::logLine(stderr, "snapshot: unknown --bg-image-mode '{}' (fill, fit, stretch, tile, center); keeping fill", *value);
            }
        } else if (word == "--tek-glow") {
            arguments.shouldGlowTekVectors = true;
        } else if (word == "--tek-color-mode") {
            if (const std::string* value = next())
                arguments.tekColorMode = std::atoi(value->c_str()) ? 1 : 0;
        } else if (word == "--tek-beam") {
            if (hexInto(next(), arguments.tekBeamColor))
                arguments.haveTekColorArgument = true;
        } else if (word == "--tek-bg") {
            if (hexInto(next(), arguments.tekBackgroundColor))
                arguments.haveTekColorArgument = true;
        } else if (word == "--beam") {
            if (hexInto(next(), arguments.beamColor))
                arguments.haveBeamColor = true;
        } else if (word == "--bg") {
            if (hexInto(next(), arguments.backgroundColor))
                arguments.haveBackgroundColor = true;
        } else if (word == "--link-style-per-kind") {
            arguments.linkStylePerKind = true;
        } else if (word == "--unfocused") {
            arguments.isUnfocused = true;
        } else if (word == "--clear-to-mark") {
            arguments.shouldClearToMark = true;
        } else if (word == "--clear-preserve-input") {
            arguments.shouldClearPreservingInput = true;
        } else if (word == "--draws-sized-text") {
            arguments.drawsSizedText = true;
        } else if (word == "--print-metrics") {
            arguments.shouldPrintMetrics = true;
        } else if (word == "--view-offset") {
            if (const std::string* value = next()) {
                arguments.viewOffset = std::atoi(value->c_str());
                arguments.haveViewOffset = true;
            }
        } else if (word == "--select-rect") {
            const std::string* value = next();
            int row0 = 0, column0 = 0, row1 = 0, column1 = 0;
            if (value && std::sscanf(value->c_str(), "%d,%d,%d,%d", &row0, &column0, &row1, &column1) == 4) {
                arguments.selectRow0 = row0;
                arguments.selectColumn0 = column0;
                arguments.selectRow1 = row1;
                arguments.selectColumn1 = column1;
                arguments.haveSelectRect = true;
            }
        } else if (word == "--select-points") {
            const std::string* value = next();
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            if (value && std::sscanf(value->c_str(), "%lf,%lf,%lf,%lf", &x0, &y0, &x1, &y1) == 4) {
                arguments.selectPointX0 = x0;
                arguments.selectPointY0 = y0;
                arguments.selectPointX1 = x1;
                arguments.selectPointY1 = y1;
                arguments.haveSelectPoints = true;
            }
        } else if (word == "--reflow-from-columns") {
            if (const std::string* value = next())
                arguments.reflowFromColumns = std::atoi(value->c_str());
        } else if (word == "--search") {
            const std::string* value = next();
            arguments.searchLiteral = value ? *value : std::string();
        }
    }
    return arguments.requested ? SnapshotRequestParse::Requested : SnapshotRequestParse::NotRequested;
}

/// Which of the snapshot's flags a statement writes into the case's document.
enum class SnapshotDocumentStage : std::uint8_t {
    /// Font, size, background opacity, the tube and the backdrop: what both
    /// backends draw from.
    EveryBackend,
    /// The theme and per-kind link styling, which only the surface renderer
    /// reads from the document; the portable frame takes them from the
    /// arguments.
    SurfaceRenderer,
};

/// Writes the flags that are the configuration's own keys into the case's
/// document, as lines of it rather than values beside it, so every reader of
/// the configuration draws from the one fold.
///
/// - Parameters:
///   - arguments: The snapshot's arguments.
///   - stage: Which of the flags to write.
///   - document: The case's document, written in place.
inline void stateSnapshotDocument(const Diagnostics::SnapshotArguments& arguments, SnapshotDocumentStage stage, std::string& document)
{
    using Preferences::Key;
    using Preferences::SerializeKind;
    using Preferences::Value;
    const auto state = [&document](Key key, const Value& value) {
        if (std::optional<std::string> written = Preferences::writeValueIntoBody(key, value, document))
            document = std::move(*written);
    };
    if (stage == SnapshotDocumentStage::SurfaceRenderer) {
        if (!arguments.themeName.empty())
            state(Key::AppearanceTheme, Value::ofText(SerializeKind::String, arguments.themeName));
        if (arguments.linkStylePerKind) {
            state(Key::UnderlineLinks, Value::ofBool(SerializeKind::Bool, true));
            state(Key::UnderlineLinkStylePerKind, Value::ofBool(SerializeKind::Bool, true));
        }
        return;
    }
    if (!arguments.fontName.empty())
        state(Key::FontFamily, Value::ofText(SerializeKind::String, arguments.fontName));
    if (arguments.fontSize > 0)
        state(Key::FontSize, Value::ofNumber(SerializeKind::Double, arguments.fontSize));
    if (arguments.haveBackgroundOpacity)
        state(Key::AppearanceBackgroundOpacity, Value::ofNumber(SerializeKind::Double, arguments.backgroundOpacity));
    if (arguments.shouldGlowTekVectors)
        state(Key::TekGlowVectors, Value::ofBool(SerializeKind::Bool, true));
    if (arguments.tekColorMode == 1) {
        state(Key::TekColorMode, Value::ofInteger(SerializeKind::Int, 1));
        state(Key::TekBeamColor, Value::ofText(SerializeKind::String, std::format("#{:06x}", arguments.tekBeamColor & 0xFFFFFFu)));
        state(Key::TekBackgroundColor, Value::ofText(SerializeKind::String, std::format("#{:06x}", arguments.tekBackgroundColor & 0xFFFFFFu)));
    }
    if (!arguments.backgroundImage.empty()) {
        state(Key::AppearanceBackgroundImage, Value::ofText(SerializeKind::String, arguments.backgroundImage));
        state(Key::AppearanceBackgroundImageOpacity, Value::ofNumber(SerializeKind::Double, arguments.backgroundImageOpacity));
        const auto mode = static_cast<Preferences::BackgroundImageMode>(arguments.backgroundImageMode);
        state(Key::AppearanceBackgroundImageMode, Value::ofText(SerializeKind::EnumToken, Preferences::backgroundImageModeToString(mode)));
    }
}

/// Keeps a grid of at least 2 by 2 and a scale of at least 1, the least a
/// frame is drawn at.
///
/// - Parameter arguments: The snapshot's arguments, adjusted in place.
inline void normaliseSnapshotGeometry(Diagnostics::SnapshotArguments& arguments)
{
    arguments.columns = std::max(arguments.columns, 2);
    arguments.rows = std::max(arguments.rows, 2);
    if (arguments.scale < 1.0)
        arguments.scale = 1.0;
}

/// Reads a file's bytes whole, as the case's document or its feed.
///
/// - Parameters:
///   - path: The file.
///   - contents: Receives the bytes, appended.
/// - Returns: Whether the file could be opened.
[[nodiscard]] inline bool readSnapshotFile(const std::string& path, std::string& contents)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::array<char, 4096> buffer {};
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
        contents.append(buffer.data(), count);
    std::fclose(file);
    return true;
}

/// How a portable snapshot ended.
enum class PortableSnapshotOutcome : std::uint8_t {
    /// The PNG was written.
    Written,
    /// `--theme` named no theme.
    ThemeNotFound,
    /// The renderer produced nothing, or the file could not be written.
    WriteFailed,
};

/// Renders the snapshot through the portable software renderer and writes the
/// PNG, the frame `--snapshot-backend portable` asks of this app.
///
/// - Parameters:
///   - arguments: The snapshot's arguments, already normalised.
///   - configuration: The configuration the case's document set.
///   - hasConfiguration: Whether the case named a configuration file; without
///     one, spacing and padding draw at zero rather than at the defaults.
/// - Returns: How the render ended.
[[nodiscard]] inline PortableSnapshotOutcome renderPortableSnapshot(const Diagnostics::SnapshotArguments& arguments, const Preferences::Values& configuration, bool hasConfiguration)
{
    auto terminal = std::make_unique<Terminal>();
    resize(*terminal, arguments.rows, arguments.columns);
    // One flag declares both OSC 66 facts: every snapshot renderer draws
    // sized blocks and proportional runs alike.
    terminal->drawsSizedText = arguments.drawsSizedText;
    terminal->drawsProportionalText = arguments.drawsSizedText;
    terminal->passwordDetect = Preferences::Effective::passwordDetect(configuration);

    // The reader's `[cursor]` settings, through the Core entry a live session
    // reaches from `TerminalSession::start`. This arm builds a bare model
    // instead of a session, so nothing else seeds it: `cursorShapePref` is
    // zero-initialised, 0 is Block, and `effectiveStyleForUserPreference`
    // resolves it through a `default:` arm that reads an unseeded field and a
    // chosen Block the same way.
    Preferences::Effective::applyCursorPreferences(*terminal, configuration);

    Rendering::RenderConfiguration render = Preferences::Effective::renderConfiguration(configuration);
    // Without a configuration file there is nothing to space or inset by: the
    // fold carries the built-in defaults, which are not zero, and a snapshot
    // asked for no configuration would draw them.
    if (!hasConfiguration) {
        render.lineSpacingPoints = 0.0;
        render.paddingLeftPoints = 0.0;
        render.paddingRightPoints = 0.0;
        render.paddingTopPoints = 0.0;
        render.paddingBottomPoints = 0.0;
    }
    // The portable renderer's own size, chosen for the Unifont scale below
    // rather than inherited from Core, and moving it moves goldens.
    // native-default: sized for the Unifont raster, not Core's point size.
    const double requestedSize = arguments.fontSize > 0.0 ? arguments.fontSize : configuration.fontSize.value_or(10.0);
    const double portableScale = 1.0;
    const int unifontScale = Rendering::unifontScaleForRequestedSize(requestedSize, portableScale);
    const int lineSpacingInPixels = Preferences::Effective::lineSpacingInPixels(render, portableScale);
    Rendering::UnifontRasterizer rasterizer(unifontScale, lineSpacingInPixels, Preferences::Effective::characterSpacingInPixels(render, portableScale));
    const Rendering::CellMetrics metrics = rasterizer.cellMetrics();
    Rendering::TerminalSnapshotOptions options;
    Preferences::Effective::applyPageSettings(options.style, render, portableScale);
    if (arguments.havePaddingX) {
        options.style.paddingInPixels.left = static_cast<float>(arguments.paddingX * portableScale);
        options.style.paddingInPixels.right = static_cast<float>(arguments.paddingX * portableScale);
    }
    if (arguments.havePaddingY) {
        options.style.paddingInPixels.top = static_cast<float>(arguments.paddingY * portableScale);
        options.style.paddingInPixels.bottom = static_cast<float>(arguments.paddingY * portableScale);
    }
    const IntSize cell { Rendering::cellDimension(metrics.cellWidth), Rendering::cellDimension(metrics.cellHeight) };
    const IntSize textArea { cell.width * terminal->columns, cell.height * terminal->rows };
    const auto edge = [](float pixels) { return std::max(static_cast<int>(std::lround(pixels)), 0); };
    const auto& padding = options.style.paddingInPixels;
    publishPixelGeometry(*terminal,
        { cell, textArea, { textArea.width + edge(padding.left) + edge(padding.right), textArea.height + edge(padding.top) + edge(padding.bottom) } });

    // --theme folds into the configuration, so Core's one rule reads one input:
    // the named theme, or the palette a fresh Terminal is constructed with when
    // nothing names one.
    Preferences::Values themed = configuration;
    if (!arguments.themeName.empty())
        themed.themeName = arguments.themeName;
    if (!Runtime::seedConfiguredTheme(*terminal, themed))
        return PortableSnapshotOutcome::ThemeNotFound;

    if (arguments.haveFeed && !arguments.feed.empty())
        processBytes(terminal.get(), arguments.feed);
    if (arguments.shouldClearToMark)
        Screen::clearToPreviousMark(terminal.get(), nullptr, [] { }, [](std::span<const char>) { });
    else if (arguments.shouldClearPreservingInput)
        clearBufferPreserveInput(terminal.get());

    options.shouldDrawPasswordLock = true;
    options.viewOffset = arguments.haveViewOffset ? arguments.viewOffset : 0;
    Preferences::Effective::applyRuleSettings(options.style, render, portableScale);
    options.style.unifontScale = unifontScale;
    Preferences::Effective::applyCursorSettings(options, render);
    // A still frame is drawn as a bell it was fed rings: the flash at its peak.
    if (terminal->bellPending)
        options.bellFlashAlpha = Preferences::Effective::bellFlashAlpha(render, 1, 1);
    options.style.shouldEnhanceTextContrast = render.shouldEnhanceTextContrast;
    options.style.backgroundOpacity = static_cast<float>(render.backgroundOpacity);
    // Link styling is Core's fold, with --link-style-per-kind forcing the
    // per-kind rules on top of the settings. Both backends describe the same
    // screen, so a link that draws underlined and dotted in one has to draw
    // underlined and dotted in the other.
    Preferences::Effective::applyLinkSettings(options, render, terminal.get(),
        arguments.linkStylePerKind ? Preferences::Effective::PerKindLinkStyling::Forced : Preferences::Effective::PerKindLinkStyling::FromSettings);
    Preferences::Effective::applyTekSettings(options, render);
    options.tekDeviceScale = static_cast<float>(portableScale);

    const std::vector<std::uint8_t> png = Rendering::renderTerminalSnapshotPNG(*terminal, options);
    if (png.empty())
        return PortableSnapshotOutcome::WriteFailed;
    std::FILE* file = std::fopen(arguments.outPath.c_str(), "wb");
    if (!file)
        return PortableSnapshotOutcome::WriteFailed;
    const bool didWriteAll = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    if (std::fclose(file) != 0 || !didWriteAll)
        return PortableSnapshotOutcome::WriteFailed;
    PAL::logLine(stdout, "Ubiquitty --snapshot: wrote portable reference {}", arguments.outPath);
    // This branch uses the portable software renderer, so it names the same path
    // the portable tool does: an arm asking for the macOS renderer and landing
    // here differs from the arm it named in both fields.
    PAL::logLine(stdout, "{}", Rendering::renderPathMarkerLine(Rendering::CellBuilder::Core, Rendering::FrameCompositor::Software, Rendering::FrameEncoder::Native));
    return PortableSnapshotOutcome::Written;
}

} // namespace Ubiquitty::App
