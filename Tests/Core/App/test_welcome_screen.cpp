// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_welcome_screen.cpp — the Welcome screen's bytes, rendered, and the
// vocabulary a click on one of its buttons is matched against.
//
// The screen and the dispatcher share one table of action names. These cases
// hold the two ends of it: every target the screen emits names an action, a
// target naming anything else names none, and the buttons a reader sees are
// the links that carry those targets once a real terminal has drawn them.
//
// The logo is Core's choice from the pane's facts: the app icon as a kitty
// graphics image — ClearDark on a dark ground, Default on a light one, the
// render nearest the box's size, square and centred in the box, scaled down
// to the space a small grid has — or no logo rows at all where it cannot be
// drawn; there is no text stand-in. These cases hold that choice, the drawn
// quad's size, aspect and place, and that the image leaves nothing to copy.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Accessibility/TextProjection.h"
#include "Core/App/WelcomeIconData.h"
#include "Core/App/WelcomeScreen.h"
#include "Core/CellPen.h"
#include "Core/Codec/Base64.h"
#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/RowMeta.h"
#include "Core/Screen/Screen.h"
#include "Core/Screen/VisibleLink.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/Themes/TermPaletteFromTheme.h"
#include "Core/Themes/Themes.h"
#include "Core/VT/Constants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Ubiquitty::App::Welcome::Action;
using Ubiquitty::App::Welcome::actionForURI;
using Ubiquitty::App::Welcome::ImageDrawing;
using Ubiquitty::App::Welcome::isActionURI;
using Ubiquitty::App::Welcome::Pane;
using Ubiquitty::App::Welcome::paneOf;
using Ubiquitty::App::Welcome::screenContent;
namespace IconData = Ubiquitty::App::Welcome::IconData;

constexpr int defaultRows = 24;
constexpr int defaultColumns = 80;
constexpr int scanCells = 2048;
// The default 12 pt face's drawn cell, in device pixels at 2x: a 7.2 pt
// advance and an 18 pt line, so the box is taller than it is wide.
constexpr double cellWidth = 14.4;
constexpr double cellHeight = 36;
constexpr double deviceScale = 2;
// The logo's box, and the column it starts in.
constexpr int logoColumns = 16;
constexpr int logoRows = 8;
constexpr int logoColumn = 32;
// The row an 80-by-24 grid leaves above the screen: the screen and the
// prompt take 21 of its rows, two or more spare.
constexpr int topRows = 1;
// The house themes' grounds: Noir and Noir Clair.
constexpr Ubiquitty::ColorRGBA darkGround { 0x15, 0x17, 0x1c };
constexpr Ubiquitty::ColorRGBA lightGround { 0xf4, 0xf5, 0xf8 };

// Scratch for the link scan, sized past any row these cases draw.
struct Scratch {
    std::uint32_t codePoints[scanCells];
    std::uint16_t linkIDs[scanCells];

    Ubiquitty::Screen::LinkScanBuffer buffer() { return { codePoints, linkIDs, scanCells }; }
};

// The screen on a terminal that draws sized blocks and proportional runs, as
// every frontend that shows it does — the runs are claims the proportional
// face fills — or on one that draws neither, where every run prints plainly
// at the cursor and the labels read as text.
enum class SizedText {
    Plain,
    Drawn,
};

// A pane that shows the image, on `ground`, at a cell and a scale.
Pane imagePane(Ubiquitty::ColorRGBA ground, double width = cellWidth, double height = cellHeight, double scale = deviceScale)
{
    Pane pane;
    pane.ground = ground;
    pane.imageDrawing = ImageDrawing::Drawn;
    pane.cellPixelWidth = width;
    pane.cellPixelHeight = height;
    pane.deviceScale = scale;
    return pane;
}

// A terminal whose published geometry is the pane's, the way a frontend's
// publish leaves it.
std::unique_ptr<Ubiquitty::Terminal> geometryTerminal(const Pane& pane)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, defaultRows, defaultColumns);
    terminal->cellPixelWidth = static_cast<std::uint16_t>(std::lround(pane.cellPixelWidth));
    terminal->cellPixelHeight = static_cast<std::uint16_t>(std::lround(pane.cellPixelHeight));
    terminal->textAreaPixelWidth = static_cast<std::uint16_t>(std::lround(pane.cellPixelWidth * defaultColumns));
    terminal->textAreaPixelHeight = static_cast<std::uint16_t>(std::lround(pane.cellPixelHeight * defaultRows));
    return terminal;
}

std::unique_ptr<Ubiquitty::Terminal> renderedWelcomeScreen(SizedText sizedText = SizedText::Plain, const Pane& pane = imagePane(darkGround))
{
    auto terminal = geometryTerminal(pane.cellPixelWidth > 0 ? pane : imagePane(darkGround));
    terminal->drawsSizedText = sizedText == SizedText::Drawn;
    terminal->drawsProportionalText = sizedText == SizedText::Drawn;
    UbiquittyTest::feed(*terminal, screenContent(pane));
    return terminal;
}

// The `OSC 8` targets in `content`, in order, closing links included as empty
// strings.
std::vector<std::string> linkTargets(std::string_view content)
{
    constexpr std::string_view opener = "\x1b]8;;";
    constexpr std::string_view terminator = "\x1b\\";
    std::vector<std::string> targets;
    std::size_t position = content.find(opener);
    while (position != std::string_view::npos) {
        const std::size_t targetStart = position + opener.size();
        const std::size_t targetEnd = content.find(terminator, targetStart);
        if (targetEnd == std::string_view::npos)
            break;
        targets.emplace_back(content.substr(targetStart, targetEnd - targetStart));
        position = content.find(opener, targetEnd);
    }
    return targets;
}

// The content's kitty graphics transfer: the first block's keys, and the
// payload of every block joined, or two empty strings when it carries none.
struct KittyImage {
    std::string keys;
    std::string payload;
    int blocks = 0;
};

KittyImage kittyImage(std::string_view content)
{
    constexpr std::string_view opener = "\x1b_G";
    KittyImage image;
    for (std::size_t start = content.find(opener); start != std::string_view::npos; start = content.find(opener, start + 1)) {
        const std::size_t keysStart = start + opener.size();
        const std::size_t semicolon = content.find(';', keysStart);
        const std::size_t end = content.find("\x1b\\", keysStart);
        if (semicolon == std::string_view::npos || end == std::string_view::npos || semicolon > end)
            return {};
        if (image.blocks == 0)
            image.keys = std::string(content.substr(keysStart, semicolon - keysStart));
        image.payload += content.substr(semicolon + 1, end - semicolon - 1);
        ++image.blocks;
    }
    return image;
}

// A render's PNG as the base64 a transfer carries.
std::string base64Of(const IconData::IconAsset& asset)
{
    std::string encoded(Ubiquitty::Codec::base64EncodedLength(asset.png.size()), '\0');
    const auto written = Ubiquitty::Codec::base64Encode(asset.png, { encoded.data(), encoded.size() });
    encoded.resize(written ? written->size() : 0);
    return encoded;
}

// The render the content transfers, or null.
// A composite is the dark-ground render whose canvas its stated size is; a
// picture is the light-ground render whose PNG it carries.
const IconData::IconAsset* transferredAsset(std::string_view content)
{
    const KittyImage image = kittyImage(content);
    int side = 0;
    if (std::sscanf(image.keys.c_str(), "a=T,f=32,s=%d", &side) == 1) {
        for (const IconData::IconAsset& asset : IconData::darkGroundIcons) {
            if (asset.canvasPixelSize == side && image.payload.size() == Ubiquitty::Codec::base64EncodedLength(static_cast<std::size_t>(side) * side * 4))
                return &asset;
        }
        return nullptr;
    }
    for (const IconData::IconAsset& asset : IconData::lightGroundIcons) {
        if (image.payload == base64Of(asset))
            return &asset;
    }
    return nullptr;
}

// The composite a transfer carries, decoded to straight RGBA.
std::vector<std::uint8_t> transferredComposite(std::string_view content)
{
    const std::string payload = kittyImage(content).payload;
    std::vector<char> bytes(payload.size());
    const auto decoded = Ubiquitty::Codec::base64Decode(payload, { bytes.data(), bytes.size() });
    if (!decoded || decoded->empty())
        return {};
    return { decoded->begin(), decoded->end() };
}

bool isDarkGroundAsset(const IconData::IconAsset* asset)
{
    for (const IconData::IconAsset& candidate : IconData::darkGroundIcons) {
        if (asset == &candidate)
            return true;
    }
    return false;
}

// Whether the content holds a braille code point (U+2800-U+28FF, E2 A0-A3 xx
// in UTF-8): the screen has no text stand-in for its icon.
bool hasBraille(std::string_view content)
{
    for (std::size_t at = content.find('\xe2'); at != std::string_view::npos && at + 1 < content.size(); at = content.find('\xe2', at + 1)) {
        const auto lead = static_cast<unsigned char>(content[at + 1]);
        if (lead >= 0xa0 && lead <= 0xa3)
            return true;
    }
    return false;
}

// The one image placement on the terminal, or null.
const Ubiquitty::Image::TerminalImagePlacement* onlyPlacement(const Ubiquitty::Terminal& terminal)
{
    const Ubiquitty::Image::TerminalImagePlacement* found = nullptr;
    for (const auto& placement : terminal.images.placements) {
        if (!placement.used)
            continue;
        if (found)
            return nullptr;
        found = &placement;
    }
    return found;
}

// The quad a renderer draws the logo as, at the pane's device-pixel cell, the
// grid's origin at zero.
Ubiquitty::Image::ImageQuad drawnQuad(const Ubiquitty::Terminal& terminal, const Pane& pane, int* count)
{
    std::array<Ubiquitty::Image::ImageQuad, Ubiquitty::Image::maxTerminalImagePlacementCount> quads {};
    *count = Ubiquitty::Image::resolveImagePlacements(
        { terminal.images.stored.data(), terminal.images.stored.size() },
        { terminal.images.placements.data(), terminal.images.placements.size() },
        { terminal.columns, terminal.rows },
        { static_cast<float>(pane.cellPixelWidth), static_cast<float>(pane.cellPixelHeight) },
        {}, 0, quads);
    return quads[0];
}

// The row the double-height name's top half is on, or -1.
int nameRow(const Ubiquitty::Terminal& terminal)
{
    for (int row = 0; row < terminal.rows; ++row) {
        if (terminal.grid->rowMeta(row).lineAttribute == Ubiquitty::lineAttributeDoubleHeightTop)
            return row;
    }
    return -1;
}

// One visible row as text: ASCII as itself, blanks as spaces, anything wider
// than ASCII as '?', so a column index into the string is a column on screen.
std::string rowText(const Ubiquitty::Terminal& terminal, int row)
{
    std::string text;
    for (int column = 0; column < terminal.columns; ++column) {
        const Ubiquitty::Cell* here = UbiquittyTest::cell(terminal, row, column);
        const std::uint32_t codePoint = here ? here->codePoint : 0;
        if (codePoint == 0)
            text.push_back(' ');
        else if (codePoint < 0x80)
            text.push_back(static_cast<char>(codePoint));
        else
            text.push_back('?');
    }
    return text;
}

// The target of the link under (row, column), or "" when there is none. Asks
// only for a declared link, which is what a plain click opens.
std::string declaredTargetAt(Ubiquitty::Terminal& terminal, int row, int column)
{
    Scratch scratch;
    char uri[Ubiquitty::VT::maxLinkURILength] = { 0 };
    Ubiquitty::Screen::VisibleLink link {};
    if (!Ubiquitty::Screen::linkAtVisibleCell(&terminal, 0, row, column, scratch.buffer(), uri, &link,
            Ubiquitty::Screen::LinkScanScope::Declared))
        return {};
    return { uri };
}

struct Button {
    std::string_view label;
    std::string_view target;
};

} // namespace

TEST(anActionTargetNamesItsActionByExactName)
{
    CHECK(actionForURI("ubiquitty:action/shell-integration") == Action::ShellIntegration);
    CHECK(actionForURI("ubiquitty:action/connect") == Action::Connect);
    CHECK(actionForURI("ubiquitty:action/settings") == Action::Settings); // preferences-vocabulary-ok: the button's own name
    CHECK(actionForURI("ubiquitty:action/accessibility") == Action::Accessibility);

    // A near miss is no action at all: nothing about a target is parsed beyond
    // the one string comparison.
    CHECK(actionForURI("ubiquitty:action/shell-integration/extra") == Action::Unrecognized);
    CHECK(actionForURI("ubiquitty:action/shell-integrationx") == Action::Unrecognized);
    CHECK(actionForURI("ubiquitty:action/Connect") == Action::Unrecognized);
    CHECK(actionForURI("ubiquitty:action/") == Action::Unrecognized);
    CHECK(actionForURI("https://example.com/ubiquitty:action/settings") == Action::Unrecognized);
    CHECK(actionForURI("") == Action::Unrecognized);
}

TEST(theActionNamespaceIsClaimedWholeAndNothingElse)
{
    // An unknown name is still the frontend's, so it never falls through to a
    // handler or to the system opener.
    CHECK(isActionURI("ubiquitty:action/"));
    CHECK(isActionURI("ubiquitty:action/no-such-action"));
    CHECK(isActionURI("ubiquitty:action/shell-integration"));

    CHECK(!isActionURI("https://example.com/"));
    CHECK(!isActionURI("xubiquitty:action/settings"));
    CHECK(!isActionURI("ubiquitty:settings"));
    CHECK(!isActionURI(""));
}

TEST(everyTargetOnTheScreenNamesAnActionAndEveryLinkCloses)
{
    const std::vector<std::string> expected {
        "ubiquitty:action/shell-integration", "",
        "ubiquitty:action/connect", "",
        "ubiquitty:action/settings", "",
        "ubiquitty:action/accessibility", ""
    };
    for (const Pane& pane : { Pane {}, imagePane(darkGround) }) {
        const std::vector<std::string> targets = linkTargets(screenContent(pane));
        REQUIRE(targets.size() == expected.size());
        for (std::size_t index = 0; index < targets.size(); ++index) {
            CHECK_STR_EQ(targets[index], expected[index]);
            if (!targets[index].empty())
                CHECK_MSG(actionForURI(targets[index]) != Action::Unrecognized, "a target the screen emits names an action");
        }
    }
}

TEST(theScreenFitsTheDefaultGridWithoutScrollingItsHeadline)
{
    auto terminal = renderedWelcomeScreen();
    CHECK_MSG(terminal->screens.active->cursorY < defaultRows - 1, "the screen leaves a row for the shell prompt that follows it");
    CHECK_EQ(terminal->screens.active->cursorX, 0);

    // The name is one double-height pair, and the only one.
    int topRow = -1;
    int pairs = 0;
    for (int row = 0; row + 1 < terminal->rows; ++row) {
        if (terminal->grid->rowMeta(row).lineAttribute == Ubiquitty::lineAttributeDoubleHeightTop) {
            ++pairs;
            topRow = row;
            CHECK_EQ(terminal->grid->rowMeta(row + 1).lineAttribute, Ubiquitty::lineAttributeDoubleHeightBottom);
        }
    }
    CHECK_EQ(pairs, 1);
    CHECK_EQ(topRow, topRows + logoRows + 1);
    CHECK_STR_EQ(rowText(*terminal, topRow).substr(0, 9), "Ubiquitty");
    CHECK_STR_EQ(rowText(*terminal, topRow + 1).substr(0, 9), "Ubiquitty");
}

TEST(aNarrowPaneKeepsItsLogoAndActionsOnScreen)
{
    for (const int columns : { 64, 30 }) {
        Pane pane = imagePane(darkGround);
        pane.columns = columns;
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*terminal, defaultRows, columns);
        terminal->drawsSizedText = true;
        terminal->drawsProportionalText = true;
        UbiquittyTest::feed(*terminal, screenContent(pane));

        CHECK_MSG(nameRow(*terminal) >= 6, "the logo and name remain on screen");
        CHECK_MSG(terminal->screens.active->cursorY <= defaultRows - 2, "a row remains for the shell prompt");
        for (const std::string_view action : { "shell-integration", "connect", "settings", "accessibility" }) {
            const std::string target = "ubiquitty:action/" + std::string(action);
            bool found = false;
            for (int row = 0; row < terminal->rows && !found; ++row) {
                for (int column = 0; column < terminal->columns; ++column) {
                    if (declaredTargetAt(*terminal, row, column) == target) {
                        found = true;
                        break;
                    }
                }
            }
            CHECK_MSG(found, target.c_str());
        }
    }
}

// The screen starts a row down only where two rows are spare, so one still
// follows the prompt, and a tall grid does not push it further: the screen
// stays at the top, where a reader starts reading.
TEST(theScreenStartsARowDownOnlyWithTwoRowsToSpare)
{
    const auto nameRowAt = [](int rows) {
        Pane pane = imagePane(darkGround);
        pane.rows = rows;
        auto terminal = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*terminal, rows, defaultColumns);
        UbiquittyTest::feed(*terminal, screenContent(pane));
        CHECK_MSG(terminal->screens.active->cursorY <= rows - 2, "a row follows the prompt");
        return nameRow(*terminal);
    };
    CHECK_EQ(nameRowAt(22), logoRows + 1);
    CHECK_EQ(nameRowAt(23), logoRows + 2);
    CHECK_EQ(nameRowAt(24), logoRows + 2);
    CHECK_EQ(nameRowAt(60), logoRows + 2);
}

// The icon as a renderer draws it: the canvas's quad, and the icon's own
// square on it, centred, at the size the render's table records.
struct DrawnIcon {
    double left = 0;
    double top = 0;
    double width = 0;
    double height = 0;
    Ubiquitty::Image::ImageFilter filter = Ubiquitty::Image::ImageFilter::Nearest;
};

DrawnIcon drawnIcon(const Ubiquitty::Terminal& terminal, const Pane& pane, const IconData::IconAsset& asset)
{
    int count = 0;
    const Ubiquitty::Image::ImageQuad quad = drawnQuad(terminal, pane, &count);
    CHECK_MSG(count == 1, "the logo draws as one quad");
    const double canvasWidth = quad.maxXInPixels - quad.minXInPixels;
    const double canvasHeight = quad.maxYInPixels - quad.minYInPixels;
    const double scale = static_cast<double>(asset.pixelSize) / asset.canvasPixelSize;
    DrawnIcon icon;
    icon.width = canvasWidth * scale;
    icon.height = canvasHeight * scale;
    icon.left = quad.minXInPixels + (canvasWidth - icon.width) / 2;
    icon.top = quad.minYInPixels + (canvasHeight - icon.height) / 2;
    icon.filter = quad.filter;
    return icon;
}

// The icon is one image, transferred in the protocol's blocks, drawn square —
// its true aspect within a pixel — as wide as the box within a pixel, centred
// across the box, and down as far below the top as above the name, to the
// pixel the offset is stated in; one texel a device pixel at the default face,
// shadow canvas included. The text below starts on the row it starts on under
// the braille.
TEST(theLogoIsTheIconSquareAndCentredInTheTilesBox)
{
    const Pane pane = imagePane(darkGround);
    const std::string content = screenContent(pane);
    CHECK_MSG(!hasBraille(content), "no braille");
    const KittyImage image = kittyImage(content);
    // A 317 px canvas, 22 columns from column 29, 3.6 px rounded to 4 px down.
    CHECK_STR_EQ(image.keys, "a=T,f=32,s=317,v=317,q=2,C=1,c=22,Y=4,m=1");
    CHECK(image.blocks > 1);
    const IconData::IconAsset* asset = transferredAsset(content);
    REQUIRE(asset != nullptr);
    CHECK_EQ(asset->pixelSize, 230);
    CHECK_EQ(asset->canvasPixelSize, 317);
    CHECK(isDarkGroundAsset(asset));

    auto terminal = geometryTerminal(pane);
    UbiquittyTest::feed(*terminal, content);
    const Ubiquitty::Image::TerminalImagePlacement* placement = onlyPlacement(*terminal);
    REQUIRE_MSG(placement != nullptr, "the logo is one image placement");
    CHECK_EQ(static_cast<int>(placement->cursorX), logoColumn - 3);
    CHECK_EQ(static_cast<int>(placement->cursorY), topRows);

    const DrawnIcon icon = drawnIcon(*terminal, pane, *asset);
    CHECK_MSG(std::fabs(icon.width - icon.height) <= 1.0, "the icon keeps its aspect");
    CHECK_MSG(std::fabs(icon.width - logoColumns * cellWidth) < 1.0, "as wide as the box");
    CHECK_MSG(icon.width == 230.0, "at the render's own size");
    CHECK_MSG(icon.filter == Ubiquitty::Image::ImageFilter::Nearest, "drawn one texel a pixel");
    const double slackLeft = icon.left - logoColumn * cellWidth;
    const double slackRight = (logoColumn + logoColumns) * cellWidth - (icon.left + icon.width);
    CHECK_MSG(std::fabs(slackLeft - slackRight) <= 1.0, "centred across the box");
    const double above = icon.top - topRows * cellHeight;
    const double belowToName = (topRows + logoRows + 1) * cellHeight - (icon.top + icon.height);
    CHECK_MSG(std::fabs(above - belowToName) <= 1.01, "as far below the top of its box as above the name");
    CHECK_MSG(above > cellHeight, "clear of the box's top by more than a row");

    CHECK_EQ(nameRow(*terminal), topRows + logoRows + 1);
}

// The pane's measured cell is already in device pixels. Changing the backing
// scale alone cannot move the same image; an odd grid width exercises both
// sub-cell offsets rather than only the vertical one.
TEST(theIconOffsetsStayInDevicePixelsAcrossBackingScales)
{
    for (const int scale : { 1, 2, 3 }) {
        Pane pane = imagePane(darkGround, cellWidth, cellHeight, scale);
        pane.columns = 79;
        const std::string content = screenContent(pane);
        CHECK_STR_EQ(kittyImage(content).keys, "a=T,f=32,s=317,v=317,q=2,C=1,c=22,X=7,Y=4,m=1");
        auto terminal = geometryTerminal(pane);
        Ubiquitty::resize(*terminal, pane.rows, pane.columns);
        UbiquittyTest::feed(*terminal, content.substr(0, content.find("\x1b#3")));
        const Ubiquitty::Image::TerminalImagePlacement* placement = onlyPlacement(*terminal);
        REQUIRE(placement != nullptr);
        CHECK_EQ(placement->pixelOffsetX, 7);
        CHECK_EQ(placement->pixelOffsetY, 4);
        int count = 0;
        const Ubiquitty::Image::ImageQuad quad = drawnQuad(*terminal, pane, &count);
        CHECK_EQ(count, 1);
        CHECK(std::fabs(quad.minXInPixels - 410.2) < 0.001);
        CHECK(std::fabs(quad.minYInPixels - (topRows * cellHeight + 4.0)) < 0.001);
        CHECK(std::fabs(quad.maxXInPixels - quad.minXInPixels - 317.0) < 0.001);
        CHECK(std::fabs(quad.maxYInPixels - quad.minYInPixels - 317.0) < 0.001);
    }
}

// A cell wider than half its height makes the box wider than tall, so the
// rows above the name bound the icon — with its shadow's canvas, which cannot
// start above the first row — and it is still centred across the box; the
// nearest render at or above the side is the one sent, and drawn filtered
// down.
TEST(aWideCellCentresTheIconAcrossTheBox)
{
    const Pane pane = imagePane(darkGround, 18, 32);
    const std::string content = screenContent(pane);
    const IconData::IconAsset* asset = transferredAsset(content);
    REQUIRE(asset != nullptr);
    CHECK_EQ(asset->pixelSize, 230);
    auto terminal = geometryTerminal(pane);
    UbiquittyTest::feed(*terminal, content);
    const DrawnIcon icon = drawnIcon(*terminal, pane, *asset);
    CHECK(std::fabs(icon.width - icon.height) <= 1.0);
    CHECK_MSG(icon.height <= logoRows * 32.0, "within the box's height");
    const double above = icon.top - topRows * 32.0;
    const double belowToName = (topRows + logoRows + 1) * 32.0 - (icon.top + icon.height);
    CHECK_MSG(std::fabs(above - belowToName) <= 1.01, "centred above the name");
    const double slackLeft = icon.left - logoColumn * 18.0;
    const double slackRight = (logoColumn + logoColumns) * 18.0 - (icon.left + icon.width);
    CHECK_MSG(std::fabs(slackLeft - slackRight) <= 1.01, "centred to the pixel the offset is stated in");
    CHECK(icon.filter == Ubiquitty::Image::ImageFilter::Smooth);
    CHECK_EQ(nameRow(*terminal), topRows + logoRows + 1);
}

// The buttons' colours follow the ground by one rule: a plain plate is the
// ground moved toward the far pole — a light plate on a light ground, not the
// dark slate — and every label reads on its plate at WCAG AA or better.
TEST(theButtonsAreDerivedFromTheGroundAndTheirLabelsRead)
{
    const auto luminance = [](int red, int green, int blue) {
        const auto linear = [](int channel) {
            const double value = channel / 255.0;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(red) + 0.7152 * linear(green) + 0.0722 * linear(blue);
    };
    for (const Ubiquitty::ColorRGBA ground : { darkGround, lightGround }) {
        const std::string content = screenContent(imagePane(ground));
        const double groundLuminance = luminance(ground.red, ground.green, ground.blue);
        int buttons = 0;
        for (std::size_t at = content.find("\x1b[48;2;"); at != std::string::npos; at = content.find("\x1b[48;2;", at + 1)) {
            int plate[3] = {};
            int label[3] = {};
            if (std::sscanf(content.c_str() + at, "\x1b[48;2;%d;%d;%dm\x1b[38;2;%d;%d;%dm", &plate[0], &plate[1], &plate[2], &label[0], &label[1], &label[2]) != 6)
                continue;
            ++buttons;
            const double plateLuminance = luminance(plate[0], plate[1], plate[2]);
            const double labelLuminance = luminance(label[0], label[1], label[2]);
            const double contrast = (std::max(plateLuminance, labelLuminance) + 0.05) / (std::min(plateLuminance, labelLuminance) + 0.05);
            CHECK_MSG(contrast >= 4.5, "every label reads on its plate");
            const bool isLightGround = groundLuminance > 0.5;
            CHECK_MSG(isLightGround == (plateLuminance > 0.5), "the plate is on the ground's side of light and dark");
        }
        CHECK_EQ(buttons, 4);
    }
}

// The render follows the backing scale: the default face's box, 115.2 points
// across, is the 115, 230 or 346 render's side at 1x, 2x and 3x.
TEST(eachScaleGetsTheRenderItsBoxIs)
{
    const std::array<int, 3> expected { 115, 230, 346 };
    const std::array<int, 3> canvases { 158, 317, 475 };
    for (const int scale : { 1, 2, 3 }) {
        const IconData::IconAsset* asset = transferredAsset(screenContent(imagePane(darkGround, 7.2 * scale, 18.0 * scale, scale)));
        REQUIRE(asset != nullptr);
        CHECK_EQ(asset->pixelSize, expected[static_cast<std::size_t>(scale - 1)]);
        CHECK_EQ(asset->canvasPixelSize, canvases[static_cast<std::size_t>(scale - 1)]);
    }
}

// The appearance follows the pane's ground: ClearDark on a dark one, Default
// on a light one, the line at half luma, the rule a theme's own darkness uses.
TEST(aDarkGroundGetsClearDarkAndALightOneDefault)
{
    CHECK(isDarkGroundAsset(transferredAsset(screenContent(imagePane(darkGround)))));
    CHECK(!isDarkGroundAsset(transferredAsset(screenContent(imagePane(lightGround)))));
    CHECK(transferredAsset(screenContent(imagePane(lightGround))) == &IconData::lightGroundIcons[1]);
    CHECK(isDarkGroundAsset(transferredAsset(screenContent(imagePane({ 127, 127, 127 })))));
    CHECK(!isDarkGroundAsset(transferredAsset(screenContent(imagePane({ 128, 128, 128 })))));
}

// Every render decodes through the decoder a program's image goes through, to
// the square its table says.
TEST(everyRenderDecodesToItsSize)
{
    for (const auto* table : { &IconData::darkGroundIcons, &IconData::lightGroundIcons }) {
        for (const IconData::IconAsset& asset : *table) {
            Pane pane = imagePane(darkGround);
            auto terminal = geometryTerminal(pane);
            std::string transfer = "\x1b_Ga=T,f=100,q=2,C=1;";
            // Every PNG, the dark-ground layers among them, decodes through
            // the decoder a program's image goes through.
            transfer += base64Of(asset);
            transfer += "\x1b\\";
            UbiquittyTest::feed(*terminal, transfer);
            const Ubiquitty::Image::TerminalImage* stored = nullptr;
            for (const auto& candidate : terminal->images.stored) {
                if (candidate.used)
                    stored = &candidate;
            }
            REQUIRE(stored != nullptr);
            CHECK_EQ(static_cast<int>(stored->width), asset.canvasPixelSize);
            CHECK_EQ(static_cast<int>(stored->height), asset.canvasPixelSize);
        }
    }
}

// The pane is the terminal's own: the ground its theme gave it, its grid, and
// the drawn cell its frontend published, recovered from the text area at the
// fractional advance. So the house dark theme gets ClearDark and its light
// twin Default, decided in Core from the session.
TEST(thePaneIsTheTerminalsOwnTheme)
{
    for (const char* name : { "Noir", "Noir Clair" }) {
        auto terminal = geometryTerminal(imagePane(darkGround));
        REQUIRE(Ubiquitty::Themes::applyThemeToTerminal(*terminal, Ubiquitty::Themes::indexForName(name)));
        const Pane pane = paneOf(*terminal, Ubiquitty::Preferences::Values {}, deviceScale, ImageDrawing::Drawn);
        CHECK(pane.isGeometryPublished);
        CHECK(std::fabs(pane.cellPixelWidth - cellWidth) < 0.001);
        CHECK(std::fabs(pane.cellPixelHeight - cellHeight) < 0.001);
        CHECK_EQ(pane.columns, defaultColumns);
        CHECK_EQ(pane.rows, defaultRows);
        const bool isDarkTheme = std::string_view(name) == "Noir";
        CHECK(isDarkGroundAsset(transferredAsset(screenContent(pane))) == isDarkTheme);
    }
}

// A grid too small for the box scales the icon down to the space it has —
// the screen's width, the rows above the name — keeping its aspect, centred,
// from a smaller render filtered down; the name keeps its place below.
TEST(aSmallGridScalesTheIconDownToItsSpace)
{
    Pane narrow = imagePane(darkGround);
    narrow.columns = 30;
    const std::string content = screenContent(narrow);
    const IconData::IconAsset* asset = transferredAsset(content);
    REQUIRE(asset != nullptr);
    // Only the logo: the screen's text is laid out for 80 columns and wraps in
    // 30, scrolling the picture with it.
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, defaultRows, 30);
    UbiquittyTest::feed(*terminal, content.substr(0, content.find("\x1b#3")));
    const DrawnIcon icon = drawnIcon(*terminal, narrow, *asset);
    CHECK_MSG(std::fabs(icon.width - icon.height) <= 1.0, "the icon keeps its aspect");
    CHECK_MSG(icon.width < logoColumns * cellWidth, "smaller than the box");
    CHECK_MSG(icon.width >= 32.0, "and still an icon");
    const double canvasRight = icon.left + icon.width * asset->canvasPixelSize / asset->pixelSize;
    CHECK_MSG(canvasRight <= 30 * cellWidth + 1.0, "its canvas, shadow and all, within the grid");
    const double slackLeft = icon.left;
    const double slackRight = 30 * cellWidth - (icon.left + icon.width);
    CHECK_MSG(std::fabs(slackLeft - slackRight) <= cellWidth, "centred across the grid");
    CHECK_EQ(terminal->screens.active->cursorY, topRows + logoRows + 1);

    // Few rows: the logo takes what is left above the blank row and the name.
    Pane shortGrid = imagePane(darkGround);
    shortGrid.rows = 6;
    auto shortTerminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*shortTerminal, 6, defaultColumns);
    const std::string shortContent = screenContent(shortGrid);
    REQUIRE(transferredAsset(shortContent) != nullptr);
    UbiquittyTest::feed(*shortTerminal, shortContent.substr(0, shortContent.find("\x1b#3")));
    const DrawnIcon shortIcon = drawnIcon(*shortTerminal, shortGrid, *transferredAsset(shortContent));
    CHECK_MSG(shortIcon.height <= 4 * cellHeight + 1.0, "the icon is no taller than the four logo rows");
    const double above = shortIcon.top;
    const double belowToName = 5 * cellHeight - (shortIcon.top + shortIcon.height);
    CHECK_MSG(std::fabs(above - belowToName) <= 1.01, "centred above the name");
}

// Where the icon cannot be drawn there is no stand-in: the logo's rows are
// left out and the name opens the screen, a row down as the 24 rows leave
// it. That is an arm that draws no images — none today — and a space too
// small for a recognisable icon.
TEST(withoutAnIconTheNameOpensTheScreen)
{
    const auto expectNoLogo = [](const Pane& pane, const char* why) {
        const std::string content = screenContent(pane);
        CHECK_MSG(content.find("\x1b_G") == std::string::npos, why);
        CHECK_MSG(!hasBraille(content), why);
        CHECK_MSG(content.starts_with("\x1b[H\x1b[2J\x1b[2;1H\x1b#3"), why);
    };
    Pane refused = imagePane(darkGround);
    refused.imageDrawing = ImageDrawing::Refused;
    expectNoLogo(refused, "an arm that draws no images");
    Pane tiny = imagePane(darkGround);
    tiny.columns = 3;
    expectNoLogo(tiny, "a grid too narrow for a 32 px icon");
}

// A terminal that has published no geometry still gets the icon: the pane
// carries the configured face's nominal cell, says it is nominal, and the
// screen draws from it.
TEST(anUnpublishedPaneDrawsFromTheConfiguredFace)
{
    auto unpublished = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*unpublished, defaultRows, defaultColumns);
    Ubiquitty::Preferences::Values config;
    config.fontSize = 12.0;
    const Pane pane = paneOf(*unpublished, config, deviceScale, ImageDrawing::Drawn);
    CHECK(!pane.isGeometryPublished);
    CHECK(std::fabs(pane.cellPixelWidth - cellWidth) < 0.001);
    CHECK(std::fabs(pane.cellPixelHeight - cellHeight) < 0.001);
    const IconData::IconAsset* asset = transferredAsset(screenContent(pane));
    REQUIRE(asset != nullptr);
    CHECK_EQ(asset->pixelSize, 230);
}

// No pane of any shape gets braille: the stand-in is gone, not merely unused.
TEST(noPaneGetsBraille)
{
    std::vector<Pane> panes { Pane {}, imagePane(darkGround), imagePane(lightGround), imagePane(darkGround, 18, 32), imagePane(darkGround, 0, 0) };
    for (const int columns : { 1, 3, 10, 30, 49, 50, 80, 200 }) {
        for (const int rows : { 1, 2, 4, 9, 24 }) {
            Pane pane = imagePane(darkGround);
            pane.columns = columns;
            pane.rows = rows;
            panes.push_back(pane);
        }
    }
    Pane refused = imagePane(darkGround);
    refused.imageDrawing = ImageDrawing::Refused;
    panes.push_back(refused);
    for (const Pane& pane : panes)
        CHECK(!hasBraille(screenContent(pane)));
}

// The name is set in the pane's default foreground — the body text's colour,
// whatever the theme — bold, while the tagline under it keeps the accent.
TEST(theNameIsTheBodyTextsColour)
{
    for (const Ubiquitty::ColorRGBA ground : { darkGround, lightGround }) {
        auto terminal = renderedWelcomeScreen(SizedText::Plain, imagePane(ground));
        const int top = nameRow(*terminal);
        REQUIRE(top >= 0);
        for (const int row : { top, top + 1 }) {
            const Ubiquitty::Cell* first = UbiquittyTest::cell(*terminal, row, 0);
            REQUIRE(first != nullptr);
            const Ubiquitty::Pen& pen = Ubiquitty::cellPen(*first);
            CHECK_MSG(pen.foregroundIndex == Ubiquitty::colorDefault, "the name takes the theme's own foreground");
            CHECK_MSG(pen.attributes.contains(Ubiquitty::VT::Attribute::Bold), "and stays bold");
        }
        int taglineRow = -1;
        for (int row = top + 2; row < terminal->rows && taglineRow < 0; ++row) {
            if (rowText(*terminal, row).find("the terminal that keeps up") != std::string::npos)
                taglineRow = row;
        }
        REQUIRE(taglineRow > 0);
        const std::size_t column = rowText(*terminal, taglineRow).find("the terminal");
        const Ubiquitty::Pen& tagline = Ubiquitty::cellPen(*UbiquittyTest::cell(*terminal, taglineRow, static_cast<int>(column)));
        CHECK_MSG(tagline.foregroundIndex == Ubiquitty::colorRGB && tagline.foregroundRed == 136 && tagline.foregroundBlue == 190,
            "the tagline keeps the accent");
    }
}

// On a dark ground the tile takes the ground's own tone: a lighter shade of
// it, hue kept, in OKLab, for a blue-black, a purple-black and a neutral
// ground alike; the light grounds keep the Default rendition's black tile.
// The composite is the same bytes every time.
TEST(aDarkTileTakesTheGroundsTone)
{
    struct Ground {
        const char* name;
        Ubiquitty::ColorRGBA colour;
    };
    const Ground darkGrounds[] = { { "blue-black", { 0x0d, 0x11, 0x17 } }, { "purple-black", { 0x19, 0x17, 0x24 } }, { "neutral", { 0x28, 0x28, 0x28 } } };
    const auto toOKLab = [](double red, double green, double blue) {
        const auto linear = [](double value) {
            value /= 255;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        const double r = linear(red), g = linear(green), b = linear(blue);
        const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
        const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
        const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
        return std::array<double, 3> { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
    };
    for (const Ground& ground : darkGrounds) {
        const std::string content = screenContent(imagePane(ground.colour));
        CHECK_MSG(content == screenContent(imagePane(ground.colour)), "the composite is deterministic");
        const IconData::IconAsset* asset = transferredAsset(content);
        REQUIRE(asset != nullptr);
        REQUIRE(isDarkGroundAsset(asset));
        const std::vector<std::uint8_t> composite = transferredComposite(content);
        REQUIRE(composite.size() == static_cast<std::size_t>(asset->canvasPixelSize) * asset->canvasPixelSize * 4);
        // A patch of plain tile: high on the icon, centred, below the rim
        // and above the mark.
        const int inset = (asset->canvasPixelSize - asset->pixelSize) / 2;
        const int top = inset + asset->pixelSize * 8 / 100;
        const int left = inset + asset->pixelSize / 2 - 8;
        std::array<double, 3> mean {};
        int samples = 0;
        for (int y = top; y < top + 6; ++y) {
            for (int x = left; x < left + 16; ++x) {
                const std::size_t index = (static_cast<std::size_t>(y) * asset->canvasPixelSize + x) * 4;
                CHECK(composite[index + 3] == 255);
                for (std::size_t channel = 0; channel < 3; ++channel)
                    mean[channel] += composite[index + channel];
                ++samples;
            }
        }
        for (double& channel : mean)
            channel /= samples;
        // The expected tile at that height: the derivation's head and foot,
        // mixed as far down the gradient as the patch sits.
        const Ubiquitty::App::Welcome::TileTone tone = Ubiquitty::App::Welcome::tileTone(ground.colour);
        const double along = (top + 3 - inset) / (asset->pixelSize * 0.7);
        const auto expected = toOKLab(tone.top.red + (tone.bottom.red - tone.top.red) * along, tone.top.green + (tone.bottom.green - tone.top.green) * along,
            tone.top.blue + (tone.bottom.blue - tone.top.blue) * along);
        const auto actual = toOKLab(mean[0], mean[1], mean[2]);
        const double deltaE = std::hypot(expected[0] - actual[0], expected[1] - actual[1], expected[2] - actual[2]);
        CHECK_MSG(deltaE < 0.02, ground.name);
        // And the tone is the ground's: lighter, the same hue.
        const auto groundLab = toOKLab(ground.colour.red, ground.colour.green, ground.colour.blue);
        const auto tileLab = toOKLab(tone.top.red, tone.top.green, tone.top.blue);
        CHECK_MSG(std::fabs(tileLab[0] - groundLab[0] - Ubiquitty::App::Welcome::tileLightnessStep) < 0.01, ground.name);
        if (std::hypot(groundLab[1], groundLab[2]) > 0.01)
            CHECK_MSG(std::fabs(std::atan2(tileLab[2], tileLab[1]) - std::atan2(groundLab[2], groundLab[1])) < 0.25, ground.name);
    }
    for (const Ubiquitty::ColorRGBA light : { lightGround, Ubiquitty::ColorRGBA { 0xfd, 0xf6, 0xe3 } }) {
        const IconData::IconAsset* asset = transferredAsset(screenContent(imagePane(light)));
        REQUIRE(asset != nullptr);
        CHECK_MSG(!isDarkGroundAsset(asset), "a light ground keeps the Default rendition");
    }
}

// The image writes no cell: Select All copies the screen's text with nothing in
// the logo's rows, and an assistive reader meets the name first.
TEST(theImageLeavesNothingToCopyOrReadAloud)
{
    for (const Ubiquitty::ColorRGBA ground : { darkGround, lightGround }) {
        auto terminal = renderedWelcomeScreen(SizedText::Plain, imagePane(ground));
        REQUIRE(onlyPlacement(*terminal) != nullptr);
        for (int row = 0; row < logoRows; ++row)
            CHECK_STR_EQ(rowText(*terminal, row), std::string(static_cast<std::size_t>(defaultColumns), ' '));

        Ubiquitty::Selection::Session session;
        Ubiquitty::Selection::init(session);
        Ubiquitty::Selection::selectAllText(session, *terminal);
        const std::string copied = Ubiquitty::Selection::selectedText(session, *terminal);
        CHECK_MSG(copied.find("f=100") == std::string::npos, "no sequence bytes in the copy");
        CHECK_MSG(copied.find("iVBORw0KGgo") == std::string::npos, "no payload in the copy");
        const std::size_t firstText = copied.find_first_not_of(" \n");
        REQUIRE(firstText != std::string::npos);
        CHECK_STR_EQ(copied.substr(firstText, 9), "Ubiquitty");

        const std::string spoken = Ubiquitty::Accessibility::visibleScreenText(*terminal, 0);
        const std::size_t firstSpoken = spoken.find_first_not_of(" \n");
        REQUIRE(firstSpoken != std::string::npos);
        CHECK_STR_EQ(spoken.substr(firstSpoken, 9), "Ubiquitty");
    }
}

// With sized text drawn, every button is a run on a plate the width of its
// claim, and the link is the whole claim: a click anywhere on the plate is
// the action, and the cell past the plate is not.
TEST(aSizedButtonIsTheLinkAcrossItsWholeClaim)
{
    auto terminal = renderedWelcomeScreen(SizedText::Drawn);
    const Button buttons[] = {
        { "", "ubiquitty:action/shell-integration" },
        { "", "ubiquitty:action/connect" },
        { "", "ubiquitty:action/settings" }, // preferences-vocabulary-ok: the button's own name
        { "", "ubiquitty:action/accessibility" },
    };
    const int expectedCells[] = { 26, 14, 10, 15 };
    int buttonRow = -1;
    for (int row = 0; row < terminal->rows && buttonRow < 0; ++row) {
        for (int column = 0; column < terminal->columns; ++column) {
            if (declaredTargetAt(*terminal, row, column) == buttons[0].target) {
                buttonRow = row;
                break;
            }
        }
    }
    REQUIRE_MSG(buttonRow >= 0, "the buttons are on screen");
    int previousEnd = -1;
    for (std::size_t index = 0; index < 4; ++index) {
        int first = -1;
        int last = -1;
        for (int column = 0; column < terminal->columns; ++column) {
            if (declaredTargetAt(*terminal, buttonRow, column) != buttons[index].target)
                continue;
            if (first < 0)
                first = column;
            CHECK_MSG(last < 0 || column == last + 1, "a button's claim is one contiguous stretch");
            last = column;
        }
        REQUIRE_MSG(first >= 0, "every button is on the row");
        CHECK_EQ(last - first + 1, expectedCells[index]);
        CHECK_MSG(previousEnd < 0 || first == previousEnd + 3, "two cells between buttons, outside both links");
        previousEnd = last;
    }
    CHECK_MSG(declaredTargetAt(*terminal, buttonRow, previousEnd + 1).empty(), "the cell past the last plate is no link");
}

TEST(eachButtonARenderedScreenShowsIsTheLinkToItsAction)
{
    auto terminal = renderedWelcomeScreen(SizedText::Plain, imagePane(darkGround));
    const Button buttons[] = {
        { "Install shell integration", "ubiquitty:action/shell-integration" },
        { "K  Connect", "ubiquitty:action/connect" },
        { "Settings", "ubiquitty:action/settings" }, // preferences-vocabulary-ok: the label a reader sees
        { "Accessibility", "ubiquitty:action/accessibility" },
    };
    for (const Button& button : buttons) {
        int foundRow = -1;
        int foundColumn = -1;
        for (int row = 0; row < terminal->rows && foundRow < 0; ++row) {
            const std::size_t column = rowText(*terminal, row).find(button.label);
            if (column != std::string::npos) {
                foundRow = row;
                foundColumn = static_cast<int>(column);
            }
        }
        REQUIRE_MSG(foundRow >= 0, "every button's label is on screen");
        // The first and last cells of the label both carry the target: a
        // button is clickable across its whole face.
        const int lastColumn = foundColumn + static_cast<int>(button.label.size()) - 1;
        CHECK_STR_EQ(declaredTargetAt(*terminal, foundRow, foundColumn), button.target);
        CHECK_STR_EQ(declaredTargetAt(*terminal, foundRow, lastColumn), button.target);
        // The cell after the label is outside the link.
        CHECK_STR_EQ(declaredTargetAt(*terminal, foundRow, lastColumn + 1), "");
    }
}

UBIQUITTY_TEST_MAIN()
